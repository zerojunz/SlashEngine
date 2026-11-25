// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
// Each pixel's SDFGI light, for the GI buffers: one triangle over the screen texture's layers 5
// (ambient light and how much it covers) and 6 (reflections and how much), from the prepass's depth
// and normals and the cascades sdfgi.hlsl keeps. run_sdfgi in src/sdfgi.cpp is its CPU twin; keep
// them in step.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

cbuffer Gather : register(b0, space3) {
    float4 to_world[4];  // the prepass's clip space to the world, a row each
    float4 view_rows[3]; // the world to the camera's space, a row each
    float4 camera;       // the camera's place in the world, then the light's energy
    uint4 grid;          // the cells a side, the cascades, flags (SdfgiFlags)
    float4 params;       // the y scale, the normal bias, the target's width and height
};

Texture2D<float4> depth_map : register(t0, space2);
Texture2D<float4> normal_map : register(t1, space2);
SamplerState depth_sampler : register(s0, space2);
SamplerState normal_sampler : register(s1, space2);

// The cascades' words (src/sdfgi_gpu.hpp). Metal numbers storage buffers after the uniforms,
// and WebGPU's go in a bind group of their own.
#if defined(SLASH_MSL)
StructuredBuffer<uint> cell_words : register(t1, space2);
StructuredBuffer<uint> field_words : register(t2, space2);
StructuredBuffer<uint> probe_words : register(t3, space2);
StructuredBuffer<uint> items : register(t4, space2);
#elif defined(SLASH_WGSL)
StructuredBuffer<uint> cell_words : register(t0, space0);
StructuredBuffer<uint> field_words : register(t1, space0);
StructuredBuffer<uint> probe_words : register(t2, space0);
StructuredBuffer<uint> items : register(t3, space0);
#else
StructuredBuffer<uint> cell_words : register(t2, space2);
StructuredBuffer<uint> field_words : register(t3, space2);
StructuredBuffer<uint> probe_words : register(t4, space2);
StructuredBuffer<uint> items : register(t5, space2);
#endif

#include "sdfgi_common.hlsl"

static const uint kFlagOcclusion = 1u;
static const float kOffsetShare = 0.003;
static const float kOffsetLeast = 0.002;
static const uint kMaxCascades = 8u;

// ray_surface: a pixel's place in the world moved off it along its normal, and the normal
// turned to face the camera. False where the prepass drew nothing.
bool surface_at(uint2 pixel, out float3 origin, out float3 normal) {
    origin = float3(0.0, 0.0, 0.0);
    normal = float3(0.0, 0.0, 0.0);
    float depth = depth_map.Load(int3(pixel, 0)).r;
    if (!(depth > 0.0)) {
        return false;
    }
    float4 ndc = float4(((float(pixel.x) + 0.5) / params.z * 2.0) - 1.0,
                        1.0 - ((float(pixel.y) + 0.5) / params.w * 2.0), depth, 1.0);
    float4 clip = float4(dot(to_world[0], ndc), dot(to_world[1], ndc), dot(to_world[2], ndc),
                         dot(to_world[3], ndc));
    if (clip.w == 0.0) {
        return false;
    }
    float3 world = clip.xyz / clip.w;
    float3 seen = float3(dot(view_rows[0].xyz, world) + view_rows[0].w,
                         dot(view_rows[1].xyz, world) + view_rows[1].w,
                         dot(view_rows[2].xyz, world) + view_rows[2].w);
    float3 facing = (normal_map.Load(int3(pixel, 0)).rgb * 2.0) - 1.0;
    if (dot(facing, seen) > 0.0) {
        facing = -facing;
    }
    float size = length(facing);
    if (!(size > 0.0)) {
        return false;
    }
    normal = (view_rows[0].xyz * (facing.x / size)) + (view_rows[1].xyz * (facing.y / size)) +
             (view_rows[2].xyz * (facing.z / size));
    origin = world + (normal * ((kOffsetShare * length(seen)) + kOffsetLeast));
    return true;
}

// sdfvoxel_gi_process: a cascade's probes around a point (in probes) seen along a normal and a
// reflection.
void probe_light(uint index, float3 cascade_pos, float3 normal, float3 reflection,
                 float roughness, out float3 diffuse, out float3 specular) {
    Cascade c = cascade_at(index);
    float csize = float(grid_size());
    float normal_bias = (params.y / csize) * float(kProbeAxis - 1u);
    cascade_pos += normal * normal_bias;
    int3 base = int3(floor(cascade_pos));
    float3 diffuse_accum = float3(0.0, 0.0, 0.0);
    float weight_accum = 0.0;
    float3 specular_accum = float3(0.0, 0.0, 0.0);
    uint irradiance = probe_irradiance_at(c);
    uint radiance = probe_radiance_at(c);
    [loop] for (uint j = 0u; j < 8u; ++j) {
        int3 probe = base + int3(int(j & 1u), int((j >> 1u) & 1u), int((j >> 2u) & 1u));
        float3 probe_to_pos = cascade_pos - float3(probe);
        float3 probe_dir = normalize(-probe_to_pos);
        float3 trilinear = float3(1.0, 1.0, 1.0) - abs(probe_to_pos);
        float weight =
            trilinear.x * trilinear.y * trilinear.z * max(0.005, dot(normal, probe_dir));
        if ((grid.z & kFlagOcclusion) != 0u) {
            float occ = float(grid_size() / 16u);
            float limit = (occ - 0.5) / occ;
            float3 p = float3(probe);
            float3 at = clamp(cascade_pos, p - limit, p + limit) * occ;
            weight *= max(occlusion_at(c, at, probe_parity(c, probe)), 0.01);
        }
        float3 d = probe_texel(irradiance, probe, normal);
        diffuse_accum += d * weight;
        weight_accum += weight;
        float3 s = float3(0.0, 0.0, 0.0);
        if (roughness < 0.99) {
            s = probe_texel(radiance, probe, reflection);
        }
        if (roughness > 0.2) {
            s = lerp(s, probe_texel(irradiance, probe, reflection), (roughness - 0.2) * 1.25);
        }
        specular_accum += s * weight;
    }
    if (weight_accum > 0.0) {
        diffuse_accum *= 1.0 / weight_accum;
        specular_accum *= 1.0 / weight_accum;
    }
    diffuse = diffuse_accum;
    specular = specular_accum;
}

// A cascade's place relative to the camera, and the scale to probes.
float3 cascade_position(uint i, float3 camera_scaled) {
    return cascade_at(i).offset - camera_scaled;
}
float cascade_to_probe(uint i) {
    return 1.0 / (float(grid_size()) * cascade_at(i).cell_size / float(kProbeAxis - 1u));
}

// The light a smooth reflection meets in a cascade at a point (relative to the camera), and the
// distance to step on.
float4 reflection_hit(uint k, float3 ray_pos, float3 camera_scaled, float softness,
                      out float fdistance) {
    Cascade c = cascade_at(k);
    float3 p = (ray_pos - cascade_position(k, camera_scaled)) * c.to_cell;
    fdistance = sdf_at(c, p) - 1.1;
    float4 light = float4(0.0, 0.0, 0.0, 0.0);
    if (fdistance < softness) {
        light.a = clamp(1.0 - (fdistance / softness), 0.0, 1.0);
        light.rgb = light_at(c, p) * 0.5 * light.a;
    }
    fdistance *= c.cell_size;
    return light;
}

struct Gathered {
    float4 ambient : SV_Target0;
    float4 reflection : SV_Target1;
};

Gathered fs_main(float4 position : SV_Position) {
    Gathered out_light;
    out_light.ambient = float4(0.0, 0.0, 0.0, 0.0);
    out_light.reflection = float4(0.0, 0.0, 0.0, 0.0);
    uint2 pixel = uint2(position.xy);
    float3 origin;
    float3 normal;
    uint count = cascade_count();
    if (count == 0u || !surface_at(pixel, origin, normal)) {
        return out_light;
    }
    float roughness = clamp(normal_map.Load(int3(pixel, 0)).a * 255.0 / 127.0, 0.0, 1.0);
    float3 view = normalize(origin - camera.xyz);
    float3 reflection = view - normal * (2.0 * dot(view, normal));
    float y_mult = params.x;
    float3 camera_scaled = float3(camera.x, camera.y * y_mult, camera.z);
    float3 vertex = origin - camera.xyz;
    vertex.y *= y_mult;
    normal.y *= y_mult;
    reflection.y *= y_mult;
    normal = normalize(normal);
    reflection = normalize(reflection);
    float size = float(grid_size());
    float probe_size = float(kProbeAxis - 1u);
    uint cascade = count;
    float3 cascade_pos = float3(0.0, 0.0, 0.0);
    // The loops here have no break or continue: Tint's SPIR-V reader can't take loops that
    // leave early with values.
    [loop] for (uint i = 0u; i < count && cascade == count; ++i) {
        float3 pos = (vertex - cascade_position(i, camera_scaled)) * cascade_to_probe(i);
        if (all(pos >= 0.0) && all(pos < probe_size)) {
            cascade = i;
            cascade_pos = pos;
        }
    }
    if (cascade == count) {
        return out_light;
    }
    float4 ambient = float4(0.0, 0.0, 0.0, 1.0);
    float4 reflected = float4(0.0, 0.0, 0.0, 1.0);
    float3 diffuse;
    float3 specular;
    probe_light(cascade, cascade_pos, normal, reflection, roughness, diffuse, specular);
    {
        float blend_from = (probe_size / 2.0) - 2.5;
        float blend_to = blend_from + 2.0;
        float3 inner = vertex * cascade_to_probe(cascade);
        float len = length(inner);
        inner = normalize(inner);
        len *= max(abs(inner.x), max(abs(inner.y), abs(inner.z)));
        float blend = len >= blend_from ? smoothstep(blend_from, blend_to, len) : 0.0;
        if (blend > 0.0) {
            if (cascade == count - 1u) {
                ambient.a = 1.0 - blend;
                reflected.a = 1.0 - blend;
            } else {
                float3 diffuse2;
                float3 specular2;
                float3 pos2 = (vertex - cascade_position(cascade + 1u, camera_scaled)) *
                              cascade_to_probe(cascade + 1u);
                probe_light(cascade + 1u, pos2, normal, reflection, roughness, diffuse2,
                            specular2);
                diffuse = lerp(diffuse, diffuse2, blend);
                specular = lerp(specular, specular2, blend);
            }
        }
    }
    // Smooth surfaces march their reflection through the distance fields.
    if (roughness < 0.2) {
        float blend_size = (size / probe_size) * 0.5;
        float radius[kMaxCascades];
        uint first = 0xFFFFu;
        float base_distance = length(vertex);
        [unroll] for (uint r = 0u; r < kMaxCascades; ++r) {
            radius[r] = 0.0;
        }
        [loop] for (uint i2 = 0u; i2 < count; ++i2) {
            radius[i2] = cascade_at(i2).cell_size * ((size * 0.5) - blend_size);
            if (first == 0xFFFFu && base_distance < radius[i2]) {
                first = i2;
            }
        }
        uint at = min(first, count - 1u);
        float max_distance = radius[count - 1u];
        float3 ray_pos = vertex;
        {
            float prev = at > 0u ? radius[at - 1u] : 0.0;
            float base_blend = (base_distance - prev) / (radius[at] - prev);
            float bias = (1.0 + base_blend) * 1.1;
            float most = max(abs(reflection.x), max(abs(reflection.y), abs(reflection.z)));
            ray_pos += ((reflection / most) + (normal * 1.4)) * (bias * cascade_at(at).cell_size);
        }
        float softness = 0.2 + (min(1.0, roughness * 5.0) * 4.0);
        float4 accum = float4(0.0, 0.0, 0.0, 0.0);
        uint k = 0u;
        bool found = false;
        // The loop, with a bound on its steps.
        [loop] for (int steps = 0;
                    steps < 4096 && length(ray_pos) < max_distance && accum.a <= 0.99; ++steps) {
            if (!found && k >= at && length(ray_pos) < radius[k]) {
                uint next = min(k + 1u, count - 1u);
                at = max(k, at);
                float fdistance;
                float4 light = reflection_hit(k, ray_pos, camera_scaled, softness, fdistance);
                if (k < count - 1u) {
                    float fdistance2;
                    float4 light2 =
                        reflection_hit(next, ray_pos, camera_scaled, softness, fdistance2);
                    float prev2 = k == 0u ? 0.0 : radius[k - 1u];
                    float blend2 =
                        clamp((length(ray_pos) - prev2) / (radius[k] - prev2), 0.0, 1.0);
                    light += (light2 - light) * blend2;
                    fdistance += (fdistance2 - fdistance) * blend2;
                }
                accum += light;
                ray_pos += reflection * fdistance;
                found = true;
            }
            ++k;
            if (k == count) {
                k = 0u;
                found = false;
            }
        }
        float3 light_rgb = accum.rgb * (1.0 / max(accum.a, 1e-5));
        float alpha = min(1.0, accum.a);
        float b = min(1.0, roughness * 5.0);
        float sa = 1.0 - b;
        reflected.a = (alpha * sa) + b;
        if (reflected.a == 0.0) {
            specular = float3(0.0, 0.0, 0.0);
        } else {
            specular = ((light_rgb * (alpha * sa)) + (specular * b)) * (1.0 / reflected.a);
        }
    }
    ambient.rgb = diffuse * camera.w;
    reflected.rgb = specular * camera.w;
    out_light.ambient = ambient;
    out_light.reflection = reflected;
    return out_light;
}
