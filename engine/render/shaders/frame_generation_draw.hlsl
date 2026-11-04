// Draws a generated frame (frame_generation.hlsl's output, a uint of RGBA bytes a pixel) into a
// render target of its size: one triangle over the target.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

cbuffer Generated : register(b0, space3) {
    uint4 frame; // the width in pixels and where the frame starts in the words
};

// Metal numbers storage buffers after the uniforms, and WebGPU's go in a bind group of their own.
#if defined(SLASH_MSL)
StructuredBuffer<uint> words : register(t1, space2);
#elif defined(SLASH_WGSL)
StructuredBuffer<uint> words : register(t0, space0);
#else
StructuredBuffer<uint> words : register(t0, space2);
#endif

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    uint2 pos = (uint2)position.xy;
    uint packed = words[frame.y + pos.y * frame.x + pos.x];
    return float4((float)(packed & 0xFFu), (float)((packed >> 8u) & 0xFFu),
                  (float)((packed >> 16u) & 0xFFu), (float)(packed >> 24u)) / 255.0;
}
