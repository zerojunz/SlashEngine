// Shadow maps: meshes drawn into a shadow atlas from a light, depth only, as Godot's Compatibility
// renderer draws them (drivers/gles3/shaders/scene.glsl with RENDER_SHADOWS). Depths are pushed
// away from the light by the pass's bias, as Godot pushes them. Alpha scissor materials cut their
// holes in the shadow too. Keep it in step with shadow_vertex and shadow_keeps in
// src/shadow.cpp, its CPU twins. The vertex input, Instance and Surface are mesh.hlsl's.

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
};

struct VertexInput {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : TEXCOORD4;
};

struct VertexOutput {
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 position : SV_Position;
};

VertexOutput vs_main(VertexInput input) {
    VertexOutput output;
    float4 world = mul(model, float4(input.position, 1.0));
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
    output.uv = input.uv * uv1.xy + uv1.zw;
    output.color = input.color;
    return output;
}

Texture2D<float4> albedo_texture : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);

static const uint kVertexColorAlbedo = 2u;
static const uint kAlphaScissor = 4u;

cbuffer Surface : register(b0, space3) {
    float4 albedo;
    float4 emission;
    float4 surface;
    float4 metallic_channel;
    float4 roughness_channel;
    uint4 options; // x: mesh.hlsl's flags, y: the alpha scissor threshold's bits
};

void fs_main(VertexOutput input) {
    float4 texel = albedo_texture.Sample(albedo_sampler, input.uv);
    if ((options.x & kAlphaScissor) != 0u) {
        float alpha = albedo.a * texel.a;
        if ((options.x & kVertexColorAlbedo) != 0u) {
            alpha *= input.color.a;
        }
        if (alpha < asfloat(options.y)) {
            discard;
        }
    }
}
