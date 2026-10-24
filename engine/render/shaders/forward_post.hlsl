// Post-processing for a frame's 3D world, as Godot's Forward+ renderer does it
// (servers/rendering/renderer_rd/shaders/effects/copy.glsl's gaussian glow, luminance_reduce.glsl
// and tonemap.glsl): the sky and meshes draw linear into a texture of half floats; with auto
// exposure, the luminance passes average its brightness down to one texel; glow's passes blur its
// bright parts into up to seven textures, the first half the target's size and each next one half
// the last; and the tonemap pass exposes the colors, blends the glow in, tonemaps, adjusts and
// corrects them as it draws over the frame's target. Every pass draws one triangle over its whole
// target. Keep it in step with src/forward_post.cpp, its CPU twin.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// The source: the world's texture, or the glow level before.
Texture2D<float4> source : register(t0, space2);
Texture2D<float4> glow0 : register(t1, space2);
Texture2D<float4> glow1 : register(t2, space2);
Texture2D<float4> glow2 : register(t3, space2);
Texture2D<float4> glow3 : register(t4, space2);
Texture2D<float4> glow4 : register(t5, space2);
Texture2D<float4> glow5 : register(t6, space2);
Texture2D<float4> glow6 : register(t7, space2);
Texture2D<float4> lut : register(t8, space2);
// The measured brightness, in one texel of floats: the last frame's in the last luminance pass.
// It and the luminance texture a luminance pass reads are only loaded, never filtered, since
// WebGPU filters no floats.
Texture2D<float4> brightness : register(t9, space2);
Texture2D<float4> luminance_source : register(t10, space2);
SamplerState source_sampler : register(s0, space2);
SamplerState glow0_sampler : register(s1, space2);
SamplerState glow1_sampler : register(s2, space2);
SamplerState glow2_sampler : register(s3, space2);
SamplerState glow3_sampler : register(s4, space2);
SamplerState glow4_sampler : register(s5, space2);
SamplerState glow5_sampler : register(s6, space2);
SamplerState glow6_sampler : register(s7, space2);
SamplerState lut_sampler : register(s8, space2);
SamplerState brightness_sampler : register(s9, space2);
SamplerState luminance_source_sampler : register(s10, space2);

static const uint kGlowFirst = 0u;
static const uint kGlowNext = 1u;
static const uint kLuminanceFirst = 3u;
static const uint kLuminance = 4u;
static const uint kLuminanceFinal = 5u;

static const uint kUseGlow = 1u;
static const uint kUseAdjustments = 2u;
static const uint kUseAutoExposure = 16u;
static const float kMinLuminance = 0.0001;
static const uint kCorrection1D = 1u;
static const uint kCorrection3D = 2u;

static const uint kTonemapLinear = 0u;
static const uint kTonemapReinhard = 1u;
static const uint kTonemapFilmic = 2u;
static const uint kTonemapAces = 3u;

static const uint kGlowAdditive = 0u;
static const uint kGlowScreen = 1u;
static const uint kGlowSoftlight = 2u;
static const uint kGlowReplace = 3u;
static const uint kGlowMix = 4u;

cbuffer ForwardPost : register(b0, space3) {
    float4 target;      // the pass's target's width and height, then one over them
    float4 glow;        // strength, bloom, HDR threshold, HDR scale
    float4 glow_first;  // the first pass's exposure and luminance cap; level 0's width and height
    float4 levels_low;  // levels 0 to 3's weights, 0 for those not added
    float4 levels_high; // levels 4 to 6's weights, then the intensity (glow_mix for the mix mode)
    float4 tonemap;     // exposure, white, the 3D LUT's size
    float4 tonemapper;  // the tonemapper's parameters, as Godot's tonemapper_params
    float4 adjust;      // brightness, contrast, saturation
    // Auto exposure's scale, the brightness's least and most, and how far it moves this frame
    float4 exposure;
    float4 source_size; // a luminance pass's source's width and height
    // The pass (the glow passes, 2 for the tonemap pass, or the luminance passes), the
    // tonemapper, 1 to blend glow in | 2 to adjust | the color correction (0, kCorrection1D or
    // kCorrection3D) times 4 | kUseAutoExposure, and the glow blend mode.
    uint4 options;
};

// The measured brightness over auto exposure's scale, which the colors are divided by.
float exposure_divisor() {
    return max(brightness.Load(int3(0, 0, 0)).r, kMinLuminance) / exposure.x;
}

// --- Auto exposure's luminance, as luminance_reduce.glsl ---

// A pixel's 8 x 8 block of its source averaged, summed in the work group's order: each of the
// first half of the block's 64 values adds the one half a block on, then the first quarter the
// one a quarter on, down to one. The first pass takes the world's brightest channel, and the
// rest the luminance texture before. Texels past the source's edge count 0, and the sum is divided
// by those inside. The last pass moves from the last frame's brightness by exposure.w and keeps
// it from exposure.y to exposure.z.
float luminance_pass(int2 pixel, uint kind) {
    float block[64];
    int2 origin = pixel * 8;
    int2 size = (int2)source_size.xy;
    for (int t = 0; t < 64; ++t) {
        int2 at = origin + int2(t & 7, t >> 3);
        float value = 0.0;
        if (at.x < size.x && at.y < size.y && kind == kLuminanceFirst) {
            float4 texel = source.Load(int3(at, 0));
            value = max(texel.r, max(texel.g, texel.b));
        } else if (at.x < size.x && at.y < size.y) {
            value = luminance_source.Load(int3(at, 0)).r;
        }
        block[t] = value;
    }
    for (int half_block = 32; half_block >= 1; half_block >>= 1) {
        for (int i = 0; i < half_block; ++i) {
            block[i] += block[i + half_block];
        }
    }
    int2 inside = min(size - origin, int2(8, 8));
    float average = block[0] / (float)(inside.x * inside.y);
    if (kind == kLuminanceFinal) {
        float previous = brightness.Load(int3(0, 0, 0)).r;
        average = min(max(previous + (average - previous) * exposure.w, exposure.y), exposure.z);
    }
    return average;
}

// --- Glow, as copy.glsl's MODE_GAUSSIAN_GLOW ---

// Where along one axis a pixel's blur samples the source, in pixels of the target, which has size
// pixels: the middle of an even pixel, kept from 0.5 to size - 1.5, and an odd pixel one past the
// even one before it, as copy.glsl fills its cache of 16 x 16 samples.
float glow_place(int n, float size) {
    bool odd = (n & 1) != 0;
    float even = (float)(odd ? n - 1 : n);
    return min(max(even + 0.5, 0.5), size - 1.5) + (odd ? 1.0 : 0.0);
}

// The weights of the 9-tap Gaussian kernel, from the middle out.
float glow_kernel(int offset) {
    int i = abs(offset);
    return i == 0 ? 0.2024 : (i == 1 ? 0.1790 : (i == 2 ? 0.1240 : (i == 3 ? 0.0672 : 0.0285)));
}

// A sample of the source, for the target's pixel (x, y); the first pass tonemaps it so a few very
// bright pixels don't swamp the blur (Karis' average).
float4 glow_sample(int x, int y, bool first) {
    float2 place = float2(glow_place(x, target.x), glow_place(y, target.y));
    float4 color = source.SampleLevel(source_sampler, place * target.zw, 0.0);
    if (first) {
        color /= 1.0 + dot(color.rgb, float3(0.299, 0.587, 0.114) / max(glow_first.y, 6.0));
    }
    return color;
}

// Taps are summed in copy.glsl's order: the middle, those after it, those before it, then the
// outermost two.
static const int kGlowTaps[9] = {0, 1, 2, 3, -1, -2, -3, 4, -4};

float4 glow_pass(int2 pixel, bool first) {
    float4 color = 0.0;
    for (int j = 0; j < 9; ++j) {
        int dy = kGlowTaps[j];
        float4 row = 0.0;
        for (int i = 0; i < 9; ++i) {
            int dx = kGlowTaps[i];
            row += glow_sample(pixel.x + dx, pixel.y + dy, first) * glow_kernel(dx);
        }
        color += row * glow_kernel(dy);
    }
    if (first) {
        color /= 1.0 - dot(color.rgb, float3(0.299, 0.587, 0.114) / max(glow_first.y, 6.0));
    }
    color *= glow.x;
    if (first && (options.z & kUseAutoExposure) != 0u) {
        color /= exposure_divisor();
    }
    if (first) {
        color *= glow_first.x;
        float luminance = max(color.r, max(color.g, color.b));
        float feedback = max(smoothstep(glow.z, glow.z + glow.w, luminance), glow.y);
        color = min(color * feedback, (float4)glow_first.y);
    }
    return color;
}

// --- The tonemap pass, as tonemap.glsl ---

float3 tonemap_reinhard(float3 color) {
    float white_squared = tonemapper.x;
    float3 white_squared_color = white_squared * color;
    return (white_squared_color + color * color) / (white_squared_color + white_squared);
}

float3 tonemap_filmic(float3 color) {
    const float A = 0.22 * 2.0 * 2.0;
    const float B = 0.30 * 2.0;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.01;
    const float F = 0.30;
    float3 mapped =
        ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
    return mapped / tonemapper.x;
}

// From https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl (MIT License), as
// Godot adapts it. GLSL's mat3 takes columns, so these rows are its columns and its color *= m is
// mul(m, color).
float3 tonemap_aces(float3 color) {
    const float bias = 1.8;
    const float A = 0.0245786;
    const float B = 0.000090537;
    const float C = 0.983729;
    const float D = 0.432951;
    const float E = 0.238081;
    const float3x3 rgb_to_rrt = float3x3(
        float3(0.59719 * bias, 0.35458 * bias, 0.04823 * bias),
        float3(0.07600 * bias, 0.90834 * bias, 0.01566 * bias),
        float3(0.02840 * bias, 0.13383 * bias, 0.83777 * bias));
    const float3x3 odt_to_rgb = float3x3(float3(1.60475, -0.53108, -0.07367),
                                         float3(-0.10208, 1.10813, -0.00605),
                                         float3(-0.00327, -0.07276, 1.07602));
    color = mul(rgb_to_rrt, color);
    float3 mapped = (color * (color + A) - B) / (color * (C * color + D) + E);
    mapped = mul(odt_to_rgb, mapped);
    return mapped / tonemapper.x;
}

// allenwp's tonemapping curve (https://allenwp.com/blog/2025/05/29/allenwp-tonemapping-curve/),
// for non-negative linear values: a Reinhard-like shoulder above 18% gray and a power toe below.
float3 allenwp_curve(float3 x) {
    const float crossover = 0.18;
    const float shoulder_max = 1.0 - crossover;
    float3 s = x - crossover;
    float3 slope_s = tonemapper.z * s;
    s = slope_s * (1.0 + s / tonemapper.w) / (1.0 + (slope_s / shoulder_max));
    s += crossover;
    float3 t = pow(x, tonemapper.x);
    t = t / (t + tonemapper.y);
    return float3(x.r < crossover ? t.r : s.r, x.g < crossover ? t.g : s.g,
                  x.b < crossover ? t.b : s.b);
}

// Godot 4.6's simplification of EaryChow's AgX, as Blender uses it
// (https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBasesRGB.py): Rec. 709 to Rec. 2020 with
// AgX's inset, allenwp's curve, then the outset and back. These rows are GLSL's columns, so its
// m * color is mul(color, m).
float3 tonemap_agx(float3 color) {
    const float3x3 inset = float3x3(
        float3(0.544814746488245, 0.140416948464053, 0.0888104196149096),
        float3(0.373787398372697, 0.754137554567394, 0.178871756420858),
        float3(0.0813978551390581, 0.105445496968552, 0.732317823964232));
    const float3x3 outset = float3x3(
        float3(1.96488741169489, -0.299313364904742, -0.164352742528393),
        float3(-0.855988495690215, 1.32639796461980, -0.238183969428088),
        float3(-0.108898916004672, -0.0270845997150571, 1.40253671195648));
    color = mul(color, inset);
    color = allenwp_curve(color);
    color = min((float3)1.0, color);
    return mul(color, outset);
}

float3 apply_tonemapping(float3 color, uint kind) {
    if (kind == kTonemapLinear) {
        return color;
    }
    color = max((float3)0.0, color);
    if (kind == kTonemapReinhard) {
        return tonemap_reinhard(color);
    }
    if (kind == kTonemapFilmic) {
        return tonemap_filmic(color);
    }
    if (kind == kTonemapAces) {
        return tonemap_aces(color);
    }
    return tonemap_agx(color);
}

// The exact sRGB curves, as tonemap.glsl has them.
float3 linear_to_srgb(float3 color) {
    float3 curve = 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055;
    float3 toe = 12.92 * color;
    return float3(color.r < 0.0031308 ? toe.r : curve.r, color.g < 0.0031308 ? toe.g : curve.g,
                  color.b < 0.0031308 ? toe.b : curve.b);
}

// The cubic B-spline's weights and offsets, as tonemap.glsl's bicubic glow filter.
float w0(float a) {
    return (1.0 / 6.0) * (a * (a * (-a + 3.0) - 3.0) + 1.0);
}
float w1(float a) {
    return (1.0 / 6.0) * (a * a * (3.0 * a - 6.0) + 4.0);
}
float w2(float a) {
    return (1.0 / 6.0) * (a * (a * (-3.0 * a + 3.0) + 3.0) + 1.0);
}
float w3(float a) {
    return (1.0 / 6.0) * (a * a * a);
}
float g0(float a) {
    return w0(a) + w1(a);
}
float g1(float a) {
    return w2(a) + w3(a);
}
float h0(float a) {
    return -1.0 + w1(a) / (w0(a) + w1(a));
}
float h1(float a) {
    return 1.0 + w3(a) / (w2(a) + w3(a));
}

// A level of size texels sampled bicubically, in four bilinear taps.
float3 bicubic(Texture2D<float4> image, SamplerState state, float2 uv, float2 size) {
    float2 pixel_size = 1.0 / size;
    uv = uv * size + 0.5;
    float2 iuv = floor(uv);
    float2 fuv = frac(uv);
    float g0x = g0(fuv.x);
    float g1x = g1(fuv.x);
    float h0x = h0(fuv.x);
    float h1x = h1(fuv.x);
    float h0y = h0(fuv.y);
    float h1y = h1(fuv.y);
    float2 p0 = (float2(iuv.x + h0x, iuv.y + h0y) - 0.5) * pixel_size;
    float2 p1 = (float2(iuv.x + h1x, iuv.y + h0y) - 0.5) * pixel_size;
    float2 p2 = (float2(iuv.x + h0x, iuv.y + h1y) - 0.5) * pixel_size;
    float2 p3 = (float2(iuv.x + h1x, iuv.y + h1y) - 0.5) * pixel_size;
    return ((g0(fuv.y) * (g0x * image.SampleLevel(state, p0, 0.0) +
                          g1x * image.SampleLevel(state, p1, 0.0))) +
            (g1(fuv.y) * (g0x * image.SampleLevel(state, p2, 0.0) +
                          g1x * image.SampleLevel(state, p3, 0.0))))
        .rgb;
}

// Level i's size: level 0's halved i times, at least 1.
float2 level_size(uint i) {
    return max(floor(glow_first.zw / (float)(1u << i)), 1.0);
}

// Every level glow adds, each by its weight.
float3 gather_glow(float2 uv) {
    float3 color = 0.0;
    if (levels_low.x > 0.0) {
        color += bicubic(glow0, glow0_sampler, uv, level_size(0u)) * levels_low.x;
    }
    if (levels_low.y > 0.0) {
        color += bicubic(glow1, glow1_sampler, uv, level_size(1u)) * levels_low.y;
    }
    if (levels_low.z > 0.0) {
        color += bicubic(glow2, glow2_sampler, uv, level_size(2u)) * levels_low.z;
    }
    if (levels_low.w > 0.0) {
        color += bicubic(glow3, glow3_sampler, uv, level_size(3u)) * levels_low.w;
    }
    if (levels_high.x > 0.0) {
        color += bicubic(glow4, glow4_sampler, uv, level_size(4u)) * levels_high.x;
    }
    if (levels_high.y > 0.0) {
        color += bicubic(glow5, glow5_sampler, uv, level_size(5u)) * levels_high.y;
    }
    if (levels_high.z > 0.0) {
        color += bicubic(glow6, glow6_sampler, uv, level_size(6u)) * levels_high.z;
    }
    return color;
}

// Glow over a color by every blend mode but mix, as tonemap.glsl's apply_glow.
float3 apply_glow(float3 color, float3 bloom, float white, uint mode) {
    if (mode == kGlowAdditive) {
        return color + bloom;
    }
    if (mode == kGlowScreen) {
        bloom = clamp(bloom, 0.0, white);
        return color + bloom - (color * bloom / white);
    }
    if (mode == kGlowSoftlight) {
        bloom = clamp(bloom, 0.0, 1.0);
        float3 lifted = float3(
            color.r <= 0.25 ? ((16.0 * color.r - 12.0) * color.r + 4.0) * color.r : sqrt(color.r),
            color.g <= 0.25 ? ((16.0 * color.g - 12.0) * color.g + 4.0) * color.g : sqrt(color.g),
            color.b <= 0.25 ? ((16.0 * color.b - 12.0) * color.b + 4.0) * color.b
                            : sqrt(color.b));
        float3 soft = color + bloom * (lifted - color);
        return float3(color.r > 1.0 ? color.r : soft.r, color.g > 1.0 ? color.g : soft.g,
                      color.b > 1.0 ? color.b : soft.b);
    }
    return bloom;
}

// A 3D LUT laid out as size slices of size x size texels side by side, sampled as a 3D texture
// with linear filtering and clamped edges samples it.
float3 lut_3d(float3 color) {
    float size = tonemap.z;
    float3 texel = clamp(color * size - 0.5, 0.0, size - 1.0);
    float slice = min(floor(texel.z), size - 2.0);
    float blend = texel.z - slice;
    if (size < 2.0) {
        slice = 0.0;
        blend = 0.0;
    }
    float2 within = float2(texel.x + 0.5, texel.y + 0.5);
    float2 width = float2(size * size, size);
    float3 low = lut.SampleLevel(lut_sampler, (within + float2(slice * size, 0.0)) / width, 0.0).rgb;
    float3 high =
        lut.SampleLevel(lut_sampler, (within + float2((slice + 1.0) * size, 0.0)) / width, 0.0)
            .rgb;
    return lerp(low, high, blend);
}

float4 tonemap_pass(float4 position) {
    float2 uv = position.xy * target.zw;
    float4 color = source.Load(int3((int2)position.xy, 0));
    uint flags = options.z;
    float exposed = tonemap.x;
    if ((flags & kUseAutoExposure) != 0u) {
        exposed *= 1.0 / exposure_divisor();
    }
    color.rgb *= exposed;
    uint mode = options.w;
    bool glowing = (flags & kUseGlow) != 0u;
    float intensity = levels_high.w;
    float3 bloom = 0.0;
    if (glowing) {
        bloom = gather_glow(uv) * intensity;
    }
    if (glowing && mode != kGlowSoftlight) {
        if (mode == kGlowMix) {
            color.rgb = color.rgb * (1.0 - intensity) + bloom;
        } else {
            color.rgb = apply_glow(color.rgb, bloom, tonemap.y, mode);
        }
    }
    color.rgb = apply_tonemapping(color.rgb, options.y);
    if (glowing && mode == kGlowSoftlight) {
        // After tonemapping, to a tonemapped glow, so colors past 1 don't show its seam.
        color.rgb = apply_glow(color.rgb, apply_tonemapping(bloom, options.y), tonemap.y, mode);
    }
    if ((flags & kUseAdjustments) != 0u) {
        // Brightness on linear values, contrast and saturation on sRGB ones.
        color.rgb *= adjust.x;
        color.rgb = linear_to_srgb(color.rgb);
        color.rgb = lerp((float3)0.5, color.rgb, adjust.y);
        color.rgb = lerp((float3)(dot((float3)1.0, color.rgb) * (1.0 / 3.0)), color.rgb, adjust.z);
        uint correction = (flags >> 2u) & 3u;
        if (correction != 0u) {
            color.rgb = clamp(color.rgb, 0.0, 1.0);
        }
        if (correction == kCorrection1D) {
            color.r = lut.SampleLevel(lut_sampler, float2(color.r, 0.5), 0.0).r;
            color.g = lut.SampleLevel(lut_sampler, float2(color.g, 0.5), 0.0).g;
            color.b = lut.SampleLevel(lut_sampler, float2(color.b, 0.5), 0.0).b;
        } else if (correction == kCorrection3D) {
            color.rgb = lut_3d(color.rgb);
        }
    } else {
        color.rgb = linear_to_srgb(color.rgb);
    }
    return color;
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    uint pass_kind = options.x;
    if (pass_kind == kGlowFirst || pass_kind == kGlowNext) {
        return glow_pass((int2)position.xy, pass_kind == kGlowFirst);
    }
    if (pass_kind >= kLuminanceFirst) {
        return (float4)luminance_pass((int2)position.xy, pass_kind);
    }
    return tonemap_pass(position);
}
