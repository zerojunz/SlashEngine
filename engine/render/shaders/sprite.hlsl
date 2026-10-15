// The 2D shader: triangles with a position in pixels, a texture coordinate and a color that
// multiplies the texture. Registers follow SDL's GPU conventions: vertex uniforms in space1,
// fragment textures and samplers in space2.

cbuffer View : register(b0, space1) {
    // Maps positions to clip space; column-major, so it multiplies column vectors.
    float4x4 view_projection;
};

struct VertexInput {
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VertexOutput {
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 position : SV_Position;
};

VertexOutput vs_main(VertexInput input) {
    VertexOutput output;
    output.position = mul(view_projection, float4(input.position, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

Texture2D<float4> sprite_texture : register(t0, space2);
SamplerState sprite_sampler : register(s0, space2);

float4 fs_main(VertexOutput input) : SV_Target0 {
    return sprite_texture.Sample(sprite_sampler, input.uv) * input.color;
}
