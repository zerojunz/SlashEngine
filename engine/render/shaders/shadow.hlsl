// Shadow maps: meshes drawn into a shadow atlas from a light, depth only, as Godot's Compatibility
// renderer draws them (drivers/gles3/shaders/scene.glsl with RENDER_SHADOWS). Depths are pushed
// away from the light by the pass's bias, as Godot pushes them. Alpha scissor materials cut their
// holes in the shadow too. Keep it in step with shadow_vertex and shadow_keeps in
// src/shadow.cpp, its CPU twins. The vertex input, Instance and Surface are mesh.hlsl's. With
// GAME_SHADER defined, as mesh.hlsl's, a game's spatial shader's fragment() cuts the holes, and
// with GAME_VERTEX its vertex() places the corners.

cbuffer LightView : register(b0, space1) {
    float4x4 view;       // the world to the pass's camera space
    float4x4 projection; // its camera space to clip space, with reversed depth
    // x: the bias; y: 1 pushes by the bias along the ray, as an omni light's linear depth; z: 1
    // clamps depths to the near plane, a directional light's pancake.
    float4 pass_options;
};

cbuffer Instance : register(b1, space1) {
    float4x4 model;
    float4x4 normal_model;
    float4 uv1;
    float4 lightmap_uv;
};

struct VertexInput {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : TEXCOORD4;
    float2 uv2 : TEXCOORD5;
};

#ifdef GAME_SHADER
// A game's spatial shader's fragment() cuts its holes, as mesh.hlsl's game_surface reads it.
struct VertexOutput {
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float3 binormal : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float4 color : TEXCOORD5;
    float2 uv2 : TEXCOORD6;
    float4 position : SV_Position;
};
#else
struct VertexOutput {
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    float3 object : TEXCOORD2; // the place on the mesh, for alpha hash
    float4 position : SV_Position;
};
#endif

VertexOutput vs_main(VertexInput input) {
    VertexOutput output;
#ifdef GAME_VERTEX
    GameVertex moved = game_vertex(input.position, input.normal, input.tangent, input.uv,
                                   input.uv2, input.color, false);
    float4 world = float4(moved.world, 1.0);
#else
    float4 world = mul(model, float4(input.position, 1.0));
#endif
    float3 v = mul(view, world).xyz;
    if (pass_options.y != 0.0) {
        v += v * (pass_options.x / max(length(v), 1e-6));
    } else {
        // Godot's normalize(v) * bias / length(v).
        v += v * (pass_options.x / max(dot(v, v), 1e-12));
    }
    output.position = mul(projection, float4(v, 1.0));
    if (pass_options.z != 0.0) {
        output.position.z = min(output.position.z, output.position.w * 0.9999);
    }
#if defined(GAME_VERTEX)
    output.world = world.xyz;
    output.normal = normalize(moved.normal);
    output.tangent = normalize(moved.tangent);
    output.binormal = normalize(moved.binormal);
    output.uv = moved.uv;
    output.color = moved.color;
    output.uv2 = moved.uv2 * lightmap_uv.zw + lightmap_uv.xy;
#elif defined(GAME_SHADER)
    float3 binormal = normalize(cross(input.normal, input.tangent.xyz) * input.tangent.w);
    output.world = world.xyz;
    output.normal = normalize(mul((float3x3)normal_model, input.normal));
    output.tangent = normalize(mul((float3x3)normal_model, input.tangent.xyz));
    output.binormal = normalize(mul((float3x3)normal_model, binormal));
    output.uv = input.uv * uv1.xy + uv1.zw;
    output.color = input.color;
    output.uv2 = input.uv2 * lightmap_uv.zw + lightmap_uv.xy;
#else
    output.uv = input.uv * uv1.xy + uv1.zw;
    output.color = input.color;
    output.object = input.position;
#endif
    return output;
}

#ifndef GAME_SHADER
Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
#endif

static const uint kVertexColorAlbedo = 2u;
static const uint kAlphaScissor = 4u;
static const uint kAlphaHash = 1024u;

cbuffer Surface : register(b0, space3) {
    float4 albedo;
    float4 emission;
    float4 surface;
    float4 metallic_channel;
    float4 roughness_channel;
    uint4 options; // x: mesh.hlsl's flags, y: the alpha scissor threshold's bits
};

#ifdef GAME_SHADER
void fs_main(VertexOutput input, bool front : SV_IsFrontFace) {
    // A shader that writes ALPHA_SCISSOR_THRESHOLD cuts its holes; one that only writes ALPHA
    // casts its whole shape, as Godot's transparent materials do.
    if ((options.x & kAlphaScissor) != 0u) {
        GameSurface game =
            game_surface(input.world, input.normal, input.tangent, input.binormal, input.uv,
                         input.uv2, input.color, float4(input.position.xyz, 1.0), front, false);
        if (game.albedo.a < game.normal.w) {
            discard;
        }
    }
}
#else
// mesh.hlsl's alpha hash, which cuts the same dither out of the shadow, seen from the light.
float alpha_hash_value(float3 cell) {
    uint3 v = uint3(int3(clamp(cell, -1073741824.0, 1073741824.0))) * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return float(v.x >> 8u) * (1.0 / 16777216.0);
}

float alpha_hash_threshold(float3 pos, float3 dx, float3 dy, float hash_scale) {
    float delta_max = max(length(dx), length(dy));
    float level = clamp(log2(1.0 / (hash_scale * delta_max)), -64.0, 64.0);
    float low = floor(level);
    float lerp_factor = level - low;
    float high = lerp_factor > 0.0 ? low + 1.0 : low;
    float a_low = alpha_hash_value(floor(exp2(low) * pos));
    float a_high = alpha_hash_value(floor(exp2(high) * pos));
    float a_interp = (1.0 - lerp_factor) * a_low + lerp_factor * a_high;
    float min_lerp = min(lerp_factor, 1.0 - lerp_factor);
    float threshold;
    if (a_interp < 1.0 - min_lerp) {
        threshold = a_interp < min_lerp
                        ? a_interp * a_interp / (2.0 * min_lerp * (1.0 - min_lerp))
                        : (a_interp - 0.5 * min_lerp) / (1.0 - min_lerp);
    } else {
        threshold = 1.0 - ((1.0 - a_interp) * (1.0 - a_interp) / (2.0 * min_lerp * (1.0 - min_lerp)));
    }
    return clamp(threshold, 0.00001, 1.0);
}

void fs_main(VertexOutput input) {
    float4 texel = albedo_texture.Sample(albedo_sampler, input.uv);
    if ((options.x & kAlphaScissor) != 0u) {
        float alpha = albedo.a * texel.a;
        if ((options.x & kVertexColorAlbedo) != 0u) {
            alpha *= input.color.a;
        }
        float threshold = asfloat(options.y);
        if ((options.x & kAlphaHash) != 0u) {
            threshold = alpha_hash_threshold(input.object, ddx(input.object), ddy(input.object),
                                             threshold);
        }
        if (alpha < threshold) {
            discard;
        }
    }
}
#endif
