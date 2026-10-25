// The CPU twin of shaders/fsr3_*.hlsl, ported from AMD's FidelityFX SDK 1.1.4
// (ffx_fsr3upscaler_*.h).
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

#include "fsr3.hpp"

#include "forward_post.hpp"

#include "slash/core/bits.hpp"
#include "slash/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

// The CPU twin of shaders/fsr3*.hlsl, the upscaler of AMD's FidelityFX Super Resolution 3.1. Each
// function follows the one of the same name in AMD's code, line for line where it can, and libm's
// functions are slash's, so every machine draws the same pixels. Keep the two in step.
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
namespace slash::render::detail {

namespace {

using Float2 = std::array<float, 2>;
using Int2 = std::array<int, 2>;

constexpr float kFp16Min = 6.10e-05F;
constexpr float kFp16Max = 65504.0F;
constexpr float kEpsilon = kFp16Min;
constexpr float kTonemapEpsilon = kFp16Min;
constexpr float kFp32Max = 3.402823466e+38F;
constexpr float kFp32Min = 1.175494351e-38F;
constexpr float kReconstructedDepthBilinearWeightThreshold = kEpsilon * 10.0F;
constexpr float kUpsampleLanczosWeightScale = 1.0F / 16.0F;
constexpr float kAverageLanczosWeightPerFrame = 0.74F * kUpsampleLanczosWeightScale;
constexpr float kLockThreshold = 1.0F;
constexpr float kLockMax = 2.0F;
constexpr std::size_t kReactive = 0;
constexpr std::size_t kDisocclusion = 1;
constexpr std::size_t kShadingChange = 2;
constexpr std::size_t kAccumulation = 3;
constexpr float kVelocityFactor = 1.0F;
constexpr float kAccumulationAddedPerFrame = 1.0F / 3.0F;
constexpr float kMinDisocclusionAccumulation = -1.0F / 3.0F;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::size_t kSetSize = 5;

float saturate(float x) {
    return std::clamp(x, 0.0F, 1.0F);
}
float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}
Float3 lerp3(const Float3& a, const Float3& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)};
}
Float4 lerp4(const Float4& a, const Float4& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t), lerp(a[3], b[3], t)};
}
// AMD's ffxPow for x of 0 or more: 0 to any power is 0.
float power_of(float x, float y) {
    return x <= 0.0F ? 0.0F : slash::exp(y * slash::ln(x));
}
float sine(float x) {
    return slash::cos_sin(x).y;
}
float sign_of(float x) {
    if (x > 0.0F) {
        return 1.0F;
    }
    return x < 0.0F ? -1.0F : 0.0F;
}
// round(x) > 1 as AMD's passes test it: a half rounded either way gives the same answer.
bool rounds_above_one(float x) {
    return std::floor(x + 0.5F) > 1.0F;
}
float length2(const Float2& v) {
    return std::sqrt((v[0] * v[0]) + (v[1] * v[1]));
}
Float3 add3(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Float3 sub3(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Float3 mul3(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
Float3 mul3(const Float3& a, const Float3& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}
Float3 div3(const Float3& a, float s) {
    return {a[0] / s, a[1] / s, a[2] / s};
}
Float3 div3(const Float3& a, const Float3& b) {
    return {a[0] / b[0], a[1] / b[1], a[2] / b[2]};
}
Float3 min3(const Float3& a, const Float3& b) {
    return {std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2])};
}
Float3 max3(const Float3& a, const Float3& b) {
    return {std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2])};
}
float dot3(const Float3& a, const Float3& b) {
    return ((a[0] * b[0]) + (a[1] * b[1])) + (a[2] * b[2]);
}
float dot2(const Float2& a, const Float2& b) {
    return (a[0] * b[0]) + (a[1] * b[1]);
}

// A texel value as a GPU stores it in a texture of 8-bit unorms.
float round_unorm8(float value) {
    return std::nearbyint(saturate(value) * 255.0F) / 255.0F;
}

Float3 ycocg_to_rgb(const Float3& c) {
    return {c[0] + c[1] - c[2], c[0] + c[2], c[0] - c[1] - c[2]};
}
Float3 rgb_to_ycocg(const Float3& c) {
    return {(0.25F * c[0]) + (0.5F * c[1]) + (0.25F * c[2]), (0.5F * c[0]) - (0.5F * c[2]),
            (-0.25F * c[0]) + (0.5F * c[1]) - (0.25F * c[2])};
}
float rgb_to_luma(const Float3& c) {
    return (c[0] * 0.2126F) + (c[1] * 0.7152F) + (c[2] * 0.0722F);
}
Float3 tonemap(const Float3& c) {
    return div3(c, std::max(std::max(0.0F, c[0]), std::max(c[1], c[2])) + 1.0F);
}
Float3 inverse_tonemap(const Float3& c) {
    return div3(c, std::max(kTonemapEpsilon, 1.0F - std::max(c[0], std::max(c[1], c[2]))));
}

float get_4k_velocity(const Float2& motion) {
    return length2({motion[0] * 3840.0F, motion[1] * 2160.0F});
}
float min_divided_by_max(float v0, float v1, float on_zero) {
    const float m = std::max(v0, v1);
    return m != 0.0F ? std::min(v0, v1) / m : on_zero;
}
float reconstructed_depth_mv_px_threshold(float nearest_depth_in_meters) {
    return lerp(0.25F, 0.75F, saturate(nearest_depth_in_meters / 100.0F));
}

bool is_uv_inside(const Float2& uv) {
    return (uv[0] >= 0.0F && uv[0] <= 1.0F) && (uv[1] >= 0.0F && uv[1] <= 1.0F);
}
// FSR 3's ClampLoad: inside the texture whatever the offset.
Int2 clamp_load(const Int2& sample, const Int2& offset, const Int2& size) {
    return {std::max(0, std::min(sample[0] + offset[0], size[0] - 1)),
            std::max(0, std::min(sample[1] + offset[1], size[1] - 1))};
}
Float2 clamp_uv(const Float2& uv, const Int2& texture_size, const Int2& resource_size) {
    Float2 out{};
    for (std::size_t i = 0; i < 2; ++i) {
        const float location = uv.at(i) * static_cast<float>(texture_size.at(i));
        const float clamped =
            std::max(0.5F, std::min(location, static_cast<float>(texture_size.at(i)) - 0.5F));
        out.at(i) = clamped / static_cast<float>(resource_size.at(i));
    }
    return out;
}
bool is_on_screen(const Int2& pos, const Int2& size) {
    return pos[0] >= 0 && pos[1] >= 0 && pos[0] < size[0] && pos[1] < size[1];
}
// ffx_fsr3upscaler_sample.h's ClampCoord: at least one texel in from the edges.
Int2 clamp_coord(const Int2& sample, const Int2& offset, const Int2& size) {
    Int2 r{std::max(1, std::min(sample[0] + offset[0], size[0] - 2)),
           std::max(1, std::min(sample[1] + offset[1], size[1] - 2))};
    return {std::clamp(r[0], 0, size[0] - 1), std::clamp(r[1], 0, size[1] - 1)};
}
Float2 uv_of(const Int2& pos, const Int2& size) {
    return {(static_cast<float>(pos[0]) + 0.5F) / static_cast<float>(size[0]),
            (static_cast<float>(pos[1]) + 0.5F) / static_cast<float>(size[1])};
}
Float2 jittered_uv(const Float2& uv, const Float2& jitter, const Int2& size) {
    return {uv[0] + (jitter[0] / static_cast<float>(size[0])),
            uv[1] + (jitter[1] / static_cast<float>(size[1]))};
}
// A texel at uv of a texture of size, as int2(uv * size) truncates it.
Int2 truncated(const Float2& uv, const Int2& size) {
    return {static_cast<int>(uv[0] * static_cast<float>(size[0])),
            static_cast<int>(uv[1] * static_cast<float>(size[1]))};
}

std::size_t pixels(const Int2& size) {
    return static_cast<std::size_t>(size[0]) * static_cast<std::size_t>(size[1]);
}
std::size_t index_of(const Int2& pos, const Int2& size) {
    return (static_cast<std::size_t>(pos[1]) * static_cast<std::size_t>(size[0])) +
           static_cast<std::size_t>(pos[0]);
}

// A texture of the passes, rows top to bottom, its loads kept inside it.
struct Plane {
    Int2 size{};
    std::size_t channels = 4;
    std::span<const float> data;

    [[nodiscard]] std::size_t at(const Int2& p) const {
        const int x = std::clamp(p[0], 0, size[0] - 1);
        const int y = std::clamp(p[1], 0, size[1] - 1);
        return index_of({x, y}, size) * channels;
    }
    [[nodiscard]] Float4 load(const Int2& p) const {
        const std::size_t i = at(p);
        Float4 out{0.0F, 0.0F, 0.0F, 0.0F};
        for (std::size_t c = 0; c < channels; ++c) {
            out.at(c) = data[i + c];
        }
        return out;
    }
    [[nodiscard]] float r(const Int2& p) const { return data[at(p)]; }
    [[nodiscard]] Float2 rg(const Int2& p) const {
        const std::size_t i = at(p);
        return {data[i], data[i + 1]};
    }
    [[nodiscard]] Float3 rgb(const Int2& p) const {
        const std::size_t i = at(p);
        return {data[i], data[i + 1], data[i + 2]};
    }
    // FSR3_LINEAR_SAMPLE: filtered linearly at uv with clamped edges.
    [[nodiscard]] Float4 linear(const Float2& uv) const {
        const float px = (uv[0] * static_cast<float>(size[0])) - 0.5F;
        const float py = (uv[1] * static_cast<float>(size[1])) - 0.5F;
        const float bx = std::floor(px);
        const float by = std::floor(py);
        const Int2 base{static_cast<int>(bx), static_cast<int>(by)};
        const float fx = px - bx;
        const float fy = py - by;
        const Float4 top = lerp4(load(base), load({base[0] + 1, base[1]}), fx);
        const Float4 bottom =
            lerp4(load({base[0], base[1] + 1}), load({base[0] + 1, base[1] + 1}), fx);
        return lerp4(top, bottom, fy);
    }
};

float lanczos2_no_clamp(float x) {
    return std::abs(x) < kEpsilon
               ? 1.0F
               : (sine(kPi * x) / (kPi * x)) * (sine(0.5F * kPi * x) / (0.5F * kPi * x));
}
float lanczos2(float x) {
    return lanczos2_no_clamp(std::min(std::abs(x), 2.0F));
}
Float4 lanczos2(const std::array<Float4, 4>& colors, float t) {
    const std::array<float, 4> weights{lanczos2(-1.0F - t), lanczos2(-0.0F - t),
                                       lanczos2(+1.0F - t), lanczos2(+2.0F - t)};
    const float sum = weights[0] + weights[1] + weights[2] + weights[3];
    Float4 out{};
    for (std::size_t c = 0; c < 4; ++c) {
        out.at(c) = ((weights[0] * colors[0].at(c)) + (weights[1] * colors[1].at(c)) +
                     (weights[2] * colors[2].at(c)) + (weights[3] * colors[3].at(c))) /
                    sum;
    }
    return out;
}
float lanczos2_approx_sq(float x2) {
    x2 = std::min(x2, 4.0F);
    const float a = ((2.0F / 5.0F) * x2) - 1.0F;
    const float b = ((1.0F / 4.0F) * x2) - 1.0F;
    return (((25.0F / 16.0F) * a * a) - ((25.0F / 16.0F) - 1.0F)) * (b * b);
}

// FSR3_LANCZOS2_SAMPLE in fsr3.hlsl.
Float4 lanczos2_sample(const Plane& plane, const Float2& uv) {
    Float2 px{(uv[0] * static_cast<float>(plane.size[0])) - 0.5F,
              (uv[1] * static_cast<float>(plane.size[1])) - 0.5F};
    const Float2 frac{px[0] - std::floor(px[0]), px[1] - std::floor(px[1])};
    px[0] = std::max(0.0F, std::min(static_cast<float>(plane.size[0] - 1), px[0]));
    px[1] = std::max(0.0F, std::min(static_cast<float>(plane.size[1] - 1), px[1]));
    const Int2 base{static_cast<int>(std::floor(px[0])), static_cast<int>(std::floor(px[1]))};
    std::array<Float4, 4> rows{};
    Float4 dering_min{};
    Float4 dering_max{};
    for (int y = 0; y < 4; ++y) {
        std::array<Float4, 4> colors{};
        for (int x = 0; x < 4; ++x) {
            colors.at(static_cast<std::size_t>(x)) =
                plane.load(clamp_coord(base, {x - 1, y - 1}, plane.size));
        }
        rows.at(static_cast<std::size_t>(y)) = lanczos2(colors, frac[0]);
        for (std::size_t c = 0; c < 4; ++c) {
            const float low = std::min(colors[1].at(c), colors[2].at(c));
            const float high = std::max(colors[1].at(c), colors[2].at(c));
            if (y == 1) {
                dering_min.at(c) = low;
                dering_max.at(c) = high;
            } else if (y == 2) {
                dering_min.at(c) = std::min(dering_min.at(c), low);
                dering_max.at(c) = std::max(dering_max.at(c), high);
            }
        }
    }
    Float4 out = lanczos2(rows, frac[1]);
    for (std::size_t c = 0; c < 4; ++c) {
        out.at(c) = std::min(std::max(out.at(c), dering_min.at(c)), dering_max.at(c));
    }
    return out;
}

struct BilinearSamplingData {
    std::array<Int2, 4> offsets{};
    std::array<float, 4> weights{};
    Int2 base{};
};

BilinearSamplingData bilinear_sampling_data(const Float2& uv, const Int2& size) {
    BilinearSamplingData data;
    const Float2 px{(uv[0] * static_cast<float>(size[0])) - 0.5F,
                    (uv[1] * static_cast<float>(size[1])) - 0.5F};
    const Float2 floored{std::floor(px[0]), std::floor(px[1])};
    data.base = {static_cast<int>(floored[0]), static_cast<int>(floored[1])};
    const Float2 frac{px[0] - floored[0], px[1] - floored[1]};
    data.offsets = {Int2{0, 0}, Int2{1, 0}, Int2{0, 1}, Int2{1, 1}};
    data.weights = {(1.0F - frac[0]) * (1.0F - frac[1]), frac[0] * (1.0F - frac[1]),
                    (1.0F - frac[0]) * frac[1], frac[0] * frac[1]};
    return data;
}

// The uniforms as the shaders read them, with fsr3.hlsl's accessors.
struct Constants {
    Int2 render{};
    Int2 display{};
    Int2 half{};
    Float2 jitter{};
    Float2 previous_jitter{};
    Float2 downscale{};
    Float4 view_depth{};
    float exposure = 1.0F;
    float phases = 0.0F;
    float frame_index = 0.0F;
    bool ortho = false;

    explicit Constants(const Fsr3Uniforms& u)
        : render{u.fsr2.sizes[0], u.fsr2.sizes[1]}, display{u.fsr2.sizes[2], u.fsr2.sizes[3]},
          half{std::max(render[0] / 2, 1), std::max(render[1] / 2, 1)},
          jitter{u.fsr2.jitter_scale[0], u.fsr2.jitter_scale[1]},
          previous_jitter{u.previous[0], u.previous[1]},
          downscale{u.fsr2.jitter_scale[2], u.fsr2.jitter_scale[3]}, view_depth(u.fsr2.view_depth),
          exposure(u.fsr2.frame[0] == 0.0F ? 1.0F : u.fsr2.frame[0]), phases(u.fsr2.frame[1]),
          frame_index(u.fsr2.frame[2]), ortho(u.fsr2.options[3] != 0) {}

    [[nodiscard]] Int2 hr_pos_from_lr_pos(const Int2& lr) const {
        Float2 out{};
        for (std::size_t i = 0; i < 2; ++i) {
            const float jittered = static_cast<float>(lr.at(i)) + 0.5F - jitter.at(i);
            out.at(i) =
                (jittered / static_cast<float>(render.at(i))) * static_cast<float>(display.at(i));
        }
        return {static_cast<int>(std::floor(out[0])), static_cast<int>(std::floor(out[1]))};
    }
    [[nodiscard]] float view_space_depth(float device_depth) const {
        if (ortho) {
            return (device_depth - view_depth[0]) / view_depth[1];
        }
        return view_depth[1] / (device_depth - view_depth[0]);
    }
    [[nodiscard]] Int2 level_size(int level) const {
        return {std::max(half[0] >> level, 1), std::max(half[1] >> level, 1)};
    }
};

// fsr3_prepare.hlsl.
struct Prepared {
    std::vector<float> dilated_depth;
    std::vector<float> dilated_motion;
    std::vector<float> farthest_depth;
    std::vector<float> luma;
};

Prepared prepare_pass(const Constants& k, const Plane& depth, const Plane& motion,
                      const Plane& color) {
    constexpr std::array<Int2, 9> kOffsets{{
        {0, 0},
        {1, 0},
        {0, 1},
        {0, -1},
        {-1, 0},
        {-1, 1},
        {1, 1},
        {-1, -1},
        {1, -1},
    }};
    Prepared out;
    out.dilated_depth.resize(pixels(k.render));
    out.dilated_motion.resize(pixels(k.render) * 2);
    out.farthest_depth.resize(pixels(k.render));
    out.luma.resize(pixels(k.render));
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            // FindDepthExtents
            Int2 nearest{x, y};
            float nearest_depth = depth.r(nearest);
            float farthest = nearest_depth;
            for (std::size_t i = 1; i < kOffsets.size(); ++i) {
                const Int2 pos{x + kOffsets.at(i)[0], y + kOffsets.at(i)[1]};
                if (is_on_screen(pos, k.render)) {
                    const float d = depth.r(pos);
                    if (d > nearest_depth) {
                        farthest = std::min(farthest, d);
                        nearest = pos;
                        nearest_depth = d;
                    }
                }
            }
            const std::size_t at = index_of({x, y}, k.render);
            out.dilated_depth[at] = nearest_depth;
            const Float2 mv = motion.rg(nearest);
            out.dilated_motion[at * 2] = round_half(mv[0]);
            out.dilated_motion[(at * 2) + 1] = round_half(mv[1]);
            out.farthest_depth[at] = round_half(std::min(k.view_space_depth(farthest), kFp16Max));
            out.luma[at] = round_half(rgb_to_luma(max3({0.0F, 0.0F, 0.0F}, color.rgb({x, y}))));
        }
    }
    return out;
}

// fsr3_reconstruct.hlsl: the nearest depths moved to where they were in the frame before, as bits,
// the nearest kept.
std::vector<std::uint32_t> reconstruct_pass(const Constants& k, const Plane& dilated_depth,
                                            const Plane& dilated_motion) {
    std::vector<std::uint32_t> reconstructed(pixels(k.render), 0U);
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            const float depth = dilated_depth.r({x, y});
            Float2 mv = dilated_motion.rg({x, y});
            const float nearest_in_meters = std::min(k.view_space_depth(depth), kFp16Max);
            const float keep =
                get_4k_velocity(mv) > reconstructed_depth_mv_px_threshold(nearest_in_meters) ? 1.0F
                                                                                             : 0.0F;
            mv = {mv[0] * keep, mv[1] * keep};
            const Float2 uv = uv_of({x, y}, k.render);
            const BilinearSamplingData info =
                bilinear_sampling_data({uv[0] + mv[0], uv[1] + mv[1]}, k.render);
            for (std::size_t i = 0; i < 4; ++i) {
                if (info.weights.at(i) > kReconstructedDepthBilinearWeightThreshold) {
                    const Int2 store{info.base[0] + info.offsets.at(i)[0],
                                     info.base[1] + info.offsets.at(i)[1]};
                    if (is_on_screen(store, k.render)) {
                        std::uint32_t& slot = reconstructed[index_of(store, k.render)];
                        slot = std::max(slot, slash::bit_cast<std::uint32_t>(depth));
                    }
                }
            }
        }
    }
    return reconstructed;
}

using SampleSet = std::array<float, kSetSize>;

void compare_swap(SampleSet& set, std::size_t i, std::size_t j) {
    const float tmp = std::min(set.at(i), set.at(j));
    set.at(j) = std::max(set.at(i), set.at(j));
    set.at(i) = tmp;
}

void sort_set(SampleSet& set) {
    compare_swap(set, 0, 3);
    compare_swap(set, 1, 4);
    compare_swap(set, 0, 2);
    compare_swap(set, 1, 3);
    compare_swap(set, 0, 1);
    compare_swap(set, 2, 4);
    compare_swap(set, 1, 2);
    compare_swap(set, 3, 4);
    compare_swap(set, 2, 3);
}

float compute_minimum_difference(SampleSet set0, SampleSet set1) {
    float min_diff = kFp16Max - 1.0F;
    std::size_t a = 0;
    std::size_t b = 0;
    sort_set(set0);
    sort_set(set1);
    const float max_of = std::min(set0[kSetSize - 1], set1[kSetSize - 1]);
    if (max_of > kFp32Min) {
        for (std::size_t i = 0; i < kSetSize && (min_diff < kFp16Max); i++) {
            float diff = set0.at(a) - set1.at(b);
            if (std::abs(diff) > kFp16Min) {
                diff = sign_of(diff) * (1.0F - min_divided_by_max(set0.at(a), set1.at(b), 0.0F));
                min_diff = (std::abs(diff) < std::abs(min_diff)) ? diff : min_diff;
                a += set0.at(a) < set1.at(b) ? 1U : 0U;
                b += set0.at(std::min(a, kSetSize - 1)) >= set1.at(b) ? 1U : 0U;
            } else {
                min_diff = kFp16Max;
            }
        }
    }
    return min_diff * (min_diff < (kFp16Max - 1.0F) ? 1.0F : 0.0F);
}

constexpr std::array<Int2, kSetSize> kSetOffsets{{{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

float compute_diff(const Constants& k, const Plane& luma, const Plane& previous_luma,
                   const Float2& uv, const Float2& motion) {
    SampleSet current{};
    const Float2 uv_jittered = jittered_uv(uv, k.jitter, k.render);
    const Int2 current_base{
        static_cast<int>(std::floor(uv_jittered[0] * static_cast<float>(k.render[0]))),
        static_cast<int>(std::floor(uv_jittered[1] * static_cast<float>(k.render[1])))};
    for (std::size_t i = 0; i < kSetSize; ++i) {
        const Int2 pos = clamp_load(current_base, kSetOffsets.at(i), k.render);
        current.at(i) = std::max(luma.r(pos) * k.exposure, kEpsilon);
    }
    const Float2 previous_jittered = jittered_uv(uv, k.previous_jitter, k.render);
    const Float2 reprojected{previous_jittered[0] + motion[0], previous_jittered[1] + motion[1]};
    if (!is_uv_inside(reprojected)) {
        return 0.0F;
    }
    const Int2 previous_base{
        static_cast<int>(std::floor(reprojected[0] * static_cast<float>(k.render[0]))),
        static_cast<int>(std::floor(reprojected[1] * static_cast<float>(k.render[1])))};
    SampleSet previous{};
    for (std::size_t i = 0; i < kSetSize; ++i) {
        const Int2 pos = clamp_load(previous_base, kSetOffsets.at(i), k.render);
        previous.at(i) = std::max((previous_luma.r(pos) * 1.0F) * k.exposure, kEpsilon);
    }
    return compute_minimum_difference(current, previous);
}

// fsr3_pyramid.hlsl: the farthest depths and the shading change pyramid's first level.
struct Pyramid {
    std::vector<float> farthest_depth;
    std::vector<float> level;
};

Pyramid pyramid_pass(const Constants& k, const Plane& farthest_depth, const Plane& dilated_motion,
                     const Plane& luma, const Plane& previous_luma) {
    constexpr std::array<Int2, 4> kOffsets{{{0, 0}, {0, 1}, {1, 0}, {1, 1}}};
    Pyramid out;
    out.farthest_depth.resize(pixels(k.half));
    out.level.resize(pixels(k.half) * 2);
    for (int y = 0; y < k.half[1]; ++y) {
        for (int x = 0; x < k.half[0]; ++x) {
            float depth_sum = 0.0F;
            Float2 change_sum{0.0F, 0.0F};
            for (const Int2& offset : kOffsets) {
                const Int2 pos{(x * 2) + offset[0], (y * 2) + offset[1]};
                depth_sum += farthest_depth.r(clamp_load(pos, {0, 0}, k.render));
                // ShadingChangeSource
                const Int2 sample = clamp_load(pos, {0, 0}, k.render);
                const float diff = compute_diff(k, luma, previous_luma, uv_of(sample, k.render),
                                                dilated_motion.rg(sample));
                change_sum = {change_sum[0] + diff, change_sum[1] + sign_of(diff)};
            }
            const std::size_t at = index_of({x, y}, k.half);
            out.farthest_depth[at] = round_half(depth_sum * 0.25F);
            out.level[at * 2] = round_half(change_sum[0] * 0.25F);
            out.level[(at * 2) + 1] = round_half(change_sum[1] * 0.25F);
        }
    }
    return out;
}

// fsr3_reduce.hlsl: the level after level_before.
std::vector<float> reduce_pass(const Constants& k, int level, const Plane& level_before) {
    const Int2 size = k.level_size(level);
    std::vector<float> out(pixels(size) * 2);
    for (int y = 0; y < size[1]; ++y) {
        for (int x = 0; x < size[0]; ++x) {
            const Int2 base{x * 2, y * 2};
            const Float2 v0 = level_before.rg(base);
            const Float2 v1 = level_before.rg({base[0], base[1] + 1});
            const Float2 v2 = level_before.rg({base[0] + 1, base[1]});
            const Float2 v3 = level_before.rg({base[0] + 1, base[1] + 1});
            const std::size_t at = index_of({x, y}, size);
            out[at * 2] = round_half((((v0[0] + v1[0]) + v2[0]) + v3[0]) * 0.25F);
            out[(at * 2) + 1] = round_half((((v0[1] + v1[1]) + v2[1]) + v3[1]) * 0.25F);
        }
    }
    return out;
}

// fsr3_shading_change.hlsl.
std::vector<float> shading_change_pass(const Constants& k, const std::array<Plane, 3>& levels) {
    std::vector<float> out(pixels(k.half));
    for (int y = 0; y < k.half[1]; ++y) {
        for (int x = 0; x < k.half[0]; ++x) {
            const Float2 uv_jittered = jittered_uv(uv_of({x, y}, k.half), k.jitter, k.render);
            const Float2 mip_uv = clamp_uv(uv_jittered, k.half, k.level_size(0));
            float change = 0.0F;
            for (const Plane& level : levels) {
                const Float4 sample = level.linear(mip_uv);
                const float value = std::abs(sample[0] * sample[1]);
                if (value > 0.0F) {
                    change = std::max(change, value);
                }
            }
            out[index_of({x, y}, k.half)] = round_unorm8(saturate(change));
        }
    }
    return out;
}

// fsr3_reactivity.hlsl.
struct Reactivity {
    std::vector<float> masks;
    std::vector<float> accumulation;
};

struct ReactivityInputs {
    const Plane* dilated_motion = nullptr;
    const Plane* dilated_depth = nullptr;
    const Plane* shading_change = nullptr;
    const Plane* previous_accumulation = nullptr;
    const std::vector<std::uint32_t>* reconstructed = nullptr;
};

float compute_disocclusions(const Constants& k, const std::vector<std::uint32_t>& reconstructed,
                            const Float2& uv, Float2 motion, float current_depth_view_space) {
    const float nearest_in_meters = std::min(current_depth_view_space * 1.0F, kFp16Max);
    const float keep =
        get_4k_velocity(motion) > reconstructed_depth_mv_px_threshold(nearest_in_meters) ? 1.0F
                                                                                         : 0.0F;
    motion = {motion[0] * keep, motion[1] * keep};
    const BilinearSamplingData info =
        bilinear_sampling_data({uv[0] + motion[0], uv[1] + motion[1]}, k.render);
    float disocclusion = 0.0F;
    float weight_sum = 0.0F;
    bool potential = true;
    for (std::size_t i = 0; i < 4 && potential; i++) {
        const Int2 pos = clamp_load(info.base, info.offsets.at(i), k.render);
        if (is_on_screen(pos, k.render)) {
            const float weight = info.weights.at(i);
            if (weight > kReconstructedDepthBilinearWeightThreshold) {
                const float previous_nearest = k.view_space_depth(
                    slash::bit_cast<float>(reconstructed[index_of(pos, k.render)]));
                const float depth_difference = current_depth_view_space - previous_nearest;
                potential = potential && (depth_difference > kFp32Min);
                if (potential) {
                    const float half_viewport_width =
                        length2({static_cast<float>(k.render[0]) * 0.5F,
                                 static_cast<float>(k.render[1]) * 0.5F});
                    const float depth_threshold =
                        std::max(current_depth_view_space, previous_nearest);
                    constexpr float kSep = 1.37e-05F;
                    const float required_separation = kSep * half_viewport_width * depth_threshold;
                    disocclusion += saturate(required_separation / depth_difference) * weight;
                    weight_sum += weight;
                }
            }
        }
    }
    return (potential && weight_sum > 0.0F) ? saturate(1.0F - (disocclusion / weight_sum)) : 0.0F;
}

float compute_motion_divergence(const Constants& k, const Plane& dilated_motion,
                                const Plane& dilated_depth, const Float2& uv, const Float2& motion,
                                float current_depth) {
    const Int2 reprojected = truncated({uv[0] + motion[0], uv[1] + motion[1]}, k.render);
    const float reprojected_depth = dilated_depth.r(reprojected);
    const Float2 reprojected_motion = dilated_motion.rg(reprojected);
    const float reprojected_velocity = get_4k_velocity(reprojected_motion);
    const float velocity = get_4k_velocity(motion);
    if (velocity == 0.0F) {
        return 0.0F;
    }
    const float nucleus_in_meters = k.view_space_depth(reprojected_depth) * 1.0F;
    const float current_in_meters = k.view_space_depth(current_depth) * 1.0F;
    const float distance_factor = min_divided_by_max(nucleus_in_meters, current_in_meters, 0.0F);
    const float velocity_factor = saturate(velocity / 10.0F);
    return ((1.0F - saturate(reprojected_velocity / velocity)) * distance_factor) * velocity_factor;
}

Reactivity reactivity_pass(const Constants& k, const ReactivityInputs& in) {
    Reactivity out;
    out.masks.resize(pixels(k.render) * 4);
    out.accumulation.resize(pixels(k.render));
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            const Float2 uv = uv_of({x, y}, k.render);
            const Float2 motion = in.dilated_motion->rg({x, y});
            const float dilated_depth = in.dilated_depth->r({x, y});
            const float disocclusion = compute_disocclusions(k, *in.reconstructed, uv, motion,
                                                             k.view_space_depth(dilated_depth));
            const Float2 shading_uv =
                clamp_uv({uv[0] - (k.jitter[0] / static_cast<float>(k.render[0])),
                          uv[1] - (k.jitter[1] / static_cast<float>(k.render[1]))},
                         k.half, k.half);
            const float shading_change = saturate(in.shading_change->linear(shading_uv)[0]);
            const float reactiveness =
                std::max(compute_motion_divergence(k, *in.dilated_motion, *in.dilated_depth, uv,
                                                   motion, dilated_depth),
                         0.0F);
            // UpdateAccumulation
            const Float2 reprojected{uv[0] + motion[0], uv[1] + motion[1]};
            float accumulation = 0.0F;
            if (is_uv_inside(reprojected)) {
                const Float2 reprojected_hw = clamp_uv(reprojected, k.render, k.render);
                accumulation = saturate(in.previous_accumulation->linear(reprojected_hw)[0]);
            }
            accumulation = lerp(accumulation, 0.0F, shading_change);
            accumulation = lerp(accumulation, std::min(kMinDisocclusionAccumulation, accumulation),
                                disocclusion);
            accumulation *= rounds_above_one(accumulation * 100.0F) ? 1.0F : 0.0F;
            const std::size_t at = index_of({x, y}, k.render);
            out.masks[(at * 4) + kReactive] = round_unorm8(reactiveness);
            out.masks[(at * 4) + kDisocclusion] = round_unorm8(disocclusion);
            out.masks[(at * 4) + kShadingChange] = round_unorm8(shading_change);
            out.masks[(at * 4) + kAccumulation] = round_unorm8(accumulation);
            out.accumulation[at] =
                round_unorm8(saturate(accumulation + kAccumulationAddedPerFrame));
        }
    }
    return out;
}

// fsr3_lock.hlsl's ComputeThinFeatureConfidence.
float thin_feature_confidence(const Constants& k, const Plane& luma, const Int2& pos) {
    constexpr std::array<Int2, 9> kOffsets{{
        {0, 0},
        {-1, -1},
        {0, -1},
        {1, -1},
        {-1, 0},
        {1, 0},
        {-1, 1},
        {0, 1},
        {1, 1},
    }};
    std::array<float, 9> samples{};
    float luma_min = kFp32Max;
    float luma_max = kFp32Min;
    for (std::size_t i = 0; i < kOffsets.size(); ++i) {
        samples.at(i) = luma.r(clamp_load(pos, kOffsets.at(i), k.render)) * k.exposure;
        luma_min = std::min(luma_min, samples.at(i));
        luma_max = std::max(luma_max, samples.at(i));
    }
    const float range = luma_max - luma_min;
    if (range == 0.0F) {
        return 0.0F;
    }
    constexpr float kThreshold = 0.9F;
    float dissimilar_min = kFp32Max;
    float dissimilar_max = 0.0F;
    std::uint32_t mask = 1U;
    constexpr std::array<std::uint32_t, 4> kRejectionMasks{
        (1U << 1U) | (1U << 2U) | (1U << 4U) | 1U,
        (1U << 2U) | (1U << 3U) | (1U << 5U) | 1U,
        (1U << 4U) | (1U << 6U) | (1U << 7U) | 1U,
        (1U << 5U) | (1U << 7U) | (1U << 8U) | 1U,
    };
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const float difference = std::abs(samples.at(i) - samples[0]) / range;
        if (difference < kThreshold) {
            mask |= 1U << static_cast<std::uint32_t>(i);
        } else {
            dissimilar_min = std::min(dissimilar_min, samples.at(i));
            dissimilar_max = std::max(dissimilar_max, samples.at(i));
        }
    }
    const bool ridge = samples[0] > dissimilar_max || samples[0] < dissimilar_min;
    if (!ridge) {
        return 0.0F;
    }
    for (const std::uint32_t rejection : kRejectionMasks) {
        if ((mask & rejection) == rejection) {
            return 0.0F;
        }
    }
    return 1.0F - (luma_min / luma_max);
}

// fsr3_lock.hlsl.
std::vector<float> lock_pass(const Constants& k, const Plane& luma) {
    std::vector<float> out(pixels(k.display), 0.0F);
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            const Int2 hr = k.hr_pos_from_lr_pos({x, y});
            if (!is_on_screen(hr, k.display)) {
                continue;
            }
            const float strength = thin_feature_confidence(k, luma, {x, y});
            if (strength > (1.0F / 100.0F)) {
                out[index_of(hr, k.display)] = round_unorm8(strength);
            }
        }
    }
    return out;
}

// fsr3_instability.hlsl.
struct Instability {
    std::vector<float> luma_history;
    std::vector<float> instability;
};

Instability instability_pass(const Constants& k, const Plane& dilated_motion, const Plane& masks,
                             const Plane& luma, const Plane& luma_history) {
    Instability out;
    out.luma_history.resize(pixels(k.render) * 4);
    out.instability.resize(pixels(k.render));
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            Float4 history{0.0F, 0.0F, 0.0F, 0.0F};
            float factor = 0.0F;
            const Float2 motion = dilated_motion.rg({x, y});
            const Float2 uv = uv_of({x, y}, k.render);
            const Float2 uv_current = jittered_uv(uv, k.jitter, k.render);
            const Float2 uv_previous = jittered_uv(uv, k.previous_jitter, k.render);
            const Float2 reprojected{uv_previous[0] + motion[0], uv_previous[1] + motion[1]};
            if (is_uv_inside(reprojected)) {
                const Float4 m = masks.linear(clamp_uv(uv_current, k.render, k.render));
                const float reactive = saturate(m[kReactive]);
                const float disocclusion = saturate(m[kDisocclusion]);
                const float shading_change = saturate(m[kShadingChange]);
                const float accumulation = saturate(m[kAccumulation]);
                if (accumulation > 0.9F) {
                    const float current =
                        luma.linear(clamp_uv(uv_current, k.render, k.render))[0] * k.exposure;
                    const Float4 sampled =
                        luma_history.linear(clamp_uv(reprojected, k.render, k.render));
                    for (std::size_t c = 0; c < 4; ++c) {
                        history.at(c) = (sampled.at(c) * 1.0F) * k.exposure;
                    }
                    // ComputeLumaInstabilityFactor
                    float instability = 0.0F;
                    const float diffs0 = current - history[0];
                    const float similarity0 = min_divided_by_max(current, history[0], 1.0F);
                    float max_similarity = similarity0;
                    if (similarity0 < 1.0F) {
                        for (std::size_t i = 1; i <= 3; i++) {
                            const float diffs1 = current - history.at(i);
                            const float similarity1 =
                                min_divided_by_max(current, history.at(i), 0.0F);
                            if (sign_of(diffs0) == sign_of(diffs1)) {
                                max_similarity = std::max(max_similarity, similarity1);
                            }
                        }
                        instability = max_similarity > similarity0 ? 1.0F : 0.0F;
                    }
                    history = {current, history[0], history[1], history[2]};
                    for (float& value : history) {
                        value /= k.exposure;
                    }
                    factor = instability * (history[3] != 0.0F ? 1.0F : 0.0F);
                    const float velocity_weight = 1.0F - saturate(get_4k_velocity(motion) / 20.0F);
                    factor *= (((velocity_weight * (1.0F - disocclusion)) * (1.0F - reactive)) *
                               (1.0F - shading_change));
                }
            }
            const std::size_t at = index_of({x, y}, k.render);
            for (std::size_t c = 0; c < 4; ++c) {
                out.luma_history[(at * 4) + c] = round_half(history.at(c));
            }
            out.instability[at] = round_half(factor);
        }
    }
    return out;
}

// fsr3_accumulate.hlsl.
struct AccumulateInputs {
    const Plane* color = nullptr;
    const Plane* dilated_motion = nullptr;
    const Plane* instability = nullptr;
    const Plane* farthest_depth = nullptr;
    const Plane* masks = nullptr;
    const Plane* history = nullptr;
    const Plane* new_locks = nullptr;
};

struct RectificationBox {
    Float3 center{};
    Float3 vec{};
    Float3 aabb_min{};
    Float3 aabb_max{};
    float center_weight = 0.0F;

    void add_sample(bool initial, const Float3& color, float weight) {
        if (initial) {
            aabb_min = color;
            aabb_max = color;
            const Float3 weighted = mul3(color, weight);
            center = weighted;
            vec = mul3(color, weighted);
            center_weight = weight;
        } else {
            aabb_min = min3(aabb_min, color);
            aabb_max = max3(aabb_max, color);
            const Float3 weighted = mul3(color, weight);
            center = add3(center, weighted);
            vec = add3(vec, mul3(color, weighted));
            center_weight += weight;
        }
    }
    void compute_variance_box_data() {
        center_weight = std::abs(center_weight) > kFp32Min ? center_weight : 1.0F;
        center = div3(center, center_weight);
        vec = div3(vec, center_weight);
        const Float3 variance = sub3(vec, mul3(center, center));
        vec = {std::sqrt(std::abs(variance[0])), std::sqrt(std::abs(variance[1])),
               std::sqrt(std::abs(variance[2]))};
    }
};

class Accumulator {
public:
    Accumulator(const Constants& k, const AccumulateInputs& in) : k_(k), in_(in) {}

    [[nodiscard]] Float4 run(const Int2& hr) const {
        // InitPassData
        const Float2 hr_uv = uv_of(hr, k_.display);
        const Float2 lr_uv_jittered = jittered_uv(hr_uv, k_.jitter, k_.render);
        const Float2 lr_uv_hw = clamp_uv(lr_uv_jittered, k_.render, k_.render);
        const Float2 motion = in_.dilated_motion->rg(truncated(hr_uv, k_.render));
        const float velocity = get_4k_velocity(motion);
        const Float2 reprojected_hr_uv{hr_uv[0] + motion[0], hr_uv[1] + motion[1]};
        const bool existing = is_uv_inside(reprojected_hr_uv);
        const float luma_instability =
            in_.instability->linear(clamp_uv(hr_uv, k_.render, k_.render))[0];
        const float farthest_in_meters =
            in_.farthest_depth->linear(clamp_uv(lr_uv_jittered, k_.half, k_.half))[0];
        const bool new_sample = !existing || k_.frame_index == 0.0F;
        const Float4 m = in_.masks->linear(lr_uv_hw);
        const float reactive = saturate(m[kReactive]);
        const float disocclusion = saturate(m[kDisocclusion]);
        const float shading_change = saturate(m[kShadingChange]);
        float accumulation = saturate(m[kAccumulation]);
        accumulation *= rounds_above_one(accumulation * 100.0F) ? 1.0F : 0.0F;

        Float3 history_color{0.0F, 0.0F, 0.0F};
        float lock = 0.0F;
        // ReprojectHistoryColor
        if (existing && !new_sample) {
            const Float4 reprojected = lanczos2_sample(*in_.history, reprojected_hr_uv);
            history_color = {reprojected[0], reprojected[1], reprojected[2]};
            history_color = mul3(history_color, 1.0F);
            history_color = mul3(history_color, k_.exposure);
            history_color = rgb_to_ycocg(history_color);
            lock = reprojected[3];
        }

        // UpdateLockStatus
        lock *= new_sample ? 0.0F : 1.0F;
        const float lifetime_decrease_factor =
            std::max(saturate(shading_change), std::max(reactive, disocclusion));
        lock = std::max(0.0F, lock - (lifetime_decrease_factor * kLockMax));
        const float lock_contribution =
            saturate(saturate(lock - kLockThreshold) * (kLockMax - kLockThreshold));
        const float new_lock_intensity =
            in_.new_locks->r(hr) * (1.0F - std::max(shading_change * 0.0F, reactive));
        lock = std::max(0.0F, std::min(lock + new_lock_intensity, kLockMax));
        const float lifetime_decrease = (0.1F / k_.phases) * (1.0F - lifetime_decrease_factor);
        lock = std::max(0.0F, lock - lifetime_decrease);
        lock *= is_uv_inside({hr_uv[0] - motion[0], hr_uv[1] - motion[1]}) ? 1.0F : 0.0F;

        // ComputeBaseAccumulationWeight
        float history_weight = accumulation;
        history_weight = std::min(
            history_weight, lerp(history_weight, 0.15F,
                                 saturate(std::max(0.0F, (velocity * kVelocityFactor) / 0.5F))));

        // ComputeUpsampledColorAndWeight
        RectificationBox box;
        Float3 upsampled_color{0.0F, 0.0F, 0.0F};
        float upsampled_weight = 0.0F;
        upsample(hr, {disocclusion, shading_change, accumulation}, history_weight, box,
                 upsampled_color, upsampled_weight);

        // RectifyHistory
        const float velocity_factor = saturate(velocity / 20.0F);
        const float distance_factor = saturate(0.75F - (farthest_in_meters / 20.0F));
        const float accumulation_factor = 1.0F - accumulation;
        const float reactive_factor = power_of(reactive, 1.0F / 2.0F);
        const float box_scale_t = std::max(
            velocity_factor,
            std::max(distance_factor,
                     std::max(accumulation_factor, std::max(reactive_factor, shading_change))));
        const float box_scale = lerp(3.0F, 1.0F, box_scale_t);
        const Float3 scaled_box_vec = mul3(mul3(box.vec, {1.7F, 1.0F, 1.0F}), box_scale);
        const Float3 clamped_scaled_box_vec =
            max3(scaled_box_vec, {1.193e-7F, 1.193e-7F, 1.193e-7F});
        const Float3 transformed = div3(sub3(history_color, box.center), clamped_scaled_box_vec);
        const float length = std::sqrt(dot3(transformed, transformed));
        if (length > 1.0F) {
            const Float3 clamped = div3(transformed, length);
            const Float3 final_clamped = add3(mul3(clamped, scaled_box_vec), box.center);
            const float contribution =
                (std::max(luma_instability, lock_contribution) * accumulation) *
                (1.0F - disocclusion);
            history_color = lerp3(final_clamped, history_color, saturate(contribution));
        }

        // Accumulate
        history_weight *= history_weight > kFp16Min ? 1.0F : 0.0F;
        history_weight = std::max(kEpsilon, history_weight + upsampled_weight);
        upsampled_color = rgb_to_ycocg(tonemap(ycocg_to_rgb(upsampled_color)));
        history_color = rgb_to_ycocg(tonemap(ycocg_to_rgb(history_color)));
        const float alpha = saturate(upsampled_weight / history_weight);
        history_color = lerp3(history_color, upsampled_color, alpha);
        history_color = ycocg_to_rgb(history_color);
        history_color = inverse_tonemap(history_color);

        history_color = div3(history_color, k_.exposure);
        history_color = max3(history_color, {0.0F, 0.0F, 0.0F});
        return {history_color[0], history_color[1], history_color[2], lock};
    }

private:
    struct Masks {
        float disocclusion = 0.0F;
        float shading_change = 0.0F;
        float accumulation = 0.0F;
    };

    void upsample(const Int2& hr, const Masks& masks, float& history_weight, RectificationBox& box,
                  Float3& color, float& weight) const {
        const Float2 dst_output{static_cast<float>(hr[0]) + 0.5F, static_cast<float>(hr[1]) + 0.5F};
        const Float2 src_output{dst_output[0] * k_.downscale[0], dst_output[1] * k_.downscale[1]};
        const Int2 src_input{static_cast<int>(std::floor(src_output[0])),
                             static_cast<int>(std::floor(src_output[1]))};
        const Float2 src_unjittered{(static_cast<float>(src_input[0]) + 0.5F) - k_.jitter[0],
                                    (static_cast<float>(src_input[1]) + 0.5F) - k_.jitter[1]};
        const Float2 base_offset{src_unjittered[0] - src_output[0],
                                 src_unjittered[1] - src_output[1]};
        const bool flip_col = src_unjittered[0] > src_output[0];
        const bool flip_row = src_unjittered[1] > src_output[1];
        const Int2 offset_tl{flip_col ? -2 : -1, flip_row ? -2 : -1};
        const bool initial = masks.accumulation == 0.0F;

        const auto col_row = [&](int row, int col) {
            return Int2{flip_col ? (3 - col) : col, flip_row ? (3 - row) : row};
        };
        std::array<Float3, 9> samples{};
        std::size_t index = 0;
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                const Int2 cr = col_row(row, col);
                const Int2 src{src_input[0] + offset_tl[0] + cr[0],
                               src_input[1] + offset_tl[1] + cr[1]};
                // LoadPreparedColor
                const Float3 rgb = mul3(
                    max3({0.0F, 0.0F, 0.0F}, in_.color->rgb(clamp_load(src, {0, 0}, k_.render))),
                    k_.exposure);
                samples.at(index) = rgb_to_ycocg(rgb);
                ++index;
            }
        }
        if (initial) {
            for (Float3& sample : samples) {
                sample = rgb_to_ycocg(tonemap(ycocg_to_rgb(sample)));
            }
        }

        const float kernel_bias_max = std::min(1.99F, 1.0F + ((1.0F / k_.downscale[0]) - 1.0F));
        const float kernel_bias_min = std::max(1.0F, ((1.0F + kernel_bias_max) * 0.3F));
        const float kernel_bias_weight =
            std::min(1.0F - (masks.disocclusion * 0.5F),
                     std::min(1.0F - masks.shading_change, saturate(history_weight * 5.0F)));
        const float kernel_bias = lerp(kernel_bias_min, kernel_bias_max, kernel_bias_weight);

        index = 0;
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                const Int2 cr = col_row(row, col);
                const Float2 offset{static_cast<float>(offset_tl[0]) + static_cast<float>(cr[0]),
                                    static_cast<float>(offset_tl[1]) + static_cast<float>(cr[1])};
                const Float2 src_offset{base_offset[0] + offset[0], base_offset[1] + offset[1]};
                const Int2 src{src_input[0] + offset_tl[0] + cr[0],
                               src_input[1] + offset_tl[1] + cr[1]};
                const float on_screen = is_on_screen(src, k_.render) ? 1.0F : 0.0F;
                if (!initial) {
                    const Float2 biased{src_offset[0] * kernel_bias, src_offset[1] * kernel_bias};
                    const float sample_weight =
                        on_screen * lanczos2_approx_sq(dot2(biased, biased));
                    color = add3(color, mul3(samples.at(index), sample_weight));
                    weight += sample_weight;
                }
                constexpr float kRectificationCurveBias = -2.3F;
                const float offset_sq = dot2(src_offset, src_offset);
                const float box_weight =
                    slash::exp(kRectificationCurveBias * offset_sq) * on_screen;
                box.add_sample(row == 0 && col == 0, samples.at(index), box_weight);
                ++index;
            }
        }
        box.compute_variance_box_data();

        weight *= weight > kEpsilon ? 1.0F : 0.0F;
        if (weight > kEpsilon) {
            color = div3(color, weight);
            weight *= kAverageLanczosWeightPerFrame;
            color = min3(max3(color, box.aabb_min), box.aabb_max);
        }
        if (initial) {
            color = rgb_to_ycocg(inverse_tonemap(ycocg_to_rgb(box.center)));
            weight = 1.0F;
            history_weight = 0.0F;
        }
    }

    const Constants& k_;
    const AccumulateInputs& in_;
};

} // namespace

Fsr3Uniforms fsr3_uniforms(const Fsr2Frame& frame, const std::array<float, 2>& previous_jitter) {
    Fsr3Uniforms u;
    u.fsr2 = fsr2_uniforms(frame);
    u.fsr2.options[0] = 0;
    u.fsr2.options[1] = 0;
    // In render pixels with y down, as FSR 2's jitter.
    u.previous = {previous_jitter[0] * static_cast<float>(frame.render_width) * 0.5F,
                  -previous_jitter[1] * static_cast<float>(frame.render_height) * 0.5F, 0.0F, 0.0F};
    return u;
}

std::array<std::uint32_t, 2> fsr3_half_size(std::uint32_t render_width,
                                            std::uint32_t render_height) {
    return {std::max(render_width / 2U, 1U), std::max(render_height / 2U, 1U)};
}

std::array<std::uint32_t, 2> fsr3_level_size(std::uint32_t render_width,
                                             std::uint32_t render_height, std::uint32_t level) {
    const auto half = fsr3_half_size(render_width, render_height);
    return {std::max(half[0] >> level, 1U), std::max(half[1] >> level, 1U)};
}

void run_fsr3(const Fsr3Uniforms& uniforms, const Fsr2Inputs& inputs, Fsr3History& history,
              std::span<float> out) {
    const Constants k(uniforms);
    const std::size_t render_pixels = pixels(k.render);
    const std::size_t display_pixels = pixels(k.display);
    if (k.frame_index == 0.0F || history.color.size() != display_pixels * 4 ||
        history.accumulation.size() != render_pixels) {
        history.color.assign(display_pixels * 4, 0.0F);
        history.accumulation.assign(render_pixels, 0.0F);
        history.luma.assign(render_pixels, 0.0F);
        history.luma_history.assign(render_pixels * 4, 0.0F);
    }
    const Plane color{.size = k.render, .channels = 4, .data = inputs.color};
    const Plane depth{.size = k.render, .channels = 1, .data = inputs.depth};
    // LoadInputMotionVector with Godot's derived motion vectors, as FSR 2's.
    std::vector<float> loaded(inputs.motion.begin(), inputs.motion.end());
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            const std::size_t at = index_of({x, y}, k.render);
            if (loaded[at * 2] <= kInvalidMotion && loaded[(at * 2) + 1] <= kInvalidMotion) {
                const std::array<float, 2> derived = derive_motion_vector(
                    uniforms.fsr2.reprojection, uv_of({x, y}, k.render), depth.r({x, y}));
                loaded[at * 2] = derived[0];
                loaded[(at * 2) + 1] = derived[1];
            }
        }
    }
    const Plane motion{.size = k.render, .channels = 2, .data = loaded};

    const Prepared prepared = prepare_pass(k, depth, motion, color);
    const Plane dilated_depth{.size = k.render, .channels = 1, .data = prepared.dilated_depth};
    const Plane dilated_motion{.size = k.render, .channels = 2, .data = prepared.dilated_motion};
    const Plane farthest{.size = k.render, .channels = 1, .data = prepared.farthest_depth};
    const Plane luma{.size = k.render, .channels = 1, .data = prepared.luma};
    const Plane previous_luma{.size = k.render, .channels = 1, .data = history.luma};
    const std::vector<std::uint32_t> reconstructed =
        reconstruct_pass(k, dilated_depth, dilated_motion);

    const Pyramid pyramid = pyramid_pass(k, farthest, dilated_motion, luma, previous_luma);
    const std::vector<float> level1 =
        reduce_pass(k, 1, {.size = k.level_size(0), .channels = 2, .data = pyramid.level});
    const std::vector<float> level2 =
        reduce_pass(k, 2, {.size = k.level_size(1), .channels = 2, .data = level1});
    const std::array<Plane, 3> levels{
        Plane{.size = k.level_size(0), .channels = 2, .data = pyramid.level},
        Plane{.size = k.level_size(1), .channels = 2, .data = level1},
        Plane{.size = k.level_size(2), .channels = 2, .data = level2}};
    const std::vector<float> shading_change = shading_change_pass(k, levels);

    const Plane shading{.size = k.half, .channels = 1, .data = shading_change};
    const Plane previous_accumulation{
        .size = k.render, .channels = 1, .data = history.accumulation};
    const Reactivity reactivity =
        reactivity_pass(k, {.dilated_motion = &dilated_motion,
                            .dilated_depth = &dilated_depth,
                            .shading_change = &shading,
                            .previous_accumulation = &previous_accumulation,
                            .reconstructed = &reconstructed});
    const std::vector<float> new_locks = lock_pass(k, luma);

    const Plane masks{.size = k.render, .channels = 4, .data = reactivity.masks};
    const Plane previous_luma_history{
        .size = k.render, .channels = 4, .data = history.luma_history};
    const Instability instability =
        instability_pass(k, dilated_motion, masks, luma, previous_luma_history);

    const Plane instability_plane{.size = k.render, .channels = 1, .data = instability.instability};
    const Plane farthest_half{.size = k.half, .channels = 1, .data = pyramid.farthest_depth};
    const Plane history_color{.size = k.display, .channels = 4, .data = history.color};
    const Plane locks{.size = k.display, .channels = 1, .data = new_locks};
    const AccumulateInputs accumulate_inputs{.color = &color,
                                             .dilated_motion = &dilated_motion,
                                             .instability = &instability_plane,
                                             .farthest_depth = &farthest_half,
                                             .masks = &masks,
                                             .history = &history_color,
                                             .new_locks = &locks};
    const Accumulator accumulator(k, accumulate_inputs);
    std::vector<float> next_color(display_pixels * 4);
    for (int y = 0; y < k.display[1]; ++y) {
        for (int x = 0; x < k.display[0]; ++x) {
            const Float4 a = accumulator.run({x, y});
            const std::size_t at = index_of({x, y}, k.display);
            for (std::size_t c = 0; c < 4; ++c) {
                next_color[(at * 4) + c] = round_half(a.at(c));
            }
        }
    }
    run_fsr2_rcas(uniforms.fsr2, next_color, out);
    history.color = std::move(next_color);
    history.accumulation = reactivity.accumulation;
    history.luma = prepared.luma;
    history.luma_history = instability.luma_history;
}

} // namespace slash::render::detail
