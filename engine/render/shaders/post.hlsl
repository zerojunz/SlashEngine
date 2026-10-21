// Post-processing for a frame's 3D world, as Godot's Compatibility renderer does it
// (drivers/gles3/shaders/effects/glow.glsl and post.glsl): the meshes draw untonemapped into a
// texture, glow's filter pass keeps that texture's bright parts at half its size, four textures
// each half the size of the last blur them down and up again (dual filtering, after Marius
// Bjørge's "Bandwidth-Efficient Rendering", SIGGRAPH 2015), and the post pass blends the glow in,
// tonemaps, adjusts and corrects colors as it draws over the frame's target. Every pass draws one
// triangle over its whole target. Keep it in step with src/post.cpp, its CPU twin.

#include "tonemap.hlsl"

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

Texture2D<float4> source : register(t0, space2);
Texture2D<float4> glow_texture : register(t1, space2);
Texture2D<float4> lut : register(t2, space2);
SamplerState source_sampler : register(s0, space2);
SamplerState glow_sampler : register(s1, space2);
SamplerState lut_sampler : register(s2, space2);

static const uint kGlowFilter = 0u;
static const uint kGlowDownsample = 1u;
static const uint kGlowUpsample = 2u;

static const uint kCorrection1D = 1u;
static const uint kCorrection3D = 2u;

cbuffer Post : register(b0, space3) {
    // xy: one over the target's size, which places each pixel's texture coordinate; zw: the
    // pixel size the pass's taps are spaced by, Godot's pixel_size.
    float4 pixel;
    float4 glow;    // intensity, bloom, HDR threshold, HDR scale
    float4 grade;   // white, the luminance cap, the 3D LUT's size
    float4 adjust;  // brightness, contrast, saturation
    // The pass (the glow passes, or 3 for the post pass), the tonemapper, then 1 to blend glow
    // in and 1 to adjust, and the color correction: 0, kCorrection1D or kCorrection3D, by 4.
    uint4 options;
};

float4 tap(Texture2D<float4> image, SamplerState state, float2 uv) {
    return image.SampleLevel(state, uv, 0.0);
}

// Five taps, the middle one weighted four, as the filter and downsample passes take them.
float4 five_taps(float2 uv, float2 half_pixel) {
    float4 color = tap(source, source_sampler, uv) * 4.0;
    color += tap(source, source_sampler, uv - half_pixel);
    color += tap(source, source_sampler, uv + half_pixel);
    color += tap(source, source_sampler, uv - float2(half_pixel.x, -half_pixel.y));
    color += tap(source, source_sampler, uv + float2(half_pixel.x, -half_pixel.y));
    return color;
}

// Eight taps around a diamond, its sides' middles weighted two, as the upsample pass and the post
// pass's glow take them.
float4 eight_taps(Texture2D<float4> image, SamplerState state, float2 uv, float2 half_pixel) {
    float4 color = tap(image, state, uv + float2(-half_pixel.x * 2.0, 0.0));
    color += tap(image, state, uv + float2(-half_pixel.x, half_pixel.y)) * 2.0;
    color += tap(image, state, uv + float2(0.0, half_pixel.y * 2.0));
    color += tap(image, state, uv + float2(half_pixel.x, half_pixel.y)) * 2.0;
    color += tap(image, state, uv + float2(half_pixel.x * 2.0, 0.0));
    color += tap(image, state, uv + float2(half_pixel.x, -half_pixel.y)) * 2.0;
    color += tap(image, state, uv + float2(0.0, -half_pixel.y * 2.0));
    color += tap(image, state, uv + float2(-half_pixel.x, -half_pixel.y)) * 2.0;
    return color / 12.0;
}

// A 3D LUT laid out as size slices of size x size texels side by side, sampled as a 3D texture
// with linear filtering and clamped edges samples it.
float3 lut_3d(float3 color) {
    float size = grade.z;
    float3 texel = clamp(color * size - 0.5, 0.0, size - 1.0);
    float slice = min(floor(texel.z), size - 2.0);
    float blend = texel.z - slice;
    if (size < 2.0) {
        slice = 0.0;
        blend = 0.0;
    }
    float2 within = float2(texel.x + 0.5, texel.y + 0.5);
    float2 width = float2(size * size, size);
    float3 low = tap(lut, lut_sampler, (within + float2(slice * size, 0.0)) / width).rgb;
    float3 high = tap(lut, lut_sampler, (within + float2((slice + 1.0) * size, 0.0)) / width).rgb;
    return lerp(low, high, blend);
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    float2 uv = position.xy * pixel.xy;
    float2 half_pixel = pixel.zw * 0.5;
    uint pass_kind = options.x;
    if (pass_kind == kGlowFilter) {
        // The source is twice the target's size, so each tap averages four of its pixels.
        float3 color = five_taps(uv, half_pixel).rgb / 8.0;
        float brightest = max(color.r, max(color.g, color.b));
        float feedback =
            max(smoothstep(glow.z, glow.z + glow.w, brightest), glow.y);
        return float4(min(color * feedback, (float3)grade.y), 1.0);
    }
    if (pass_kind == kGlowDownsample) {
        return five_taps(uv, half_pixel) / 8.0;
    }
    if (pass_kind == kGlowUpsample) {
        return eight_taps(source, source_sampler, uv, half_pixel);
    }

    float4 color = tap(source, source_sampler, uv);
    if ((options.z & 1u) != 0u) {
        // Screen blending, which Godot's Compatibility renderer always uses.
        float3 bloom = saturate(eight_taps(glow_texture, glow_sampler, uv, half_pixel).rgb * glow.x);
        color.rgb = max(color.rgb + bloom - color.rgb * bloom, 0.0);
    }
    color.rgb = srgb_to_linear(color.rgb);
    color.rgb = apply_tonemapping(color.rgb, grade.x, options.y);
    color.rgb = linear_to_srgb(color.rgb);
    if ((options.z & 2u) != 0u) {
        color.rgb = lerp((float3)0.0, color.rgb, adjust.x);
        color.rgb = lerp((float3)0.5, color.rgb, adjust.y);
        color.rgb = lerp((float3)(dot((float3)1.0, color.rgb) * 0.33333), color.rgb, adjust.z);
        uint correction = options.z >> 2u;
        if (correction == kCorrection1D) {
            color.r = tap(lut, lut_sampler, float2(color.r, 0.5)).r;
            color.g = tap(lut, lut_sampler, float2(color.g, 0.5)).g;
            color.b = tap(lut, lut_sampler, float2(color.b, 0.5)).b;
        } else if (correction == kCorrection3D) {
            color.rgb = lut_3d(color.rgb);
        }
    }
    return color;
}
