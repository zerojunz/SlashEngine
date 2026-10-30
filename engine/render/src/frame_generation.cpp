// The CPU twin of frame generation, ported from AMD's FidelityFX SDK 1.1.4
// (ffx_frameinterpolation_*.h and ffx_opticalflow_*.h).
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "frame_generation.hpp"

#include "forward_post.hpp"
#include "fsr2.hpp"

#include "slash/core/bits.hpp"
#include "slash/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

// Each function follows the one of the same name in AMD's code, line for line where it can, and
// libm's functions are slash's, so every machine makes the same pixels. Keep the shaders and these
// in step.
namespace slash::render::detail {

namespace {

using Float2 = std::array<float, 2>;
using Float3 = std::array<float, 3>;
using Int2 = std::array<int, 2>;
using UInt2 = std::array<std::uint32_t, 2>;

constexpr float kEpsilon = 1e-03F;
constexpr std::uint32_t kPriorityHighMax = (1U << 10U) - 1U;
constexpr std::uint32_t kPriorityLowMax = (1U << 5U) - 1U;
constexpr std::uint32_t kPriorityLowOffset = 16;
constexpr std::uint32_t kPriorityHighOffset = 21;
constexpr std::uint32_t kPrimaryBit = 1U << 31U;
constexpr std::uint32_t kSearchRadius = 8;
constexpr std::uint32_t kHistogramBins = 256;
constexpr std::uint32_t kHistogramsPerDim = 3;
constexpr std::uint32_t kHistograms = kHistogramsPerDim * kHistogramsPerDim;
constexpr std::uint32_t kHistogramShifts = 3;
// The inpainting pyramid's levels the interpolation reads.
constexpr int kGameFieldLevels = 11;
constexpr int kInpaintingLevels = 10;

// HLSL's saturate: NaN is 0.
float saturate(float x) {
    if (!(x > 0.0F)) {
        return 0.0F;
    }
    return x < 1.0F ? x : 1.0F;
}
// HLSL's max(0, x): NaN is 0.
float max0(float x) {
    return x > 0.0F ? x : 0.0F;
}
float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}
Float3 lerp3(const Float3& a, const Float3& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)};
}
float dot2(const Float2& a, const Float2& b) {
    return (a[0] * b[0]) + (a[1] * b[1]);
}
float dot3(const Float3& a, const Float3& b) {
    return ((a[0] * b[0]) + (a[1] * b[1])) + (a[2] * b[2]);
}
float length2(const Float2& v) {
    return std::sqrt(dot2(v, v));
}
float length3(const Float3& v) {
    return std::sqrt(dot3(v, v));
}
Float2 add2(const Float2& a, const Float2& b) {
    return {a[0] + b[0], a[1] + b[1]};
}
Float2 sub2(const Float2& a, const Float2& b) {
    return {a[0] - b[0], a[1] - b[1]};
}
Float2 mul2(const Float2& a, float s) {
    return {a[0] * s, a[1] * s};
}
Float2 mul2(const Float2& a, const Float2& b) {
    return {a[0] * b[0], a[1] * b[1]};
}
Float2 div2(const Float2& a, const Float2& b) {
    return {a[0] / b[0], a[1] / b[1]};
}
Float2 normalize2(const Float2& v) {
    const float length = length2(v);
    return {v[0] / length, v[1] / length};
}
Float2 to_float(const Int2& v) {
    return {static_cast<float>(v[0]), static_cast<float>(v[1])};
}
// AMD's ffxPow for x of 0 or more: 0 to any power is 0; NaN below 0, as pow.
float power_of(float x, float y) {
    if (x < 0.0F || std::isnan(x)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return x == 0.0F ? 0.0F : slash::exp(y * slash::ln(x));
}
// HLSL's round: halves to even.
float round_even(float x) {
    return std::nearbyint(x);
}
// A float's value as the uint HLSL converts it to: 0 for NaN and below.
std::uint32_t to_uint(float x) {
    if (!(x > 0.0F)) {
        return 0;
    }
    if (x >= 4294967296.0F) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(x);
}
// HLSL's int(x): toward 0.
int to_int(float x) {
    if (std::isnan(x)) {
        return 0;
    }
    return static_cast<int>(std::clamp(x, -2147483648.0F, 2147483520.0F));
}

// f32tof16: the half float's bits, rounded to nearest even.
std::uint32_t half_bits(float value) {
    const float rounded = round_half(value);
    const auto bits = slash::bit_cast<std::uint32_t>(rounded);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    if (std::isnan(rounded)) {
        return sign | 0x7E00U;
    }
    const std::uint32_t magnitude = bits & 0x7FFFFFFFU;
    if (magnitude == 0x7F800000U) {
        return sign | 0x7C00U;
    }
    const int exponent = static_cast<int>(magnitude >> 23U) - 127;
    if (exponent >= -14) {
        return sign | (static_cast<std::uint32_t>(exponent + 15) << 10U) |
               ((magnitude & 0x7FFFFFU) >> 13U);
    }
    // Below 2^-14 halves step by 2^-24.
    return sign | static_cast<std::uint32_t>(std::fabs(rounded) * 16777216.0F);
}
// f16tof32 of the low 16 bits.
float half_value(std::uint32_t bits) {
    const std::uint32_t sign = (bits & 0x8000U) << 16U;
    const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x3FFU;
    if (exponent == 0) {
        const float value = static_cast<float>(mantissa) / 16777216.0F;
        return sign != 0 ? -value : value;
    }
    if (exponent == 31) {
        return slash::bit_cast<float>(sign | 0x7F800000U | (mantissa << 13U));
    }
    return slash::bit_cast<float>(sign | ((exponent + 112U) << 23U) | (mantissa << 13U));
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(saturate(value) * 255.0F));
}
float from_byte(std::uint8_t value) {
    return static_cast<float>(value) / 255.0F;
}

bool on_screen(const Int2& pos, const Int2& size) {
    return static_cast<std::uint32_t>(pos[0]) < static_cast<std::uint32_t>(size[0]) &&
           static_cast<std::uint32_t>(pos[1]) < static_cast<std::uint32_t>(size[1]);
}
bool uv_inside(const Float2& uv) {
    return (uv[0] > 0.0F && uv[0] < 1.0F) && (uv[1] > 0.0F && uv[1] < 1.0F);
}
std::size_t index_of(const Int2& pos, const Int2& size) {
    return (static_cast<std::size_t>(pos[1]) * static_cast<std::size_t>(size[0])) +
           static_cast<std::size_t>(pos[0]);
}
// Thread groups of 8 x 8 over a size, as AMD dispatches its passes: the threads past the size run
// too, and may write inside it.
Int2 dispatched(const Int2& size) {
    return {((size[0] + 7) / 8) * 8, ((size[1] + 7) / 8) * 8};
}

// GetBilinearSamplingData.
struct Bilinear {
    Int2 base{};
    std::array<float, 4> weights{};
};
constexpr std::array<Int2, 4> kBilinearOffsets{Int2{0, 0}, Int2{1, 0}, Int2{0, 1}, Int2{1, 1}};
Bilinear bilinear(const Float2& uv, const Int2& size) {
    const float px = (uv[0] * static_cast<float>(size[0])) - 0.5F;
    const float py = (uv[1] * static_cast<float>(size[1])) - 0.5F;
    const float fx = px - std::floor(px);
    const float fy = py - std::floor(py);
    return {.base = {to_int(std::floor(px)), to_int(std::floor(py))},
            .weights = {(1.0F - fx) * (1.0F - fy), fx * (1.0F - fy), (1.0F - fx) * fy, fx * fy}};
}
Int2 tap(const Bilinear& b, std::size_t i) {
    return {b.base[0] + kBilinearOffsets.at(i)[0], b.base[1] + kBilinearOffsets.at(i)[1]};
}

// A linear sample with clamped addressing, as AMD's s_LinearClamp, filtered in code: two lerps
// across, then one down.
template <std::size_t N, typename Load>
std::array<float, N> sample_linear(const Float2& uv, const Int2& size, Load load) {
    const float px = (uv[0] * static_cast<float>(size[0])) - 0.5F;
    const float py = (uv[1] * static_cast<float>(size[1])) - 0.5F;
    const float fx = px - std::floor(px);
    const float fy = py - std::floor(py);
    const int x0 = to_int(std::floor(px));
    const int y0 = to_int(std::floor(py));
    const auto clamp_x = [&](int x) { return std::clamp(x, 0, size[0] - 1); };
    const auto clamp_y = [&](int y) { return std::clamp(y, 0, size[1] - 1); };
    const std::array<float, N> a = load(Int2{clamp_x(x0), clamp_y(y0)});
    const std::array<float, N> b = load(Int2{clamp_x(x0 + 1), clamp_y(y0)});
    const std::array<float, N> c = load(Int2{clamp_x(x0), clamp_y(y0 + 1)});
    const std::array<float, N> d = load(Int2{clamp_x(x0 + 1), clamp_y(y0 + 1)});
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        const float top = lerp(a.at(i), b.at(i), fx);
        const float bottom = lerp(c.at(i), d.at(i), fx);
        out.at(i) = lerp(top, bottom, fy);
    }
    return out;
}

// ffxLinearFromSrgb, which the backbuffer's colors go through (transfer function 0).
float linear_from_srgb(float value) {
    constexpr auto kLinearScale = static_cast<float>(1.0 / 12.92);
    constexpr auto kScale = static_cast<float>(1.0 / 1.055);
    constexpr auto kOffset = static_cast<float>(0.055 / 1.055);
    if (value < 0.04045F) {
        return value * kLinearScale;
    }
    return power_of((value * kScale) + kOffset, 2.4F);
}
Float3 raw_to_linear(const Float3& raw) {
    return {linear_from_srgb(raw[0]), linear_from_srgb(raw[1]), linear_from_srgb(raw[2])};
}
// RawRGBToLuminance.
float raw_to_luminance(const Float3& raw) {
    return dot3(raw_to_linear(raw), {0.2126F, 0.7152F, 0.0722F});
}
float min_divided_by_max(float v0, float v1) {
    const float m = std::max(v0, v1);
    return m != 0.0F ? std::min(v0, v1) / m : 0.0F;
}
float normalized_dot3(const Float3& v0, const Float3& v1) {
    const float max_length = std::max(length3(v0), length3(v1));
    if (!(max_length > 0.0F)) {
        return 1.0F;
    }
    const Float3 a{v0[0] / max_length, v0[1] / max_length, v0[2] / max_length};
    const Float3 b{v1[0] / max_length, v1[1] / max_length, v1[2] / max_length};
    return dot3(a, b);
}

// --- Optical flow ---------------------------------------------------------------------------

// A level of a luma pyramid.
struct LumaLevel {
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    // r_optical_flow_input[pos]: 0 outside.
    [[nodiscard]] std::uint32_t raw(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return 0;
        }
        return pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                      static_cast<std::size_t>(x)];
    }
    // LoadOpticalFlowInput: at least 1, so msad4 never masks a byte.
    [[nodiscard]] std::uint32_t load(int x, int y) const { return std::max(1U, raw(x, y)); }
    // LoadFirstImagePackedLuma and LoadSecondImagePackedLuma: four pixels from x, a byte each,
    // the pixels past the edges repeating those at them (GetPackedLuma).
    [[nodiscard]] std::uint32_t packed(int x, int y) const {
        const int ax = std::max(0, std::min(width - 4, x));
        const int ay = std::max(0, std::min(height - 1, y));
        std::uint32_t packed = load(ax, ay) | (load(ax + 1, ay) << 8U) | (load(ax + 2, ay) << 16U) |
                               (load(ax + 3, ay) << 24U);
        if (x < 0) {
            const std::uint32_t filler = packed & 0xFFU;
            for (int shift = -1; shift >= -3; --shift) {
                if (x <= shift) {
                    packed = (packed << 8U) | filler;
                }
            }
        } else if (x > width - 4) {
            const std::uint32_t filler = packed & 0xFF000000U;
            for (int shift = 3; shift >= 1; --shift) {
                if (x >= width - shift) {
                    packed = (packed >> 8U) | filler;
                }
            }
        }
        return packed;
    }
};

// Sad: the sum of the four bytes' differences.
std::uint32_t sad(std::uint32_t a, std::uint32_t b) {
    std::uint32_t sum = 0;
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        const auto x = static_cast<int>((a >> shift) & 0xFFU);
        const auto y = static_cast<int>((b >> shift) & 0xFFU);
        sum += static_cast<std::uint32_t>(std::abs(x - y));
    }
    return sum;
}

// PrepareLuma (transfer function 0), then the input pyramid: each level's pixel the average of
// the level 0 pixels under it, which SPD's float sums make exactly, rounded down.
std::array<std::vector<std::uint8_t>, kOpticalFlowLevels>
luma_pyramid(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height) {
    std::array<std::vector<std::uint8_t>, kOpticalFlowLevels> pyramid;
    pyramid[0].resize(std::size_t{width} * height);
    for (std::size_t i = 0; i < pyramid[0].size(); ++i) {
        const float y =
            ((0.2126F * from_byte(rgba[i * 4])) + (0.7152F * from_byte(rgba[(i * 4) + 1]))) +
            (0.0722F * from_byte(rgba[(i * 4) + 2]));
        pyramid[0][i] = static_cast<std::uint8_t>(std::min(to_uint(y * 255.0F), 255U));
    }
    for (std::uint32_t level = 1; level < kOpticalFlowLevels; ++level) {
        const std::uint32_t w = width >> level;
        const std::uint32_t h = height >> level;
        const std::uint32_t span = 1U << level;
        pyramid.at(level).resize(std::size_t{w} * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint32_t sum = 0;
                for (std::uint32_t dy = 0; dy < span; ++dy) {
                    for (std::uint32_t dx = 0; dx < span; ++dx) {
                        sum += pyramid[0][(std::size_t{(y * span) + dy} * width) +
                                          (std::size_t{x} * span) + dx];
                    }
                }
                pyramid.at(level)[(std::size_t{y} * w) + x] =
                    static_cast<std::uint8_t>(sum >> (2U * level));
            }
        }
    }
    return pyramid;
}

// GenerateSceneChangeDetectionHistogram: a histogram of the lumas in each of 3 x 3 regions, read
// four pixels at a time across, so the last four of a region's row may reach past it.
std::vector<std::uint32_t> scene_histograms(const LumaLevel& luma) {
    std::vector<std::uint32_t> histograms(std::size_t{kHistograms} * kHistogramBins, 0);
    const auto width = static_cast<std::uint32_t>(luma.width);
    const auto height = static_cast<std::uint32_t>(luma.height);
    const std::uint32_t div_x = width / kHistogramsPerDim;
    const std::uint32_t div_y = height / kHistogramsPerDim;
    const std::uint32_t strata = (width / 4) / kHistogramsPerDim;
    const std::uint32_t threads_x = ((strata + 31) / 32) * 32;
    for (std::uint32_t z = 0; z < kHistograms; ++z) {
        const std::uint32_t start_x = div_x * (z % kHistogramsPerDim);
        const std::uint32_t start_y = div_y * (z / kHistogramsPerDim);
        for (std::uint32_t gy = 0; gy < 128; ++gy) {
            for (std::uint32_t gx = 0; gx < threads_x; ++gx) {
                const std::uint32_t x = start_x + (4 * gx);
                if (x >= start_x + div_x) {
                    continue;
                }
                for (std::uint32_t y = start_y + gy; y < start_y + div_y; y += 128) {
                    for (std::uint32_t i = 0; i < 4; ++i) {
                        const std::uint32_t value =
                            luma.load(static_cast<int>(x + i), static_cast<int>(y));
                        ++histograms[(std::size_t{z} * kHistogramBins) + value];
                    }
                }
            }
        }
    }
    return histograms;
}

// The reductions in ComputeSCDHistogramsDivergence: halves added in until one is left.
float tree_sum(std::array<float, kHistogramBins> values) {
    for (std::uint32_t stride = kHistogramBins / 2; stride >= 1; stride /= 2) {
        for (std::uint32_t i = 0; i < stride; ++i) {
            values.at(i) += values.at(i + stride);
        }
    }
    return values[0];
}

// ComputeSCDHistogramsDivergence: each region's histogram smoothed by a Gaussian, shifted a bin
// each way and not, and compared with the frame before's by their Kullback-Leibler divergences
// both ways; the least of the three shifts' sums over the regions is how much the scene changed.
// The unshifted histograms are kept for the next frame. Every group reads the histograms before
// any changes them.
std::uint32_t scene_change(const std::vector<std::uint32_t>& histograms,
                           OpticalFlowHistory& history) {
    constexpr std::array<float, 6> kKernel{0.0088122291F, 0.027143577F, 0.065114059F,
                                           0.12164907F,   0.17699835F,  0.20056541F};
    constexpr float kFactor = 1000000.0F;
    std::array<std::uint32_t, kHistogramShifts> sums{};
    std::vector<float> kept(std::size_t{kHistograms} * kHistogramBins, 0.0F);
    for (std::uint32_t shift = 0; shift < kHistogramShifts; ++shift) {
        for (std::uint32_t region = 0; region < kHistograms; ++region) {
            const std::size_t first = std::size_t{region} * kHistogramBins;
            const auto source = [&](int i) {
                return static_cast<float>(
                    histograms[first + static_cast<std::size_t>(std::clamp(i, 0, 255))]);
            };
            std::array<float, kHistogramBins> filtered{};
            for (int i = 0; i < static_cast<int>(kHistogramBins); ++i) {
                const int read = i - 5;
                float value = 0.0F;
                for (int k = 0; k < 11; ++k) {
                    value += kKernel.at(static_cast<std::size_t>(k <= 5 ? k : 10 - k)) *
                             source(read + k);
                }
                value += 1.0F;
                if (shift == 0) {
                    filtered.at(i == 0 ? 255 : static_cast<std::size_t>(i - 1)) =
                        i == 0 ? 1.0F : value;
                } else if (shift == 1) {
                    filtered.at(static_cast<std::size_t>(i)) = value;
                } else {
                    filtered.at(i == 255 ? 0 : static_cast<std::size_t>(i + 1)) =
                        i == 255 ? 1.0F : value;
                }
            }
            const float total = tree_sum(filtered);
            std::array<float, kHistogramBins> forward{};
            std::array<float, kHistogramBins> backward{};
            for (std::size_t i = 0; i < kHistogramBins; ++i) {
                const float current = filtered.at(i) / total;
                const float previous = history.scene_histograms[first + i];
                forward.at(i) = current * slash::ln(current / previous);
                backward.at(i) = previous * slash::ln(previous / current);
                if (shift == 1) {
                    kept[first + i] = current;
                }
            }
            const float divergence = std::fabs(tree_sum(forward)) + std::fabs(tree_sum(backward));
            const float result = 1.0F - slash::exp(-divergence);
            sums.at(shift) += to_uint((result / static_cast<float>(kHistograms)) * kFactor);
        }
    }
    const float value = static_cast<float>(std::min(sums[0], std::min(sums[1], sums[2]))) / kFactor;
    history.scene_history = (history.scene_history << 1U) | (value > 0.45F ? 1U : 0U);
    history.scene_histograms = std::move(kept);
    return history.scene_history;
}

// EncodeSearchCoord with the fix for the top left's bias: ties go to the smallest move.
std::uint32_t encode_search(std::uint32_t x, std::uint32_t y) {
    const auto distance = [](std::uint32_t c) {
        return static_cast<std::uint32_t>(std::abs(static_cast<int>(c) - 8));
    };
    return (distance(y) << 12U) | (distance(x) << 8U) | (y << 4U) | x;
}

using Vectors = std::vector<Int2>;

// ComputeOpticalFlowAdvanced at a level: each block of 8 x 8 pixels of the frame's luma looked
// for within 8 pixels of where the level before put it in the frame before's, the closest match by
// the sum of differences; at the finest level, a block that matches as well where it is stays.
// It searches every block of the level, where AMD's dispatch can stop a block short of an odd
// size.
Vectors search(const LumaLevel& first, const LumaLevel& second, const Vectors& prediction,
               const std::array<std::uint32_t, 2>& size, bool predicted, bool finest) {
    Vectors out(std::size_t{size[0]} * size[1], Int2{0, 0});
    std::array<std::array<std::uint8_t, 24>, 24> window{};
    std::array<std::array<std::uint8_t, 8>, 8> block{};
    for (std::uint32_t by = 0; by < size[1]; ++by) {
        for (std::uint32_t bx = 0; bx < size[0]; ++bx) {
            const std::size_t at = (std::size_t{by} * size[0]) + bx;
            const Int2 current = predicted ? prediction[at] : Int2{0, 0};
            const int px = static_cast<int>(bx * kOpticalFlowBlock);
            const int py = static_cast<int>(by * kOpticalFlowBlock);
            const int ox = px + current[0] - static_cast<int>(kSearchRadius);
            const int oy = py + current[1] - static_cast<int>(kSearchRadius);
            for (int row = 0; row < 24; ++row) {
                for (int word = 0; word < 6; ++word) {
                    const std::uint32_t packed = second.packed(ox + (word * 4), oy + row);
                    for (int b = 0; b < 4; ++b) {
                        window.at(static_cast<std::size_t>(row))
                            .at((static_cast<std::size_t>(word) * 4) +
                                static_cast<std::size_t>(b)) =
                            static_cast<std::uint8_t>(
                                (packed >> (8U * static_cast<std::uint32_t>(b))) & 0xFFU);
                    }
                }
            }
            std::uint32_t still = 0;
            for (int row = 0; row < 8; ++row) {
                for (int half = 0; half < 2; ++half) {
                    const std::uint32_t packed = first.packed(px + (half * 4), py + row);
                    if (finest) {
                        still += sad(packed, second.packed(px + (half * 4), py + row));
                    }
                    for (int b = 0; b < 4; ++b) {
                        block.at(static_cast<std::size_t>(row))
                            .at((static_cast<std::size_t>(half) * 4) +
                                static_cast<std::size_t>(b)) =
                            static_cast<std::uint8_t>(
                                (packed >> (8U * static_cast<std::uint32_t>(b))) & 0xFFU);
                    }
                }
            }
            std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t sy = 0; sy < 2 * kSearchRadius; ++sy) {
                for (std::uint32_t sx = 0; sx < 2 * kSearchRadius; ++sx) {
                    std::uint32_t sum = 0;
                    for (std::size_t r = 0; r < 8; ++r) {
                        for (std::size_t c = 0; c < 8; ++c) {
                            sum += static_cast<std::uint32_t>(
                                std::abs(static_cast<int>(window.at(sy + r).at(sx + c)) -
                                         static_cast<int>(block.at(r).at(c))));
                        }
                    }
                    best = std::min(best, (sum << 16U) | encode_search(sx, sy));
                }
            }
            Int2 moved{
                current[0] + (static_cast<int>(best & 0xFU) - static_cast<int>(kSearchRadius)),
                current[1] +
                    (static_cast<int>((best >> 4U) & 0xFU) - static_cast<int>(kSearchRadius))};
            if (finest && still <= (best >> 16U)) {
                moved = {0, 0};
            }
            out[at] = moved;
        }
    }
    return out;
}

Int2 load_vector(const Vectors& vectors, const std::array<std::uint32_t, 2>& size, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(size[0]) || y >= static_cast<int>(size[1])) {
        return {0, 0};
    }
    return vectors[(static_cast<std::size_t>(y) * size[0]) + static_cast<std::size_t>(x)];
}

// FilterOpticalFlow: each vector replaced by the one of its 3 x 3 neighbors closest to the rest.
Vectors filter(const Vectors& vectors, const std::array<std::uint32_t, 2>& size) {
    Vectors out(vectors.size());
    for (std::uint32_t y = 0; y < size[1]; ++y) {
        for (std::uint32_t x = 0; x < size[0]; ++x) {
            std::array<Int2, 9> near{};
            std::size_t n = 0;
            for (int xx = -1; xx < 2; ++xx) {
                for (int yy = -1; yy < 2; ++yy) {
                    near.at(n++) = load_vector(vectors, size, static_cast<int>(x) + xx,
                                               static_cast<int>(y) + yy);
                }
            }
            std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t i = 0; i < 9; ++i) {
                std::uint32_t sum = 0;
                for (std::uint32_t j = 0; j < 9; ++j) {
                    const auto dx = static_cast<std::uint32_t>(near.at(i)[0] - near.at(j)[0]);
                    const auto dy = static_cast<std::uint32_t>(near.at(i)[1] - near.at(j)[1]);
                    sum = (dx * dx) + ((dy * dy) + sum);
                }
                best = std::min((sum << 4U) | i, best);
            }
            out[(std::size_t{y} * size[0]) + x] = near.at(best & 0xFU);
        }
    }
    return out;
}

// ScaleOpticalFlowAdvanced: each block of the next finer level takes, of the four coarser
// vectors nearest it, the one that matches its pixels best, doubled.
Vectors scale(const Vectors& vectors, const std::array<std::uint32_t, 2>& size,
              const std::array<std::uint32_t, 2>& next, const LumaLevel& first,
              const LumaLevel& second) {
    Vectors out(std::size_t{next[0]} * next[1]);
    for (std::uint32_t y = 0; y < next[1]; ++y) {
        for (std::uint32_t x = 0; x < next[0]; ++x) {
            std::uint32_t best_sad = std::numeric_limits<std::uint32_t>::max();
            Int2 best{};
            for (int z = 0; z < 4; ++z) {
                const int x_offset = (z % 2) - 1 + static_cast<int>(x % 2);
                const int y_offset = (z / 2) - 1 + static_cast<int>(y % 2);
                const Int2 nearest = load_vector(vectors, size, static_cast<int>(x / 2) + x_offset,
                                                 static_cast<int>(y / 2) + y_offset);
                std::uint32_t sum = 0;
                for (int n = 0; n < 4; ++n) {
                    const int lx = static_cast<int>(x) * 4;
                    const int ly = (static_cast<int>(y) * 4) + n;
                    sum +=
                        sad(first.packed(lx, ly), second.packed(lx + nearest[0], ly + nearest[1]));
                }
                if (sum < best_sad) {
                    best_sad = sum;
                    best = {nearest[0] * 2, nearest[1] * 2};
                }
            }
            out[(std::size_t{y} * next[0]) + x] = best;
        }
    }
    return out;
}

// --- Frame interpolation --------------------------------------------------------------------

// The frame's constants (cbFI) that the passes read.
struct Constants {
    Int2 render{};
    Int2 display{};
    Int2 flow{};         // GetOpticalFlowSize2
    Int2 flow_texture{}; // the optical flow's vectors at level 0
    Float2 flow_scale{}; // opticalFlowScale: 1 over the display size
    FrameGenerationCamera camera;
    std::uint32_t frames = 0; // FrameIndexSinceLastReset

    [[nodiscard]] float view_space_depth(float device_depth) const {
        if (camera.ortho) {
            return (device_depth - camera.view_depth[0]) / camera.view_depth[1];
        }
        return camera.view_depth[1] / (device_depth - camera.view_depth[0]);
    }
    // GetViewSpacePosition.
    [[nodiscard]] Float3 view_space_position(const Int2& pos, const Int2& size,
                                             float device_depth) const {
        const float z = view_space_depth(device_depth);
        const Float2 ndc{
            ((static_cast<float>(pos[0]) / static_cast<float>(size[0])) * 2.0F) + -1.0F,
            ((static_cast<float>(pos[1]) / static_cast<float>(size[1])) * -2.0F) + 1.0F};
        const float scale = camera.ortho ? 1.0F : z;
        return {(camera.view_depth[2] * ndc[0]) * scale, (camera.view_depth[3] * ndc[1]) * scale,
                z};
    }
};

// A vector field entry (VectorFieldEntry).
struct Entry {
    Float2 motion{};
    float high = 0.0F;
    float low = 0.0F;
    bool valid = false;
    bool primary = false;
    bool in_painted = false;
    float velocity = 0.0F;
    bool neg_outside = false;
    bool pos_outside = false;
};

UInt2 pack_entry(bool primary, std::uint32_t high, std::uint32_t low, const Float2& motion) {
    const std::uint32_t priority = (primary ? kPrimaryBit : 0U) |
                                   ((high & kPriorityHighMax) << kPriorityHighOffset) |
                                   ((low & kPriorityLowMax) << kPriorityLowOffset);
    return {priority | half_bits(motion[0]), priority | half_bits(motion[1])};
}

Entry unpack_entry(const UInt2& packed) {
    Entry e;
    e.high = static_cast<float>((packed[0] >> kPriorityHighOffset) & kPriorityHighMax) /
             static_cast<float>(kPriorityHighMax);
    e.low = static_cast<float>((packed[0] >> kPriorityLowOffset) & kPriorityLowMax) /
            static_cast<float>(kPriorityLowMax);
    e.primary = (packed[0] & kPrimaryBit) != 0;
    e.valid = e.high > 0.0F;
    if (e.valid && !e.primary) {
        e.high = 1.0F - e.high;
    }
    e.motion = {half_value(packed[0] & 0xFFFFU), half_value(packed[1] & 0xFFFFU)};
    return e;
}

// A vector field: two uints a pixel, written by atomic max.
struct Field {
    Int2 size{};
    std::vector<UInt2> values;

    explicit Field(const Int2& s)
        : size(s),
          values(static_cast<std::size_t>(s[0]) * static_cast<std::size_t>(s[1]), UInt2{0, 0}) {}
    [[nodiscard]] UInt2 load(const Int2& pos) const {
        return on_screen(pos, size) ? values[index_of(pos, size)] : UInt2{0, 0};
    }
    // UpdateGameMotionVectorFieldEx: returns the larger of what the two held.
    std::uint32_t update(const Int2& pos, const UInt2& packed) {
        UInt2& value = values[index_of(pos, size)];
        const std::uint32_t before = std::max(value[0], value[1]);
        value = {std::max(value[0], packed[0]), std::max(value[1], packed[1])};
        return before;
    }
};

// A pyramid of RGBA half floats, level k at the size over 2^(k + 1).
using Pyramid = std::vector<std::vector<Float4>>;

Int2 level_size(const Int2& size, int level) {
    return {size[0] >> (level + 1), size[1] >> (level + 1)};
}

// The inpainting pyramids, a level a pass: each texel reduced from the 2 x 2 under it, the first
// level's from the source, each rounded to half floats as stored.
template <typename Source, typename Reduce>
Pyramid make_pyramid(const Int2& size, int levels, Source source, Reduce reduce) {
    Pyramid pyramid;
    for (int level = 0; level < levels; ++level) {
        const Int2 s = level_size(size, level);
        if (s[0] < 1 || s[1] < 1) {
            break;
        }
        std::vector<Float4> texels(static_cast<std::size_t>(s[0]) * static_cast<std::size_t>(s[1]));
        for (int y = 0; y < s[1]; ++y) {
            for (int x = 0; x < s[0]; ++x) {
                std::array<Float4, 4> v{};
                for (std::size_t i = 0; i < 4; ++i) {
                    const Int2 at{(x * 2) + kBilinearOffsets.at(i)[0],
                                  (y * 2) + kBilinearOffsets.at(i)[1]};
                    if (level == 0) {
                        v.at(i) = source(at);
                    } else {
                        const Int2 below = level_size(size, level - 1);
                        v.at(i) = pyramid[static_cast<std::size_t>(level - 1)][index_of(at, below)];
                    }
                }
                Float4 out = reduce(v);
                for (float& c : out) {
                    c = round_half(c);
                }
                texels[index_of({x, y}, s)] = out;
            }
        }
        pyramid.push_back(std::move(texels));
    }
    return pyramid;
}

// Everything the interpolation passes share.
class Interpolation {
public:
    Interpolation(const Constants& k, std::span<const std::uint8_t> previous,
                  std::span<const std::uint8_t> current, const FramePrepared& prepared,
                  const OpticalFlow& flow)
        : k_(k), previous_(previous), current_(current), prepared_(prepared), flow_(flow),
          game_field_(k.render), flow_field_(k.flow),
          interpolated_depth_(pixel_count(k.render), 0U),
          disocclusion_(pixel_count(k.render) * 2, 0.0F) {}

    // The passes that prepare the vector fields, when the frame isn't a reset.
    void prepare() {
        reconstruct_previous_depth();
        game_vector_field();
        game_pyramid_ = make_pyramid(
            k_.render, kGameFieldLevels,
            [&](const Int2& at) {
                const Entry e = unpack_entry(game_field_.load(at));
                return Float4{e.motion[0], e.motion[1], e.high, e.low};
            },
            [](const std::array<Float4, 4>& v) {
                Float4 sum{};
                float weights = 0.0F;
                for (const Float4& s : v) {
                    const float weight = s[2] > 0.0F ? 1.0F : 0.0F;
                    for (std::size_t c = 0; c < 4; ++c) {
                        sum.at(c) += s.at(c) * weight;
                    }
                    weights += weight;
                }
                const float divide = weights > kEpsilon ? weights : 1.0F;
                for (float& c : sum) {
                    c /= divide;
                }
                return sum;
            });
        optical_flow_vector_field();
        disocclusion_mask();
    }

    // computeFrameinterpolation, then the inpainting pyramid and computeInpainting, into out.
    void interpolate(std::span<std::uint8_t> out) {
        const Int2 display = k_.display;
        std::vector<std::uint8_t> mask(pixel_count(display), 0);
        const Int2 range = dispatched(display);
        for (int y = 0; y < range[1]; ++y) {
            for (int x = 0; x < range[0]; ++x) {
                const Int2 pos{x, y};
                if (!on_screen(pos, display)) {
                    continue;
                }
                Float3 color{};
                float in_painting = 0.0F;
                if (k_.frames == 0) {
                    color = current_color(pos);
                } else {
                    interpolated_color(pos, color, in_painting);
                }
                const std::size_t at = index_of(pos, display);
                for (std::size_t c = 0; c < 3; ++c) {
                    out[(at * 4) + c] = to_byte(color.at(c));
                }
                out[(at * 4) + 3] = 255;
                mask[at] = to_byte(in_painting);
            }
        }
        const Pyramid pyramid = make_pyramid(
            display, kInpaintingLevels,
            [&](const Int2& at) {
                const std::size_t i = index_of(at, display);
                return Float4{from_byte(out[i * 4]), from_byte(out[(i * 4) + 1]),
                              from_byte(out[(i * 4) + 2]), saturate(1.0F - from_byte(mask[i]))};
            },
            [](const std::array<Float4, 4>& v) {
                const float sum = ((v[0][3] + v[1][3]) + v[2][3]) + v[3][3];
                if (sum == 0.0F) {
                    return Float4{};
                }
                Float4 mixed{};
                for (std::size_t c = 0; c < 4; ++c) {
                    mixed.at(c) = ((((v[0].at(c) * v[0][3]) + (v[1].at(c) * v[1][3])) +
                                    (v[2].at(c) * v[2][3])) +
                                   (v[3].at(c) * v[3][3])) /
                                  sum;
                }
                return mixed;
            });
        for (int y = 0; y < display[1]; ++y) {
            for (int x = 0; x < display[0]; ++x) {
                const std::size_t at = index_of({x, y}, display);
                const float weight = from_byte(mask[at]);
                if (!(weight > kEpsilon)) {
                    continue;
                }
                const Float3 color{from_byte(out[at * 4]), from_byte(out[(at * 4) + 1]),
                                   from_byte(out[(at * 4) + 2])};
                const Float3 filled = lerp3(color, inpainting(pyramid, {x, y}), weight);
                for (std::size_t c = 0; c < 3; ++c) {
                    out[(at * 4) + c] = to_byte(filled.at(c));
                }
            }
        }
    }

private:
    static std::size_t pixel_count(const Int2& size) {
        return static_cast<std::size_t>(size[0]) * static_cast<std::size_t>(size[1]);
    }

    [[nodiscard]] Float3 color_of(std::span<const std::uint8_t> image, const Int2& pos) const {
        const std::size_t at = index_of(pos, k_.display) * 4;
        return {from_byte(image[at]), from_byte(image[at + 1]), from_byte(image[at + 2])};
    }
    [[nodiscard]] Float3 current_color(const Int2& pos) const { return color_of(current_, pos); }
    // SamplePreviousBackbuffer and SampleCurrentBackbuffer.
    [[nodiscard]] Float3 sample(std::span<const std::uint8_t> image, const Float2& uv) const {
        const std::array<float, 3> c =
            sample_linear<3>(uv, k_.display, [&](const Int2& at) { return color_of(image, at); });
        return {c[0], c[1], c[2]};
    }

    [[nodiscard]] float dilated_depth(const Int2& pos) const {
        return on_screen(pos, k_.render) ? prepared_.dilated_depth[index_of(pos, k_.render)] : 0.0F;
    }
    [[nodiscard]] Float2 dilated_motion(const Int2& pos) const {
        if (!on_screen(pos, k_.render)) {
            return {0.0F, 0.0F};
        }
        const std::size_t at = index_of(pos, k_.render) * 2;
        return {prepared_.dilated_motion[at], prepared_.dilated_motion[at + 1]};
    }

    // ReconstructPrevDepth of reconstructPreviousDepth: each pixel's depth pushed halfway back
    // along its motion, the nearest kept.
    void reconstruct_previous_depth() {
        const Int2 range = dispatched(k_.render);
        const Float2 size = to_float(k_.render);
        for (int y = 0; y < range[1]; ++y) {
            for (int x = 0; x < range[0]; ++x) {
                const Int2 pos{x, y};
                const Float2 motion = mul2(dilated_motion(pos), 0.5F);
                const float depth = dilated_depth(pos);
                const Float2 uv = div2(add2(to_float(pos), {0.5F, 0.5F}), size);
                const Bilinear b = bilinear(add2(uv, motion), k_.render);
                for (std::size_t i = 0; i < 4; ++i) {
                    const Int2 at = tap(b, i);
                    if (b.weights.at(i) > kEpsilon && on_screen(at, k_.render)) {
                        std::uint32_t& kept = interpolated_depth_[index_of(at, k_.render)];
                        kept = std::max(kept, slash::bit_cast<std::uint32_t>(depth));
                    }
                }
            }
        }
    }

    // getPriorityFactorFromViewSpaceDepth.
    static std::uint32_t depth_priority(float view_depth) {
        const float scaled = power_of(view_depth, 0.33F);
        const std::uint32_t priority = to_uint((1.0F - (scaled * (1.0F / (1.0F + scaled)))) *
                                               static_cast<float>(kPriorityHighMax));
        return std::max(1U, priority);
    }

    // computeGameFieldMvs: each pixel's vector laid at where it is halfway back, its priority by
    // its depth and how alike it looks in the two frames; then a line of secondary vectors behind
    // it, which fill what it uncovers. Every primary vector goes in before any secondary.
    void game_vector_field() {
        const Int2 range = dispatched(k_.render);
        const Float2 size = to_float(k_.render);
        struct Secondary {
            Float2 location{};
            Float2 motion{};
            Float2 half{};
            std::uint32_t high = 0;
        };
        std::vector<Secondary> secondaries;
        for (int y = 0; y < range[1]; ++y) {
            for (int x = 0; x < range[0]; ++x) {
                const Int2 pos{x, y};
                const Float2 uv = div2(add2(to_float(pos), {0.5F, 0.5F}), size);
                const float depth = dilated_depth(pos);
                const Float2 motion = dilated_motion(pos);
                const Float2 half = mul2(motion, 0.5F);
                const Float2 location = add2(uv, half);
                const std::uint32_t high = depth_priority(k_.view_space_depth(depth));
                const Float3 previous = sample(previous_, add2(uv, motion));
                const Float3 current = sample(current_, uv);
                const float previous_luma = 0.001F + raw_to_luminance(previous);
                const float current_luma = 0.001F + raw_to_luminance(current);
                const std::uint32_t low =
                    to_uint(round_even(min_divided_by_max(previous_luma, current_luma) *
                                       static_cast<float>(kPriorityLowMax))) *
                    (uv_inside(add2(uv, motion)) ? 1U : 0U);
                const UInt2 packed = pack_entry(true, high, low, half);
                const Bilinear b = bilinear(location, k_.render);
                for (std::size_t i = 0; i < 4; ++i) {
                    const Int2 at = tap(b, i);
                    if (on_screen(at, k_.render)) {
                        game_field_.update(at, packed);
                    }
                }
                if (length2(mul2(half, size)) > kEpsilon) {
                    secondaries.push_back(
                        {.location = location, .motion = motion, .half = half, .high = high});
                }
            }
        }
        const float step = length2({1.0F / size[0], 1.0F / size[1]});
        for (const Secondary& s : secondaries) {
            const Float2 direction = normalize2(s.motion);
            const float stop = std::min(length2(s.half), length2({0.5F, 0.5F}));
            const std::uint32_t high = std::max(1U, kPriorityHighMax - s.high);
            bool write = true;
            std::uint32_t primary_hits = 0;
            for (float scale = step; scale <= stop && write; scale += step) {
                const Float2 location = sub2(s.location, mul2(direction, scale));
                const Bilinear b = bilinear(location, k_.render);
                const Float2 to_center = normalize2(sub2({0.5F, 0.5F}, location));
                const std::uint32_t low =
                    to_uint(max0(dot2(to_center, direction)) * static_cast<float>(kPriorityLowMax));
                const UInt2 packed = pack_entry(false, high, low, s.half);
                const Int2 at = tap(b, 0);
                write = write && on_screen(at, k_.render);
                if (write) {
                    const std::uint32_t before = game_field_.update(at, packed);
                    primary_hits += (before & kPrimaryBit) != 0 ? 1U : 0U;
                    write = write && primary_hits <= 3;
                }
            }
        }
    }

    // ComputeMvInpaintingLevel.
    [[nodiscard]] Float4 game_pyramid_level(const Float2& uv, int level, const Int2& size) const {
        const Bilinear b = bilinear(uv, size);
        float sum = 0.0F;
        Float4 color{};
        for (std::size_t i = 0; i < 4; ++i) {
            const Int2 at = tap(b, i);
            if (on_screen(at, size)) {
                const Float4& s =
                    game_pyramid_[static_cast<std::size_t>(level)][index_of(at, size)];
                const float weight = (b.weights.at(i) * (s[2] > 0.0F ? 1.0F : 0.0F)) * s[2];
                sum += weight;
                for (std::size_t c = 0; c < 4; ++c) {
                    color.at(c) += s.at(c) * weight;
                }
            }
        }
        const float divide = sum > 0.0F ? sum : 1.0F;
        for (float& c : color) {
            c /= divide;
        }
        return color;
    }

    // LoadInpaintedGameFieldMv: the game's vector here, or one filled in from the pyramid.
    [[nodiscard]] Entry game_vector(const Float2& uv) const {
        const Int2 pos{to_int(uv[0] * static_cast<float>(k_.render[0])),
                       to_int(uv[1] * static_cast<float>(k_.render[1]))};
        Entry e = unpack_entry(game_field_.load(pos));
        if (!e.valid) {
            Int2 size = k_.render;
            Float4 filled{};
            for (int level = 0; level < kGameFieldLevels && filled[3] == 0.0F; ++level) {
                size = {size[0] / 2, size[1] / 2};
                if (static_cast<std::size_t>(level) < game_pyramid_.size()) {
                    filled = game_pyramid_level(uv, level, size);
                }
            }
            e.motion = {filled[0], filled[1]};
            e.high = filled[2];
            e.low = filled[3];
            e.in_painted = true;
        }
        e.neg_outside = !uv_inside(sub2(uv, e.motion));
        e.pos_outside = !uv_inside(add2(uv, e.motion));
        e.velocity = length2(e.motion);
        return e;
    }

    [[nodiscard]] Float2 flow_vector(const Int2& pos) const {
        if (!on_screen(pos, k_.flow_texture)) {
            return {0.0F, 0.0F};
        }
        const std::size_t at = index_of(pos, k_.flow_texture) * 2;
        return mul2(
            {static_cast<float>(flow_.vectors[at]), static_cast<float>(flow_.vectors[at + 1])},
            k_.flow_scale);
    }

    // computeOpticalFlowVectorField and computeOpticalFlowFieldMvs: the optical flow's vectors
    // smoothed and laid halfway back, their priority by how fast they move.
    void optical_flow_vector_field() {
        const Int2 range = dispatched(k_.flow);
        const Float2 rect = to_float(k_.display);
        const Float2 flow_size = to_float(k_.flow);
        for (int y = 0; y < range[1]; ++y) {
            for (int x = 0; x < range[0]; ++x) {
                const Int2 pos{x, y};
                Float2 average{};
                float weights = 0.0F;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const Float2 v = flow_vector({x + dx, y + dy});
                        const float length = length2(mul2(v, rect));
                        const float weight = max0(512.0F - length) * (length > 1.0F ? 1.0F : 0.0F);
                        average = add2(average, mul2(v, weight));
                        weights += weight;
                    }
                }
                average = {average[0] / weights, average[1] / weights};
                Float2 vector{};
                weights = 0.0F;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const Float2 v = flow_vector({x + dx, y + dy});
                        const float length = length2(mul2(v, rect));
                        const float length_factor =
                            max0(512.0F - length) * (length > 1.0F ? 1.0F : 0.0F);
                        const float weight =
                            max0(power_of(dot2(average, v), 1.25F)) * length_factor;
                        vector = add2(vector, mul2(v, weight));
                        weights += weight;
                    }
                }
                if (weights > kEpsilon) {
                    vector = {vector[0] / weights, vector[1] / weights};
                }
                const Float2 uv = div2(add2(to_float(pos), {0.5F, 0.5F}), flow_size);
                const Float2 half = mul2(vector, 0.5F);
                const Float3 previous = sample(previous_, add2(uv, vector));
                const Float3 current = sample(current_, uv);
                const float previous_luma = 0.001F + raw_to_luminance(previous);
                const float current_luma = 0.001F + raw_to_luminance(current);
                const float velocity = length2(mul2(vector, rect));
                const std::uint32_t high = (velocity > 1.0F ? 1U : 0U) *
                                           to_uint(saturate(velocity / length2(mul2(rect, 0.05F))) *
                                                   static_cast<float>(kPriorityHighMax));
                if (high > 0) {
                    const std::uint32_t low =
                        to_uint(round_even(min_divided_by_max(previous_luma, current_luma) *
                                           static_cast<float>(kPriorityLowMax))) *
                        (uv_inside(add2(uv, vector)) ? 1U : 0U);
                    const UInt2 packed = pack_entry(true, high, low, half);
                    const Bilinear b = bilinear(add2(uv, half), k_.flow);
                    for (std::size_t i = 0; i < 4; ++i) {
                        const Int2 at = tap(b, i);
                        if (on_screen(at, k_.flow)) {
                            flow_field_.update(at, packed);
                        }
                    }
                }
            }
        }
    }

    // SampleOpticalFlowMotionVectorField.
    [[nodiscard]] Entry flow_entry(const Float2& uv) const {
        const Bilinear b = bilinear(uv, k_.flow);
        Entry e;
        float weights = 0.0F;
        for (std::size_t i = 0; i < 4; ++i) {
            const Int2 at = tap(b, i);
            if (on_screen(at, k_.flow)) {
                const float weight = b.weights.at(i);
                const Entry s = unpack_entry(flow_field_.load(at));
                e.motion = add2(e.motion, mul2(s.motion, weight));
                e.high += s.high * weight;
                e.low += s.low * weight;
                weights += weight;
            }
        }
        if (weights > 0.0F) {
            e.motion = {e.motion[0] / weights, e.motion[1] / weights};
            e.high /= weights;
            e.low /= weights;
        }
        e.neg_outside = !uv_inside(sub2(uv, e.motion));
        e.pos_outside = !uv_inside(add2(uv, e.motion));
        e.velocity = length2(e.motion);
        return e;
    }

    // ComputeDepthClip: how much of what is at a depth halfway the frame before (which 0) or
    // this frame (which 1) shows at uv, by AMD's minimum separation.
    [[nodiscard]] float depth_clip(int which, const Float2& uv, float depth) const {
        const float current_view = k_.view_space_depth(depth);
        const Bilinear b = bilinear(uv, k_.render);
        float covered = 0.0F;
        float weights = 0.0F;
        for (std::size_t i = 0; i < 4; ++i) {
            const Int2 at = tap(b, i);
            if (!on_screen(at, k_.render)) {
                continue;
            }
            const float weight = b.weights.at(i);
            if (!(weight > kEpsilon)) {
                continue;
            }
            const float previous_depth =
                which == 0
                    ? slash::bit_cast<float>(prepared_.previous_depth[index_of(at, k_.render)])
                    : prepared_.dilated_depth[index_of(at, k_.render)];
            const float previous_view = k_.view_space_depth(previous_depth);
            const float difference = current_view - previous_view;
            if (difference > 0.0F) {
                const float plane = std::min(previous_depth, depth);
                const Float3 center =
                    k_.view_space_position({to_int(static_cast<float>(k_.render[0]) * 0.5F),
                                            to_int(static_cast<float>(k_.render[1]) * 0.5F)},
                                           k_.render, plane);
                const Float3 corner = k_.view_space_position({0, 0}, k_.render, plane);
                const float half_width = length2(to_float(k_.render));
                const float threshold = std::min(current_view, previous_view);
                const float fov = length3(corner) / length3(center);
                const float required = ((1.37e-05F * fov) * half_width) * threshold;
                covered += ((required / difference) >= 1.0F ? 1.0F : 0.0F) * weight;
                weights += weight;
            }
        }
        return weights > 0.0F ? saturate(1.0F - (covered / weights)) : 0.0F;
    }

    // computeDisocclusionMask: whether what is halfway can be seen in the frame before (x) and
    // in this frame (y).
    void disocclusion_mask() {
        const Int2 range = dispatched(k_.render);
        const Float2 size = to_float(k_.render);
        for (int y = 0; y < range[1]; ++y) {
            for (int x = 0; x < range[0]; ++x) {
                const Int2 pos{x, y};
                if (!on_screen(pos, k_.render)) {
                    continue;
                }
                const auto depth =
                    slash::bit_cast<float>(interpolated_depth_[index_of(pos, k_.render)]);
                const Float2 uv = div2(add2(to_float(pos), {0.5F, 0.5F}), size);
                const Entry game = game_vector(uv);
                const float to_previous = 1.0F - depth_clip(0, add2(uv, game.motion), depth);
                const float to_current = 1.0F - depth_clip(1, sub2(uv, game.motion), depth);
                Float2 mask{to_previous >= kEpsilon ? 1.0F : 0.0F,
                            to_current >= kEpsilon ? 1.0F : 0.0F};
                const Float2 source = mul2(game.motion, 2.0F);
                const Float2 before = mul2(add2(uv, source), size);
                const Float2 after = mul2(sub2(uv, source), size);
                mask[0] = saturate(
                    mask[0] +
                    (on_screen({to_int(before[0]), to_int(before[1])}, k_.render) ? 0.0F : 1.0F));
                mask[1] = saturate(
                    mask[1] +
                    (on_screen({to_int(after[0]), to_int(after[1])}, k_.render) ? 0.0F : 1.0F));
                const std::size_t at = index_of(pos, k_.render) * 2;
                disocclusion_[at] = mask[0];
                disocclusion_[at + 1] = mask[1];
            }
        }
    }

    // SampleTextureBilinear: the frame before's or this frame's colors at uv moved by motion,
    // from the pixels inside the frame.
    struct Source {
        Float3 raw{};
        float weights = 0.0F;
    };
    [[nodiscard]] Source sample_frame(bool current, const Float2& uv, const Float2& motion) const {
        const Bilinear b = bilinear(add2(uv, motion), k_.display);
        Float3 color{};
        float weights = 0.0F;
        for (std::size_t i = 0; i < 4; ++i) {
            const Int2 at = tap(b, i);
            if (at[0] >= 0 && at[0] < k_.display[0] && at[1] >= 0 && at[1] < k_.display[1]) {
                const float weight = b.weights.at(i);
                const Float3 c = color_of(current ? current_ : previous_, at);
                for (std::size_t j = 0; j < 3; ++j) {
                    color.at(j) += c.at(j) * weight;
                }
                weights += weight;
            }
        }
        if (weights != 0.0F) {
            color = {color[0] / weights, color[1] / weights, color[2] / weights};
        } else {
            color = {};
        }
        return {.raw = color, .weights = weights};
    }

    // computeInterpolatedColor: the two frames blended halfway along the game's vectors and
    // along the optical flow's, the one whose two colors agree better kept; and how much of it
    // the inpainting fills.
    void interpolated_color(const Int2& pos, Float3& color, float& in_painting) const {
        const Float2 uv =
            div2(add2(to_float(pos), {0.5F, 0.5F}), to_float(k_.display)); // the rect is the frame
        const Entry game = game_vector(uv);
        const Entry flow = flow_entry(uv);
        const std::array<float, 2> mask = sample_linear<2>(uv, k_.render, [&](const Int2& at) {
            const std::size_t i = index_of(at, k_.render) * 2;
            return std::array<float, 2>{disocclusion_[i], disocclusion_[i + 1]};
        });
        Float2 disocclusion{saturate(mask[0]) == 1.0F ? 1.0F : 0.0F,
                            saturate(mask[1]) == 1.0F ? 1.0F : 0.0F};
        const Source previous_game = sample_frame(false, uv, game.motion);
        const Source current_game = sample_frame(true, uv, {-game.motion[0], -game.motion[1]});
        const Source previous_flow = sample_frame(false, uv, flow.motion);
        const Source current_flow = sample_frame(true, uv, {-flow.motion[0], -flow.motion[1]});

        disocclusion[0] *= game.pos_outside ? 0.0F : 1.0F;
        disocclusion[1] *= game.neg_outside ? 0.0F : 1.0F;
        in_painting =
            saturate(std::max(in_painting, length2(disocclusion) <= kEpsilon ? 1.0F : 0.0F));
        float t = 0.5F;
        t += 0.5F * (1.0F - disocclusion[0]);
        t -= 0.5F * (1.0F - disocclusion[1]);
        color = lerp3(previous_game.raw, current_game.raw, saturate(t));
        const float disoccluded = saturate(1.0F - std::min(disocclusion[0], disocclusion[1]));
        if (previous_game.weights == 0.0F) {
            color = current_game.raw;
        } else if (current_game.weights == 0.0F) {
            color = previous_game.raw;
        }
        if (previous_game.weights == 0.0F && current_game.weights == 0.0F) {
            in_painting = 1.0F;
        }

        float flow_t = 0.5F;
        if (previous_flow.weights > 0.0F && current_flow.weights > 0.0F) {
            flow_t = 0.5F;
        } else if (previous_flow.weights > 0.0F) {
            flow_t = 0.0F;
        } else {
            flow_t = 1.0F;
        }
        const Float3 flow_color = lerp3(previous_flow.raw, current_flow.raw, flow_t);
        const float flow_similarity = normalized_dot3(previous_flow.raw, current_flow.raw);
        float game_similarity = normalized_dot3(previous_game.raw, current_game.raw);
        game_similarity = lerp(std::max(kEpsilon, game_similarity), 1.0F, saturate(disoccluded));
        float bias = saturate(game_similarity / std::max(kEpsilon, flow_similarity));
        bias = lerp(bias, 1.0F, k_.frames < 10 ? 1.0F : 0.0F);
        color = lerp3(flow_color, color, saturate(bias));
    }

    // ComputeInpainting: what the pyramid's levels hold around a pixel, the finer weighing more.
    [[nodiscard]] Float3 inpainting(const Pyramid& pyramid, const Int2& pos) const {
        const Float2 uv = div2(add2(to_float(pos), {0.5F, 0.5F}), to_float(k_.display));
        Float4 color{};
        Int2 size = k_.display;
        for (int level = 0; level < kInpaintingLevels; ++level) {
            size = {size[0] / 2, size[1] / 2};
            if (static_cast<std::size_t>(level) >= pyramid.size()) {
                continue;
            }
            const Bilinear b = bilinear(uv, size);
            Float4 mip{};
            for (std::size_t i = 0; i < 4; ++i) {
                const Int2 at = tap(b, i);
                if (on_screen(at, size)) {
                    const Float4& s = pyramid[static_cast<std::size_t>(level)][index_of(at, size)];
                    const float weight = b.weights.at(i) * (s[3] > 0.0F ? 1.0F : 0.0F);
                    for (std::size_t c = 0; c < 3; ++c) {
                        mip.at(c) += s.at(c) * weight;
                    }
                    mip[3] += weight;
                }
            }
            if (mip[3] > 0.0F) {
                // pow(1 - level / 10, 3), multiplied out.
                const float base = 1.0F - (static_cast<float>(level) / 10.0F);
                const float level_weight = ((base * base) * base) * mip[3];
                for (std::size_t c = 0; c < 3; ++c) {
                    color.at(c) += (mip.at(c) / mip[3]) * level_weight;
                }
                color[3] += level_weight;
            }
        }
        return {color[0] / color[3], color[1] / color[3], color[2] / color[3]};
    }

    const Constants& k_;
    std::span<const std::uint8_t> previous_;
    std::span<const std::uint8_t> current_;
    const FramePrepared& prepared_;
    const OpticalFlow& flow_;
    Field game_field_;
    Field flow_field_;
    std::vector<std::uint32_t> interpolated_depth_;
    std::vector<float> disocclusion_;
    Pyramid game_pyramid_;
};

} // namespace

std::array<std::uint32_t, 2> optical_flow_size(std::uint32_t width, std::uint32_t height,
                                               std::uint32_t level) {
    std::array<std::uint32_t, 2> size{(width + kOpticalFlowBlock - 1) / kOpticalFlowBlock,
                                      (height + kOpticalFlowBlock - 1) / kOpticalFlowBlock};
    for (std::uint32_t i = 0; i < level; ++i) {
        size = {(size[0] + 1) / 2, (size[1] + 1) / 2};
    }
    return size;
}

OpticalFlow run_optical_flow(std::span<const std::uint8_t> rgba, std::uint32_t width,
                             std::uint32_t height, bool reset, OpticalFlowHistory& history) {
    if (reset || history.width != width || history.height != height) {
        history = OpticalFlowHistory{};
        history.width = width;
        history.height = height;
        for (std::uint32_t level = 0; level < kOpticalFlowLevels; ++level) {
            history.luma.at(level).assign(std::size_t{width >> level} * (height >> level), 0);
        }
        history.scene_histograms.assign(std::size_t{kHistograms} * kHistogramBins, 0.0F);
    } else {
        ++history.frame_index;
    }
    std::array<std::vector<std::uint8_t>, kOpticalFlowLevels> pyramid =
        luma_pyramid(rgba, width, height);
    const auto level_of = [&](const std::vector<std::uint8_t>& pixels, std::uint32_t level) {
        return LumaLevel{.pixels = pixels.data(),
                         .width = static_cast<int>(width >> level),
                         .height = static_cast<int>(height >> level)};
    };
    OpticalFlow flow;
    flow.scene_history = scene_change(scene_histograms(level_of(pyramid[0], 0)), history);
    const bool scene_changed = history.frame_index <= 5 || (flow.scene_history & 0xFU) != 0;
    const std::array<std::uint32_t, 2> finest = optical_flow_size(width, height, 0);
    flow.width = finest[0];
    flow.height = finest[1];
    Vectors prediction;
    Vectors vectors;
    for (int level = static_cast<int>(kOpticalFlowLevels) - 1; level >= 0; --level) {
        const auto l = static_cast<std::uint32_t>(level);
        const std::array<std::uint32_t, 2> size = optical_flow_size(width, height, l);
        const LumaLevel first = level_of(pyramid.at(l), l);
        const LumaLevel second = level_of(history.luma.at(l), l);
        const Vectors searched =
            scene_changed ? Vectors(std::size_t{size[0]} * size[1], Int2{0, 0})
                          : search(first, second, prediction, size,
                                   level != static_cast<int>(kOpticalFlowLevels) - 1, level == 0);
        vectors = filter(searched, size);
        if (level > 0) {
            const std::array<std::uint32_t, 2> next = optical_flow_size(width, height, l - 1);
            prediction = scene_changed ? Vectors(std::size_t{next[0]} * next[1], Int2{0, 0})
                                       : scale(vectors, size, next, first, second);
        }
    }
    flow.vectors.resize(vectors.size() * 2);
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        flow.vectors[i * 2] = vectors[i][0];
        flow.vectors[(i * 2) + 1] = vectors[i][1];
    }
    history.luma = std::move(pyramid);
    return flow;
}

bool uses_frame_generation(const Scene3D& scene) {
    return scene.frame_generation && scene.method == RenderMethod::ForwardPlus && !scene.capture &&
           !scene.path_trace;
}

FrameGenerationCamera frame_generation_camera(const Mat4& projection) {
    // AMD's setupDeviceDepthToViewSpaceDepthParams from the projection itself, as FSR 2's.
    const Mat4& p = projection;
    const bool ortho = p[11] == 0.0F;
    const float a = p[10];
    const float b = p[14];
    // The horizontal field of view's tangent, which AMD works out through atan and tan.
    return {.view_depth = {ortho ? b : -a, ortho ? -a : b, 1.0F / p[0], 1.0F / p[5]},
            .ortho = ortho,
            .tan_half_fov = 1.0F / p[0]};
}

FramePrepared prepare_frame_generation(const FrameGenerationInputs& inputs,
                                       std::uint32_t display_width, std::uint32_t display_height) {
    const Int2 size{static_cast<int>(inputs.width), static_cast<int>(inputs.height)};
    const Float2 display{static_cast<float>(display_width), static_cast<float>(display_height)};
    FramePrepared out;
    out.width = inputs.width;
    out.height = inputs.height;
    out.camera = inputs.camera;
    const std::size_t pixels = std::size_t{inputs.width} * inputs.height;
    out.dilated_depth.assign(pixels, 0.0F);
    out.dilated_motion.assign(pixels * 2, 0.0F);
    out.previous_depth.assign(pixels, 0U);
    const auto depth_at = [&](const Int2& pos) {
        return on_screen(pos, size) ? inputs.depth[index_of(pos, size)] : 0.0F;
    };
    // LoadInputMotionVector, a pixel where nothing drew worked out from the camera.
    const auto motion_at = [&](const Int2& pos) -> Float2 {
        if (!on_screen(pos, size)) {
            return {0.0F, 0.0F};
        }
        const std::size_t at = index_of(pos, size);
        if (inputs.depth[at] == 0.0F) {
            const Float2 uv{(static_cast<float>(pos[0]) + 0.5F) / static_cast<float>(size[0]),
                            (static_cast<float>(pos[1]) + 0.5F) / static_cast<float>(size[1])};
            return derive_motion_vector(inputs.reprojection, uv, 0.0F);
        }
        return {inputs.motion[at * 2], inputs.motion[(at * 2) + 1]};
    };
    constexpr std::array<Int2, 9> kOffsets{Int2{0, 0},  Int2{1, 0},   Int2{0, 1},
                                           Int2{0, -1}, Int2{-1, 0},  Int2{-1, 1},
                                           Int2{1, 1},  Int2{-1, -1}, Int2{1, -1}};
    const Int2 range = dispatched(size);
    for (int y = 0; y < range[1]; ++y) {
        for (int x = 0; x < range[0]; ++x) {
            // FindNearestDepth, depth reversed.
            const Int2 pos{x, y};
            Int2 nearest_at = pos;
            float nearest = depth_at(pos);
            for (std::size_t i = 1; i < kOffsets.size(); ++i) {
                const Int2 at{x + kOffsets.at(i)[0], y + kOffsets.at(i)[1]};
                if (on_screen(at, size) && depth_at(at) > nearest) {
                    nearest_at = at;
                    nearest = depth_at(at);
                }
            }
            Float2 motion = motion_at(nearest_at);
            if (on_screen(pos, size)) {
                const std::size_t at = index_of(pos, size);
                out.dilated_depth[at] = nearest;
                out.dilated_motion[at * 2] = round_half(motion[0]);
                out.dilated_motion[(at * 2) + 1] = round_half(motion[1]);
            }
            // ReconstructPrevDepth.
            motion = mul2(motion, length2(mul2(motion, display)) > 0.1F ? 1.0F : 0.0F);
            const Float2 uv{(static_cast<float>(x) + 0.5F) / static_cast<float>(size[0]),
                            (static_cast<float>(y) + 0.5F) / static_cast<float>(size[1])};
            const Bilinear b = bilinear(add2(uv, motion), size);
            for (std::size_t i = 0; i < 4; ++i) {
                const Int2 at = tap(b, i);
                if (b.weights.at(i) > kEpsilon && on_screen(at, size)) {
                    std::uint32_t& kept = out.previous_depth[index_of(at, size)];
                    kept = std::max(kept, slash::bit_cast<std::uint32_t>(nearest));
                }
            }
        }
    }
    return out;
}

void run_frame_generation(std::span<const std::uint8_t> current, std::uint32_t width,
                          std::uint32_t height, const FramePrepared& prepared, bool reset,
                          FrameGenerationHistory& history, std::span<std::uint8_t> out) {
    reset = reset || history.width != width || history.height != height ||
            history.previous.size() != current.size();
    const OpticalFlow flow = run_optical_flow(current, width, height, reset, history.flow);
    // setupFrameinterpolationResources.
    if (reset || (flow.scene_history & 0xFU) != 0) {
        history.frames = 0;
    } else {
        ++history.frames;
    }
    if (reset) {
        history.previous.assign(current.begin(), current.end());
    }
    Constants k;
    k.render = {static_cast<int>(prepared.width), static_cast<int>(prepared.height)};
    k.display = {static_cast<int>(width), static_cast<int>(height)};
    k.flow_scale = {1.0F / static_cast<float>(width), 1.0F / static_cast<float>(height)};
    // GetOpticalFlowSize, from the scale as AMD's shader works it out.
    k.flow = {to_int((1.0F / k.flow_scale[0]) / static_cast<float>(kOpticalFlowBlock)),
              to_int((1.0F / k.flow_scale[1]) / static_cast<float>(kOpticalFlowBlock))};
    k.flow_texture = {static_cast<int>(flow.width), static_cast<int>(flow.height)};
    k.camera = prepared.camera;
    k.frames = history.frames;
    Interpolation interpolation(k, history.previous, current, prepared, flow);
    if (!reset) {
        interpolation.prepare();
    }
    interpolation.interpolate(out);
    history.width = width;
    history.height = height;
    history.previous.assign(current.begin(), current.end());
}

} // namespace slash::render::detail
