#include "forward_post.hpp"

#include "slash/core/bits.hpp"
#include "slash/core/math.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace slash::render::detail {

namespace {

// x to the power y for x of 0 or more, the same bits on every platform.
float power(float x, float y) {
    if (!(x > 0.0F)) {
        return 0.0F;
    }
    return slash::exp(y * slash::ln(x));
}

float smoothstep(float low, float high, float x) {
    const float t = std::clamp((x - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

// The 3x3 matrix whose rows are r, times the column c, as HLSL's mul(float3x3(r...), c).
Float3 rows_times(const std::array<Float3, 3>& r, const Float3& c) {
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = (r.at(i)[0] * c[0]) + (r.at(i)[1] * c[1]) + (r.at(i)[2] * c[2]);
    }
    return out;
}

// The row c times the 3x3 matrix whose rows are r, as HLSL's mul(c, float3x3(r...)).
Float3 times_rows(const Float3& c, const std::array<Float3, 3>& r) {
    Float3 out{};
    for (std::size_t j = 0; j < 3; ++j) {
        out.at(j) = (c[0] * r[0].at(j)) + (c[1] * r[1].at(j)) + (c[2] * r[2].at(j));
    }
    return out;
}

// Godot's environment_get_white for a texture of half floats: at least 1, and 2 for AgX; linear
// colors have no white but 1.
float forward_white(const PostProcess& post) {
    switch (post.tonemapper) {
    case Tonemapper::Linear:
        return 1.0F;
    case Tonemapper::Agx:
        return std::max(2.0F, post.white);
    case Tonemapper::Reinhard:
    case Tonemapper::Filmic:
    case Tonemapper::Aces:
        break;
    }
    return std::max(1.0F, post.white);
}

// Godot's environment_get_tonemap_parameters: what each tonemapper needs of white, worked out
// once for the frame.
Float4 tonemapper_parameters(const PostProcess& post, float white) {
    switch (post.tonemapper) {
    case Tonemapper::Linear:
        return {};
    case Tonemapper::Reinhard:
        return {white * white, 0.0F, 0.0F, 0.0F};
    case Tonemapper::Filmic: {
        constexpr float kA = 0.22F * 2.0F * 2.0F;
        constexpr float kB = 0.30F * 2.0F;
        constexpr float kC = 0.10F;
        constexpr float kD = 0.20F;
        constexpr float kE = 0.01F;
        constexpr float kF = 0.30F;
        return {(((white * ((kA * white) + (kC * kB))) + (kD * kE)) /
                 ((white * ((kA * white) + kB)) + (kD * kF))) -
                    (kE / kF),
                0.0F, 0.0F, 0.0F};
    }
    case Tonemapper::Aces: {
        constexpr float kA = 0.0245786F;
        constexpr float kB = 0.000090537F;
        constexpr float kC = 0.983729F;
        constexpr float kD = 0.432951F;
        constexpr float kE = 0.238081F;
        const float biased = white * 1.8F;
        return {((biased * (biased + kA)) - kB) / ((biased * ((kC * biased) + kD)) + kE), 0.0F,
                0.0F, 0.0F};
    }
    case Tonemapper::Agx:
        break;
    }
    // allenwp's curve (https://allenwp.com/blog/2025/05/29/allenwp-tonemapping-curve/): a toe that
    // meets the shoulder at 18% gray with the same slope, and a shoulder that reaches 1 at white.
    constexpr float kCrossover = 0.18F;
    constexpr float kShoulderMax = 1.0F - kCrossover;
    const float contrast = post.agx_contrast;
    const float toe_a = ((1.0F / kCrossover) - 1.0F) * power(kCrossover, contrast);
    const float slope_denominator = power(kCrossover, contrast) + toe_a;
    const float slope = (contrast * power(kCrossover, contrast - 1.0F) * toe_a) /
                        (slope_denominator * slope_denominator);
    const float w = (white - kCrossover) * (white - kCrossover) / kShoulderMax * slope;
    return {contrast, toe_a, slope, w};
}

Float3 tonemap_reinhard(const Float3& color, const Float4& parameters) {
    const float white_squared = parameters[0];
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float c = color.at(i);
        const float white_squared_color = white_squared * c;
        out.at(i) = (white_squared_color + (c * c)) / (white_squared_color + white_squared);
    }
    return out;
}

Float3 tonemap_filmic(const Float3& color, const Float4& parameters) {
    constexpr float kA = 0.22F * 2.0F * 2.0F;
    constexpr float kB = 0.30F * 2.0F;
    constexpr float kC = 0.10F;
    constexpr float kD = 0.20F;
    constexpr float kE = 0.01F;
    constexpr float kF = 0.30F;
    const auto curve = [](float x) {
        return (((x * ((kA * x) + (kC * kB))) + (kD * kE)) / ((x * ((kA * x) + kB)) + (kD * kF))) -
               (kE / kF);
    };
    return {curve(color[0]) / parameters[0], curve(color[1]) / parameters[0],
            curve(color[2]) / parameters[0]};
}

Float3 tonemap_aces(const Float3& color, const Float4& parameters) {
    constexpr float kBias = 1.8F;
    constexpr float kA = 0.0245786F;
    constexpr float kB = 0.000090537F;
    constexpr float kC = 0.983729F;
    constexpr float kD = 0.432951F;
    constexpr float kE = 0.238081F;
    const std::array<Float3, 3> rgb_to_rrt{
        {{0.59719F * kBias, 0.35458F * kBias, 0.04823F * kBias},
         {0.07600F * kBias, 0.90834F * kBias, 0.01566F * kBias},
         {0.02840F * kBias, 0.13383F * kBias, 0.83777F * kBias}}};
    const std::array<Float3, 3> odt_to_rgb{{{1.60475F, -0.53108F, -0.07367F},
                                            {-0.10208F, 1.10813F, -0.00605F},
                                            {-0.00327F, -0.07276F, 1.07602F}}};
    const auto curve = [](float x) { return ((x * (x + kA)) - kB) / ((x * ((kC * x) + kD)) + kE); };
    const Float3 rrt = rows_times(rgb_to_rrt, color);
    const Float3 mapped = rows_times(odt_to_rgb, {curve(rrt[0]), curve(rrt[1]), curve(rrt[2])});
    return {mapped[0] / parameters[0], mapped[1] / parameters[0], mapped[2] / parameters[0]};
}

float allenwp_curve(float x, const Float4& parameters) {
    constexpr float kCrossover = 0.18F;
    constexpr float kShoulderMax = 1.0F - kCrossover;
    if (x < kCrossover) {
        const float t = power(x, parameters[0]);
        return t / (t + parameters[1]);
    }
    const float s = x - kCrossover;
    const float slope_s = parameters[2] * s;
    return (slope_s * (1.0F + (s / parameters[3])) / (1.0F + (slope_s / kShoulderMax))) +
           kCrossover;
}

Float3 tonemap_agx(const Float3& color, const Float4& parameters) {
    const std::array<Float3, 3> inset{
        {{0.544814746488245F, 0.140416948464053F, 0.0888104196149096F},
         {0.373787398372697F, 0.754137554567394F, 0.178871756420858F},
         {0.0813978551390581F, 0.105445496968552F, 0.732317823964232F}}};
    const std::array<Float3, 3> outset{
        {{1.96488741169489F, -0.299313364904742F, -0.164352742528393F},
         {-0.855988495690215F, 1.32639796461980F, -0.238183969428088F},
         {-0.108898916004672F, -0.0270845997150571F, 1.40253671195648F}}};
    Float3 curved = times_rows(color, inset);
    for (float& value : curved) {
        value = std::min(1.0F, allenwp_curve(value, parameters));
    }
    return times_rows(curved, outset);
}

Float3 apply_tonemapping(const Float3& color, Tonemapper tonemapper, const Float4& parameters) {
    if (tonemapper == Tonemapper::Linear) {
        return color;
    }
    const Float3 positive{std::max(color[0], 0.0F), std::max(color[1], 0.0F),
                          std::max(color[2], 0.0F)};
    switch (tonemapper) {
    case Tonemapper::Reinhard:
        return tonemap_reinhard(positive, parameters);
    case Tonemapper::Filmic:
        return tonemap_filmic(positive, parameters);
    case Tonemapper::Aces:
        return tonemap_aces(positive, parameters);
    case Tonemapper::Linear:
    case Tonemapper::Agx:
        break;
    }
    return tonemap_agx(positive, parameters);
}

// The exact sRGB curves.
float linear_to_srgb_exact(float c) {
    if (c < 0.0031308F) {
        return 12.92F * c;
    }
    return (1.055F * power(c, 1.0F / 2.4F)) - 0.055F;
}

float srgb_to_linear_exact(float c) {
    if (c < 0.04045F) {
        return c * (1.0F / 12.92F);
    }
    return power((c + 0.055F) * (1.0F / 1.055F), 2.4F);
}

// A half-float texture sampled bilinearly with its edges clamped, its texel centers at half-texel
// coordinates, as on the GPU.
Float4 sample_hdr(const HdrImage& image, float u, float v) {
    const auto texel = [&](float x, float y) {
        const auto column =
            static_cast<std::size_t>(std::clamp(x, 0.0F, static_cast<float>(image.width - 1)));
        const auto row =
            static_cast<std::size_t>(std::clamp(y, 0.0F, static_cast<float>(image.height - 1)));
        const std::size_t at = ((row * image.width) + column) * 4;
        return Float4{image.pixels[at], image.pixels[at + 1], image.pixels[at + 2],
                      image.pixels[at + 3]};
    };
    const float x = (u * static_cast<float>(image.width)) - 0.5F;
    const float y = (v * static_cast<float>(image.height)) - 0.5F;
    const float x0 = std::floor(x);
    const float y0 = std::floor(y);
    const float tx = x - x0;
    const float ty = y - y0;
    const Float4 top_left = texel(x0, y0);
    const Float4 top_right = texel(x0 + 1.0F, y0);
    const Float4 bottom_left = texel(x0, y0 + 1.0F);
    const Float4 bottom_right = texel(x0 + 1.0F, y0 + 1.0F);
    Float4 color{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float top = top_left.at(i) + ((top_right.at(i) - top_left.at(i)) * tx);
        const float bottom = bottom_left.at(i) + ((bottom_right.at(i) - bottom_left.at(i)) * tx);
        color.at(i) = top + ((bottom - top) * ty);
    }
    return color;
}

// --- Glow, as copy.glsl's MODE_GAUSSIAN_GLOW ---

// Where along one axis glow samples the source for the target's pixel n, in pixels of the target,
// which has size pixels: as glow_place in forward_post.hlsl.
float glow_place(int n, float size) {
    const bool odd = (n & 1) != 0;
    const auto even = static_cast<float>(odd ? n - 1 : n);
    return std::min(std::max(even + 0.5F, 0.5F), size - 1.5F) + (odd ? 1.0F : 0.0F);
}

constexpr std::array<float, 5> kGlowKernel{0.2024F, 0.1790F, 0.1240F, 0.0672F, 0.0285F};
constexpr std::array<int, 9> kGlowTaps{0, 1, 2, 3, -1, -2, -3, 4, -4};
constexpr int kGlowReach = 4;
// The side of the blocks each luminance pass averages, as luminance_reduce.glsl's BLOCK_SIZE.
constexpr std::uint32_t kLuminanceBlock = 8;

float glow_kernel(int offset) {
    return kGlowKernel.at(static_cast<std::size_t>(std::abs(offset)));
}

Float4 scale4(const Float4& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s, a[3] * s};
}

void add4(Float4& a, const Float4& b, float weight) {
    for (std::size_t i = 0; i < 4; ++i) {
        a.at(i) += b.at(i) * weight;
    }
}

// --- The tonemap pass, as tonemap.glsl ---

float w0(float a) {
    return (1.0F / 6.0F) * ((a * ((a * (-a + 3.0F)) - 3.0F)) + 1.0F);
}
float w1(float a) {
    return (1.0F / 6.0F) * ((a * a * ((3.0F * a) - 6.0F)) + 4.0F);
}
float w2(float a) {
    return (1.0F / 6.0F) * ((a * ((a * ((-3.0F * a) + 3.0F)) + 3.0F)) + 1.0F);
}
float w3(float a) {
    return (1.0F / 6.0F) * (a * a * a);
}
float g0(float a) {
    return w0(a) + w1(a);
}
float g1(float a) {
    return w2(a) + w3(a);
}
float h0(float a) {
    return -1.0F + (w1(a) / (w0(a) + w1(a)));
}
float h1(float a) {
    return 1.0F + (w3(a) / (w2(a) + w3(a)));
}

// A level sampled bicubically, in four bilinear taps, as tonemap.glsl's texture2D_bicubic.
Float3 bicubic(const HdrImage& image, float u, float v) {
    const auto width = static_cast<float>(image.width);
    const auto height = static_cast<float>(image.height);
    const float x = (u * width) + 0.5F;
    const float y = (v * height) + 0.5F;
    const float ix = std::floor(x);
    const float iy = std::floor(y);
    const float fx = x - ix;
    const float fy = y - iy;
    const float g0x = g0(fx);
    const float g1x = g1(fx);
    const float h0x = h0(fx);
    const float h1x = h1(fx);
    const float h0y = h0(fy);
    const float h1y = h1(fy);
    const auto tap = [&](float px, float py) {
        return sample_hdr(image, (px - 0.5F) * (1.0F / width), (py - 0.5F) * (1.0F / height));
    };
    const Float4 p0 = tap(ix + h0x, iy + h0y);
    const Float4 p1 = tap(ix + h1x, iy + h0y);
    const Float4 p2 = tap(ix + h0x, iy + h1y);
    const Float4 p3 = tap(ix + h1x, iy + h1y);
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = (g0(fy) * ((g0x * p0.at(i)) + (g1x * p1.at(i)))) +
                    (g1(fy) * ((g0x * p2.at(i)) + (g1x * p3.at(i))));
    }
    return out;
}

Float3 gather_glow(const ForwardPostUniforms& uniforms, std::span<const HdrImage> levels, float u,
                   float v) {
    const std::array<float, kMaxGlowLevels> weights{
        uniforms.levels_low[0], uniforms.levels_low[1],  uniforms.levels_low[2],
        uniforms.levels_low[3], uniforms.levels_high[0], uniforms.levels_high[1],
        uniforms.levels_high[2]};
    Float3 color{};
    for (std::size_t i = 0; i < weights.size() && i < levels.size(); ++i) {
        if (weights.at(i) > 0.0F) {
            const Float3 level = bicubic(levels[i], u, v);
            for (std::size_t c = 0; c < 3; ++c) {
                color.at(c) += level.at(c) * weights.at(i);
            }
        }
    }
    return color;
}

Float3 apply_glow(const Float3& color, const Float3& glow, float white, GlowBlendMode mode) {
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float c = color.at(i);
        switch (mode) {
        case GlowBlendMode::Additive:
            out.at(i) = c + glow.at(i);
            break;
        case GlowBlendMode::Screen: {
            const float g = std::clamp(glow.at(i), 0.0F, white);
            out.at(i) = c + g - (c * g / white);
            break;
        }
        case GlowBlendMode::Softlight: {
            const float g = std::clamp(glow.at(i), 0.0F, 1.0F);
            const float lifted = c <= 0.25F ? (((16.0F * c) - 12.0F) * c + 4.0F) * c : std::sqrt(c);
            out.at(i) = c > 1.0F ? c : c + (g * (lifted - c));
            break;
        }
        case GlowBlendMode::Replace:
        case GlowBlendMode::Mix:
            out.at(i) = glow.at(i);
            break;
        }
    }
    return out;
}

Float3 lut_3d(const Image& lut, float size, const Float3& color) {
    Float3 texel{};
    for (std::size_t i = 0; i < 3; ++i) {
        texel.at(i) = std::clamp((color.at(i) * size) - 0.5F, 0.0F, size - 1.0F);
    }
    float slice = std::min(std::floor(texel[2]), size - 2.0F);
    float blend = texel[2] - slice;
    if (size < 2.0F) {
        slice = 0.0F;
        blend = 0.0F;
    }
    const float width = size * size;
    const float v = (texel[1] + 0.5F) / size;
    const Float4 low =
        sample_image(lut, Filter::Linear, false, (texel[0] + 0.5F + (slice * size)) / width, v);
    const Float4 high = sample_image(lut, Filter::Linear, false,
                                     (texel[0] + 0.5F + ((slice + 1.0F) * size)) / width, v);
    return {lerp(low[0], high[0], blend), lerp(low[1], high[1], blend),
            lerp(low[2], high[2], blend)};
}

// The pixels of a luminance pass's target, each the average of its 8 x 8 block of the source,
// whose texels value gives (the world's brightest channel, or the luminance before), summed in
// luminance_reduce.glsl's order: each of the first half of the block's 64 values adds the one
// half a block on, then the first quarter the one a quarter on, down to one. Texels past the
// source's edge count 0 and the sum is divided by those inside. The last pass moves from
// previous by the adjust uniform and keeps it from the least to the most.
template <typename Value>
void reduce_luminance(const ForwardPostUniforms& uniforms, float previous, LuminanceImage& target,
                      const Value& value) {
    target.width = static_cast<std::uint32_t>(uniforms.target[0]);
    target.height = static_cast<std::uint32_t>(uniforms.target[1]);
    const auto source_width = static_cast<std::uint32_t>(uniforms.source[0]);
    const auto source_height = static_cast<std::uint32_t>(uniforms.source[1]);
    const bool final =
        uniforms.options[0] == static_cast<std::uint32_t>(ForwardPass::LuminanceFinal);
    target.values.assign(std::size_t{target.width} * target.height, 0.0F);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const std::uint32_t left = x * kLuminanceBlock;
            const std::uint32_t top = y * kLuminanceBlock;
            std::array<float, std::size_t{kLuminanceBlock} * kLuminanceBlock> block{};
            for (std::uint32_t t = 0; t < block.size(); ++t) {
                const std::uint32_t at_x = left + (t % kLuminanceBlock);
                const std::uint32_t at_y = top + (t / kLuminanceBlock);
                block.at(t) =
                    at_x < source_width && at_y < source_height ? value(at_x, at_y) : 0.0F;
            }
            for (std::size_t half = block.size() / 2; half >= 1; half /= 2) {
                for (std::size_t t = 0; t < half; ++t) {
                    block.at(t) += block.at(t + half);
                }
            }
            const std::uint32_t inside_x = std::min(source_width - left, kLuminanceBlock);
            const std::uint32_t inside_y = std::min(source_height - top, kLuminanceBlock);
            float average = block[0] / static_cast<float>(inside_x * inside_y);
            if (uniforms.options[1] != 0 && uniforms.options[2] != 0) {
                average = slash::exp(average);
            }
            if (final) {
                average =
                    std::min(std::max(previous + ((average - previous) * uniforms.exposure[3]),
                                      uniforms.exposure[1]),
                             uniforms.exposure[2]);
            }
            target.values[(std::size_t{y} * target.width) + x] = average;
        }
    }
}

// --- Depth of field, as bokeh_dof.glsl ---

// The samples a side of a box's or hexagon's blur, and how far a circle's steps go, for each
// BokehQuality.
constexpr std::array<std::uint32_t, 4> kDofSamples{6, 12, 12, 24};
constexpr std::array<float, 4> kDofCircleScale{8.0F, 4.0F, 1.0F, 0.5F};
constexpr float kGoldenAngle = 2.39996323F;
constexpr float kTwoPi = 6.28318531F;

float fract(float x) {
    return x - std::floor(x);
}

// bokeh_dof_inc.glsl's hash12n: a number from 0 up to 1 for a place.
float hash12n(float x, float y) {
    float px = fract(x * 5.3987F);
    float py = fract(y * 5.4421F);
    const float dot = (py * (px + 21.5351F)) + (px * (py + 14.3137F));
    px += dot;
    py += dot;
    return fract(px * py * 95.4307F);
}

// The distance from the camera along its view of a depth buffer value.
float dof_distance(const ForwardPostUniforms& uniforms, float depth) {
    return (uniforms.dof_depth[2] - (depth * uniforms.dof_depth[3])) /
           (uniforms.dof_depth[0] - (depth * uniforms.dof_depth[1]));
}

// The depth buffer's value for one of the frame's pixels, as the shader's dof_depth_pixel: the
// same pixel's, or the one under its center where the depth buffer is smaller.
float dof_depth(const ForwardPostUniforms& uniforms, std::span<const float> depth,
                const HdrImage& target, std::uint32_t x, std::uint32_t y) {
    if (uniforms.dof_jitter[2] == 0.0F) {
        return depth[(std::size_t{y} * target.width) + x];
    }
    const auto width = static_cast<std::uint32_t>(uniforms.dof_jitter[2]);
    const auto height = static_cast<std::uint32_t>(uniforms.dof_jitter[3]);
    const auto at = [](std::uint32_t p, float size, float rcp, std::uint32_t most) {
        return std::min(static_cast<std::uint32_t>((static_cast<float>(p) + 0.5F) * size * rcp),
                        most - 1);
    };
    return depth[(std::size_t{at(y, uniforms.dof_jitter[3], uniforms.target[3], height)} * width) +
                 at(x, uniforms.dof_jitter[2], uniforms.target[2], width)];
}

// A pixel's blur size at distance from the camera, as get_blur_size: negative for near blur.
float dof_blur_size(const ForwardPostUniforms& uniforms, float distance) {
    const std::uint32_t flags = uniforms.dof_options[1];
    const float far_begin = uniforms.dof[0];
    const float far_end = uniforms.dof[1];
    const float near_begin = uniforms.dof[2];
    const float near_end = uniforms.dof[3];
    if ((flags & kDofNear) != 0 && distance < near_begin) {
        if ((flags & kDofPhysicalNear) != 0) {
            const float d = std::abs(near_begin - distance);
            return -(d / (near_begin - d)) * uniforms.dof_blur[2];
        }
        return -(1.0F - smoothstep(near_end, near_begin, distance)) * uniforms.dof_blur[0];
    }
    if ((flags & kDofFar) != 0 && distance > far_begin) {
        if ((flags & kDofPhysicalFar) != 0) {
            const float d = std::abs(far_begin - distance);
            return (d / (far_begin + d)) * uniforms.dof_blur[1];
        }
        return smoothstep(far_begin, far_end, distance) * uniforms.dof_blur[0];
    }
    return 0.0F;
}

Float4 mix4(const Float4& a, const Float4& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t), lerp(a[3], b[3], t)};
}

// weighted_filter_dir: source blurred along dir, in pixels of the pass, from (u, v). Each sample
// counts as far as the smaller of its and the pixel's blur sizes reaches it.
Float4 weighted_filter_dir(const ForwardPostUniforms& uniforms, const HdrImage& source, float dx,
                           float dy, float u, float v) {
    dx *= uniforms.target[2];
    dy *= uniforms.target[3];
    const Float4 color = sample_hdr(source, u, v);
    Float4 accum = color;
    float total = 1.0F;
    const auto steps = static_cast<int>(uniforms.dof_options[0]);
    const float blur_scale = uniforms.dof_blur[0] / static_cast<float>(steps);
    if ((uniforms.dof_options[1] & kDofJitter) != 0) {
        const float jitter = hash12n(u + uniforms.dof_jitter[0], v + uniforms.dof_jitter[0]) - 0.5F;
        u += dx * jitter;
        v += dy * jitter;
    }
    for (int i = -steps; i <= steps; ++i) {
        if (i == 0) {
            continue;
        }
        float radius = static_cast<float>(i) * blur_scale;
        const Float4 sample = sample_hdr(source, u + (dx * radius), v + (dy * radius));
        radius = std::abs(radius);
        const float limit = sample[3] < color[3] ? std::abs(sample[3]) : std::abs(color[3]);
        const float m = smoothstep(radius - 0.5F, radius + 0.5F, limit);
        add4(accum, mix4(color, sample, m), 1.0F);
        total += 1.0F;
    }
    return scale4(accum, 1.0F / total);
}

// Where depth of field's blur reads from for the pass's pixel (x, y): the centers of its pixels,
// or the full-size source's, for a first pass at half size.
std::array<float, 2> dof_place(const ForwardPostUniforms& uniforms, std::uint32_t x,
                               std::uint32_t y) {
    const std::uint32_t flags = uniforms.dof_options[1];
    const bool circle = uniforms.options[0] == static_cast<std::uint32_t>(ForwardPass::DofCircle);
    float offset = 0.5F;
    if ((flags & kDofHalf) != 0 && (circle || (flags & kDofSecond) == 0)) {
        offset = 0.25F;
    }
    return {(static_cast<float>(x) * uniforms.target[2]) + (offset * uniforms.target[2]),
            (static_cast<float>(y) * uniforms.target[3]) + (offset * uniforms.target[3])};
}

void resize_target(const ForwardPostUniforms& uniforms, HdrImage& target) {
    target.width = static_cast<std::uint32_t>(uniforms.target[0]);
    target.height = static_cast<std::uint32_t>(uniforms.target[1]);
    target.pixels.assign(std::size_t{target.width} * target.height * 4, 0.0F);
}

void store_half(HdrImage& target, std::uint32_t x, std::uint32_t y, const Float4& color) {
    const std::size_t at = ((std::size_t{y} * target.width) + x) * 4;
    for (std::size_t i = 0; i < 4; ++i) {
        target.pixels[at + i] = round_half(color.at(i));
    }
}

Float4 load_hdr(const HdrImage& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

} // namespace

float round_half(float value) {
    if (std::isnan(value)) {
        return value;
    }
    const auto bits = slash::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = bits & 0x80000000U;
    const std::uint32_t magnitude = bits & 0x7FFFFFFFU;
    // At least 65520 rounds up past half's largest value, 65504.
    if (magnitude >= 0x477FF000U) {
        return slash::bit_cast<float>(sign | 0x7F800000U);
    }
    const auto exponent = static_cast<int>(magnitude >> 23U) - 127;
    // Below 2^-25, half the smallest half, everything rounds to 0.
    if (exponent < -25) {
        return slash::bit_cast<float>(sign);
    }
    // Halves keep 10 bits after the point, and from 2^-14 down a step of 2^-24. Rounds to a
    // multiple of the step by adding and taking away a number whose last bit is worth one step,
    // as float addition rounds ties to even.
    const float step = std::ldexp(1.0F, std::max(exponent, -14) - 10);
    const float big = step * 8388608.0F; // 2^23 steps: its last bit is worth one step
    const float rounded = (slash::bit_cast<float>(magnitude) + big) - big;
    return slash::bit_cast<float>(sign | slash::bit_cast<std::uint32_t>(rounded));
}

Float4 forward_clear(const Color& clear) {
    return {round_half(srgb_to_linear_exact(clear.r)), round_half(srgb_to_linear_exact(clear.g)),
            round_half(srgb_to_linear_exact(clear.b)), round_half(clear.a)};
}

GlowPlan forward_glow_plan(const PostProcess& post, std::uint32_t width, std::uint32_t height) {
    GlowPlan plan;
    // Godot's Image::get_image_required_mipmaps: halvings to 1 x 1.
    int mipmaps = 0;
    for (std::uint32_t size = std::max(width, height); size > 1; size >>= 1U) {
        ++mipmaps;
    }
    const int levels = std::clamp(mipmaps - 1, 1, static_cast<int>(kMaxGlowLevels));
    for (std::size_t i = 0; i < kMaxGlowLevels; ++i) {
        plan.sizes.at(i) = {.width = std::max((width >> 1U) >> i, 1U),
                            .height = std::max((height >> 1U) >> i, 1U)};
    }
    int last = -1;
    for (std::size_t i = 0; i < kMaxGlowLevels; ++i) {
        if (post.glow_levels.at(i) > 0.01F) {
            last = static_cast<int>(i);
        }
    }
    plan.count = static_cast<std::uint32_t>(std::max(std::min(last, levels - 1) + 1, 1));
    return plan;
}

ForwardPostUniforms forward_post_uniforms(const PostProcess& post, ForwardPass pass,
                                          std::uint32_t target_width, std::uint32_t target_height,
                                          std::uint32_t frame_width, std::uint32_t frame_height,
                                          std::uint32_t lut_width, std::uint32_t lut_height) {
    ForwardPostUniforms out;
    const auto width = static_cast<float>(std::max(target_width, 1U));
    const auto height = static_cast<float>(std::max(target_height, 1U));
    out.target = {width, height, 1.0F / width, 1.0F / height};
    out.glow = {post.glow_strength, post.glow_bloom, post.glow_hdr_threshold, post.glow_hdr_scale};
    const GlowPlan plan = forward_glow_plan(post, frame_width, frame_height);
    out.glow_first = {post.exposure, post.glow_hdr_luminance_cap,
                      static_cast<float>(plan.sizes[0].width),
                      static_cast<float>(plan.sizes[0].height)};
    std::array<float, kMaxGlowLevels> weights{};
    for (std::size_t i = 0; i < plan.count; ++i) {
        if (post.glow_levels.at(i) > 0.0001F) {
            weights.at(i) = post.glow_levels.at(i);
        }
    }
    const float intensity =
        post.glow_blend_mode == GlowBlendMode::Mix ? post.glow_mix : post.glow_intensity;
    out.levels_low = {weights[0], weights[1], weights[2], weights[3]};
    out.levels_high = {weights[4], weights[5], weights[6], intensity};
    const float white = forward_white(post);
    const std::uint32_t correction =
        post.color_correction == TextureId::None ? 0 : correction_kind(lut_width, lut_height);
    out.tonemap = {post.exposure, white,
                   correction == kCorrection3D ? static_cast<float>(lut_height) : 0.0F, 0.0F};
    out.tonemapper = tonemapper_parameters(post, white);
    out.adjust = {post.brightness, post.contrast, post.saturation, 0.0F};
    out.exposure = {post.auto_exposure_scale, post.auto_exposure_min, post.auto_exposure_max,
                    std::clamp(post.auto_exposure_adjust, 0.0F, 1.0F)};
    std::uint32_t flags = 0;
    if (post.glow) {
        flags |= 1U;
    }
    if (post.adjustments) {
        flags |= 2U | (correction << 2U);
    }
    if (post.auto_exposure) {
        flags |= kForwardAutoExposure;
    }
    out.options = {static_cast<std::uint32_t>(pass), static_cast<std::uint32_t>(post.tonemapper),
                   flags, static_cast<std::uint32_t>(post.glow_blend_mode)};
    return out;
}

std::vector<PostSize> luminance_plan(std::uint32_t width, std::uint32_t height) {
    std::vector<PostSize> plan;
    PostSize size{.width = width, .height = height};
    do {
        size = {.width = std::max(size.width / kLuminanceBlock, 1U),
                .height = std::max(size.height / kLuminanceBlock, 1U)};
        plan.push_back(size);
    } while (size.width != 1 || size.height != 1);
    return plan;
}

ForwardPostUniforms luminance_uniforms(const PostProcess& post, ForwardPass pass, PostSize source,
                                       PostSize target, bool last) {
    ForwardPostUniforms out;
    const auto width = static_cast<float>(target.width);
    const auto height = static_cast<float>(target.height);
    out.target = {width, height, 1.0F / width, 1.0F / height};
    out.exposure = {post.auto_exposure_scale, post.auto_exposure_min, post.auto_exposure_max,
                    std::clamp(post.auto_exposure_adjust, 0.0F, 1.0F)};
    out.source = {static_cast<float>(source.width), static_cast<float>(source.height), 0.0F, 0.0F};
    out.options = {static_cast<std::uint32_t>(pass), post.auto_exposure_log ? 1U : 0U,
                   last ? 1U : 0U, 0};
    return out;
}

void run_luminance_pass(const ForwardPostUniforms& uniforms, const HdrImage& world,
                        LuminanceImage& target) {
    reduce_luminance(uniforms, 0.0F, target, [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t at = ((std::size_t{y} * world.width) + x) * 4;
        const float brightest =
            std::max(world.pixels[at], std::max(world.pixels[at + 1], world.pixels[at + 2]));
        return uniforms.options[1] != 0 ? slash::ln(std::max(brightest, kMinLuminance)) : brightest;
    });
}

void run_luminance_pass(const ForwardPostUniforms& uniforms, const LuminanceImage& source,
                        float previous, LuminanceImage& target) {
    reduce_luminance(uniforms, previous, target, [&](std::uint32_t x, std::uint32_t y) {
        return source.values[(std::size_t{y} * source.width) + x];
    });
}

bool uses_dof(const PostProcess& post) {
    return (post.dof_far || post.dof_near) && post.dof_amount > 0.0F;
}

std::vector<DofStep> dof_plan(const PostProcess& post, std::uint32_t width, std::uint32_t height) {
    // As Godot's can_use_effects: no post effects under 8 x 8 pixels.
    if (!uses_dof(post) || width < 8 || height < 8) {
        return {};
    }
    const PostSize full{.width = width, .height = height};
    const PostSize half{.width = std::max(width >> 1U, 1U), .height = std::max(height >> 1U, 1U)};
    std::vector<DofStep> plan{{.pass = ForwardPass::DofSize,
                               .source = DofTexture::World,
                               .target = DofTexture::Sized,
                               .size = full}};
    const BokehShape shape = post.depth_of_field.bokeh_shape;
    if (shape == BokehShape::Circle) {
        plan.push_back({.pass = ForwardPass::DofCircle,
                        .source = DofTexture::Sized,
                        .target = DofTexture::Half0,
                        .size = half});
        plan.push_back({.pass = ForwardPass::DofComposite,
                        .source = DofTexture::Half0,
                        .target = DofTexture::Out,
                        .size = full});
        return plan;
    }
    const ForwardPass blur =
        shape == BokehShape::Box ? ForwardPass::DofBox : ForwardPass::DofHexagon;
    if (post.depth_of_field.bokeh_quality <= BokehQuality::Low) {
        plan.push_back(
            {.pass = blur, .source = DofTexture::Sized, .target = DofTexture::Half0, .size = half});
        plan.push_back({.pass = blur,
                        .source = DofTexture::Half0,
                        .target = DofTexture::Half1,
                        .size = half,
                        .second = true});
        plan.push_back({.pass = ForwardPass::DofComposite,
                        .source = DofTexture::Half1,
                        .target = DofTexture::Out,
                        .size = full});
    } else {
        plan.push_back({.pass = blur,
                        .source = DofTexture::Sized,
                        .target = DofTexture::Secondary,
                        .size = full});
        plan.push_back({.pass = blur,
                        .source = DofTexture::Secondary,
                        .target = DofTexture::Out,
                        .size = full,
                        .second = true});
    }
    return plan;
}

ForwardPostUniforms dof_uniforms(const PostProcess& post, const DofStep& step,
                                 const std::array<float, 16>& projection, PostSize depth) {
    ForwardPostUniforms out;
    const auto width = static_cast<float>(step.size.width);
    const auto height = static_cast<float>(step.size.height);
    out.target = {width, height, 1.0F / width, 1.0F / height};
    out.options = {static_cast<std::uint32_t>(step.pass), 0, 0, 0};
    const auto quality = static_cast<std::size_t>(post.depth_of_field.bokeh_quality);
    const bool box_or_hexagon =
        step.pass == ForwardPass::DofBox || step.pass == ForwardPass::DofHexagon;
    const bool half = (box_or_hexagon && post.depth_of_field.bokeh_quality <= BokehQuality::Low) ||
                      step.pass == ForwardPass::DofCircle;
    const float bokeh = post.dof_amount * 64.0F;
    const bool physical_far = post.dof_far_transition < 0.0F;
    const bool physical_near = post.dof_near_transition < 0.0F;
    float blur = physical_far && physical_near ? 32.0F : bokeh;
    if (box_or_hexagon && half) {
        blur *= 0.5F;
    }
    out.dof = {post.dof_far_distance, post.dof_far_distance + post.dof_far_transition,
               post.dof_near_distance, post.dof_near_distance - post.dof_near_transition};
    out.dof_blur = {blur, bokeh, bokeh,
                    step.pass == ForwardPass::DofCircle ? kDofCircleScale.at(quality) : 0.5F};
    out.dof_depth = {projection[10], projection[11], projection[14], projection[15]};
    out.dof_jitter = {post.dof_jitter_seed, 0.0F, static_cast<float>(depth.width),
                      static_cast<float>(depth.height)};
    std::uint32_t flags = 0;
    flags |= post.dof_far ? kDofFar : 0U;
    flags |= post.dof_near ? kDofNear : 0U;
    flags |= physical_far ? kDofPhysicalFar : 0U;
    flags |= physical_near ? kDofPhysicalNear : 0U;
    flags |= half ? kDofHalf : 0U;
    flags |= step.second ? kDofSecond : 0U;
    flags |= box_or_hexagon && step.target == DofTexture::Out ? kDofToWorld : 0U;
    flags |= post.depth_of_field.use_jitter ? kDofJitter : 0U;
    out.dof_options = {box_or_hexagon ? kDofSamples.at(quality) : 0U, flags, 0, 0};
    return out;
}

void run_dof_size_pass(const ForwardPostUniforms& uniforms, const HdrImage& world,
                       std::span<const float> depth, HdrImage& target) {
    resize_target(uniforms, target);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            Float4 color = load_hdr(world, x, y);
            color[3] = dof_blur_size(
                uniforms, dof_distance(uniforms, dof_depth(uniforms, depth, target, x, y)));
            store_half(target, x, y, color);
        }
    }
}

void run_dof_blur_pass(const ForwardPostUniforms& uniforms, const HdrImage& source,
                       const HdrImage& world, HdrImage& target) {
    resize_target(uniforms, target);
    const auto pass = static_cast<ForwardPass>(uniforms.options[0]);
    const std::uint32_t flags = uniforms.dof_options[1];
    const bool second = (flags & kDofSecond) != 0;
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const auto [u, v] = dof_place(uniforms, x, y);
            Float4 color{};
            if (pass == ForwardPass::DofBox) {
                color = second ? weighted_filter_dir(uniforms, source, 0.0F, 1.0F, u, v)
                               : weighted_filter_dir(uniforms, source, 1.0F, 0.0F, u, v);
            } else if (pass == ForwardPass::DofHexagon) {
                color = second ? weighted_filter_dir(uniforms, source, 0.866025404F, 0.5F, u, v)
                               : weighted_filter_dir(uniforms, source, 0.0F, 1.0F, u, v);
                if (second) {
                    const Float4 other =
                        weighted_filter_dir(uniforms, source, -0.866025404F, 0.5F, u, v);
                    for (std::size_t i = 0; i < 3; ++i) {
                        color.at(i) = std::min(color.at(i), other.at(i));
                    }
                    color[3] = (color[3] + other[3]) * 0.5F;
                }
            } else {
                // MODE_BOKEH_CIRCULAR: samples on a spiral, in pixels of the full size
                const float pixel_x = uniforms.target[2] * 0.5F;
                const float pixel_y = uniforms.target[3] * 0.5F;
                color = sample_hdr(source, u, v);
                const float initial_blur = color[3];
                float accum = 1.0F;
                float radius = uniforms.dof_blur[3];
                for (float angle = 0.0F; radius < uniforms.dof_blur[0];) {
                    const Vec2 turn = slash::cos_sin(angle);
                    const Float4 sample = sample_hdr(source, u + (turn.x * pixel_x * radius),
                                                     v + (turn.y * pixel_y * radius));
                    float sample_size = std::abs(sample[3]);
                    if (sample[3] > initial_blur) {
                        sample_size = std::clamp(sample_size, 0.0F, std::abs(initial_blur) * 2.0F);
                    }
                    const float m = smoothstep(radius - 0.5F, radius + 0.5F, sample_size);
                    add4(color, mix4(scale4(color, 1.0F / accum), sample, m), 1.0F);
                    accum += 1.0F;
                    radius += uniforms.dof_blur[3] / radius;
                    angle += kGoldenAngle;
                    if (angle >= kTwoPi) {
                        angle -= kTwoPi;
                    }
                }
                color = scale4(color, 1.0F / accum);
            }
            if ((flags & kDofToWorld) != 0) {
                color[3] = load_hdr(world, x, y)[3];
            }
            store_half(target, x, y, color);
        }
    }
}

void run_dof_composite_pass(const ForwardPostUniforms& uniforms, const HdrImage& world,
                            std::span<const float> depth, const HdrImage& bokeh, HdrImage& target) {
    resize_target(uniforms, target);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const auto [u, v] = dof_place(uniforms, x, y);
            Float4 color = load_hdr(world, x, y);
            const float size = dof_blur_size(
                uniforms, dof_distance(uniforms, dof_depth(uniforms, depth, target, x, y)));
            const Float4 blurred = sample_hdr(bokeh, u, v);
            const float amount =
                blurred[3] < size
                    ? std::min(1.0F, std::max(0.0F, std::max(std::abs(size), std::abs(blurred[3]))))
                    : std::min(1.0F, std::max(0.0F, std::abs(size)));
            for (std::size_t i = 0; i < 3; ++i) {
                color.at(i) = lerp(color.at(i), blurred.at(i), amount);
            }
            store_half(target, x, y, color);
        }
    }
}

void run_forward_glow_pass(const ForwardPostUniforms& uniforms, const HdrImage& source,
                           HdrImage& target, float luminance) {
    target.width = static_cast<std::uint32_t>(uniforms.target[0]);
    target.height = static_cast<std::uint32_t>(uniforms.target[1]);
    const auto width = static_cast<int>(target.width);
    const auto height = static_cast<int>(target.height);
    const bool first = uniforms.options[0] == static_cast<std::uint32_t>(ForwardPass::GlowFirst);
    const float cap = uniforms.glow_first[1];
    const float scale = 1.0F / std::max(cap, 6.0F);
    const Float3 weights{0.299F * scale, 0.587F * scale, 0.114F * scale};
    const auto karis = [&](const Float4& color) {
        return (color[0] * weights[0]) + (color[1] * weights[1]) + (color[2] * weights[2]);
    };

    // Every sample the pixels' taps take, from kGlowReach before the first pixel to kGlowReach
    // after the last, then the taps of each row summed across, in forward_post.hlsl's order.
    const int across = width + (2 * kGlowReach);
    const int down = height + (2 * kGlowReach);
    std::vector<Float4> samples(static_cast<std::size_t>(across) * static_cast<std::size_t>(down));
    for (int y = 0; y < down; ++y) {
        const float v = glow_place(y - kGlowReach, uniforms.target[1]) * uniforms.target[3];
        for (int x = 0; x < across; ++x) {
            const float u = glow_place(x - kGlowReach, uniforms.target[0]) * uniforms.target[2];
            Float4 color = sample_hdr(source, u, v);
            if (first) {
                color = scale4(color, 1.0F / (1.0F + karis(color)));
            }
            samples[(static_cast<std::size_t>(y) * static_cast<std::size_t>(across)) +
                    static_cast<std::size_t>(x)] = color;
        }
    }
    std::vector<Float4> rows(static_cast<std::size_t>(width) * static_cast<std::size_t>(down));
    for (int y = 0; y < down; ++y) {
        for (int x = 0; x < width; ++x) {
            Float4 row{};
            for (const int dx : kGlowTaps) {
                add4(row,
                     samples[(static_cast<std::size_t>(y) * static_cast<std::size_t>(across)) +
                             static_cast<std::size_t>(x + kGlowReach + dx)],
                     glow_kernel(dx));
            }
            rows[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                 static_cast<std::size_t>(x)] = row;
        }
    }

    target.pixels.assign(std::size_t{target.width} * target.height * 4, 0.0F);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Float4 color{};
            for (const int dy : kGlowTaps) {
                add4(color,
                     rows[(static_cast<std::size_t>(y + kGlowReach + dy) *
                           static_cast<std::size_t>(width)) +
                          static_cast<std::size_t>(x)],
                     glow_kernel(dy));
            }
            if (first) {
                color = scale4(color, 1.0F / (1.0F - karis(color)));
            }
            color = scale4(color, uniforms.glow[0]);
            if (first && (uniforms.options[2] & kForwardAutoExposure) != 0) {
                const float divisor = std::max(luminance, kMinLuminance) / uniforms.exposure[0];
                for (float& channel : color) {
                    channel /= divisor;
                }
            }
            if (first) {
                color = scale4(color, uniforms.glow_first[0]);
                const float brightest = std::max(color[0], std::max(color[1], color[2]));
                const float feedback = std::max(
                    smoothstep(uniforms.glow[2], uniforms.glow[2] + uniforms.glow[3], brightest),
                    uniforms.glow[1]);
                for (float& channel : color) {
                    channel = std::min(channel * feedback, cap);
                }
            }
            const std::size_t at =
                ((static_cast<std::size_t>(y) * target.width) + static_cast<std::size_t>(x)) * 4;
            for (std::size_t i = 0; i < 4; ++i) {
                target.pixels[at + i] = round_half(color.at(i));
            }
        }
    }
}

void run_forward_tonemap_pass(const ForwardPostUniforms& uniforms, const HdrImage& world,
                              std::span<const HdrImage> levels, const Image& lut, Image& target,
                              float luminance) {
    target.pixels.resize(std::size_t{target.width} * target.height * 4);
    const auto tonemapper = static_cast<Tonemapper>(uniforms.options[1]);
    const std::uint32_t flags = uniforms.options[2];
    const auto mode = static_cast<GlowBlendMode>(uniforms.options[3]);
    const bool glowing = (flags & 1U) != 0;
    const float intensity = uniforms.levels_high[3];
    const float white = uniforms.tonemap[1];
    float exposure = uniforms.tonemap[0];
    if ((flags & kForwardAutoExposure) != 0) {
        exposure *= 1.0F / (std::max(luminance, kMinLuminance) / uniforms.exposure[0]);
    }
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) * uniforms.target[2];
            const float v = (static_cast<float>(y) + 0.5F) * uniforms.target[3];
            const std::size_t texel = ((std::size_t{y} * world.width) + x) * 4;
            Float3 rgb{world.pixels[texel] * exposure, world.pixels[texel + 1] * exposure,
                       world.pixels[texel + 2] * exposure};
            Float3 bloom{};
            if (glowing) {
                bloom = gather_glow(uniforms, levels, u, v);
                for (float& channel : bloom) {
                    channel *= intensity;
                }
            }
            if (glowing && mode != GlowBlendMode::Softlight) {
                if (mode == GlowBlendMode::Mix) {
                    for (std::size_t i = 0; i < 3; ++i) {
                        rgb.at(i) = (rgb.at(i) * (1.0F - intensity)) + bloom.at(i);
                    }
                } else {
                    rgb = apply_glow(rgb, bloom, white, mode);
                }
            }
            rgb = apply_tonemapping(rgb, tonemapper, uniforms.tonemapper);
            if (glowing && mode == GlowBlendMode::Softlight) {
                rgb = apply_glow(rgb, apply_tonemapping(bloom, tonemapper, uniforms.tonemapper),
                                 white, mode);
            }
            if ((flags & 2U) != 0) {
                for (float& channel : rgb) {
                    channel = linear_to_srgb_exact(channel * uniforms.adjust[0]);
                    channel = lerp(0.5F, channel, uniforms.adjust[1]);
                }
                const float gray = (rgb[0] + rgb[1] + rgb[2]) * (1.0F / 3.0F);
                for (float& channel : rgb) {
                    channel = lerp(gray, channel, uniforms.adjust[2]);
                }
                const std::uint32_t correction = (flags >> 2U) & 3U;
                if (correction != 0) {
                    for (float& channel : rgb) {
                        channel = std::clamp(channel, 0.0F, 1.0F);
                    }
                }
                if (correction == kCorrection1D) {
                    for (std::size_t i = 0; i < 3; ++i) {
                        rgb.at(i) = sample_image(lut, Filter::Linear, false, rgb.at(i), 0.5F).at(i);
                    }
                } else if (correction == kCorrection3D) {
                    rgb = lut_3d(lut, uniforms.tonemap[2], rgb);
                }
            } else {
                for (float& channel : rgb) {
                    channel = linear_to_srgb_exact(channel);
                }
            }
            const std::size_t at = ((std::size_t{y} * target.width) + x) * 4;
            for (std::size_t i = 0; i < 3; ++i) {
                target.pixels[at + i] = to_byte(rgb.at(i));
            }
            target.pixels[at + 3] = to_byte(world.pixels[texel + 3]);
        }
    }
}

} // namespace slash::render::detail

namespace slash::render {

DepthOfFieldSettings depth_of_field_settings_from_json(const json::Value& value) {
    DepthOfFieldSettings out;
    if (!value.is_object()) {
        return out;
    }
    const auto pick = [&](std::string_view key, const auto& names, auto& field) {
        if (const json::Value* found = value.find(key); found != nullptr && found->is_string()) {
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (found->as_string() == names.at(i)) {
                    field = static_cast<std::remove_reference_t<decltype(field)>>(i);
                }
            }
        }
    };
    pick("depth_of_field_bokeh_shape", kBokehShapeNames, out.bokeh_shape);
    pick("depth_of_field_bokeh_quality", kBokehQualityNames, out.bokeh_quality);
    if (const json::Value* found = value.find("depth_of_field_use_jitter");
        found != nullptr && found->is_bool()) {
        out.use_jitter = found->as_bool();
    }
    return out;
}

} // namespace slash::render
