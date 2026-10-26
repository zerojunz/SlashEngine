// The background of a frame's 3D world, as Godot's Compatibility renderer draws it
// (drivers/gles3/shaders/sky.glsl): a ProceduralSkyMaterial, PhysicalSkyMaterial or
// PanoramaSkyMaterial (scene/resources/3d/sky_material.cpp) worked out for each pixel's view
// direction, or the background color when fog covers it, then fogged, exposed and tonemapped as
// meshes are, or for Forward+ left linear for forward_post.hlsl. It draws one triangle over the
// whole target, before the meshes. Keep it in step with shade_sky in src/sky.cpp, its CPU twin.

#include "tonemap.hlsl"

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// A ProceduralSkyMaterial's sky_cover, a PhysicalSkyMaterial's night_sky or a
// PanoramaSkyMaterial's panorama.
Texture2D<float4> sky_texture : register(t0, space2);
SamplerState sky_sampler : register(s0, space2);

static const uint kSkyColor = 1u;
static const uint kSkyProcedural = 2u;
static const uint kSkyPhysical = 3u;
static const uint kSkyPanorama = 4u;

static const uint kSkyDeband = 1u;
static const uint kSkyTexture = 2u;

static const float kPi = 3.14159265359;

// A DirectionalLight3D the sky shows, as Godot's DirectionalLightData.
struct SkyLight {
    float4 direction_energy; // toward the light, in the world; its energy
    float4 color_size;       // sRGB; the cosine of its angular radius
    float4 disk;             // the cosines of a physical sky's sun disk's radius and half that
};

cbuffer Sky : register(b0, space3) {
    float4x4 orientation; // the camera's space to the sky's: the inverse of sky_rotation, times
                          // the camera's turn
    // Godot's: the projection's [2][0], [0][0], [2][1] and [1][1], which turn a pixel into the
    // direction it looks in.
    float4 projection;
    float4 pixel;   // one over the target's size, then the background's energy multiplier
    float4 tonemap; // exposure, white, the tonemapper, and 1 to tonemap here (2 to stay linear)
    float4 fog_color; // linear, times its energy; then 1 when fog is on
    float4 fog;       // aerial perspective, sun scatter, sky affect
    // The material's, by its kind: see sky_uniforms in src/sky.cpp.
    float4 params[6];
    SkyLight lights[4];
    uint4 options; // the kind, how many lights, kSkyDeband | kSkyTexture
};

// Eberly's approximations, as Godot's sky.glsl takes them: acos for -1 to 1, and atan2.
float acos_approx(float p_x) {
    float x = abs(p_x);
    float res = -0.156583 * x + (kPi / 2.0);
    res *= sqrt(1.0 - x);
    return (p_x >= 0.0) ? res : kPi - res;
}

float atan2_approx(float y, float x) {
    float a = min(abs(x), abs(y)) / max(abs(x), abs(y));
    float s = a * a;
    float poly = 0.0872929;
    poly = -0.301895 + poly * s;
    poly = 1.0 + poly * s;
    poly = poly * a;
    float r = abs(y) > abs(x) ? (kPi / 2.0) - poly : poly;
    r = x < 0.0 ? kPi - r : r;
    r = y < 0.0 ? -r : r;
    return r;
}

float3 procedural_sky(float3 eye, float4 cover) {
    float v_angle = clamp(eye.y, -1.0, 1.0);
    float3 sky = lerp(params[0].rgb, params[1].rgb, saturate(pow(1.0 - v_angle, params[0].w)));
    float sun_angle_max = params[2].w;
    float inv_sun_curve = params[3].w;
    uint count = options.y;
    for (uint i = 0u; i < count; ++i) {
        SkyLight light = lights[i];
        float3 sun = light.color_size.rgb * light.direction_energy.w;
        float sun_angle = dot(light.direction_energy.xyz, eye);
        float sun_size = light.color_size.w;
        if (sun_angle > sun_size) {
            sky = sun;
        } else if (sun_angle > sun_angle_max) {
            float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
            sky = lerp(sky, sun, saturate(pow(1.0 - c2, inv_sun_curve)));
        }
    }
    if ((options.z & kSkyTexture) != 0u) {
        sky += (cover.rgb * params[4].rgb) * cover.a * params[4].a;
    }
    float3 ground =
        lerp(params[2].rgb, params[3].rgb, saturate(pow(1.0 + v_angle, params[1].w)));
    return lerp(ground, sky, step(0.0, eye.y)) * params[5].x;
}

float henyey_greenstein(float cos_theta, float g) {
    const float k = 0.0795774715459;
    return k * (1.0 - g * g) / (pow(1.0 + g * g - 2.0 * g * cos_theta, 1.5));
}

float3 physical_sky(float3 eye, float4 night) {
    bool use_night = (options.z & kSkyTexture) != 0u;
    float exposure = params[3].z;
    if (options.y == 0u) {
        // There is no sun, so it shows the night sky and nothing else.
        return (use_night ? night.rgb : (float3)0.0) * exposure;
    }
    SkyLight light = lights[0];
    float3 toward = light.direction_energy.xyz;
    float zenith_angle = clamp(toward.y, -1.0, 1.0);
    float sun_energy = max(0.0, 0.757 * zenith_angle) * light.direction_energy.w;
    float sun_fade = 1.0 - saturate(1.0 - exp(toward.y));

    float rayleigh_coefficient = params[0].w - (1.0 * (1.0 - sun_fade));
    float3 rayleigh_beta = rayleigh_coefficient * params[0].rgb * 0.0001;
    // Mie coefficients from Preetham.
    float3 mie_beta = params[3].x * params[1].w * params[1].rgb * 0.000434;

    // The optical length at the zenith is 8400 for molecules and 1250 for aerosols.
    float zenith = max(0.0, eye.y);
    float optical_mass = 1.0 / (zenith + 0.15 * pow(3.885 + 54.5 * zenith, -1.253));
    float rayleigh_scatter = 8.4e3 * optical_mass;
    float mie_scatter = 1.25e3 * optical_mass;

    // Light extinction by the thickness of the atmosphere.
    float3 extinction = exp(-(rayleigh_beta * rayleigh_scatter + mie_beta * mie_scatter));

    // In scattering.
    float cos_theta = dot(eye, toward);
    float rayleigh_phase = (3.0 / (16.0 * kPi)) * (1.0 + pow(cos_theta * 0.5 + 0.5, 2.0));
    float3 beta_r_theta = rayleigh_beta * rayleigh_phase;
    float mie_phase = henyey_greenstein(cos_theta, params[2].w);
    float3 beta_m_theta = mie_beta * mie_phase;
    float3 scattered = sun_energy * ((beta_r_theta + beta_m_theta) / (rayleigh_beta + mie_beta));
    float3 lin = pow(scattered * (1.0 - extinction), (float3)1.5);
    // A hack from three.js's Sky.js, as Godot's.
    lin *= lerp((float3)1.0, pow(scattered * extinction, (float3)0.5),
                saturate(pow(1.0 - zenith_angle, 5.0)));
    // And the ground color, hacked in.
    lin *= lerp(params[2].rgb, (float3)1.0, smoothstep(-0.1, 0.1, eye.y));

    // The solar disk and out-scattering.
    float sundisk = smoothstep(light.disk.x, light.disk.y, cos_theta);
    float3 l0 = (sun_energy * extinction) * sundisk * light.color_size.rgb;
    if (use_night) {
        l0 += night.rgb * extinction;
    }
    float3 color = lin + l0;
    return pow(color, (float3)(1.0 / (1.2 + (1.2 * sun_fade)))) * exposure;
}

float3 interleaved_gradient_noise(float2 position) {
    // https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    float res = frac(magic.z * frac(dot(position, magic.xy))) * 2.0 - 1.0;
    return float3(res, -res, res) / 255.0;
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    float2 uv = position.xy * pixel.xy;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 eye = float3((ndc.x + projection.x) / projection.y,
                        (ndc.y + projection.z) / projection.w, -1.0);
    eye = normalize(mul((float3x3)orientation, eye));
    float2 coords = float2(atan2_approx(eye.x, -eye.z), acos_approx(eye.y));
    if (coords.x < 0.0) {
        coords.x += kPi * 2.0;
    }
    coords /= float2(kPi * 2.0, kPi);
    // Sampled first, while control flow is uniform, as WGSL requires.
    float4 texel = sky_texture.SampleLevel(sky_sampler, coords, 0.0);

    uint kind = options.x;
    float3 color = (float3)0.0;
    if (kind == kSkyColor) {
        color = params[0].rgb;
    } else if (kind == kSkyProcedural) {
        color = procedural_sky(eye, texel);
    } else if (kind == kSkyPhysical) {
        color = physical_sky(eye, texel);
    } else if (kind == kSkyPanorama && (options.z & kSkyTexture) != 0u) {
        // With no texture, it is black, as Godot's hint_default_black.
        color = texel.rgb * params[0].x;
    }
    color *= pixel.z;
    color = srgb_to_linear(color);

    if (fog_color.w != 0.0) {
        float3 fogged = lerp(fog_color.rgb, color, fog.x);
        if (fog.y > 0.001) {
            uint count = options.y;
            for (uint i = 0u; i < count; ++i) {
                SkyLight light = lights[i];
                float3 light_color =
                    srgb_to_linear(light.color_size.rgb) * light.direction_energy.w;
                float amount = pow(max(dot(eye, light.direction_energy.xyz), 0.0), 8.0) * kPi;
                fogged += light_color * amount * fog.y;
            }
        }
        color = lerp(color, fogged, fog.z);
    }

    if (tonemap.w == 2.0) {
        return float4(color, 1.0);
    }
    color *= tonemap.x;
    if (tonemap.w != 0.0) {
        color = apply_tonemapping(color, tonemap.y, (uint)tonemap.z);
    }
    color = linear_to_srgb(color);
    if ((options.z & kSkyDeband) != 0u) {
        color += interleaved_gradient_noise(position.xy) * pixel.z;
    }
    return float4(color, 1.0);
}
