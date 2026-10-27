// The sky seen through volumetric fog, as Godot 4.6's sky shader takes the fog's whole length
// (sky.glsl's volumetric fog, mixed in by its sky_affect): one triangle over the target, drawn
// after the sky and before the meshes, blended as ONE, SRC_ALPHA, so that a pixel becomes
// lerp(sky, sky * a + rgb, affect) of the fog's last slice there. src/volumetric_fog.cpp's fog_sky
// is its CPU twin.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

cbuffer FogSky : register(b0, space3) {
    // 1 when the frame has volumetric fog, 1 over its length, 1 over its detail spread, its sky
    // affect (MeshSceneUniforms::volumetric_fog).
    float4 volumetric_fog;
    float4 volume; // the froxels across, down and deep
    float4 screen; // the target's width and height in pixels
};

// The fog volume (src/volumetric_fog.hpp), rows across, then down, then slices deeper. Metal numbers
// storage buffers after the uniforms, and WebGPU's go in a bind group of their own.
#if defined(SLASH_MSL)
StructuredBuffer<float4> fog_froxels : register(t1, space2);
#elif defined(SLASH_WGSL)
StructuredBuffer<float4> fog_froxels : register(t0, space0);
#else
StructuredBuffer<float4> fog_froxels : register(t0, space2);
#endif

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    // The last slice, read linearly across and down as a 3D texture is at a depth of 1.
    uint3 size = (uint3)volume.xyz;
    float2 n = (float2)size.xy;
    float2 p = clamp(position.xy / screen.xy * n - 0.5, 0.0, n - 1.0);
    float2 base = floor(p);
    uint2 low = (uint2)base;
    uint2 high = min(low + 1u, size.xy - 1u);
    float2 t = p - base;
    uint slice = (size.z - 1u) * size.y;
    float4 a = lerp(fog_froxels[(slice + low.y) * size.x + low.x],
                    fog_froxels[(slice + low.y) * size.x + high.x], t.x);
    float4 b = lerp(fog_froxels[(slice + high.y) * size.x + low.x],
                    fog_froxels[(slice + high.y) * size.x + high.x], t.x);
    float4 seen = lerp(a, b, t.y);
    float affect = volumetric_fog.w;
    return float4(seen.rgb * affect, 1.0 - affect + affect * seen.a);
}
