// Volumetric fog's passes, as Godot 4.6's volumetric_fog_process.glsl runs them for its Forward+
// renderer, a thread a froxel (a column of them, gathering): each froxel's light and density
// (Density), the blur across (FilterX) and down (FilterY), and the light the fog in front of each
// froxel adds and how much it lets through (Integrate). Its CPU twin is src/volumetric_fog.cpp,
// which this follows line for line; a change to one goes in the other.
//
// The volume is a storage buffer of a float4 a froxel, in rows across, then down, then slices
// deeper, read from source and written to target: the density pass reads the frame before's
// density from source and writes this frame's, which the next frame reads. Shadow atlases are read
// a texel at a time and filtered here, and the omni and spot lights are the meshes' clusters.
//
// Metal has one list of buffers, uniforms first, so its SPIR-V is compiled with SLASH_MSL and
// numbers the buffers after the uniforms. The others keep SDL's sets: sampled textures and
// read-only buffers in 0, read-write buffers in 1, uniforms in 2.

#ifdef SLASH_MSL
#define FOG_UNIFORMS register(b0)
#define SHADOW_UNIFORMS register(b1)
#define LIGHTS_BUFFER register(t2)
#define CLUSTERS_BUFFER register(t3)
#define ITEMS_BUFFER register(t4)
#define SOURCE_BUFFER register(t5)
#define TARGET_BUFFER register(u6)
#else
#define FOG_UNIFORMS register(b0, space2)
#define SHADOW_UNIFORMS register(b1, space2)
#define LIGHTS_BUFFER register(t4, space0)
#define CLUSTERS_BUFFER register(t5, space0)
#define ITEMS_BUFFER register(t6, space0)
#define SOURCE_BUFFER register(t7, space0)
#define TARGET_BUFFER register(u0, space1)
#endif

#include "tonemap.hlsl"

struct Light {
    float4 position;  // xyz, then 1 / range
    float4 direction; // toward a directional light, the way a spot light shines; then the type
    float4 color;     // linear, times energy and pi; then the specular amount
    float4 params;    // attenuation, the cone's cosine, 1 / spot attenuation, the cull mask
    float4 shadow;    // its shadow slot or -1, 1 when baked, its volumetric fog energy
};

// A FogVolume (src/volumetric_fog.hpp's FogVolumeUniforms), kFogVolumeItems float4s of the
// frame's items.
struct Volume {
    float4 to_local[4]; // the world to the volume's space, a column each
    float4 position;   // its place in the world, then its shape (Godot's RS::FogVolumeShape)
    float4 size;       // its size (twice the froxels for a world shape), then 1 for a FogMaterial
    int4 corner;       // the first froxel it fills
    int4 corner_end;   // past the last froxel it fills
    float4 albedo;     // linear, then the material's density
    float4 glow;       // the emission, linear, then the height falloff
    float4 fade;       // the edge fade
};

static const uint kFogVolumeItems = 11u;
static const uint kFogVoxelGIItems = 15u;

static const uint kPassDensity = 0u;
static const uint kPassFilterX = 1u;
static const uint kPassFilterY = 2u;
static const uint kPassIntegrate = 3u;

cbuffer Fog : FOG_UNIFORMS {
    uint4 size;         // froxels across, down and deep, then the pass
    float4 frustum;     // the half extents at the near plane (x, y) and at length (z, w)
    float4 params;      // length, detail spread, the far plane's distance, anisotropy
    float4 scattering;  // the albedo, linear, times density; then density
    float4 emission;    // linear, times energy and density; then 1 to reproject
    float4 ambient;     // the ambient light, linear; then ambient inject times its energy
    float4 jitter;      // the frame's point in its froxels, then how much it reprojects
    float4 sky;         // 1 for the sky's light, its contribution, its energy, directional lights
    float4 screen;      // the frame's width and height in pixels
    float4 cluster;     // tiles across, a tile's words of light bits, slices per depth, decal words
    float4x4 view_to_world;
    float4x4 to_previous; // the camera's space to the frame before's
    float4 sky_rows[3];   // the world to the sky's space, a row each
    Light lights[8];      // the directional lights
    uint4 items;          // how many fog volumes, then VoxelGIs, fog_items holds
    float4 voxel_gi;      // gi_inject
};

struct DirectionalShadow {
    float4x4 matrices[4];
    float4 splits;
    float4 normal_bias;
    float4 params; // fade from and to (view z), opacity, how many cascades
    float4 options;
};

struct PositionalShadow {
    float4x4 light;
    float4x4 faces[6];
    float4 rects[6];
    float4 params;   // normal bias, opacity, an atlas texel, how many faces
    float4 position; // the light's place, then its range
};

cbuffer Shadows : SHADOW_UNIFORMS {
    float4 camera_z;
    float4 filters;
    DirectionalShadow directional_shadows[4];
    PositionalShadow positional_shadows[16];
};

Texture2D<float4> directional_atlas : register(t0, space0);
Texture2D<float4> positional_atlas : register(t1, space0);
TextureCube<float4> radiance_map : register(t2, space0);
SamplerState directional_sampler : register(s0, space0);
SamplerState positional_sampler : register(s1, space0);
Texture2D<float4> voxel_gi_atlas : register(t3, space0);
SamplerState radiance_sampler : register(s2, space0);
SamplerState voxel_gi_sampler : register(s3, space0);

StructuredBuffer<Light> clustered_lights : LIGHTS_BUFFER;
StructuredBuffer<uint> clusters : CLUSTERS_BUFFER;
// The frame's fog volumes (kFogVolumeItems float4s each), then the VoxelGIs whose light it takes
// (kFogVoxelGIItems each, as mesh.hlsl's VoxelGI: three rows from the world to its cells, its
// size and levels, its energy and the rest, then each level's place in voxel_gi_atlas).
StructuredBuffer<float4> fog_items : ITEMS_BUFFER;
StructuredBuffer<float4> source : SOURCE_BUFFER;
RWStructuredBuffer<float4> target : TARGET_BUFFER;

static const float kInvFogFade = 10.0;
static const float kRadianceMaxLevel = 5.0;
static const uint kClusterSize = 32u;
static const uint kClusterSlices = 32u;
static const float kGauss[7] = {0.071303, 0.131514, 0.189879, 0.214607,
                                0.189879, 0.131514, 0.071303};

uint froxel(uint3 at) {
    return (at.z * size.y + at.y) * size.x + at.x;
}

// The source volume at a texture coordinate, as a 3D texture with a linear sampler that clamps
// to the edge reads it.
float4 sample_source(float3 uvw) {
    float3 n = (float3)size.xyz;
    float3 p = clamp(uvw * n - 0.5, (float3)0.0, n - 1.0);
    float3 base = floor(p);
    uint3 low = (uint3)base;
    uint3 high = min(low + 1u, size.xyz - 1u);
    float3 t = p - base;
    float4 a = lerp(source[froxel(uint3(low.x, low.y, low.z))],
                    source[froxel(uint3(high.x, low.y, low.z))], t.x);
    float4 b = lerp(source[froxel(uint3(low.x, high.y, low.z))],
                    source[froxel(uint3(high.x, high.y, low.z))], t.x);
    float4 c = lerp(source[froxel(uint3(low.x, low.y, high.z))],
                    source[froxel(uint3(high.x, low.y, high.z))], t.x);
    float4 d = lerp(source[froxel(uint3(low.x, high.y, high.z))],
                    source[froxel(uint3(high.x, high.y, high.z))], t.x);
    return lerp(lerp(a, b, t.y), lerp(c, d, t.y), t.z);
}

// A shadow atlas's depth at a texture coordinate, filtered linearly from its four texels.
float atlas_depth(bool positional, float2 uv) {
    uint width;
    uint height;
    if (positional) {
        positional_atlas.GetDimensions(width, height);
    } else {
        directional_atlas.GetDimensions(width, height);
    }
    float s = (float)width;
    float2 p = clamp(uv, -1.0, 2.0) * s - 0.5;
    float2 base = floor(p);
    float2 f = p - base;
    int last = (int)width - 1;
    int2 a = clamp((int2)base, 0, last);
    int2 b = clamp((int2)base + 1, 0, last);
    float t00;
    float t10;
    float t01;
    float t11;
    if (positional) {
        t00 = positional_atlas.Load(int3(a.x, a.y, 0)).r;
        t10 = positional_atlas.Load(int3(b.x, a.y, 0)).r;
        t01 = positional_atlas.Load(int3(a.x, b.y, 0)).r;
        t11 = positional_atlas.Load(int3(b.x, b.y, 0)).r;
    } else {
        t00 = directional_atlas.Load(int3(a.x, a.y, 0)).r;
        t10 = directional_atlas.Load(int3(b.x, a.y, 0)).r;
        t01 = directional_atlas.Load(int3(a.x, b.y, 0)).r;
        t11 = directional_atlas.Load(int3(b.x, b.y, 0)).r;
    }
    return lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
}

float omni_attenuation(float distance, float inv_range, float decay) {
    float nd = distance * inv_range;
    nd *= nd;
    nd *= nd;
    nd = max(1.0 - nd, 0.0);
    nd *= nd;
    return nd * pow(max(distance, 0.0001), -decay);
}

float henyey_greenstein(float cos_theta, float g) {
    const float k = 0.0795774715459; // 1 / (4 * pi)
    float base = 1.0 + g * g - 2.0 * g * cos_theta;
    return k * (1.0 - g * g) / (base * sqrt(base));
}

float smooth(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x < edge0 ? 0.0 : 1.0;
    }
    float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

float3 view_place(float3 unit) {
    float t = unit.z;
    float3 view = float3((unit.x * 2.0 - 1.0) * lerp(frustum.x, frustum.z, t),
                         (unit.y * 2.0 - 1.0) * lerp(frustum.y, frustum.w, t), -params.x * t);
    view.y = -view.y;
    return view;
}

float directional_fog_shadow(uint slot, float3 world, float view_z) {
    float depth_z = -view_z;
    uint count = (uint)directional_shadows[slot].params.w;
    uint cascade = 0u;
    while (cascade + 1u < count && !(depth_z < directional_shadows[slot].splits[cascade])) {
        ++cascade;
    }
    float4x4 m = directional_shadows[slot].matrices[cascade];
    float4 at = mul(m, float4(world, 1.0));
    at.xyz /= at.w;
    float z_range = 1.0 / length(m[2].xyz);
    float depth = atlas_depth(false, at.xy);
    float lit = exp(min(0.0, at.z - depth) * z_range * kInvFogFade);
    float4 shadow_params = directional_shadows[slot].params;
    lit = lerp(lit, 1.0, smooth(shadow_params.x, shadow_params.y, view_z));
    return lerp(1.0 - shadow_params.z, 1.0, lit);
}

float positional_fog_shadow(uint slot, float3 world) {
    uint face = 0u;
    if (positional_shadows[slot].params.w > 1.5) {
        float3 local = mul(positional_shadows[slot].light, float4(world, 1.0)).xyz;
        float3 a = abs(local);
        if (a.x >= a.y && a.x >= a.z) {
            face = local.x > 0.0 ? 0u : 1u;
        } else if (a.y >= a.z) {
            face = local.y < 0.0 ? 2u : 3u;
        } else {
            face = local.z > 0.0 ? 4u : 5u;
        }
    }
    float4 at = mul(positional_shadows[slot].faces[face], float4(world, 1.0));
    float4 rect = positional_shadows[slot].rects[face];
    float2 uv = clamp(at.xy / at.w, rect.xy, rect.zw);
    float reference = at.z / at.w;
    float depth = atlas_depth(true, uv);
    float far = positional_shadows[slot].position.w;
    float near = min(0.025, far);
    float stored = near * far / (depth * (far - near) + near);
    float point_distance = near * far / (reference * (far - near) + near);
    float lit = exp(min(0.0, stored - point_distance) * kInvFogFade);
    return lerp(1.0 - positional_shadows[slot].params.y, 1.0, lit);
}

uint range_mask(uint word, uint first, uint end) {
    int low = clamp((int)first - (int)(word * 32u), 0, 32);
    int high = clamp((int)end - (int)(word * 32u), 0, 32);
    uint below_high = high >= 32 ? 0xFFFFFFFFu : (1u << (uint)high) - 1u;
    uint below_low = low >= 32 ? 0xFFFFFFFFu : (1u << (uint)low) - 1u;
    return below_high & ~below_low;
}

// What the fog volumes add to a froxel at id, whose point is world, as Godot's volumetric_fog.glsl
// adds each into its density, light and emissive maps: the density in 1024ths, then the scattering
// and emission, each a channel's units as Godot packs them into 11, 11 and 10 bits, held there.
// The k-th float4 of the index-th VoxelGI in fog_items.
float4 voxel_gi_item(uint index, uint k) {
    return fog_items[(items.x * kFogVolumeItems) + (index * kFogVoxelGIItems) + k];
}

// A VoxelGI's texture at one level, as a linear sampler that clamps reads a 3D texture: bilinear
// in the two slices around uvw, each read inside its own tile, then between them (mesh.hlsl's
// voxel_gi_level).
float4 voxel_gi_level(uint index, uint level, float3 uvw) {
    float3 extent = max(floor(voxel_gi_item(index, 3u).xyz / (float)(1u << level)), 1.0);
    float4 place = voxel_gi_item(index, 5u + level);
    uint columns = (uint)place.z;
    float x = clamp(uvw.x * extent.x, 0.5, extent.x - 0.5);
    float y = clamp(uvw.y * extent.y, 0.5, extent.y - 0.5);
    float z = clamp(uvw.z * extent.z - 0.5, 0.0, extent.z - 1.0);
    float z0 = floor(z);
    uint width;
    uint height;
    voxel_gi_atlas.GetDimensions(width, height);
    float2 size = float2((float)width, (float)height);
    uint low = (uint)z0;
    uint high = (uint)min(z0 + 1.0, extent.z - 1.0);
    float2 at_low = float2(place.x + (float)(low % columns) * extent.x + x,
                           place.y + (float)(low / columns) * extent.y + y);
    float2 at_high = float2(place.x + (float)(high % columns) * extent.x + x,
                            place.y + (float)(high / columns) * extent.y + y);
    float4 a = voxel_gi_atlas.SampleLevel(voxel_gi_sampler, at_low / size, 0.0);
    float4 b = voxel_gi_atlas.SampleLevel(voxel_gi_sampler, at_high / size, 0.0);
    return a + (b - a) * (z - z0);
}

// The light a VoxelGI gives the fog at a point, as volumetric_fog_process.glsl's gi_inject: every
// level at the point, front to back, times the VoxelGI's energy; none outside its box.
float3 fog_voxel_gi_light(uint index, float3 world) {
    float4 row_x = voxel_gi_item(index, 0u);
    float4 row_y = voxel_gi_item(index, 1u);
    float4 row_z = voxel_gi_item(index, 2u);
    float3 position = float3(dot(row_x.xyz, world) + row_x.w, dot(row_y.xyz, world) + row_y.w,
                             dot(row_z.xyz, world) + row_z.w);
    float4 size = voxel_gi_item(index, 3u);
    float3 bounds = size.xyz;
    if (any(position < 0.0) || any(position >= bounds)) {
        return (float3)0.0;
    }
    float3 uvw = position / bounds;
    float4 light = (float4)0.0;
    uint levels = (uint)size.w;
    for (uint level = 0u; level < levels; ++level) {
        light += (1.0 - light.a) * voxel_gi_level(index, level, uvw);
    }
    return light.rgb * voxel_gi_item(index, 4u).x;
}

struct VolumeSums {
    int density;
    uint3 scattering;
    uint3 emission;
};

// The index-th fog volume in fog_items.
Volume load_volume(uint index) {
    uint at = index * kFogVolumeItems;
    Volume v;
    v.to_local[0] = fog_items[at];
    v.to_local[1] = fog_items[at + 1u];
    v.to_local[2] = fog_items[at + 2u];
    v.to_local[3] = fog_items[at + 3u];
    v.position = fog_items[at + 4u];
    v.size = fog_items[at + 5u];
    v.corner = asint(fog_items[at + 6u]);
    v.corner_end = asint(fog_items[at + 7u]);
    v.albedo = fog_items[at + 8u];
    v.glow = fog_items[at + 9u];
    v.fade = fog_items[at + 10u];
    return v;
}

VolumeSums fog_volumes(uint3 id, float3 world) {
    VolumeSums sums;
    sums.density = 0;
    sums.scattering = (uint3)0u;
    sums.emission = (uint3)0u;
    const uint3 limits = uint3(2047u, 2047u, 1023u);
    int3 at = (int3)id;
    for (uint i = 0u; i < items.x; ++i) {
        Volume volume = load_volume(i);
        if (any(at < volume.corner.xyz) || any(at >= volume.corner_end.xyz)) {
            continue;
        }
        float3 local_pos = (volume.to_local[0] * world.x + volume.to_local[1] * world.y +
                            volume.to_local[2] * world.z + volume.to_local[3]).xyz;
        float3 full = volume.size.xyz;
        float3 half_size = full / 2.0;
        uint shape = (uint)volume.position.w;
        bool material = volume.size.w != 0.0;
        float sdf = -1.0;
        if (shape == 0u) {
            float k0 = length(local_pos / half_size);
            float k1 = length(local_pos / (half_size * half_size));
            sdf = k0 * (k0 - 1.0) / k1;
        } else if (shape == 1u) {
            float inv_height = 1.0 / max(0.001, half_size.y);
            float radius = 1.0 / max(0.001, min(half_size.x, half_size.z) * 0.5);
            float hypotenuse = sqrt(radius * radius + inv_height * inv_height);
            float2 c = float2(radius / hypotenuse, inv_height / hypotenuse);
            float q = length(local_pos.xz);
            sdf = max(dot(c, float2(q, local_pos.y - half_size.y)), -half_size.y - local_pos.y);
        } else if (shape == 2u) {
            float2 d = abs(float2(length(local_pos.xz), local_pos.y)) -
                       float2(min(half_size.x, half_size.z), half_size.y);
            sdf = min(max(d.x, d.y), 0.0) + length(max(d, (float2)0.0));
        } else if (shape == 3u) {
            float3 q = abs(local_pos) - half_size;
            sdf = length(max(q, (float3)0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
        }
        // Godot's default material doesn't read the distance, so its cells outside are culled.
        float cull_mask = 1.0;
        if (shape <= 3u && !material) {
            cull_mask = 1.0 - smooth(-0.1, 0.0, sdf);
        }
        if (!(cull_mask > 0.0)) {
            continue;
        }
        float density = 1.0;
        float3 albedo = (float3)1.0;
        float3 emission = (float3)0.0;
        if (material) {
            float height = world.y - volume.position.y;
            density = volume.albedo.w * clamp(exp2(-volume.glow.w * height), 0.0, 1.0);
            float edge = clamp(-2.0 * sdf / min(min(full.x, full.y), full.z), 0.0, 1.0);
            density *= edge > 0.0 ? pow(edge, volume.fade.x) : 0.0;
            albedo = volume.albedo.rgb;
            emission = volume.glow.rgb;
        }
        density *= cull_mask;
        if (!(abs(density) > 0.001)) {
            continue;
        }
        sums.density += (int)(density * 1024.0);
        float amount = clamp(density, 0.0, 1.0);
        if (material) {
            float3 glow = clamp(emission * amount, (float3)0.0, (float3)4.0);
            uint3 units = (uint3)(glow * float3(511.0, 511.0, 255.0));
            sums.emission = min(sums.emission + units, limits);
        }
        float3 scattered = clamp(albedo * amount, (float3)0.0, (float3)1.0);
        uint3 units = (uint3)(scattered * float3(2047.0, 2047.0, 1023.0));
        sums.scattering = min(sums.scattering + units, limits);
    }
    return sums;
}

float4 density_pass(uint3 id) {
    float3 cell = 1.0 / (float3)size.xyz;
    float3 place = (float3)id;
    float spread = params.y;
    float3 unit = place * cell + cell * 0.5;
    unit.z = pow(unit.z, spread);
    float3 view = view_place(unit);

    float4 reprojected = (float4)0.0;
    float reproject_amount = 0.0;
    if (emission.w != 0.0) {
        float4 before = mul(to_previous, float4(view, 1.0));
        before.y = -before.y;
        before.z /= -params.x;
        float t = before.z;
        before.x /= lerp(frustum.x, frustum.z, t);
        before.y /= lerp(frustum.y, frustum.w, t);
        before.xy = before.xy * 0.5 + 0.5;
        before.z = pow(before.z, 1.0 / spread);
        if (before.x > 0.0 && before.y > 0.0 && before.z > 0.0 && before.x < 1.0 &&
            before.y < 1.0 && before.z < 1.0) {
            reprojected = sample_source(before.xyz);
            reproject_amount = jitter.w;
            unit = place * cell + cell * jitter.xyz;
            unit.z = pow(unit.z, spread);
            view = view_place(unit);
        }
    }

    float3 world = mul(view_to_world, float4(view, 1.0)).xyz;
    float3 camera = mul(view_to_world, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float3 view_dir = normalize(world - camera);
    float g = params.w;

    // The fog volumes there, over the fog's own density, scattering and emission.
    VolumeSums sums = fog_volumes(id, world);
    float total_density = max(0.0, scattering.w + (float)sums.density / 1024.0);
    float3 scattered = (float3)sums.scattering / float3(2047.0, 2047.0, 1023.0) + scattering.rgb;
    float3 glowing = (float3)sums.emission / float3(511.0, 511.0, 255.0) + emission.rgb;
    float3 total_light = (float3)0.0;
    // Sampled before the branch below, at the level the froxel's density picks.
    float level = min(2.0 + total_density * (kRadianceMaxLevel - 2.0), kRadianceMaxLevel);
    float3 ray = view_dir * (g > 0.0 ? 1.0 : -1.0);
    float3 turned = float3(dot(sky_rows[0].xyz, ray), dot(sky_rows[1].xyz, ray),
                           dot(sky_rows[2].xyz, ray));
    float3 isotropic_texel = radiance_map.SampleLevel(radiance_sampler, float3(0.0, 1.0, 0.0),
                                                      level).rgb;
    float3 anisotropic_texel = radiance_map.SampleLevel(radiance_sampler, turned, level).rgb;
    if (total_density > 0.00005) {
        uint directional = (uint)sky.w;
        for (uint i = 0u; i < directional; ++i) {
            float energy = lights[i].shadow.z;
            if (!(energy > 0.001)) {
                continue;
            }
            float shadow = 1.0;
            if (lights[i].shadow.x >= 0.0) {
                uint slot = (uint)lights[i].shadow.x;
                if (directional_shadows[slot].params.z > 0.001) {
                    shadow = directional_fog_shadow(slot, world, view.z);
                }
            }
            float phase = henyey_greenstein(dot(view_dir, normalize(lights[i].direction.xyz)), g);
            total_light += lights[i].color.rgb * (shadow * phase * energy);
        }

        if (ambient.w > 0.0) {
            float3 isotropic = (float3)0.0;
            float3 anisotropic = (float3)0.0;
            if (sky.x != 0.0 && sky.y > 0.0) {
                isotropic = srgb_to_linear(isotropic_texel) * sky.z;
                anisotropic = isotropic;
                if (g != 0.0) {
                    anisotropic = srgb_to_linear(anisotropic_texel) * sky.z;
                }
            }
            total_light += lerp(ambient.rgb, lerp(isotropic, anisotropic, abs(g)), sky.y) *
                           ambient.w;
        }

        uint tile_x = (uint)(unit.x * screen.x) / kClusterSize;
        uint tile_y = (uint)(unit.y * screen.y) / kClusterSize;
        uint words = (uint)cluster.y;
        uint decal_words = (uint)cluster.w;
        uint stride = words + kClusterSlices + (decal_words > 0u ? decal_words + kClusterSlices : 0u);
        uint cell_at = (tile_y * (uint)cluster.x + tile_x) * stride;
        uint slice = (uint)clamp(-view.z * cluster.z, 0.0, (float)(kClusterSlices - 1u));
        uint range = clusters[cell_at + words + slice];
        uint first = range & 0xFFFFu;
        uint end = range >> 16u;
        for (uint word = first >> 5u; word < (end + 31u) >> 5u; ++word) {
            uint mask = clusters[cell_at + word] & range_mask(word, first, end);
            while (mask != 0u) {
                uint bit = firstbitlow(mask);
                mask &= mask - 1u;
                Light light = clustered_lights[word * 32u + bit];
                float3 to_light = light.position.xyz - world;
                float d = length(to_light);
                float energy = light.shadow.z;
                if (!(energy > 0.001) || !(d * light.position.w < 1.0)) {
                    continue;
                }
                float attenuation = omni_attenuation(d, light.position.w, light.params.x);
                if (light.direction.w == 2.0) {
                    float cone = light.params.y;
                    float scos = max(dot(-normalize(to_light), light.direction.xyz), cone);
                    float rim = max(0.0001, (1.0 - scos) / (1.0 - cone));
                    attenuation *= 1.0 - pow(rim, light.params.z);
                }
                float shadow = 1.0;
                if (light.shadow.x >= 0.0) {
                    uint slot = (uint)light.shadow.x;
                    if (positional_shadows[slot].params.y > 0.001) {
                        shadow = positional_fog_shadow(slot, world);
                    }
                }
                float phase = henyey_greenstein(dot(normalize(to_light), view_dir), g);
                total_light += light.color.rgb * (attenuation * shadow * phase * energy);
            }
        }

        uint voxel_gi_count = min((uint)voxel_gi.x, 8u);
        for (uint i = 0u; i < voxel_gi_count; ++i) {
            total_light += fog_voxel_gi_light(i, world) * voxel_gi.x;
        }
    }

    float3 lit = total_light * scattered + glowing;
    return lerp(float4(lit, total_density), reprojected, reproject_amount);
}

float4 filter_pass(uint3 id) {
    uint axis = size.w == kPassFilterX ? 0u : 1u;
    float4 sum = (float4)0.0;
    for (int i = -3; i <= 3; ++i) {
        // Each component written by name: d3dcompiler can't write a vector's component by index.
        uint3 tap = id;
        if (axis == 0u) {
            tap.x = (uint)clamp((int)id.x + i, 0, (int)size.x - 1);
        } else {
            tap.y = (uint)clamp((int)id.y + i, 0, (int)size.y - 1);
        }
        sum += source[froxel(tap)] * kGauss[i + 3];
    }
    return sum;
}

void integrate_pass(uint2 id) {
    float cell = 1.0 / (float)size.z;
    float4 accum = float4(0.0, 0.0, 0.0, 1.0);
    float previous_z = 0.0;
    for (uint i = 0u; i < size.z; ++i) {
        uint at = froxel(uint3(id, i));
        float4 fog = source[at];
        float z = params.x * pow((float)i * cell + cell * 0.5, params.y);
        float d = abs(previous_z - z);
        float transmittance = exp(-d * fog.a);
        float share = accum.a / max(fog.a, 0.00001);
        accum.rgb += (fog.rgb - fog.rgb * transmittance) * share;
        accum.a *= transmittance;
        previous_z = z;
        target[at] = accum;
    }
}

[numthreads(8, 8, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y || id.z >= size.z) {
        return;
    }
    if (size.w == kPassIntegrate) {
        integrate_pass(id.xy);
    } else if (size.w == kPassDensity) {
        target[froxel(id)] = density_pass(id);
    } else {
        target[froxel(id)] = filter_pass(id);
    }
}
