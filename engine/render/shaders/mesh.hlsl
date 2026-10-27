// The 3D shader: meshes lit by a frame's lights as Godot's Compatibility renderer lights them
// (drivers/gles3/shaders/scene.glsl), with a StandardMaterial3D's albedo, metallic, roughness,
// specular, emission and normal map, or a terrain's splat-mapped ground as the HTerrain plugin's
// Classic4 shader blends it, shadowed by the lights' shadow maps (shadow.hlsl draws them) and lit
// by the sky's radiance (radiance.hlsl filters it) or a baked lightmap, then fogged. Colors are worked in linear,
// tonemapped (tonemap.hlsl) and written as sRGB, with Godot's approximations of the two, or for
// Forward+ written linear for forward_post.hlsl.
// Keep it in step with shade_mesh in src/mesh.cpp, its CPU twin. Registers follow SDL's GPU
// conventions: vertex uniforms in space1, fragment textures and samplers in space2, fragment
// uniforms in space3. mesh_clustered.hlsl defines CLUSTERED and includes this file for Forward+,
// whose omni and spot lights and decals come from clusters of the screen instead of the scene's
// uniforms, and which reads screen-space ambient occlusion and reflections (ssao.hlsl and ssr.hlsl
// draw them). mesh_normal.hlsl defines NORMAL_ROUGHNESS for Forward+'s prepass, which draws the normals
// and roughness that ssao.hlsl and ssr.hlsl read, and mesh_motion.hlsl MOTION_VECTORS too for
// the prepass of a frame with TAA, which draws each pixel's motion vector into a second target.

cbuffer Camera : register(b0, space1) {
    float4x4 view_projection;          // world to clip space, with TAA's jitter
    float4x4 previous_view_projection; // the frame before's, without its jitter
    float4 jitter;                     // TAA's jitter in clip space, in xy
};

cbuffer Instance : register(b1, space1) {
    float4x4 model;          // the mesh's space to the world
    float4x4 normal_model;   // the inverse transpose of model's upper 3x3, in its upper 3x3
    float4 uv1;              // xy scale and zw offset, as Godot's uv1_scale and uv1_offset
    float4 lightmap_uv;      // the lightmap's rect: uv2's xy offset and zw scale
    float4x4 previous_model; // model in the frame before
};

struct VertexInput {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : TEXCOORD4;
    float2 uv2 : TEXCOORD5;
#ifdef MOTION_VECTORS
    // Where the vertex was in the frame before, from a second vertex buffer: a skinned mesh's
    // vertices before its last update, or its own.
    float3 previous : TEXCOORD6;
#endif
};

struct VertexOutput {
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float3 binormal : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float4 color : TEXCOORD5;
    float2 uv2 : TEXCOORD6;
#ifdef MOTION_VECTORS
    float4 motion_clip : TEXCOORD7;   // where the vertex is in clip space, without the jitter
    float4 previous_clip : TEXCOORD8; // and where it was in the frame before
#endif
    float4 position : SV_Position;
};

VertexOutput vs_main(VertexInput input) {
    VertexOutput output;
    float4 world = mul(model, float4(input.position, 1.0));
    output.world = world.xyz;
    output.position = mul(view_projection, world);
    // Normalized before they are interpolated, per MikkTSpace, as Godot does.
    float3 binormal = normalize(cross(input.normal, input.tangent.xyz) * input.tangent.w);
    output.normal = normalize(mul((float3x3)normal_model, input.normal));
    output.tangent = normalize(mul((float3x3)normal_model, input.tangent.xyz));
    output.binormal = normalize(mul((float3x3)normal_model, binormal));
    output.uv = input.uv * uv1.xy + uv1.zw;
    output.color = input.color;
    output.uv2 = input.uv2 * lightmap_uv.zw + lightmap_uv.xy;
#ifdef MOTION_VECTORS
    output.motion_clip = output.position;
    output.motion_clip.xy -= jitter.xy * output.position.w;
    output.previous_clip =
        mul(previous_view_projection, mul(previous_model, float4(input.previous, 1.0)));
#endif
    return output;
}

// A terrain's textures take the first eleven slots: the color map, splat map and normal map in
// albedo's, metallic's and roughness's, then its layers' albedo_bump in emission's, normal's and
// ground 2 and 3's, then their normal_roughness. A lightmap, half floats, takes the last slot.
Texture2D<float4> albedo_texture : register(t0, space2);
Texture2D<float4> metallic_texture : register(t1, space2);
Texture2D<float4> roughness_texture : register(t2, space2);
Texture2D<float4> emission_texture : register(t3, space2);
Texture2D<float4> normal_texture : register(t4, space2);
Texture2D<float4> ground_texture_2 : register(t5, space2);
Texture2D<float4> ground_texture_3 : register(t6, space2);
Texture2D<float4> ground_normal_0 : register(t7, space2);
Texture2D<float4> ground_normal_1 : register(t8, space2);
Texture2D<float4> ground_normal_2 : register(t9, space2);
Texture2D<float4> ground_normal_3 : register(t10, space2);
SamplerState albedo_sampler : register(s0, space2);
SamplerState metallic_sampler : register(s1, space2);
SamplerState roughness_sampler : register(s2, space2);
SamplerState emission_sampler : register(s3, space2);
SamplerState normal_sampler : register(s4, space2);
SamplerState ground_sampler_2 : register(s5, space2);
SamplerState ground_sampler_3 : register(s6, space2);
SamplerState ground_normal_sampler_0 : register(s7, space2);
SamplerState ground_normal_sampler_1 : register(s8, space2);
SamplerState ground_normal_sampler_2 : register(s9, space2);
SamplerState ground_normal_sampler_3 : register(s10, space2);
// The shadow atlases, depths compared with GREATER_OR_EQUAL and filtered linearly.
Texture2D directional_atlas : register(t11, space2);
Texture2D positional_atlas : register(t12, space2);
SamplerComparisonState directional_sampler : register(s11, space2);
SamplerComparisonState positional_sampler : register(s12, space2);
// The sky's radiance: sRGB, each level blurrier, filtered linearly across levels and faces.
TextureCube<float4> radiance_map : register(t13, space2);
SamplerState radiance_sampler : register(s13, space2);

static const uint kUnshaded = 1u;
static const uint kVertexColorAlbedo = 2u;
static const uint kAlphaScissor = 4u;
static const uint kEmissionTexture = 8u;
static const uint kNormalMap = 16u;
static const uint kSideCheck = 32u;
static const uint kHasAlpha = 64u;
static const uint kDisableFog = 128u;
static const uint kLightmap = 256u;
static const uint kLightmapCapture = 512u;
// Terrain flags, in options.w.
static const uint kTerrain = 1u;
static const uint kTerrainDepthBlending = 2u;
static const uint kTerrainNormals = 16u; // shifted left by the layer

// A VoxelGI a draw traces (Forward+): its light is a 3D texture kept in ground_texture_2 as each
// level's z slices side by side.
struct VoxelGI {
    float4 rows[3]; // the world to its cells' space, a row each: a unit a texel of level 0
    float4 size;    // texels along x, y and z at level 0, then how many levels
    float4 params;  // energy, bias, normal bias, 1 to blend with the ambient light
    // Each level's first slice's left and top texels in the atlas and its slices a row.
    float4 levels[10];
};

cbuffer Surface : register(b0, space3) {
    float4 albedo;            // sRGB, and alpha
    float4 emission;          // sRGB, and its energy
    float4 surface;           // metallic, roughness, specular and the normal map's scale
    float4 metallic_channel;  // which of metallic_texture's channels is metallic
    float4 roughness_channel; // and roughness_texture's roughness
    // x: the flags above, y: the alpha scissor threshold's bits, z: the draw's layers, w: the
    // terrain flags.
    uint4 options;
    float4 terrain_uv;      // each ground layer's tiling: the maps' resolution over its uv_scale
    float4 terrain_opacity; // how much the color map tints each ground layer
    float4 lightmap;        // the lightmap's exposure, then 1 for the bicubic filter
    float4 capture[9];      // the light probes' spherical harmonics, RGB
    float4 voxel_gi;        // how many of voxel_gis it traces
    VoxelGI voxel_gis[2];
};

struct Light {
    float4 position;  // xyz, then 1 / range
    float4 direction; // toward a directional light, the way a spot light shines; then the type
    float4 color;     // linear, times energy and pi; then the specular amount
    float4 params;    // attenuation, the cone's cosine, 1 / spot attenuation, the cull mask
    // Its slot in directional_shadows or positional_shadows, or -1; then 1 when it is baked into
    // lightmaps.
    float4 shadow;
};

static const float kDirectional = 0.0;
static const float kOmni = 1.0;
static const float kSpot = 2.0;
static const float kPi = 3.14159265358979323;
static const float kRadianceMaxLevel = 5.0; // Godot's RADIANCE_MAX_LOD

cbuffer Scene : register(b1, space3) {
    float4 camera;  // the camera's place, then how many lights
    float4 ambient; // linear
    // Exposure, white, the tonemapper, and 1 to tonemap here (0 when post.hlsl does it after, 2
    // to write linear colors for forward_post.hlsl).
    float4 tonemap;
    float4 fog_color; // linear, times its energy; then 0 for no fog, 1 exponential, 2 depth
    float4 fog;       // density, sun scatter, height, height density
    float4 fog_depth; // depth fog's begin, end and curve
    // 1 for ambient light from the radiance, 1 for reflections of it, the background's energy,
    // ambient light's sky contribution.
    float4 sky;
    float4 sky_rows[3]; // the world to the sky's space, a row each
    // Forward+'s clusters: tiles across, a tile's words of light bits, depth slices per unit of
    // view depth (how many over the far plane's distance).
    float4 cluster;
    // 1 when screen_texture's layer 0 holds the frame's SSAO, its light affect and its AO channel
    // affect, then 1 when its layer 1 holds its SSR.
    float4 ssao;
    float4 view_rows[3]; // the world to the camera's space, a row each, for normals
    // Forward+'s reflection probes: how many in probes, the level the roughest surfaces read, and
    // the exposure their light is multiplied by.
    float4 reflections;
    // 1 when meshes trace their VoxelGIs (Forward+, and not a reflection probe's capture), then 1
    // for high quality.
    float4 scene_voxel_gi;
    // Forward+'s decals: how many, the filter, the atlas's width and height in texels.
    float4 decal_info;
    float4 screen; // the target's width and height in pixels
    // The camera's view-projection without TAA's jitter: its x, y and w rows.
    float4 clip_rows[3];
    // Forward+'s volumetric fog: 1 when the frame has it, 1 over its length, 1 over its detail
    // spread, its sky affect; then the froxels across, down and deep in fog_froxels.
    float4 volumetric_fog;
    float4 fog_volume;
    // Every light in Compatibility; in Forward+, only the directional ones.
    Light lights[64];
};

// Matrices take a point in the world to its shadow atlas: x and y its texture coordinate there, z
// the depth to compare.
struct DirectionalShadow {
    float4x4 matrices[4]; // one a cascade
    float4 splits;        // the view depth each cascade reaches
    float4 normal_bias;   // each cascade's, in world units
    float4 params;        // fade from and to (view z), opacity, how many cascades
    float4 options;       // 1 blends cascades, an atlas texel
};

struct PositionalShadow {
    float4x4 light;    // the world to the light's own space, for an omni light's faces
    float4x4 faces[6]; // one a cube face for an omni light; a spot light's first
    float4 rects[6];   // each face's tile, its texture coordinates' low and high
    float4 params;     // normal bias over distance, opacity, an atlas texel, how many faces
    float4 position;   // the light's place
};

cbuffer Shadows : register(b2, space3) {
    float4 camera_z; // the camera matrix's z row
    float4 filters;  // samples a directional and a spot shadow takes: 1, 5 or 13
    DirectionalShadow directional_shadows[4];
    PositionalShadow positional_shadows[16];
};

#ifdef CLUSTERED
// Forward+'s omni and spot lights, nearest first, and the clusters that pick them, laid out as
// Godot's cluster buffer: for each tile of kClusterSize pixels, cluster.y words with a bit for
// each light that reaches it, then for each of kClusterSlices depth slices the first of those
// lights in the slice and one past the last, then the same for the decals in cluster.w words when
// there are some (src/clusters.cpp). The volumetric fog's froxels are in fog_froxels, rows across,
// then down, then slices deeper (src/volumetric_fog.hpp). The decals' textures are in decal_atlas, a texel a word (RGBA8,
// red lowest), its levels one after another (src/decal.cpp). SDL binds a fragment stage's
// storage buffers after its textures, and in Metal after its uniforms; WebGPU binds them as a
// group of their own, the mesh pipeline's otherwise empty group 0. The screen's layers are the
// frame's size, white when the frame draws neither: layer 0 the screen-space ambient occlusion,
// layer 1 the screen-space reflections (each pixel's reflection in RGB, premultiplied by how much
// of the sky's it replaces, in A), and with ray-traced shadows (screen.z) layers 2 and 3 their
// lights' visibility, a light a channel in order of its ray slot (light.shadow.w - 1), four a layer
// (src/ray_shadows.hpp). The reflection probes' cubes are in probe_atlas, mirrored along
// z (src/reflection.cpp), each linear, its levels filtered as radiance.hlsl filters the sky's.
Texture2DArray<float4> screen_texture : register(t14, space2);
TextureCubeArray<float4> probe_atlas : register(t15, space2);
SamplerState screen_sampler : register(s14, space2);
SamplerState probe_sampler : register(s15, space2);
struct Probe {
    float4 rows[3]; // the world to the probe's space, a row each
    float4 extents; // half the box's size, then the intensity
    float4 offset;  // origin_offset, then the blend distance
    float4 ambient; // the ambient color, linear, times its energy; then the mode
    float4 options; // 1 to box project, the cube, the reflection mask, 0
};
// A decal: its space runs x and z from 0 to 1 across its box and y from -1 to 1 up it.
struct Decal {
    float4 rows[3];        // the world to its space, a row each
    float4 rects[4];       // its albedo, normal, ORM and emission rects in the atlas, or 0
    float4 modulate;       // linear, its alpha times the distance fade
    float4 normal;         // its up in the world, then the normal fade
    float4 normal_rows[3]; // its turn, orthonormalized, a row each
    float4 params;         // emission energy, albedo mix, upper fade, lower fade
    uint4 options;         // its cull mask
};
#if defined(SLASH_MSL)
StructuredBuffer<Light> clustered_lights : register(t3, space2);
StructuredBuffer<uint> clusters : register(t4, space2);
StructuredBuffer<Probe> probes : register(t5, space2);
StructuredBuffer<Decal> decals : register(t6, space2);
StructuredBuffer<uint> decal_atlas : register(t7, space2);
StructuredBuffer<float4> fog_froxels : register(t8, space2);
#elif defined(SLASH_WGSL)
StructuredBuffer<Light> clustered_lights : register(t0, space0);
StructuredBuffer<uint> clusters : register(t1, space0);
StructuredBuffer<Probe> probes : register(t2, space0);
StructuredBuffer<Decal> decals : register(t3, space0);
StructuredBuffer<uint> decal_atlas : register(t4, space0);
StructuredBuffer<float4> fog_froxels : register(t5, space0);
#else
StructuredBuffer<Light> clustered_lights : register(t16, space2);
StructuredBuffer<uint> clusters : register(t17, space2);
StructuredBuffer<Probe> probes : register(t18, space2);
StructuredBuffer<Decal> decals : register(t19, space2);
StructuredBuffer<uint> decal_atlas : register(t20, space2);
StructuredBuffer<float4> fog_froxels : register(t21, space2);
#endif
static const uint kClusterSize = 32u;
static const uint kClusterSlices = 32u;
#endif

#include "tonemap.hlsl"

// Godot's fog_process: the fog's color at a point in the world, brightened by the directional
// lights the camera looks toward through it, and how much of it covers the point.
float4 fog_process(float3 world) {
    float3 to_point = world - camera.xyz;
    float distance = length(to_point);
    float3 color = fog_color.rgb;
    if (fog.y > 0.001) {
        float3 view = distance > 0.0 ? to_point / distance : (float3)0.0;
        uint count = (uint)camera.w;
        for (uint i = 0u; i < count; ++i) {
            if (lights[i].direction.w == kDirectional) {
                float amount = pow(max(dot(view, normalize(lights[i].direction.xyz)), 0.0), 8.0);
                color += lights[i].color.rgb * amount * fog.y;
            }
        }
    }
    float amount;
    if (fog_color.w == 2.0) {
        float z = smoothstep(fog_depth.x, fog_depth.y, distance);
        amount = pow(z, fog_depth.z) * fog.x;
    } else {
        amount = 1.0 - exp(min(0.0, -distance * fog.x));
    }
    if (abs(fog.w) >= 0.0001) {
        amount = max(amount, 1.0 - exp(min(0.0, (world.y - fog.z) * fog.w)));
    }
    return float4(color, amount);
}

#ifdef CLUSTERED
// The froxel volume read at a texture coordinate as a 3D texture is read linearly, clamped to its
// edges: src/volumetric_fog.cpp's sample_volume.
float4 sample_fog_froxels(float3 uvw) {
    uint3 size = (uint3)fog_volume.xyz;
    float3 n = (float3)size;
    float3 p = clamp(uvw * n - 0.5, 0.0, n - 1.0);
    float3 base = floor(p);
    uint3 low = (uint3)base;
    uint3 high = min(low + 1u, size - 1u);
    float3 t = p - base;
    float4 a = lerp(fog_froxels[(low.z * size.y + low.y) * size.x + low.x],
                    fog_froxels[(low.z * size.y + low.y) * size.x + high.x], t.x);
    float4 b = lerp(fog_froxels[(low.z * size.y + high.y) * size.x + low.x],
                    fog_froxels[(low.z * size.y + high.y) * size.x + high.x], t.x);
    float4 c = lerp(fog_froxels[(high.z * size.y + low.y) * size.x + low.x],
                    fog_froxels[(high.z * size.y + low.y) * size.x + high.x], t.x);
    float4 d = lerp(fog_froxels[(high.z * size.y + high.y) * size.x + low.x],
                    fog_froxels[(high.z * size.y + high.y) * size.x + high.x], t.x);
    return lerp(lerp(a, b, t.y), lerp(c, d, t.y), t.z);
}

// Godot's volumetric_fog_process: the fog in front of a point view_depth away at a place on the
// screen, the light it adds, then how much of the point's it lets through.
float4 volumetric_fog_process(float2 uv, float view_depth) {
    float z = view_depth * volumetric_fog.y;
    if (z < 0.0) {
        return (float4)0.0;
    }
    if (z < 1.0) {
        z = pow(z, volumetric_fog.z);
    }
    return sample_fog_froxels(float3(uv, z));
}
#endif

// A lit color at a point in the world, linear, as the frame's target takes it: fogged unless the
// material disables it, then left linear for Forward+, or times the exposure, tonemapped unless
// the post pass does it, and as sRGB, as Godot's scene.glsl ends. In Forward+ with volumetric
// fog, the plain fog is seen through the volumetric fog in front of it, as Godot's
// scene_forward_clustered.glsl blends them; pixel is the fragment's place on the target.
float4 finish(float3 color, float alpha, float3 world, float2 pixel) {
    bool fog_on = (options.x & kDisableFog) == 0u;
#ifdef CLUSTERED
    [branch] if (volumetric_fog.x != 0.0 && fog_on) {
        float4 fog = float4(0.0, 0.0, 0.0, 1.0);
        if (fog_color.w != 0.0) {
            float4 plain = fog_process(world);
            fog = float4(plain.rgb * plain.a, 1.0 - plain.a);
        }
        float depth = -dot(camera_z, float4(world, 1.0));
        float4 volume = volumetric_fog_process(pixel / screen.xy, depth);
        fog = float4(fog.rgb * volume.a + volume.rgb, fog.a * volume.a);
        color = color * fog.a + fog.rgb;
    } else
#endif
    if (fog_color.w != 0.0 && fog_on) {
        float4 fogged = fog_process(world);
        color = lerp(color, fogged.rgb, fogged.a);
    }
    [branch] if (tonemap.w == 2.0) {
        return float4(color, alpha);
    }
    color *= tonemap.x;
    [branch] if (tonemap.w != 0.0) {
        color = apply_tonemapping(color, tonemap.y, (uint)tonemap.z);
    }
    return float4(linear_to_srgb(color), alpha);
}

float schlick_fresnel(float u) {
    float m = 1.0 - u;
    float m2 = m * m;
    return m2 * m2 * m;
}

float d_ggx(float cos_theta_m, float alpha) {
    float a = cos_theta_m * alpha;
    float k = alpha / (1.0 - cos_theta_m * cos_theta_m + a * a);
    return k * k * (1.0 / kPi);
}

float v_ggx(float n_dot_l, float n_dot_v, float alpha) {
    return 0.5 / lerp(2.0 * n_dot_l * n_dot_v, n_dot_l + n_dot_v, alpha);
}

// light_compute in Godot's scene.glsl, with Burley diffuse and Schlick-GGX specular, its defaults.
void light_compute(float3 n, float3 l, float3 v, float3 light_color, float attenuation, float3 f0,
                   float roughness, float metallic, float specular_amount,
                   inout float3 diffuse_light, inout float3 specular_light) {
    float n_dot_l = min(dot(n, l), 1.0);
    float c_n_dot_l = max(n_dot_l, 0.0);
    float c_n_dot_v = max(dot(n, v), 1e-4);
    float3 h = normalize(v + l);
    float c_n_dot_h = clamp(dot(n, h), 0.0, 1.0);
    float c_l_dot_h = clamp(dot(l, h), 0.0, 1.0);
    if (metallic < 1.0) {
        float fd90_minus_1 = 2.0 * c_l_dot_h * c_l_dot_h * roughness - 0.5;
        float fd_v = 1.0 + fd90_minus_1 * schlick_fresnel(c_n_dot_v);
        float fd_l = 1.0 + fd90_minus_1 * schlick_fresnel(c_n_dot_l);
        float diffuse_brdf_nl = (1.0 / kPi) * fd_v * fd_l * c_n_dot_l;
        diffuse_light += light_color * diffuse_brdf_nl * attenuation;
    }
    if (roughness > 0.0) {
        float alpha_ggx = roughness * roughness;
        float d = d_ggx(c_n_dot_h, alpha_ggx);
        float g = v_ggx(c_n_dot_l, c_n_dot_v, alpha_ggx);
        float c_l_dot_h5 = schlick_fresnel(c_l_dot_h);
        float f90 = clamp(50.0 * f0.g, 0.0, 1.0);
        float3 f = f0 + (f90 - f0) * c_l_dot_h5;
        float3 specular_brdf_nl = c_n_dot_l * d * f * g;
        specular_light += specular_brdf_nl * light_color * attenuation * specular_amount;
    }
}

float omni_attenuation(float distance, float inv_range, float decay) {
    float nd = distance * inv_range;
    nd *= nd;
    nd *= nd;
    nd = max(1.0 - nd, 0.0);
    nd *= nd;
    return nd * pow(max(distance, 0.0001), -decay);
}

// smoothstep, written out, since its result is undefined for edges in falling order (the fade's)
// in some shading languages.
float smooth(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x < edge0 ? 0.0 : 1.0;
    }
    float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

// One sample of an atlas, as Godot's textureProjLod of a sampler2DShadow.
float compare_atlas(bool positional, float2 uv, float reference) {
    if (positional) {
        return positional_atlas.SampleCmpLevelZero(positional_sampler, uv, reference);
    }
    return directional_atlas.SampleCmpLevelZero(directional_sampler, uv, reference);
}

// Each tap's offset in texels, in the order Godot's sample_shadow adds them: the center, the far
// cross, the near cross and the corners. Five taps take the center and the near cross.
static const float2 kTaps[13] = {float2(0.0, 0.0),  float2(2.0, 0.0),   float2(-2.0, 0.0),
                                 float2(0.0, 2.0),  float2(0.0, -2.0),  float2(1.0, 0.0),
                                 float2(-1.0, 0.0), float2(0.0, 1.0),   float2(0.0, -1.0),
                                 float2(1.0, 1.0),  float2(-1.0, 1.0),  float2(1.0, -1.0),
                                 float2(-1.0, -1.0)};

// Godot's sample_shadow: the shadow at a point through a matrix into an atlas, from 1, 5 or 13
// samples a texel apart, each kept within clamp_rect's low and high corners. The taps run in a
// loop, so that drivers compile one sample rather than thirteen.
float sample_shadow(bool positional, float4x4 to_atlas, float3 p, float texel, float taps,
                    float4 clamp_rect) {
    float4 at = mul(to_atlas, float4(p, 1.0));
    float2 uv = at.xy / at.w;
    float reference = saturate(at.z / at.w);
    uint count = taps > 12.0 ? 13u : (taps > 4.0 ? 5u : 1u);
    float sum = 0.0;
    [loop] for (uint i = 0u; i < count; ++i) {
        float2 offset = kTaps[count == 5u && i > 0u ? i + 4u : i];
        sum += compare_atlas(positional, clamp(uv + offset * texel, clamp_rect.xy, clamp_rect.zw),
                             reference);
        // Godot stops early where the far samples agree.
        if (count == 13u && i == 4u) {
            if (sum <= 0.000001) {
                return 0.0;
            }
            if (sum >= 4.999999) {
                return 1.0;
            }
        }
    }
    return count == 13u ? sum * (1.0 / 13.0) : (count == 5u ? sum * (1.0 / 5.0) : sum);
}

// Directional cascades aren't clamped, as Godot's aren't.
static const float4 kNoClamp = float4(-2.0, -2.0, 3.0, 3.0);

// A light's shadow at a pixel, sampled in one place for every kind of light. A directional
// light's cascade is picked by view depth as Godot's scene.glsl picks it, blended with the next
// when the light says so, and offset along the vertex normal by the cascade's normal bias; an
// omni light's cube face is the one its ray from the light leaves by, and an omni or spot light
// offsets the point along the vertex normal as Godot offsets it.
float light_shadow(bool directional, uint slot, float3 world, float3 normal, float3 toward_light) {
    uint lookups = 1u; // a directional light's second is the next cascade, blended in
    uint cascade = 0u;
    float blend = 0.0;
    float view_z = 0.0;
    float3 base_bias = (float3)0.0;
    float3 p = world;
    uint face = 0u;
    float taps = filters.y;
    if (directional) {
        view_z = dot(world, camera_z.xyz) + camera_z.w;
        base_bias = normal * (1.0 - max(0.0, -dot(toward_light, normal)));
        float4 splits = directional_shadows[slot].splits;
        uint count = (uint)directional_shadows[slot].params.w;
        float depth_z = -view_z;
        taps = filters.x;
        if (count > 1u) {
            if (!(depth_z < splits[count - 1u])) {
                lookups = 0u; // past the last cascade
            } else {
                while (cascade + 1u < count && !(depth_z < splits[cascade])) {
                    ++cascade;
                }
                if (directional_shadows[slot].options.x != 0.0 && cascade + 1u < count) {
                    float from = cascade == 0u ? 0.0 : splits[cascade - 1u];
                    blend = smooth(from, splits[cascade], depth_z);
                    lookups = 2u;
                }
            }
        }
    } else {
        float4 params = positional_shadows[slot].params;
        float3 to_light = positional_shadows[slot].position.xyz - world;
        float light_length = length(to_light);
        float a_n_dot_l = abs(dot(normal, to_light / light_length));
        p = world + normal * ((1.0 - a_n_dot_l) * params.x * light_length);
        if (params.w > 1.5) {
            float3 local = mul(positional_shadows[slot].light, float4(p, 1.0)).xyz;
            float3 a = abs(local);
            if (a.x >= a.y && a.x >= a.z) {
                face = local.x > 0.0 ? 0u : 1u;
            } else if (a.y >= a.z) {
                face = local.y < 0.0 ? 2u : 3u;
            } else {
                face = local.z > 0.0 ? 4u : 5u;
            }
            taps = 1.0; // Godot samples an omni light's cube once
        }
    }
    float shadow = 1.0;
    float next = 1.0;
    [loop] for (uint k = 0u; k < lookups; ++k) {
        float4x4 to_atlas;
        float3 at;
        float texel;
        float4 clamp_rect;
        if (directional) {
            uint i = cascade + k;
            to_atlas = directional_shadows[slot].matrices[i];
            at = world + base_bias * directional_shadows[slot].normal_bias[i];
            texel = directional_shadows[slot].options.y;
            clamp_rect = kNoClamp;
        } else {
            to_atlas = positional_shadows[slot].faces[face];
            at = p;
            texel = positional_shadows[slot].params.z;
            clamp_rect = positional_shadows[slot].rects[face];
        }
        float sampled = sample_shadow(!directional, to_atlas, at, texel, taps, clamp_rect);
        if (k == 0u) {
            shadow = sampled;
        } else {
            next = sampled;
        }
    }
    if (!directional) {
        return lerp(1.0, shadow, positional_shadows[slot].params.y);
    }
    if (lookups == 2u) {
        shadow = lerp(shadow, next, blend);
    }
    float4 params = directional_shadows[slot].params;
    shadow = lerp(shadow, 1.0, smooth(params.x, params.y, view_z));
    return lerp(1.0, shadow, params.z);
}

// get_depth_blended_weights in HTerrain's shader: the splat map's weights, sharpened where one
// layer's bump stands above the others.
float4 depth_blended_weights(float4 splat, float4 bumps) {
    float dh = 0.2;
    float4 h = bumps + splat;
    h *= smoothstep((float4)0.0, (float4)0.05, splat);
    float4 d = h + dh;
    d.r -= max(h.g, max(h.b, h.a));
    d.g -= max(h.r, max(h.b, h.a));
    d.b -= max(h.g, max(h.r, h.a));
    d.a -= max(h.g, max(h.b, h.r));
    return saturate(d);
}

// unpack_normal in HTerrain's shader: an OpenGL-style normal map's texel, its green along the
// terrain's -Z and its blue along +Y.
float3 unpack_ground_normal(float4 texel) {
    float3 n = texel.xzy * 2.0 - 1.0;
    n.z = -n.z;
    return n;
}

// A direction in the mesh's space, in the world through the pixel's tangent frame: a terrain's
// vertices have the normal +Y and the tangent +X, so its binormal is -Z.
float3 frame_direction(VertexOutput input, float3 v) {
    return normalize(input.tangent * v.x + input.normal * v.y - input.binormal * v.z);
}

// One light's share of a pixel's diffuse and specular light, unless its cull mask leaves out the
// draw's layers, with the shading normal and the one shadows offset along (geometry). An opaque
// pixel of a frame with ray-traced shadows (ray_pixel, else -1) takes a light's with a ray slot
// from the screen texture in place of its shadow map's.
void add_light(Light light, float3 world, float3 normal, float3 geometry, float3 view, float3 f0,
               float roughness, float metallic, int2 ray_pixel, inout float3 diffuse_light,
               inout float3 specular_light) {
    if (((uint)light.params.w & options.z) == 0u) {
        return;
    }
    // A baked light's light is in the lightmap already, as Godot skips BAKE_STATIC lights.
    if (light.shadow.y != 0.0 && (options.x & kLightmap) != 0u) {
        return;
    }
    float type = light.direction.w;
    float slot = light.shadow.x;
    bool directional = type == kDirectional;
    float3 to_light = light.position.xyz - world;
    float3 toward = directional ? normalize(light.direction.xyz) : normalize(to_light);
    float attenuation = 1.0;
    if (!directional) {
        float distance = length(to_light);
        attenuation = omni_attenuation(distance, light.position.w, light.params.x);
        if (type == kSpot) {
            float cone = light.params.y;
            float scos = max(dot(-normalize(to_light), light.direction.xyz), cone);
            float rim = max(0.0001, (1.0 - scos) / (1.0 - cone));
            attenuation *= 1.0 - pow(rim, light.params.z);
        }
    }
    float ray = light.shadow.w;
#ifdef CLUSTERED
    if (ray_pixel.x >= 0 && ray > 0.0) {
        uint k = (uint)ray - 1u;
        attenuation *= screen_texture.Load(int4(ray_pixel, 2 + (int)(k >> 2u), 0))[k & 3u];
        ray = -1.0;
    }
#endif
    if (ray >= 0.0 && slot >= 0.0) {
        attenuation *= light_shadow(directional, (uint)slot, world, geometry, toward);
    }
    light_compute(normal, toward, view, light.color.rgb, attenuation, f0, roughness, metallic,
                  light.color.w, diffuse_light, specular_light);
}

#ifdef CLUSTERED
// The bits of a word of a tile's light bits for lights from first up to end: light word * 32 +
// bit for each bit.
uint range_mask(uint word, uint first, uint end) {
    int low = clamp((int)first - (int)(word * 32u), 0, 32);
    int high = clamp((int)end - (int)(word * 32u), 0, 32);
    uint below_high = high >= 32 ? 0xFFFFFFFFu : (1u << (uint)high) - 1u;
    uint below_low = low >= 32 ? 0xFFFFFFFFu : (1u << (uint)low) - 1u;
    return below_high & ~below_low;
}

// The words each tile takes in clusters: its lights' bits and ranges, then its decals'.
uint cluster_stride() {
    uint decal_words = (uint)cluster.w;
    return (uint)cluster.y + kClusterSlices + (decal_words > 0u ? decal_words + kClusterSlices : 0u);
}

// A texel of the decal atlas at level's offset and size, clamped to its edges, decoded from sRGB for
// the albedo and emission textures, as Godot's sRGB view of the atlas reads them.
float4 decal_texel(uint offset, uint width, uint height, int2 at, bool srgb) {
    at = clamp(at, int2(0, 0), int2((int)width - 1, (int)height - 1));
    uint word = decal_atlas[offset + (uint)at.y * width + (uint)at.x];
    float4 texel = float4((float)(word & 0xFFu), (float)((word >> 8u) & 0xFFu),
                          (float)((word >> 16u) & 0xFFu), (float)(word >> 24u)) /
                   255.0;
    if (srgb) {
        texel.rgb = srgb_to_linear(texel.rgb);
    }
    return texel;
}

// A level of the decal atlas at uv, by its nearest texel or between the four around uv.
float4 decal_level(uint level, float2 uv, bool linear_filter, bool srgb) {
    uint atlas_width = (uint)decal_info.z;
    uint atlas_height = (uint)decal_info.w;
    uint offset = 0u;
    for (uint i = 0u; i < level; ++i) {
        offset += max(atlas_width >> i, 1u) * max(atlas_height >> i, 1u);
    }
    uint width = max(atlas_width >> level, 1u);
    uint height = max(atlas_height >> level, 1u);
    float2 size = float2((float)width, (float)height);
    // The texel under uv, or the four around it; in loops, so that the shader reads texels in one
    // place.
    float2 at = linear_filter ? uv * size - 0.5 : uv * size;
    float2 low = floor(at);
    float2 f = at - low;
    int2 corner = (int2)clamp(low, -2.0, size + 1.0);
    uint taps = linear_filter ? 4u : 1u;
    float4 texels[4];
    [loop] for (uint i = 0u; i < taps; ++i) {
        texels[i] = decal_texel(offset, width, height, corner + int2(i & 1u, i >> 1u), srgb);
    }
    if (!linear_filter) {
        return texels[0];
    }
    return lerp(lerp(texels[0], texels[1], f.x), lerp(texels[2], texels[3], f.x), f.y);
}

// The decal atlas at uv, which moves by dx and dy across a pixel, as the decals' filter samples it
// (nearest, linear, then those with mipmaps, then the anisotropic ones, which filter as those with
// mipmaps): with mipmaps, the level log2 of the farther the texture moves across a pixel in texels,
// blended between the levels around it.
float4 decal_sample(float2 uv, float2 dx, float2 dy, bool srgb) {
    uint filter = (uint)decal_info.y;
    bool linear_filter = filter == 1u || filter == 3u || filter == 5u;
    uint level = 0u;
    float blend = 0.0;
    if (filter >= 2u) {
        float2 size = decal_info.zw;
        float rho = max(length(dx * size), length(dy * size));
        float lambda = rho >= 16.0 ? 4.0 : log2(rho);
        if (lambda > 0.0) {
            lambda = min(lambda, 4.0);
            float low = floor(lambda);
            level = (uint)low;
            blend = lambda - low;
        }
    }
    float4 color = (float4)0.0;
    uint levels = blend > 0.0 ? 2u : 1u;
    [loop] for (uint i = 0u; i < levels; ++i) {
        float4 sampled = decal_level(level + i, uv, linear_filter, srgb);
        color = i == 0u ? sampled : lerp(color, sampled, blend);
    }
    return color;
}

// How far a point moves across a pixel to the right (dx) and down (dy), along the plane through it
// facing geometry, from the camera's rows: what Godot takes from dFdx and dFdy of the vertex, worked
// out so that every device picks the same atlas levels.
void surface_steps(float3 world, float3 geometry, out float3 dx, out float3 dy) {
    float3 helper = abs(geometry.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 t1 = normalize(cross(helper, geometry));
    float3 t2 = cross(geometry, t1);
    float4 p = float4(world, 1.0);
    float x = dot(clip_rows[0], p);
    float y = dot(clip_rows[1], p);
    float w = dot(clip_rows[2], p);
    float dw1 = dot(clip_rows[2].xyz, t1);
    float dw2 = dot(clip_rows[2].xyz, t2);
    float2 s1 = float2((dot(clip_rows[0].xyz, t1) * w - x * dw1) / (w * w) * (0.5 * screen.x),
                       -((dot(clip_rows[1].xyz, t1) * w - y * dw1) / (w * w)) * (0.5 * screen.y));
    float2 s2 = float2((dot(clip_rows[0].xyz, t2) * w - x * dw2) / (w * w) * (0.5 * screen.x),
                       -((dot(clip_rows[1].xyz, t2) * w - y * dw2) / (w * w)) * (0.5 * screen.y));
    float det = s1.x * s2.y - s2.x * s1.y;
    if (!(abs(det) > 1.0e-20)) {
        dx = t1 * 1.0e10;
        dy = t2 * 1.0e10;
        return;
    }
    dx = (t1 * s2.y - t2 * s1.y) / det;
    dy = (t2 * s1.x - t1 * s2.x) / det;
}

// x to the power y, as Godot's fade takes it, with anything to the power 0 taken as 1 and 0 to any
// other power as 0.
float decal_fade(float x, float y) {
    if (y == 0.0) {
        return 1.0;
    }
    return x > 0.0 ? pow(x, y) : 0.0;
}

bool has_rect(float4 rect) {
    return any(rect != (float4)0.0);
}

// One decal's textures over the surface, as the body of Godot's decal loop.
void apply_decal(Decal decal, float3 world, float3 geometry, float3 dx, float3 dy,
                 inout float3 base, inout float3 normal, inout float ao, inout float roughness,
                 inout float metallic, inout float3 emitted) {
    float4 p = float4(world, 1.0);
    float3 local = float3(dot(decal.rows[0], p), dot(decal.rows[1], p), dot(decal.rows[2], p));
    if (local.x < 0.0 || local.y < -1.0 || local.z < 0.0 || local.x > 1.0 || local.y > 1.0 ||
        local.z > 1.0) {
        return; // outside its box
    }
    float fade = decal_fade(1.0 - abs(local.y), local.y > 0.0 ? decal.params.z : decal.params.w);
    if (decal.normal.w > 0.0) {
        fade *= smoothstep(decal.normal.w, 1.0, dot(geometry, decal.normal.xyz) * 0.5 + 0.5);
    }
    float2 uv_dx = float2(dot(decal.rows[0].xyz, dx), dot(decal.rows[2].xyz, dx));
    float2 uv_dy = float2(dot(decal.rows[0].xyz, dy), dot(decal.rows[2].xyz, dy));
    // Its albedo, normal, ORM and emission textures, in a loop so that the shader samples in one
    // place; the normal and ORM ones only with an albedo one, as Godot's.
    bool albedo_used = has_rect(decal.rects[0]);
    float4 texels[4];
    [loop] for (uint k = 0u; k < 4u; ++k) {
        float4 rect = decal.rects[k];
        texels[k] = (float4)0.0;
        if (has_rect(rect) && (albedo_used || k == 3u)) {
            texels[k] = decal_sample(local.xz * rect.zw + rect.xy, uv_dx * rect.zw,
                                     uv_dy * rect.zw, k == 0u || k == 3u);
        }
    }
    if (albedo_used) {
        float4 albedo = texels[0] * decal.modulate;
        albedo.a *= fade;
        base = lerp(base, albedo.rgb, albedo.a * decal.params.y);
        if (has_rect(decal.rects[1])) {
            // Y down, as most tools draw normal maps, and turned from the decal's space, whose up
            // is y, by its xzy.
            float3 n = texels[1].xyz;
            n.xy = n.xy * float2(2.0, -2.0) - float2(1.0, -1.0);
            n.z = sqrt(max(0.0, 1.0 - dot(n.xy, n.xy)));
            float3 turned = float3(dot(decal.normal_rows[0].xyz, n.xzy),
                                   dot(decal.normal_rows[1].xyz, n.xzy),
                                   dot(decal.normal_rows[2].xyz, n.xzy));
            normal = normalize(lerp(normal, turned, albedo.a));
        }
        if (has_rect(decal.rects[2])) {
            float3 orm = texels[2].xyz;
            ao = lerp(ao, orm.r, albedo.a);
            roughness = lerp(roughness, orm.g, albedo.a);
            metallic = lerp(metallic, orm.b, albedo.a);
        }
    }
    if (has_rect(decal.rects[3])) {
        // Emission adds, apart from the albedo.
        emitted += texels[3].rgb * decal.modulate.rgb * (decal.params.x * fade);
    }
}

// Godot's decals (scene_forward_clustered.glsl): those of the pixel's tile within its slice's
// range, each word's from its highest bit, that share a layer with the mesh, over its surface
// before it is lit; geometry is its vertex normal, turned to its side, as Godot's geo_normal.
void apply_decals(VertexOutput input, float3 geometry, inout float3 base, inout float3 normal,
                  inout float ao, inout float roughness, inout float metallic,
                  inout float3 emitted) {
    uint2 tile = (uint2)input.position.xy / kClusterSize;
    uint words = (uint)cluster.y;
    uint decal_words = (uint)cluster.w;
    uint cell = (tile.y * (uint)cluster.x + tile.x) * cluster_stride() + words + kClusterSlices;
    float depth = -(dot(input.world, camera_z.xyz) + camera_z.w);
    uint slice = (uint)clamp(depth * cluster.z, 0.0, (float)(kClusterSlices - 1u));
    uint range = clusters[cell + decal_words + slice];
    uint first = range & 0xFFFFu;
    uint end = range >> 16u;
    float3 geo = normalize(geometry);
    float3 dx;
    float3 dy;
    surface_steps(input.world, geo, dx, dy);
    for (uint word = first >> 5u; word < (end + 31u) >> 5u; ++word) {
        uint mask = clusters[cell + word] & range_mask(word, first, end);
        while (mask != 0u) {
            uint bit = firstbithigh(mask);
            mask &= ~(1u << bit);
            Decal decal = decals[word * 32u + bit];
            if ((decal.options.x & options.z) != 0u) {
                apply_decal(decal, input.world, geo, dx, dy, base, normal, ao, roughness,
                            metallic, emitted);
            }
        }
    }
}
#endif

#ifdef CLUSTERED
// A probe's cube read along a direction in its space: the cube holds it mirrored along z.
float3 sample_probe(Probe probe, float3 direction, float level) {
    return probe_atlas
        .SampleLevel(probe_sampler, float4(direction.xy, -direction.z, probe.options.y), level)
        .rgb;
}

// Godot's reflection_process (scene_forward_lights_inc.glsl) for each probe, as its clustered
// scene shader walks them: each word of 32 from its highest bit, the frame's probes being largest
// first. reflect_probes in src/reflection.cpp is its CPU twin.
void reflect_probes(float3 world, float3 ray, float3 normal, float roughness,
                    inout float4 reflection_accum, inout float4 ambient_accum) {
    uint count = (uint)reflections.x;
    for (uint index = 0u; index < ((count + 31u) / 32u) * 32u; ++index) {
        uint at = (index / 32u) * 32u + (31u - index % 32u);
        if (at >= count) {
            continue;
        }
        Probe probe = probes[at];
        if (((uint)probe.options.z & options.z) == 0u) {
            continue;
        }
        if (reflection_accum.a >= 1.0 && ambient_accum.a >= 1.0) {
            break;
        }
        float3 extents = probe.extents.xyz;
        float3 local_pos = float3(dot(probe.rows[0], float4(world, 1.0)),
                                  dot(probe.rows[1], float4(world, 1.0)),
                                  dot(probe.rows[2], float4(world, 1.0)));
        if (any(abs(local_pos) > extents)) {
            continue;
        }
        float blend = 1.0;
        if (probe.offset.w != 0.0) {
            float3 axis = min((float3)probe.offset.w, extents);
            float3 axes = saturate(1.0 - (abs(local_pos) - extents + axis) / axis);
            float product = axes.x * axes.y * axes.z;
            blend = product * product;
        }
        if (probe.extents.w > 0.0 && reflection_accum.a < 1.0) {
            float3 local_ray = float3(dot(probe.rows[0].xyz, ray), dot(probe.rows[1].xyz, ray),
                                      dot(probe.rows[2].xyz, ray));
            if (probe.options.x != 0.0) {
                float3 nrdir = normalize(local_ray);
                float3 rbmax = (extents - local_pos) / nrdir;
                float3 rbmin = (-extents - local_pos) / nrdir;
                float3 rbminmax = nrdir > 0.0 ? rbmax : rbmin;
                // A direction along an axis's walls never meets them: Godot divides by zero
                // there, and the zero times infinity after it is not a number.
                rbminmax = nrdir == 0.0 ? 3.402823e38 : rbminmax;
                float fa = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
                local_ray = local_pos + nrdir * fa - probe.offset.xyz;
            }
            float weight = max(0.0, blend - reflection_accum.a);
            float3 texel = sample_probe(probe, local_ray, sqrt(roughness) * reflections.y);
            reflection_accum += float4(texel * reflections.z * probe.extents.w * weight, weight);
        }
        if (ambient_accum.a >= 1.0) {
            continue;
        }
        float weight = max(0.0, blend - ambient_accum.a);
        if (probe.ambient.w == 1.0) {
            float3 local_normal =
                float3(dot(probe.rows[0].xyz, normal), dot(probe.rows[1].xyz, normal),
                       dot(probe.rows[2].xyz, normal));
            float3 texel = sample_probe(probe, local_normal, reflections.y);
            ambient_accum += float4(texel * reflections.z * weight, weight);
        } else if (probe.ambient.w == 2.0) {
            ambient_accum += float4(probe.ambient.rgb * weight, weight);
        }
    }
}
#endif

#ifdef CLUSTERED
// Godot 4.6's VoxelGI cone tracing (gi.glsl's voxel_gi_compute and its cone traces), through each
// VoxelGI's 3D texture kept as tiles of the atlas in ground_texture_2. trace_voxel_gis in
// src/voxel_gi.cpp is its CPU twin.

// A VoxelGI's texture at one level, as a linear sampler that clamps reads a 3D texture: bilinear
// in the two slices around uvw, each read inside its own tile, then between them.
float4 voxel_gi_level(uint index, uint level, float3 uvw) {
    float3 extent = max(floor(voxel_gis[index].size.xyz / (float)(1u << level)), 1.0);
    float4 place = voxel_gis[index].levels[level];
    uint columns = (uint)place.z;
    float x = clamp(uvw.x * extent.x, 0.5, extent.x - 0.5);
    float y = clamp(uvw.y * extent.y, 0.5, extent.y - 0.5);
    float z = clamp(uvw.z * extent.z - 0.5, 0.0, extent.z - 1.0);
    float z0 = floor(z);
    uint width;
    uint height;
    ground_texture_2.GetDimensions(width, height);
    float2 size = float2((float)width, (float)height);
    uint low = (uint)z0;
    uint high = (uint)min(z0 + 1.0, extent.z - 1.0);
    float2 at_low = float2(place.x + (float)(low % columns) * extent.x + x,
                           place.y + (float)(low / columns) * extent.y + y);
    float2 at_high = float2(place.x + (float)(high % columns) * extent.x + x,
                            place.y + (float)(high / columns) * extent.y + y);
    float4 a = ground_texture_2.SampleLevel(ground_sampler_2, at_low / size, 0.0);
    float4 b = ground_texture_2.SampleLevel(ground_sampler_2, at_high / size, 0.0);
    return a + (b - a) * (z - z0);
}

// textureLod with a linear sampler with mipmaps: between the two levels around lod.
float4 voxel_gi_sample(uint index, float3 uvw, float lod) {
    lod = clamp(lod, 0.0, voxel_gis[index].size.w - 1.0);
    float low = floor(lod);
    float4 color = voxel_gi_level(index, (uint)low, uvw);
    if (lod != low) {
        float4 next = voxel_gi_level(index, (uint)low + 1u, uvw);
        color = color + (next - color) * (lod - low);
    }
    return color;
}

// voxel_cone_trace: a cone from pos (in cells) along direction, its diameter growing by
// 2 tan_half_angle a cell, stepping half its diameter, until it is opaque or leaves the texture.
float4 voxel_cone_trace(uint index, float3 cell_size, float max_distance, float3 pos,
                        float3 direction, float tan_half_angle, float bias) {
    float dist = bias;
    float4 color = (float4)0.0;
    [loop] while (dist < max_distance && color.a < 1.0) {
        float diameter = max(1.0, 2.0 * tan_half_angle * dist);
        float3 uvw = (pos + direction * dist) * cell_size;
        float half_diameter = diameter * 0.5;
        if (any(abs(uvw - 0.5) > 0.5 + half_diameter * cell_size)) {
            break;
        }
        float4 scolor = voxel_gi_sample(index, uvw, log2(diameter));
        float a = 1.0 - color.a;
        color += a * scolor;
        dist += half_diameter;
    }
    return color;
}

// voxel_cone_trace_45_degrees: a cone of 45 degrees each side, each step as far as its radius
// and a level coarser.
float4 voxel_cone_trace_45_degrees(uint index, float3 cell_size, float max_distance, float3 pos,
                                   float3 direction, float bias) {
    float dist = bias;
    float4 color = (float4)0.0;
    float radius = max(0.5, dist);
    float lod = log2(radius * 2.0);
    [loop] while (dist < max_distance && color.a < 1.0) {
        float3 uvw = (pos + direction * dist) * cell_size;
        if (any(abs(uvw - 0.5) > 0.5 + radius * cell_size)) {
            break;
        }
        float4 scolor = voxel_gi_sample(index, uvw, lod);
        lod += 1.0;
        float a = 1.0 - color.a;
        color += scolor * a;
        dist += radius;
        radius = max(0.5, dist);
    }
    return color;
}

// gi.glsl's cone directions about +z: six narrower ones with their weights for high quality, else
// four of 45 degrees, a quarter each.
static const float3 kHighQualityCones[6] = {
    float3(0.0, 0.0, 1.0),           float3(0.866025, 0.0, 0.5),
    float3(0.267617, 0.823639, 0.5), float3(-0.700629, 0.509037, 0.5),
    float3(-0.700629, -0.509037, 0.5), float3(0.267617, -0.823639, 0.5)};
static const float kHighQualityWeights[6] = {0.25, 0.15, 0.15, 0.15, 0.15, 0.15};
static const float3 kCones[4] = {float3(0.707107, 0.0, 0.707107), float3(0.0, 0.707107, 0.707107),
                                 float3(-0.707107, 0.0, 0.707107),
                                 float3(0.0, -0.707107, 0.707107)};

// voxel_gi_compute: one VoxelGI's cones from a point, weighed by how far inside its box it is. A
// cone about +z turns to about the normal by tangent, bitangent and normal.
void voxel_gi_compute(uint index, float3 world, float3 normal, float3 reflection, float3 tangent,
                      float3 bitangent, float roughness, inout float4 ambient_accum,
                      inout float4 reflection_accum, inout float blend_accum) {
    VoxelGI gi = voxel_gis[index];
    float3 position = float3(dot(gi.rows[0], float4(world, 1.0)),
                             dot(gi.rows[1], float4(world, 1.0)),
                             dot(gi.rows[2], float4(world, 1.0)));
    float3 ref_vec = normalize(float3(dot(gi.rows[0].xyz, reflection),
                                      dot(gi.rows[1].xyz, reflection),
                                      dot(gi.rows[2].xyz, reflection)));
    float3 n = normalize(float3(dot(gi.rows[0].xyz, normal), dot(gi.rows[1].xyz, normal),
                                dot(gi.rows[2].xyz, normal)));
    position += n * gi.params.z;
    float3 bounds = gi.size.xyz;
    if (any(position < 0.0) || any(position > bounds)) {
        return;
    }
    float3 blendv = abs(position / bounds * 2.0 - 1.0);
    float blend = saturate(1.0 - max(blendv.x, max(blendv.y, blendv.z)));
    float max_distance = length(bounds);
    float3 cell_size = 1.0 / bounds;
    float4 light = (float4)0.0;
    if (scene_voxel_gi.y != 0.0) {
        for (uint i = 0u; i < 6u; ++i) {
            float3 c = kHighQualityCones[i];
            float3 turned = tangent * c.x + bitangent * c.y + normal * c.z;
            float3 dir = normalize(float3(dot(gi.rows[0].xyz, turned), dot(gi.rows[1].xyz, turned),
                                          dot(gi.rows[2].xyz, turned)));
            light += kHighQualityWeights[i] * voxel_cone_trace(index, cell_size, max_distance,
                                                               position, dir, 0.577, gi.params.y);
        }
    } else {
        for (uint i = 0u; i < 4u; ++i) {
            float3 c = kCones[i];
            float3 turned = tangent * c.x + bitangent * c.y + normal * c.z;
            float3 dir = normalize(float3(dot(gi.rows[0].xyz, turned), dot(gi.rows[1].xyz, turned),
                                          dot(gi.rows[2].xyz, turned)));
            light += 0.25 * voxel_cone_trace_45_degrees(index, cell_size, max_distance, position,
                                                        dir, gi.params.y);
        }
    }
    float4 irradiance = voxel_cone_trace(index, cell_size, max_distance, position, ref_vec,
                                         tan(roughness * 0.5 * 3.14159265 * 0.99), gi.params.y);
    light.rgb *= gi.params.x;
    irradiance.rgb *= gi.params.x;
    if (gi.params.w == 0.0) {
        light.a = 1.0;
        irradiance.a = 1.0;
    }
    ambient_accum += light * blend;
    reflection_accum += irradiance * blend;
    blend_accum += blend;
}

// Godot's process_gi: what the pixel's VoxelGIs give it, ambient light and reflections, each with
// how much of the sky's it replaces in alpha.
void trace_voxel_gis(float3 world, float3 normal, float3 view, float roughness,
                     out float4 ambient_accum, out float4 reflection_accum) {
    float3 reflection = normalize(reflect(-view, normal));
    roughness *= roughness;
    float3 v0 = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(v0, normal));
    float3 bitangent = normalize(cross(tangent, normal));
    ambient_accum = (float4)0.0;
    reflection_accum = (float4)0.0;
    float blend_accum = 0.0;
    uint count = min((uint)voxel_gi.x, 2u);
    for (uint i = 0u; i < count; ++i) {
        voxel_gi_compute(i, world, normal, reflection, tangent, bitangent, roughness,
                         ambient_accum, reflection_accum, blend_accum);
    }
    if (blend_accum > 0.0) {
        ambient_accum /= blend_accum;
        reflection_accum /= blend_accum;
    }
}
#endif

// Godot's textureArray_bicubic (scene_forward_clustered.glsl): a cubic B-spline over the 4x4
// texels around uv, as four bilinear taps. w0 to w3 are its basis functions, g0 and g1 the taps'
// amplitudes and h0 and h1 their offsets.
float bicubic_w0(float a) {
    return (1.0 / 6.0) * (a * (a * (-a + 3.0) - 3.0) + 1.0);
}
float bicubic_w1(float a) {
    return (1.0 / 6.0) * (a * a * (3.0 * a - 6.0) + 4.0);
}
float bicubic_w2(float a) {
    return (1.0 / 6.0) * (a * (a * (-3.0 * a + 3.0) + 3.0) + 1.0);
}
float bicubic_w3(float a) {
    return (1.0 / 6.0) * (a * a * a);
}

// The lightmap's light at uv, linear, times its exposure: bicubic or bilinear, as Godot samples it.
float3 lightmap_light(float2 uv) {
    float3 light;
    if (lightmap.y != 0.0) {
        uint width;
        uint height;
        ground_normal_3.GetDimensions(width, height);
        float2 size = float2((float)width, (float)height);
        float2 texel = 1.0 / size;
        float2 at = uv * size + 0.5;
        float2 iuv = floor(at);
        float2 fuv = at - iuv;
        float2 w0 = float2(bicubic_w0(fuv.x), bicubic_w0(fuv.y));
        float2 w1 = float2(bicubic_w1(fuv.x), bicubic_w1(fuv.y));
        float2 w2 = float2(bicubic_w2(fuv.x), bicubic_w2(fuv.y));
        float2 w3 = float2(bicubic_w3(fuv.x), bicubic_w3(fuv.y));
        float2 g0 = w0 + w1;
        float2 g1 = w2 + w3;
        float2 h0 = -1.0 + w1 / g0;
        float2 h1 = 1.0 + w3 / g1;
        float2 p0 = (iuv + float2(h0.x, h0.y) - 0.5) * texel;
        float2 p1 = (iuv + float2(h1.x, h0.y) - 0.5) * texel;
        float2 p2 = (iuv + float2(h0.x, h1.y) - 0.5) * texel;
        float2 p3 = (iuv + float2(h1.x, h1.y) - 0.5) * texel;
        light = g0.y * (g0.x * ground_normal_3.SampleLevel(ground_normal_sampler_3, p0, 0.0).rgb +
                        g1.x * ground_normal_3.SampleLevel(ground_normal_sampler_3, p1, 0.0).rgb) +
                g1.y * (g0.x * ground_normal_3.SampleLevel(ground_normal_sampler_3, p2, 0.0).rgb +
                        g1.x * ground_normal_3.SampleLevel(ground_normal_sampler_3, p3, 0.0).rgb);
    } else {
        light = ground_normal_3.SampleLevel(ground_normal_sampler_3, uv, 0.0).rgb;
    }
    return light * lightmap.x;
}

// The light probes' light along a world normal, as Godot's scene_forward_clustered.glsl evaluates
// a lightmap capture's spherical harmonics.
float3 capture_light(float3 n) {
    return 0.886227 * capture[0].rgb + 1.023327 * capture[1].rgb * n.y +
           1.023327 * capture[2].rgb * n.z + 1.023327 * capture[3].rgb * n.x +
           0.858086 * capture[4].rgb * (n.x * n.y) + 0.858086 * capture[5].rgb * (n.y * n.z) +
           0.247708 * capture[6].rgb * (3.0 * n.z * n.z - 1.0) +
           0.858086 * capture[7].rgb * (n.x * n.z) +
           0.429043 * capture[8].rgb * (n.x * n.x - n.y * n.y);
}

// A lit pixel's color, linear: its lights, reflections, emission and ambient light on base, the
// albedo, with the shading normal and the one shadows offset along (geometry), its ambient light
// darkened by ao and its direct light by ao_direct. A lightmapped pixel's ambient light is its
// lightmap's, and a captured one's its light probes', either standing in for the scene's and the
// probes', as Godot's USE_LIGHTMAP. An opaque pixel of a frame with ray-traced shadows is at
// ray_pixel, else -1.
float3 lit_color(VertexOutput input, float3 base, float3 normal, float3 geometry, float metallic,
                 float roughness, float3 emitted, float ao, float ao_direct, float4 ssr,
                 int2 ray_pixel) {
    bool lightmapped = (options.x & kLightmap) != 0u;
    bool captured = (options.x & kLightmapCapture) != 0u;
    float specular = surface.z;
    float3 view = normalize(camera.xyz - input.world);
    float3 f0 = lerp((float3)(0.16 * specular * specular), base, metallic);

    float3 diffuse_light = (float3)0.0;
    float3 specular_light = (float3)0.0;
    uint count = (uint)camera.w;
    for (uint i = 0u; i < count; ++i) {
        add_light(lights[i], input.world, normal, geometry, view, f0, roughness, metallic,
                  ray_pixel, diffuse_light, specular_light);
    }
#ifdef CLUSTERED
    // The omni and spot lights of the pixel's cluster: its tile's lights within its slice's
    // range, nearest first, as Godot's scene_forward_clustered.glsl walks them.
    uint2 tile = (uint2)input.position.xy / kClusterSize;
    uint words = (uint)cluster.y;
    uint cell = (tile.y * (uint)cluster.x + tile.x) * cluster_stride();
    float depth = -(dot(input.world, camera_z.xyz) + camera_z.w);
    uint slice = (uint)clamp(depth * cluster.z, 0.0, (float)(kClusterSlices - 1u));
    uint range = clusters[cell + words + slice];
    uint first = range & 0xFFFFu;
    uint end = range >> 16u;
    for (uint word = first >> 5u; word < (end + 31u) >> 5u; ++word) {
        uint mask = clusters[cell + word] & range_mask(word, first, end);
        while (mask != 0u) {
            uint bit = firstbitlow(mask);
            mask &= mask - 1u;
            add_light(clustered_lights[word * 32u + bit], input.world, normal, geometry, view, f0,
                      roughness, metallic, ray_pixel, diffuse_light, specular_light);
        }
    }
#endif

    // Light from the sky's radiance, as Godot's scene.glsl takes it: reflections by roughness
    // and ambient light by the normal, then specular occlusion and Lazarov's environment BRDF.
    float3 ambient_light = lightmapped ? lightmap_light(input.uv2)
                           : captured  ? capture_light(normal)
                                       : ambient.rgb;
    float3 reflected = (float3)0.0;
    if (sky.y != 0.0) {
        float3 ray = reflect(-view, normal);
        ray = lerp(ray, normal, roughness * roughness);
        float horizon = min(1.0 + dot(ray, normal), 1.0);
        float3 turned = float3(dot(sky_rows[0].xyz, ray), dot(sky_rows[1].xyz, ray),
                               dot(sky_rows[2].xyz, ray));
        float3 texel = radiance_map.SampleLevel(radiance_sampler, turned,
                                                sqrt(roughness) * kRadianceMaxLevel).rgb;
        reflected = srgb_to_linear(texel) * horizon * horizon * sky.z;
    }
    if (sky.x != 0.0 && !lightmapped && !captured) {
        float3 turned = float3(dot(sky_rows[0].xyz, normal), dot(sky_rows[1].xyz, normal),
                               dot(sky_rows[2].xyz, normal));
        float3 texel = radiance_map.SampleLevel(radiance_sampler, turned, kRadianceMaxLevel).rgb;
        ambient_light = lerp(ambient_light, srgb_to_linear(texel) * sky.z, sky.w);
    }
#ifdef CLUSTERED
    if (scene_voxel_gi.x != 0.0 && voxel_gi.x > 0.0 && !lightmapped && !captured) {
        // Forward+'s VoxelGIs, as Godot's GI buffers light a pixel: their ambient light and
        // reflections go over the sky's by how much of their cones met something.
        float4 gi_ambient;
        float4 gi_reflection;
        trace_voxel_gis(input.world, normal, view, roughness, gi_ambient, gi_reflection);
        ambient_light = lerp(ambient_light, gi_ambient.rgb, gi_ambient.a);
        reflected = lerp(reflected, gi_reflection.rgb, gi_reflection.a);
    }
    if (reflections.x > 0.0) {
        // Forward+'s reflection probes, as Godot's scene_forward_clustered.glsl takes them: where
        // they reach, their reflections and ambient light go over the sky's by how much they
        // cover.
        float4 reflection_accum = (float4)0.0;
        float4 ambient_accum = (float4)0.0;
        float3 ray = normalize(reflect(-view, normal));
        ray = lerp(ray, normal, roughness * roughness * roughness * roughness);
        reflect_probes(input.world, ray, normal, roughness, reflection_accum, ambient_accum);
        if (reflection_accum.a > 0.0) {
            reflected = reflection_accum.a < 1.0
                            ? reflected * (1.0 - reflection_accum.a) + reflection_accum.rgb
                            : reflection_accum.rgb;
        }
        if (ambient_accum.a > 0.0 && !lightmapped && !captured) {
            ambient_light = ambient_accum.a < 1.0
                                ? ambient_light * (1.0 - ambient_accum.a) + ambient_accum.rgb
                                : ambient_accum.rgb;
        }
    }
#endif
    ambient_light *= ao;
    float occlusion =
        (ambient_light.r * 0.3 + ambient_light.g * 0.59 + ambient_light.b * 0.11) * 2.0;
    occlusion = min(occlusion * 4.0, 1.0);
    float reflective = (1.0 - roughness) * metallic;
    occlusion = max(min(reflective * occlusion * 10.0, 1.0), occlusion);
    float n_dot_v = saturate(dot(normal, view));
    float4 r = roughness * float4(-1.0, -0.0275, -0.572, 0.022) +
               float4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    float2 env = float2(-1.04, 1.04) * a004 + r.zw;
    if (ssr.a < 0.0) {
        reflected *= occlusion * (env.x * f0 + env.y * clamp(50.0 * f0.g, metallic, 1.0));
    } else {
        // Godot's SSR: the reflection, faded out toward the trace's roughness cutoff, put over
        // the sky's by its premultiplied alpha.
        reflected *= occlusion;
        float4 faded = ssr * smoothstep(0.0, 1.0, 1.0 - saturate((roughness - 0.6) / 0.1));
        reflected = reflected * (1.0 - faded.a) + faded.rgb;
        reflected *= env.x * f0 + env.y * clamp(50.0 * f0.g, metallic, 1.0);
    }

    ambient_light *= base * (1.0 - metallic);
    diffuse_light *= base * ao_direct * (1.0 - metallic);
    specular_light *= ao_direct;
    return diffuse_light + specular_light + reflected + emitted + ambient_light;
}

#ifdef MOTION_VECTORS
// The prepass's targets with TAA: the normal and roughness, and the motion vector.
struct PrepassOutput {
    float4 normal_roughness : SV_Target0;
    float4 motion : SV_Target1;
};
PrepassOutput fs_main(VertexOutput input, bool front : SV_IsFrontFace) {
#else
float4 fs_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target0 {
#endif
    // Every texture is sampled first, while control flow is uniform, as WGSL requires. A
    // terrain's ground layers tile across its maps.
    bool terrain = (options.w & kTerrain) != 0u;
    float4 texel = albedo_texture.Sample(albedo_sampler, input.uv);
    float4 metallic_texel = metallic_texture.Sample(metallic_sampler, input.uv);
    float4 roughness_texel = roughness_texture.Sample(roughness_sampler, input.uv);
    float4 emission_texel =
        emission_texture.Sample(emission_sampler, terrain ? input.uv * terrain_uv.x : input.uv);
    float4 mapped_texel =
        normal_texture.Sample(normal_sampler, terrain ? input.uv * terrain_uv.y : input.uv);
    float4 ground_2 = ground_texture_2.Sample(ground_sampler_2, input.uv * terrain_uv.z);
    float4 ground_3 = ground_texture_3.Sample(ground_sampler_3, input.uv * terrain_uv.w);
    float4 normal_0 = ground_normal_0.Sample(ground_normal_sampler_0, input.uv * terrain_uv.x);
    float4 normal_1 = ground_normal_1.Sample(ground_normal_sampler_1, input.uv * terrain_uv.y);
    float4 normal_2 = ground_normal_2.Sample(ground_normal_sampler_2, input.uv * terrain_uv.z);
    float4 normal_3 = ground_normal_3.Sample(ground_normal_sampler_3, input.uv * terrain_uv.w);

    uint flags = options.x;
    float4 color = albedo * texel;
    if ((flags & kVertexColorAlbedo) != 0u) {
        color *= input.color;
    }
    // Opaque materials leave alpha alone, as Godot's StandardMaterial3D only sets ALPHA when
    // it is transparent. A terrain's color map cuts its holes here.
    float alpha = (flags & kHasAlpha) != 0u ? color.a : 1.0;
    if ((flags & kAlphaScissor) != 0u) {
        if (alpha < asfloat(options.y)) {
            discard;
        }
        alpha = 1.0;
    }
    float3 base;
    float metallic;
    float roughness;
    float3 shape = (float3)0.0;
    float3 ground = (float3)0.0;
    if (terrain) {
        // HTerrain's Classic4 shader: each layer tinted by the color map, weighed by the splat
        // map (depth-blended by the bumps), its normal added to the terrain's.
        uint terrain_flags = options.w;
        float4 normal_roughness[4] = {normal_0, normal_1, normal_2, normal_3};
        float4 albedo_bump[4] = {emission_texel, mapped_texel, ground_2, ground_3};
        float4 bumps = float4(albedo_bump[0].a, albedo_bump[1].a, albedo_bump[2].a,
                              albedo_bump[3].a);
        float4 weights = (terrain_flags & kTerrainDepthBlending) != 0u
                             ? depth_blended_weights(metallic_texel, bumps)
                             : metallic_texel;
        // Splat weights that are all 0 leave the ground black rather than undefined.
        float sum = max(weights.x + weights.y + weights.z + weights.w, 1.0e-6);
        float3 blended = (float3)0.0;
        float rough = 0.0;
        [unroll] for (uint i = 0u; i < 4u; ++i) {
            // A layer without a normal_roughness texture is flat, with roughness 1.
            float4 nr = (terrain_flags & (kTerrainNormals << i)) != 0u
                            ? normal_roughness[i]
                            : float4(0.5, 0.5, 1.0, 1.0);
            float share = weights[i] / sum;
            blended += srgb_to_linear(albedo_bump[i].rgb) *
                       lerp((float3)1.0, texel.rgb, terrain_opacity[i]) * share;
            rough += nr.a * share;
            ground += unpack_ground_normal(nr) * share;
        }
        base = srgb_to_linear(albedo.rgb) * blended;
        metallic = surface.x;
        roughness = surface.y * rough;
        shape = normalize(roughness_texel.xyz * 2.0 - 1.0);
    } else {
        base = srgb_to_linear(color.rgb);
        metallic = surface.x * dot(metallic_texel, metallic_channel);
        roughness = surface.y * dot(roughness_texel, roughness_channel);
    }
    float3 normal = terrain ? frame_direction(input, normalize(float3(shape.x + ground.x, shape.y,
                                                                      shape.z + ground.z)))
                            : input.normal;
    if ((flags & kSideCheck) != 0u && !front) {
        normal = -normal;
    }
    if ((flags & kNormalMap) != 0u) {
        // With the interpolated, unnormalized tangent frame, per MikkTSpace, as Godot does.
        float3 mapped = mapped_texel.rgb;
        mapped.xy = mapped.xy * 2.0 - 1.0;
        mapped.z = sqrt(max(0.0, 1.0 - dot(mapped.xy, mapped.xy)));
        normal = normalize(lerp(
            normal, input.tangent * mapped.x + input.binormal * mapped.y + normal * mapped.z,
            surface.w));
    } else {
        normal = normalize(normal);
    }
#ifdef NORMAL_ROUGHNESS
    // The prepass's normal in the camera's space, as Godot's normal_roughness_buffer has it, and
    // roughness, in its seven bits.
    float3 view_normal =
        normalize(float3(dot(view_rows[0].xyz, normal), dot(view_rows[1].xyz, normal),
                         dot(view_rows[2].xyz, normal)));
    float4 normal_roughness = float4(view_normal * 0.5 + 0.5, roughness * (127.0 / 255.0));
#ifdef MOTION_VECTORS
    // Godot's motion vector: the move from where the surface is to where it was, each without its
    // frame's jitter, in texture coordinates, whose y runs down.
    float2 position_uv = input.motion_clip.xy / input.motion_clip.w * 0.5;
    float2 previous_uv = input.previous_clip.xy / input.previous_clip.w * 0.5;
    float2 motion = previous_uv - position_uv;
    PrepassOutput output;
    output.normal_roughness = normal_roughness;
    output.motion = float4(motion.x, -motion.y, 0.0, 0.0);
    return output;
#else
    return normal_roughness;
#endif
#else
    float3 emitted = (float3)0.0;
    if ((flags & kUnshaded) == 0u) {
        emitted = emission.rgb;
        if ((flags & kEmissionTexture) != 0u) {
            emitted += emission_texel.rgb;
        }
        emitted = srgb_to_linear(emitted) * emission.w;
    }
    // Shadows offset the point they sample along the vertex normal, as Godot's normal_interp; a
    // terrain's along its shape's.
    float3 geometry = terrain ? frame_direction(input, shape) : normalize(input.normal);
    // Godot's SSAO: the frame's occlusion at the pixel darkens the opaque meshes' ambient light,
    // with the decals' ambient occlusion, and their direct light by its light affect times its AO
    // channel affect. Its SSR (alpha below 0 for none) goes over their reflections of the sky.
    float ao = 1.0;
    float ao_direct = 1.0;
    float4 ssr = float4(0.0, 0.0, 0.0, -1.0);
    int2 ray_pixel = int2(-1, -1);
#ifdef CLUSTERED
    if (decal_info.x > 0.0) {
        // Forward+'s decals change the surface before it is lit.
        bool flipped = (flags & kSideCheck) != 0u && !front;
        apply_decals(input, flipped ? -geometry : geometry, base, normal, ao, roughness, metallic,
                     emitted);
    }
    bool opaque = (flags & kHasAlpha) == 0u || (flags & kAlphaScissor) != 0u;
    if (ssao.x != 0.0 && opaque) {
        ao = min(ao, screen_texture.Load(int4((int2)input.position.xy, 0, 0)).r);
        ao_direct = lerp(1.0, ao, lerp(0.0, max(0.0, ssao.y), ssao.z));
    }
    if (ssao.w != 0.0 && opaque) {
        ssr = screen_texture.Load(int4((int2)input.position.xy, 1, 0));
    }
    if (screen.z != 0.0 && opaque) {
        ray_pixel = (int2)input.position.xy;
    }
#endif
    float3 lit = base;
    if ((flags & kUnshaded) == 0u) {
        lit = lit_color(input, base, normal, geometry, metallic, roughness, emitted, ao,
                        ao_direct, ssr, ray_pixel);
    }
    // One call, so the tonemappers are inlined once and the shader stays small.
    return finish(lit, alpha, input.world, input.position.xy);
#endif
}
