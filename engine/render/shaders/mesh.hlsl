// The 3D shader: meshes lit by a frame's lights as Godot's Compatibility renderer lights them
// (drivers/gles3/shaders/scene.glsl), with a StandardMaterial3D's albedo, metallic, roughness,
// specular, emission and normal map, and shadowed by the lights' shadow maps (shadow.hlsl draws
// them). Colors are worked in linear, tonemapped (tonemap.hlsl) and written as sRGB, with Godot's
// approximations of the two.
// Keep it in step with shade_mesh in src/mesh.cpp, its CPU twin. Registers follow SDL's GPU
// conventions: vertex uniforms in space1, fragment textures and samplers in space2, fragment
// uniforms in space3.

cbuffer Camera : register(b0, space1) {
    float4x4 view_projection; // world to clip space
};

cbuffer Instance : register(b1, space1) {
    float4x4 model;        // the mesh's space to the world
    float4x4 normal_model; // the inverse transpose of model's upper 3x3, in its upper 3x3
    float4 uv1;            // xy scale and zw offset, as Godot's uv1_scale and uv1_offset
};

struct VertexInput {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : TEXCOORD4;
};

struct VertexOutput {
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float3 binormal : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float4 color : TEXCOORD5;
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
    return output;
}

Texture2D<float4> albedo_texture : register(t0, space2);
Texture2D<float4> metallic_texture : register(t1, space2);
Texture2D<float4> roughness_texture : register(t2, space2);
Texture2D<float4> emission_texture : register(t3, space2);
Texture2D<float4> normal_texture : register(t4, space2);
SamplerState albedo_sampler : register(s0, space2);
SamplerState metallic_sampler : register(s1, space2);
SamplerState roughness_sampler : register(s2, space2);
SamplerState emission_sampler : register(s3, space2);
SamplerState normal_sampler : register(s4, space2);
// The shadow atlases, depths compared with GREATER_OR_EQUAL and filtered linearly.
Texture2D directional_atlas : register(t5, space2);
Texture2D positional_atlas : register(t6, space2);
SamplerComparisonState directional_sampler : register(s5, space2);
SamplerComparisonState positional_sampler : register(s6, space2);

static const uint kUnshaded = 1u;
static const uint kVertexColorAlbedo = 2u;
static const uint kAlphaScissor = 4u;
static const uint kEmissionTexture = 8u;
static const uint kNormalMap = 16u;
static const uint kSideCheck = 32u;
static const uint kHasAlpha = 64u;

cbuffer Surface : register(b0, space3) {
    float4 albedo;            // sRGB, and alpha
    float4 emission;          // sRGB, and its energy
    float4 surface;           // metallic, roughness, specular and the normal map's scale
    float4 metallic_channel;  // which of metallic_texture's channels is metallic
    float4 roughness_channel; // and roughness_texture's roughness
    // x: the flags above, y: the alpha scissor threshold's bits, z: the draw's layers.
    uint4 options;
};

struct Light {
    float4 position;  // xyz, then 1 / range
    float4 direction; // toward a directional light, the way a spot light shines; then the type
    float4 color;     // linear, times energy and pi; then the specular amount
    float4 params;    // attenuation, the cone's cosine, 1 / spot attenuation, the cull mask
    float4 shadow;    // its slot in directional_shadows or positional_shadows, or -1
};

static const float kDirectional = 0.0;
static const float kOmni = 1.0;
static const float kSpot = 2.0;
static const float kPi = 3.14159265358979323;

cbuffer Scene : register(b1, space3) {
    float4 camera;  // the camera's place, then how many lights
    float4 ambient; // linear
    // Exposure, white, the tonemapper, and 1 to tonemap here (0 when post.hlsl does it after).
    float4 tonemap;
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

#include "tonemap.hlsl"

// A lit color, linear, as the frame's target takes it: times the exposure, tonemapped unless the
// post pass does it, and as sRGB, as Godot's scene.glsl ends.
float4 finish(float3 color, float alpha) {
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

// A lit pixel's color, linear: its lights, emission and ambient light on base, the albedo.
float3 lit_color(VertexOutput input, bool front, uint flags, float3 base, float4 metallic_texel,
                 float4 roughness_texel, float3 emission_texel, float3 mapped) {
    float metallic = surface.x * dot(metallic_texel, metallic_channel);
    float roughness = surface.y * dot(roughness_texel, roughness_channel);
    float specular = surface.z;
    float3 emitted = emission.rgb;
    if ((flags & kEmissionTexture) != 0u) {
        emitted += emission_texel;
    }
    emitted = srgb_to_linear(emitted * emission.w);

    float3 normal = input.normal;
    if ((flags & kSideCheck) != 0u && !front) {
        normal = -normal;
    }
    if ((flags & kNormalMap) != 0u) {
        // With the interpolated, unnormalized tangent frame, per MikkTSpace, as Godot does.
        mapped.xy = mapped.xy * 2.0 - 1.0;
        mapped.z = sqrt(max(0.0, 1.0 - dot(mapped.xy, mapped.xy)));
        normal = normalize(lerp(
            normal, input.tangent * mapped.x + input.binormal * mapped.y + normal * mapped.z,
            surface.w));
    } else {
        normal = normalize(normal);
    }
    float3 view = normalize(camera.xyz - input.world);
    float3 f0 = lerp((float3)(0.16 * specular * specular), base, metallic);

    // Shadows offset the point they sample along the vertex normal, as Godot's normal_interp.
    float3 geometry = normalize(input.normal);
    float3 diffuse_light = (float3)0.0;
    float3 specular_light = (float3)0.0;
    uint count = (uint)camera.w;
    for (uint i = 0u; i < count; ++i) {
        Light light = lights[i];
        if (((uint)light.params.w & options.z) == 0u) {
            continue;
        }
        float type = light.direction.w;
        float slot = light.shadow.x;
        bool directional = type == kDirectional;
        float3 to_light = light.position.xyz - input.world;
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
        if (slot >= 0.0) {
            attenuation *= light_shadow(directional, (uint)slot, input.world, geometry, toward);
        }
        light_compute(normal, toward, view, light.color.rgb, attenuation, f0, roughness, metallic,
                      light.color.w, diffuse_light, specular_light);
    }

    float3 ambient_light = ambient.rgb * base * (1.0 - metallic);
    diffuse_light *= base * (1.0 - metallic);
    return diffuse_light + specular_light + emitted + ambient_light;
}

float4 fs_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target0 {
    // Every texture is sampled first, while control flow is uniform, as WGSL requires.
    float4 texel = albedo_texture.Sample(albedo_sampler, input.uv);
    float4 metallic_texel = metallic_texture.Sample(metallic_sampler, input.uv);
    float4 roughness_texel = roughness_texture.Sample(roughness_sampler, input.uv);
    float3 emission_texel = emission_texture.Sample(emission_sampler, input.uv).rgb;
    float3 mapped = normal_texture.Sample(normal_sampler, input.uv).rgb;

    uint flags = options.x;
    float4 color = albedo * texel;
    if ((flags & kVertexColorAlbedo) != 0u) {
        color *= input.color;
    }
    // Opaque materials leave alpha alone, as Godot's StandardMaterial3D only sets ALPHA when
    // it is transparent.
    float alpha = (flags & kHasAlpha) != 0u ? color.a : 1.0;
    if ((flags & kAlphaScissor) != 0u) {
        if (alpha < asfloat(options.y)) {
            discard;
        }
        alpha = 1.0;
    }
    float3 base = srgb_to_linear(color.rgb);
    float3 lit = base;
    if ((flags & kUnshaded) == 0u) {
        lit = lit_color(input, front, flags, base, metallic_texel, roughness_texel, emission_texel,
                        mapped);
    }
    // One call, so the tonemappers are inlined once and the shader stays small.
    return finish(lit, alpha);
}
