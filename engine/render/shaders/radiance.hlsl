// The sky's radiance, filtered as Godot's Compatibility renderer filters it
// (drivers/gles3/shaders/effects/cubemap_filter.glsl): each level of the radiance cube from the
// raw cube the sky is drawn into, one face at a time. Level 0 is a copy of the raw cube's; each
// next one blurs it for a rougher surface, by GGX importance samples around the texel's
// direction, mixed in linear. Keep it in step with make_radiance in src/sky.cpp, its CPU twin.

#include "tonemap.hlsl"

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

TextureCube<float4> source_cube : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer Filter : register(b0, space3) {
    // The face, one over the level's size, the samples' weight, and how many samples (0 copies
    // the level the source starts at).
    float4 params;
    float4 samples[128]; // around +z: a direction, then the source level it is sampled at
};

// The direction a point of a face looks along, s and t from -1 to 1 across it: the table every
// API selects cube faces by (cube_direction in src/sky.cpp).
float3 face_direction(uint face, float2 st) {
    if (face == 0u) {
        return float3(1.0, -st.y, -st.x);
    }
    if (face == 1u) {
        return float3(-1.0, -st.y, st.x);
    }
    if (face == 2u) {
        return float3(st.x, 1.0, st.y);
    }
    if (face == 3u) {
        return float3(st.x, -1.0, -st.y);
    }
    if (face == 4u) {
        return float3(st.x, -st.y, 1.0);
    }
    return float3(-st.x, -st.y, -1.0);
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    float2 st = position.xy * params.y * 2.0 - 1.0;
    float3 n = normalize(face_direction((uint)params.x, st));
    uint count = (uint)params.w;
    if (count == 0u) {
        return float4(source_cube.SampleLevel(source_sampler, n, 0.0).rgb, 1.0);
    }
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);
    float3 sum = (float3)0.0;
    for (uint i = 0u; i < count; ++i) {
        float4 s = samples[i];
        float3 l = tangent * s.x + bitangent * s.y + n * s.z;
        sum += srgb_to_linear(source_cube.SampleLevel(source_sampler, l, s.w).rgb) * s.z;
    }
    return float4(linear_to_srgb(sum / params.z), 1.0);
}
