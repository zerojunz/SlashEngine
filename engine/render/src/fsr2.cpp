#include "fsr2.hpp"

#include "forward_post.hpp"
#include "taa.hpp"

#include "slash/core/bits.hpp"
#include "slash/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

// The CPU twin of shaders/fsr2*.hlsl, AMD's FidelityFX Super Resolution 2.2 as Godot 4.6 runs it.
// Each function follows the one of the same name in AMD's code, line for line where it can, and
// libm's functions are slash's, so every machine draws the same pixels. Keep the two in step.
//
// Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
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

constexpr float kFp16Max = 65504.0F;
constexpr float kEpsilon = 1e-03F;
constexpr float kTonemapEpsilon = 1.0F / kFp16Max;
constexpr float kFltMax = 3.402823466e+38F;
constexpr float kReconstructedDepthBilinearWeightThreshold = 0.01F;
constexpr float kUpsampleLanczosWeightScale = 1.0F / 12.0F;
constexpr float kMaxAccumulationLanczosWeight = 1.0F;
constexpr float kAverageLanczosWeightPerFrame = 0.74F * kUpsampleLanczosWeightScale;
constexpr std::size_t kLockLifetimeRemaining = 0;
constexpr std::size_t kLockTemporalLuma = 1;
constexpr float kPi = std::numbers::pi_v<float>;

float saturate(float x) {
    return std::clamp(x, 0.0F, 1.0F);
}
float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}
Float3 lerp3(const Float3& a, const Float3& b, const Float3& t) {
    return {lerp(a[0], b[0], t[0]), lerp(a[1], b[1], t[1]), lerp(a[2], b[2], t[2])};
}
Float3 lerp3(const Float3& a, const Float3& b, float t) {
    return lerp3(a, b, {t, t, t});
}
Float4 lerp4(const Float4& a, const Float4& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t), lerp(a[3], b[3], t)};
}
float round_nearest(float x) {
    return std::floor(x + 0.5F);
}
// AMD's ffxPow for x of 0 or more: 0 to any power is 0.
float power_of(float x, float y) {
    return x <= 0.0F ? 0.0F : slash::exp(y * slash::ln(x));
}
float sine(float x) {
    return slash::cos_sin(x).y;
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
Float3 min3(const Float3& a, const Float3& b) {
    return {std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2])};
}
Float3 max3(const Float3& a, const Float3& b) {
    return {std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2])};
}
Float3 clamp3(const Float3& v, const Float3& low, const Float3& high) {
    // HLSL's clamp: min(max(v, low), high), which keeps a low above high at high.
    return min3(max3(v, low), high);
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
float rgb_to_perceived_luma(const Float3& c) {
    const float luminance = rgb_to_luma(c);
    float perceived = 0.0F;
    if (luminance <= 216.0F / 24389.0F) {
        perceived = luminance * (24389.0F / 27.0F);
    } else {
        perceived = (power_of(luminance, 1.0F / 3.0F) * 116.0F) - 16.0F;
    }
    return perceived * 0.01F;
}
Float3 tonemap(const Float3& c) {
    return mul3(c, 1.0F / (std::max(std::max(0.0F, c[0]), std::max(c[1], c[2])) + 1.0F));
}
Float3 inverse_tonemap(const Float3& c) {
    return mul3(c, 1.0F / std::max(kTonemapEpsilon, 1.0F - std::max(c[0], std::max(c[1], c[2]))));
}

Int2 clamp_load(const Int2& sample, const Int2& offset, const Int2& size) {
    Int2 result{sample[0] + offset[0], sample[1] + offset[1]};
    for (std::size_t i = 0; i < 2; ++i) {
        if (offset.at(i) < 0) {
            result.at(i) = std::max(result.at(i), 0);
        }
        if (offset.at(i) > 0) {
            result.at(i) = std::min(result.at(i), size.at(i) - 1);
        }
    }
    return result;
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
// ffx_fsr2_sample.h's ClampCoord, kept inside the texture.
Int2 clamp_coord(const Int2& sample, const Int2& offset, const Int2& size) {
    const Int2 r = clamp_load(sample, offset, size);
    return {std::clamp(r[0], 0, size[0] - 1), std::clamp(r[1], 0, size[1] - 1)};
}

// A texture of the passes, rows top to bottom, its loads kept inside it.
struct Plane {
    Int2 size{};
    std::size_t channels = 4;
    std::span<const float> data;

    [[nodiscard]] std::size_t at(const Int2& p) const {
        const int x = std::clamp(p[0], 0, size[0] - 1);
        const int y = std::clamp(p[1], 0, size[1] - 1);
        return ((static_cast<std::size_t>(y) * static_cast<std::size_t>(size[0])) +
                static_cast<std::size_t>(x)) *
               channels;
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
    // Filtered linearly at uv with clamped edges, as the shaders' LinearWeights loads.
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

// The base texel and fraction of a custom sample at uv, as DeclareCustomTextureSample.
Int2 custom_sample_base(const Float2& uv, const Int2& size, Float2& frac) {
    Float2 px{(uv[0] * static_cast<float>(size[0])) - 0.5F,
              (uv[1] * static_cast<float>(size[1])) - 0.5F};
    px[0] = std::max(0.0F, std::min(static_cast<float>(size[0]), px[0]));
    px[1] = std::max(0.0F, std::min(static_cast<float>(size[1]), px[1]));
    const Float2 base{std::floor(px[0]), std::floor(px[1])};
    frac = {px[0] - base[0], px[1] - base[1]};
    return {static_cast<int>(base[0]), static_cast<int>(base[1])};
}

// FSR2_LANCZOS2_SAMPLE in fsr2.hlsl.
Float4 lanczos2_sample(const Plane& plane, const Float2& uv) {
    Float2 frac{};
    const Int2 base = custom_sample_base(uv, plane.size, frac);
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

// FSR2_BILINEAR_SAMPLE in fsr2.hlsl.
Float4 bilinear_sample(const Plane& plane, const Float2& uv) {
    Float2 frac{};
    const Int2 base = custom_sample_base(uv, plane.size, frac);
    const Float4 c00 = plane.load(clamp_coord(base, {0, 0}, plane.size));
    const Float4 c10 = plane.load(clamp_coord(base, {1, 0}, plane.size));
    const Float4 c01 = plane.load(clamp_coord(base, {0, 1}, plane.size));
    const Float4 c11 = plane.load(clamp_coord(base, {1, 1}, plane.size));
    return lerp4(lerp4(c00, c10, frac[0]), lerp4(c01, c11, frac[0]), frac[1]);
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

float min_divided_by_max(float v0, float v1) {
    const float m = std::max(v0, v1);
    return m != 0.0F ? std::min(v0, v1) / m : 0.0F;
}

// The uniforms as the shaders read them, with fsr2.hlsl's accessors.
struct Constants {
    Int2 render{};
    Int2 display{};
    Float2 jitter{};
    Float2 downscale{};
    Float4 view_depth{};
    float exposure = 1.0F;
    float phases = 0.0F;
    int frame_index = 0;
    float sharpness = 0.0F;
    Int2 luma_mip{};
    bool copy = false;
    bool ortho = false;

    explicit Constants(const Fsr2Uniforms& u)
        : render{u.sizes[0], u.sizes[1]}, display{u.sizes[2], u.sizes[3]},
          jitter{u.jitter_scale[0], u.jitter_scale[1]},
          downscale{u.jitter_scale[2], u.jitter_scale[3]}, view_depth(u.view_depth),
          exposure(u.frame[0] == 0.0F ? 1.0F : u.frame[0]), phases(u.frame[1]),
          frame_index(static_cast<int>(u.frame[2])), sharpness(u.frame[3]),
          luma_mip{u.options[0], u.options[1]}, copy(u.options[2] == 1), ortho(u.options[3] != 0) {}

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
    [[nodiscard]] Float3 view_space_position(const Int2& pos, const Int2& size,
                                             float device_depth) const {
        const float z = view_space_depth(device_depth);
        const Float2 ndc{((static_cast<float>(pos[0]) / static_cast<float>(size[0])) * 2.0F) - 1.0F,
                         ((static_cast<float>(pos[1]) / static_cast<float>(size[1])) * -2.0F) +
                             1.0F};
        const float scale = ortho ? 1.0F : z;
        return {view_depth[2] * ndc[0] * scale, view_depth[3] * ndc[1] * scale, z};
    }
    [[nodiscard]] float max_distance_in_meters() const { return view_space_depth(0.0F); }
};

Float3 prepare_rgb(Float3 rgb, float exposure, float pre_exposure) {
    rgb = mul3(rgb, 1.0F / pre_exposure);
    rgb = mul3(rgb, exposure);
    return {std::clamp(rgb[0], 0.0F, kFp16Max), std::clamp(rgb[1], 0.0F, kFp16Max),
            std::clamp(rgb[2], 0.0F, kFp16Max)};
}
Float3 unprepare_rgb(Float3 rgb, float exposure) {
    rgb = mul3(rgb, 1.0F / exposure);
    return rgb;
}

std::size_t pixels(const Int2& size) {
    return static_cast<std::size_t>(size[0]) * static_cast<std::size_t>(size[1]);
}

// fsr2_luma.hlsl: the shading change mip.
std::vector<float> luma_pass(const Constants& k, const Plane& color) {
    std::vector<float> mip(pixels(k.luma_mip));
    const auto source_log_luma = [&](const Int2& p) {
        if (p[0] >= k.render[0] || p[1] >= k.render[1]) {
            return 0.0F;
        }
        Float2 uv{(static_cast<float>(p[0]) + 0.5F + k.jitter[0]) / static_cast<float>(k.render[0]),
                  (static_cast<float>(p[1]) + 0.5F + k.jitter[1]) /
                      static_cast<float>(k.render[1])};
        uv = clamp_uv(uv, k.render, k.render);
        const Float4 rgb = color.linear(uv);
        return slash::ln(std::max(kEpsilon, rgb_to_luma({rgb[0], rgb[1], rgb[2]})));
    };
    for (int my = 0; my < k.luma_mip[1]; ++my) {
        for (int mx = 0; mx < k.luma_mip[0]; ++mx) {
            float sum = 0.0F;
            for (int y = 0; y < 32; ++y) {
                float row = 0.0F;
                for (int x = 0; x < 32; ++x) {
                    row += source_log_luma({(mx * 32) + x, (my * 32) + y});
                }
                sum += row;
            }
            mip[(static_cast<std::size_t>(my) * static_cast<std::size_t>(k.luma_mip[0])) +
                static_cast<std::size_t>(mx)] = round_half(sum / 1024.0F);
        }
    }
    return mip;
}

// fsr2_dilate.hlsl.
struct Dilated {
    std::vector<float> depth;
    std::vector<float> motion;
    std::vector<float> lock_luma;
};

Dilated dilate_pass(const Constants& k, const Plane& depth, const Plane& motion,
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
    Dilated out;
    out.depth.resize(pixels(k.render));
    out.motion.resize(pixels(k.render) * 2);
    out.lock_luma.resize(pixels(k.render));
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            Int2 nearest{x, y};
            float nearest_depth = depth.r(nearest);
            for (std::size_t i = 1; i < kOffsets.size(); ++i) {
                const Int2 pos{x + kOffsets.at(i)[0], y + kOffsets.at(i)[1]};
                if (is_on_screen(pos, k.render)) {
                    const float d = depth.r(pos);
                    if (d > nearest_depth) {
                        nearest = pos;
                        nearest_depth = d;
                    }
                }
            }
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(k.render[0])) +
                static_cast<std::size_t>(x);
            out.depth[at] = nearest_depth;
            const Float2 mv = motion.rg(nearest);
            out.motion[at * 2] = round_half(mv[0]);
            out.motion[(at * 2) + 1] = round_half(mv[1]);
            // ComputeLockInputLuma
            Float3 rgb = max3({0.0F, 0.0F, 0.0F}, color.rgb({x, y}));
            rgb = mul3(rgb, k.exposure);
            rgb = tonemap(rgb);
            out.lock_luma[at] = round_half(power_of(rgb_to_perceived_luma(rgb), 1.0F / 6.0F));
        }
    }
    return out;
}

// fsr2_reconstruct.hlsl: the dilated depths moved to where they were in the frame before, as
// bits, the nearest kept.
std::vector<std::uint32_t> reconstruct_pass(const Constants& k, const Plane& dilated_depth,
                                            const Plane& dilated_motion) {
    std::vector<std::uint32_t> reconstructed(pixels(k.render), 0U);
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            const float depth = dilated_depth.r({x, y});
            Float2 mv = dilated_motion.rg({x, y});
            const float keep = length2({mv[0] * static_cast<float>(k.display[0]),
                                        mv[1] * static_cast<float>(k.display[1])}) > 0.1F
                                   ? 1.0F
                                   : 0.0F;
            mv = {mv[0] * keep, mv[1] * keep};
            const Float2 uv{(static_cast<float>(x) + 0.5F) / static_cast<float>(k.render[0]),
                            (static_cast<float>(y) + 0.5F) / static_cast<float>(k.render[1])};
            const BilinearSamplingData info =
                bilinear_sampling_data({uv[0] + mv[0], uv[1] + mv[1]}, k.render);
            for (std::size_t i = 0; i < 4; ++i) {
                if (info.weights.at(i) > kReconstructedDepthBilinearWeightThreshold) {
                    const Int2 store{info.base[0] + info.offsets.at(i)[0],
                                     info.base[1] + info.offsets.at(i)[1]};
                    if (is_on_screen(store, k.render)) {
                        std::uint32_t& slot =
                            reconstructed[(static_cast<std::size_t>(store[1]) *
                                           static_cast<std::size_t>(k.render[0])) +
                                          static_cast<std::size_t>(store[0])];
                        slot = std::max(slot, slash::bit_cast<std::uint32_t>(depth));
                    }
                }
            }
        }
    }
    return reconstructed;
}

// fsr2_depth_clip.hlsl.
struct Clipped {
    std::vector<float> prepared;
    std::vector<float> reactive;
};

struct DepthClipInputs {
    const Plane* dilated_motion = nullptr;
    const Plane* dilated_depth = nullptr;
    const Plane* color = nullptr;
    const Plane* previous_dilated_motion = nullptr;
    const Plane* motion = nullptr;
    const std::vector<std::uint32_t>* reconstructed = nullptr;
};

Clipped depth_clip_pass(const Constants& k, const DepthClipInputs& in) {
    const auto reconstructed_prev_depth = [&](const Int2& p) {
        const int x = std::clamp(p[0], 0, k.render[0] - 1);
        const int y = std::clamp(p[1], 0, k.render[1] - 1);
        return slash::bit_cast<float>((*in.reconstructed)[(static_cast<std::size_t>(y) *
                                                           static_cast<std::size_t>(k.render[0])) +
                                                          static_cast<std::size_t>(x)]);
    };
    const Float2 render_f{static_cast<float>(k.render[0]), static_cast<float>(k.render[1])};
    const Float2 display_f{static_cast<float>(k.display[0]), static_cast<float>(k.display[1])};

    const auto compute_depth_clip = [&](const Float2& uv_sample, float current_depth_sample) {
        const float current_depth_view_space = k.view_space_depth(current_depth_sample);
        const BilinearSamplingData info = bilinear_sampling_data(uv_sample, k.render);
        float depth = 0.0F;
        float weight_sum = 0.0F;
        for (std::size_t i = 0; i < 4; ++i) {
            const Int2 sample_pos{info.base[0] + info.offsets.at(i)[0],
                                  info.base[1] + info.offsets.at(i)[1]};
            if (!is_on_screen(sample_pos, k.render)) {
                continue;
            }
            const float weight = info.weights.at(i);
            if (weight <= kReconstructedDepthBilinearWeightThreshold) {
                continue;
            }
            const float prev_depth_sample = reconstructed_prev_depth(sample_pos);
            const float prev_nearest_depth_view_space = k.view_space_depth(prev_depth_sample);
            const float depth_diff = current_depth_view_space - prev_nearest_depth_view_space;
            if (depth_diff > 0.0F) {
                const float plane_depth = std::min(prev_depth_sample, current_depth_sample);
                const Float3 center = k.view_space_position(
                    {static_cast<int>(render_f[0] * 0.5F), static_cast<int>(render_f[1] * 0.5F)},
                    k.render, plane_depth);
                const Float3 corner = k.view_space_position({0, 0}, k.render, plane_depth);
                const float half_viewport_width = length2(render_f);
                const float depth_threshold =
                    std::max(current_depth_view_space, prev_nearest_depth_view_space);
                constexpr float kSep = 1.37e-05F;
                const float length_corner = std::sqrt(
                    (corner[0] * corner[0]) + (corner[1] * corner[1]) + (corner[2] * corner[2]));
                const float length_center = std::sqrt(
                    (center[0] * center[0]) + (center[1] * center[1]) + (center[2] * center[2]));
                const float fov = length_corner / length_center;
                const float required_depth_separation =
                    kSep * fov * half_viewport_width * depth_threshold;
                const float resolution_factor =
                    saturate(length2(render_f) / length2({1920.0F, 1080.0F}));
                const float power = lerp(1.0F, 3.0F, resolution_factor);
                depth += power_of(saturate(required_depth_separation / depth_diff), power) * weight;
                weight_sum += weight;
            }
        }
        return weight_sum > 0.0F ? saturate(1.0F - (depth / weight_sum)) : 0.0F;
    };

    const auto compute_motion_divergence = [&](const Int2& pos) {
        float minconvergence = 1.0F;
        const Float2 nucleus = in.motion->rg(pos);
        const float nucleus_velocity_lr =
            length2({nucleus[0] * render_f[0], nucleus[1] * render_f[1]});
        float max_velocity_uv = length2(nucleus);
        constexpr float kMotionVectorVelocityEpsilon = 1e-02F;
        if (nucleus_velocity_lr > kMotionVectorVelocityEpsilon) {
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    const Int2 sp = clamp_load(pos, {x, y}, k.render);
                    const Float2 mv = in.motion->rg(sp);
                    float velocity_uv = length2(mv);
                    max_velocity_uv = std::max(velocity_uv, max_velocity_uv);
                    velocity_uv = std::max(velocity_uv, max_velocity_uv);
                    minconvergence = std::min(
                        minconvergence, ((mv[0] / velocity_uv) * (nucleus[0] / velocity_uv)) +
                                            ((mv[1] / velocity_uv) * (nucleus[1] / velocity_uv)));
                }
            }
        }
        return saturate(1.0F - minconvergence) * saturate(max_velocity_uv / 0.01F);
    };

    const auto compute_depth_divergence = [&](const Int2& pos) {
        const float max_dist = k.max_distance_in_meters();
        float depth_max = 0.0F;
        float depth_min = max_dist;
        bool max_dist_found = false;
        for (int y = -1; y < 2; ++y) {
            for (int x = -1; x < 2; ++x) {
                const Int2 sample_pos{pos[0] + x, pos[1] + y};
                const float on_screen = is_on_screen(sample_pos, k.render) ? 1.0F : 0.0F;
                const float depth = k.view_space_depth(in.dilated_depth->r(sample_pos)) * on_screen;
                max_dist_found = max_dist_found || max_dist == depth;
                depth_min = std::min(depth_min, depth);
                depth_max = std::max(depth_max, depth);
            }
        }
        return (1.0F - (depth_min / depth_max)) * (max_dist_found ? 0.0F : 1.0F);
    };

    const auto compute_temporal_motion_divergence = [&](const Int2& pos) {
        const Float2 uv{(static_cast<float>(pos[0]) + 0.5F) / render_f[0],
                        (static_cast<float>(pos[1]) + 0.5F) / render_f[1]};
        const Float2 mv = in.dilated_motion->rg(pos);
        const Float2 reprojected = clamp_uv({uv[0] + mv[0], uv[1] + mv[1]}, k.render, k.render);
        const Float4 previous = in.previous_dilated_motion->linear(reprojected);
        const float px_distance = length2({mv[0] * display_f[0], mv[1] * display_f[1]});
        return px_distance > 1.0F
                   ? lerp(0.0F, 1.0F - saturate(length2({previous[0], previous[1]}) / length2(mv)),
                          saturate(power_of(px_distance / 20.0F, 3.0F)))
                   : 0.0F;
    };

    const auto evaluate_surface = [&](const Int2& pos) {
        const float d0 = k.view_space_depth(reconstructed_prev_depth({pos[0], pos[1] - 1}));
        const float d1 = k.view_space_depth(reconstructed_prev_depth({pos[0], pos[1]}));
        const float d2 = k.view_space_depth(reconstructed_prev_depth({pos[0], pos[1] + 1}));
        return 1.0F - (((d0 - d1) > (d1 * 0.01F)) && ((d1 - d2) > (d2 * 0.01F)) ? 1.0F : 0.0F);
    };

    Clipped out;
    out.prepared.resize(pixels(k.render) * 4);
    out.reactive.resize(pixels(k.render) * 2);
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            const Int2 pos{x, y};
            const Float2 depth_uv{(static_cast<float>(x) + 0.5F) / render_f[0],
                                  (static_cast<float>(y) + 0.5F) / render_f[1]};
            Float2 mv = in.dilated_motion->rg(pos);
            const float keep =
                length2({mv[0] * display_f[0], mv[1] * display_f[1]}) > 0.01F ? 1.0F : 0.0F;
            mv = {mv[0] * keep, mv[1] * keep};
            const Float2 dilated_uv{depth_uv[0] + mv[0], depth_uv[1] + mv[1]};
            const float dilated_depth = in.dilated_depth->r(pos);
            const float depth_clip =
                compute_depth_clip(dilated_uv, dilated_depth) * evaluate_surface(pos);
            // ComputePreparedInputColor
            Float3 rgb = max3({0.0F, 0.0F, 0.0F}, in.color->rgb(pos));
            rgb = rgb_to_ycocg(prepare_rgb(rgb, k.exposure, 1.0F));
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(k.render[0])) +
                static_cast<std::size_t>(x);
            out.prepared[at * 4] = round_half(rgb[0]);
            out.prepared[(at * 4) + 1] = round_half(rgb[1]);
            out.prepared[(at * 4) + 2] = round_half(rgb[2]);
            out.prepared[(at * 4) + 3] = round_half(depth_clip);
            const float motion_divergence = compute_motion_divergence(pos);
            const float temporal_motion_difference =
                saturate(compute_temporal_motion_divergence(pos) - compute_depth_divergence(pos));
            out.reactive[at * 2] = 0.0F;
            out.reactive[(at * 2) + 1] =
                round_unorm8(std::max(temporal_motion_difference, motion_divergence));
        }
    }
    return out;
}

// fsr2_lock.hlsl: the display pixels thin features lock.
std::vector<float> lock_pass(const Constants& k, const Plane& lock_luma) {
    const auto thin_feature_confidence = [&](const Int2& pos) {
        const float nucleus = lock_luma.r(pos);
        constexpr float kSimilarThreshold = 1.05F;
        float dissimilar_min = kFltMax;
        float dissimilar_max = 0.0F;
        std::uint32_t mask = 1U << 4U;
        constexpr std::array<std::uint32_t, 4> kRejectionMasks{
            (1U << 0U) | (1U << 1U) | (1U << 3U) | (1U << 4U),
            (1U << 1U) | (1U << 2U) | (1U << 4U) | (1U << 5U),
            (1U << 3U) | (1U << 4U) | (1U << 6U) | (1U << 7U),
            (1U << 4U) | (1U << 5U) | (1U << 7U) | (1U << 8U),
        };
        std::uint32_t idx = 0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x, ++idx) {
                if (x == 0 && y == 0) {
                    continue;
                }
                const float sample = lock_luma.r(clamp_load(pos, {x, y}, k.render));
                const float low = std::min(sample, nucleus);
                const float high = std::max(sample, nucleus);
                if (low > 0.0F && (high / low) < kSimilarThreshold) {
                    mask |= 1U << idx;
                } else {
                    dissimilar_min = std::min(dissimilar_min, sample);
                    dissimilar_max = std::max(dissimilar_max, sample);
                }
            }
        }
        if (nucleus <= dissimilar_max && nucleus >= dissimilar_min) {
            return false;
        }
        return std::ranges::none_of(kRejectionMasks,
                                    [&](std::uint32_t m) { return (mask & m) == m; });
    };
    std::vector<float> locks(pixels(k.display), 0.0F);
    for (int y = 0; y < k.display[1]; ++y) {
        for (int x = 0; x < k.display[0]; ++x) {
            const Float2 center{
                (((static_cast<float>(x) + 0.5F) * static_cast<float>(k.render[0])) /
                 static_cast<float>(k.display[0])) -
                    0.5F + k.jitter[0],
                (((static_cast<float>(y) + 0.5F) * static_cast<float>(k.render[1])) /
                 static_cast<float>(k.display[1])) -
                    0.5F + k.jitter[1]};
            const Int2 base{static_cast<int>(std::floor(center[0])),
                            static_cast<int>(std::floor(center[1]))};
            float lock = 0.0F;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const Int2 lr{base[0] + dx, base[1] + dy};
                    if (is_on_screen(lr, k.render) && k.hr_pos_from_lr_pos(lr) == Int2{x, y} &&
                        thin_feature_confidence(lr)) {
                        lock = 1.0F;
                    }
                }
            }
            locks[(static_cast<std::size_t>(y) * static_cast<std::size_t>(k.display[0])) +
                  static_cast<std::size_t>(x)] = lock;
        }
    }
    return locks;
}

// fsr2_accumulate.hlsl.
struct AccumulateInputs {
    const Plane* dilated_motion = nullptr;
    const Plane* prepared = nullptr;
    const Plane* reactive = nullptr;
    const Plane* history_color = nullptr;
    const Plane* history_lock_status = nullptr;
    const Plane* new_locks = nullptr;
    const Plane* luma_mip = nullptr;
    const Plane* history_luma = nullptr;
};

struct Params {
    Int2 hr_pos{};
    Float2 hr_uv{};
    Float2 lr_uv_hw_sampler{};
    Float2 motion_vector{};
    Float2 reprojected_hr_uv{};
    float hr_velocity = 0.0F;
    float depth_clip_factor = 0.0F;
    float dilated_reactive_factor = 0.0F;
    float accumulation_mask = 0.0F;
    bool is_reset_frame = false;
    bool is_existing_sample = false;
    bool is_new_sample = false;
};

struct RectificationBox {
    Float3 center{0.0F, 0.0F, 0.0F};
    Float3 vec{0.0F, 0.0F, 0.0F};
    Float3 aabb_min{kFltMax, kFltMax, kFltMax};
    Float3 aabb_max{-kFltMax, -kFltMax, -kFltMax};
    float center_weight = 0.0F;

    void add_sample(bool initial, const Float3& color, float weight) {
        const Float3 weighted = mul3(color, weight);
        if (initial) {
            aabb_min = color;
            aabb_max = color;
            center = weighted;
            vec = mul3(color, weighted);
            center_weight = weight;
        } else {
            aabb_min = min3(aabb_min, color);
            aabb_max = max3(aabb_max, color);
            center = add3(center, weighted);
            vec = add3(vec, mul3(color, weighted));
            center_weight += weight;
        }
    }
    void compute_variance_box_data() {
        center_weight = std::abs(center_weight) > kEpsilon ? center_weight : 1.0F;
        center = mul3(center, 1.0F / center_weight);
        vec = mul3(vec, 1.0F / center_weight);
        const Float3 variance = sub3(vec, mul3(center, center));
        vec = {std::sqrt(std::abs(variance[0])), std::sqrt(std::abs(variance[1])),
               std::sqrt(std::abs(variance[2]))};
    }
};

bool is_uv_inside(const Float2& uv) {
    return uv[0] >= 0.0F && uv[0] <= 1.0F && uv[1] >= 0.0F && uv[1] <= 1.0F;
}

struct Accumulated {
    Float4 color{};
    Float2 lock_status{};
    Float4 luma_history{};
};

class Accumulator {
public:
    Accumulator(const Constants& k, const AccumulateInputs& in) : k_(k), in_(in) {}

    [[nodiscard]] Accumulated run(const Int2& hr_pos) const {
        const Params params = init_params(hr_pos);
        Float3 history_color{0.0F, 0.0F, 0.0F};
        Float2 lock_status{0.0F, 0.0F};
        float temporal_reactive_factor = 0.0F;
        bool in_motion_last_frame = false;
        bool new_lock = false;
        if (params.is_existing_sample && !params.is_reset_frame) {
            reproject_history_color(params, history_color, temporal_reactive_factor,
                                    in_motion_last_frame);
            new_lock = in_.new_locks->r(params.hr_pos) > (127.0F / 255.0F);
            const Float4 sampled =
                bilinear_sample(*in_.history_lock_status, params.reprojected_hr_uv);
            lock_status = {sampled[0], sampled[1]};
        }
        float this_frame_reactive_factor =
            std::max(params.dilated_reactive_factor, temporal_reactive_factor);
        float luminance_diff = 0.0F;
        float lock_contribution_this_frame = 0.0F;
        update_lock_status(params, this_frame_reactive_factor, new_lock, lock_status,
                           lock_contribution_this_frame, luminance_diff);
        RectificationBox box;
        const Float4 upsampled =
            upsampled_color_and_weight(params, box, this_frame_reactive_factor);
        Accumulated result;
        const float luma_instability = luma_instability_factor(
            params, box, this_frame_reactive_factor, luminance_diff, result.luma_history);
        Float3 accumulation = base_accumulation_weight(params, this_frame_reactive_factor,
                                                       in_motion_last_frame, upsampled[3]);
        if (params.is_new_sample) {
            history_color = ycocg_to_rgb({upsampled[0], upsampled[1], upsampled[2]});
        } else {
            rectify_history(params, box, history_color, accumulation, lock_contribution_this_frame,
                            luma_instability);
            accumulate(history_color, accumulation, upsampled);
        }
        history_color = unprepare_rgb(history_color, k_.exposure);
        lock_status = finalize_lock_status(params, lock_status, upsampled[3]);
        temporal_reactive_factor = temporal_reactive_factor_for(params, this_frame_reactive_factor);
        result.color = {history_color[0], history_color[1], history_color[2],
                        temporal_reactive_factor};
        result.lock_status = lock_status;
        return result;
    }

private:
    [[nodiscard]] Params init_params(const Int2& hr_pos) const {
        Params params;
        params.hr_pos = hr_pos;
        const Float2 hr_uv{
            (static_cast<float>(hr_pos[0]) + 0.5F) / static_cast<float>(k_.display[0]),
            (static_cast<float>(hr_pos[1]) + 0.5F) / static_cast<float>(k_.display[1])};
        params.hr_uv = hr_uv;
        const Float2 lr_uv_jittered{hr_uv[0] + (k_.jitter[0] / static_cast<float>(k_.render[0])),
                                    hr_uv[1] + (k_.jitter[1] / static_cast<float>(k_.render[1]))};
        params.lr_uv_hw_sampler = clamp_uv(lr_uv_jittered, k_.render, k_.render);
        params.motion_vector =
            in_.dilated_motion->rg({static_cast<int>(hr_uv[0] * static_cast<float>(k_.render[0])),
                                    static_cast<int>(hr_uv[1] * static_cast<float>(k_.render[1]))});
        params.hr_velocity = length2({params.motion_vector[0] * static_cast<float>(k_.display[0]),
                                      params.motion_vector[1] * static_cast<float>(k_.display[1])});
        params.reprojected_hr_uv = {params.hr_uv[0] + params.motion_vector[0],
                                    params.hr_uv[1] + params.motion_vector[1]};
        params.is_existing_sample = is_uv_inside(params.reprojected_hr_uv);
        params.depth_clip_factor = saturate(in_.prepared->linear(params.lr_uv_hw_sampler)[3]);
        const Float4 masks = in_.reactive->linear(params.lr_uv_hw_sampler);
        params.dilated_reactive_factor = masks[0];
        params.accumulation_mask = masks[1];
        params.is_reset_frame = k_.frame_index == 0;
        params.is_new_sample = !params.is_existing_sample || params.is_reset_frame;
        return params;
    }

    void reproject_history_color(const Params& params, Float3& history_color,
                                 float& temporal_reactive_factor,
                                 bool& in_motion_last_frame) const {
        const Float4 history = lanczos2_sample(*in_.history_color, params.reprojected_hr_uv);
        history_color = prepare_rgb({history[0], history[1], history[2]}, k_.exposure, 1.0F);
        history_color = rgb_to_ycocg(history_color);
        temporal_reactive_factor = saturate(std::abs(history[3]));
        in_motion_last_frame = history[3] < 0.0F;
    }

    [[nodiscard]] float shading_change_luma(const Params& params) const {
        const Int2 mip_render_size{static_cast<int>(static_cast<float>(k_.render[0]) / 32.0F),
                                   static_cast<int>(static_cast<float>(k_.render[1]) / 32.0F)};
        const Float2 uv = clamp_uv(params.hr_uv, mip_render_size, k_.luma_mip);
        const float luma = k_.exposure * slash::exp(in_.luma_mip->linear(uv)[0]);
        return power_of(luma, 1.0F / 6.0F);
    }

    void update_lock_status(const Params& params, float& reactive_factor, bool new_lock,
                            Float2& lock_status, float& lock_contribution_this_frame,
                            float& luminance_diff) const {
        const float luma = shading_change_luma(params);
        lock_status[kLockTemporalLuma] =
            lock_status[kLockTemporalLuma] == 0.0F ? luma : lock_status[kLockTemporalLuma];
        const float previous_luma = lock_status[kLockTemporalLuma];
        luminance_diff = 1.0F - min_divided_by_max(previous_luma, luma);
        if (new_lock) {
            lock_status[kLockTemporalLuma] = luma;
            lock_status[kLockLifetimeRemaining] =
                lock_status[kLockLifetimeRemaining] != 0.0F ? 2.0F : 1.0F;
        } else if (lock_status[kLockLifetimeRemaining] <= 1.0F) {
            lock_status[kLockTemporalLuma] = lerp(lock_status[kLockTemporalLuma], luma, 0.5F);
        } else if (luminance_diff > 0.1F) {
            lock_status[kLockLifetimeRemaining] = 0.0F;
        }
        reactive_factor = std::max(reactive_factor, saturate((luminance_diff - 0.1F) * 10.0F));
        lock_status[kLockLifetimeRemaining] *= (1.0F - reactive_factor);
        lock_status[kLockLifetimeRemaining] *= saturate(1.0F - params.accumulation_mask);
        lock_status[kLockLifetimeRemaining] *= params.depth_clip_factor < 0.1F ? 1.0F : 0.0F;
        const float lifetime_contribution = saturate(lock_status[kLockLifetimeRemaining] - 1.0F);
        const float shading_change_contribution =
            saturate(min_divided_by_max(lock_status[kLockTemporalLuma], luma));
        lock_contribution_this_frame =
            saturate(saturate(lifetime_contribution * 4.0F) * shading_change_contribution);
    }

    [[nodiscard]] float max_kernel_weight() const {
        constexpr float kKernelSizeBias = 1.0F;
        const float weight = 1.0F + (((1.0F / k_.downscale[0]) - 1.0F) * kKernelSizeBias);
        return std::min(1.99F, weight);
    }

    [[nodiscard]] Float4 upsampled_color_and_weight(const Params& params, RectificationBox& box,
                                                    float reactive_factor) const {
        const Float2 dst_output_pos{static_cast<float>(params.hr_pos[0]) + 0.5F,
                                    static_cast<float>(params.hr_pos[1]) + 0.5F};
        const Float2 src_output_pos{dst_output_pos[0] * k_.downscale[0],
                                    dst_output_pos[1] * k_.downscale[1]};
        const Int2 src_input_pos{static_cast<int>(std::floor(src_output_pos[0])),
                                 static_cast<int>(std::floor(src_output_pos[1]))};
        std::array<Float3, 16> samples{};
        const Float2 src_unjittered_pos{
            (static_cast<float>(src_input_pos[0]) + 0.5F) - k_.jitter[0],
            (static_cast<float>(src_input_pos[1]) + 0.5F) - k_.jitter[1]};
        const bool flip_col = src_unjittered_pos[0] > src_output_pos[0];
        const bool flip_row = src_unjittered_pos[1] > src_output_pos[1];
        const Int2 offset_tl{flip_col ? -2 : -1, flip_row ? -2 : -1};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const std::size_t index =
                    static_cast<std::size_t>(col) + (static_cast<std::size_t>(row) * 4);
                const Int2 col_row{flip_col ? (3 - col) : col, flip_row ? (3 - row) : row};
                const Int2 sample_pos{src_input_pos[0] + offset_tl[0] + col_row[0],
                                      src_input_pos[1] + offset_tl[1] + col_row[1]};
                samples.at(index) = in_.prepared->rgb(clamp_load(sample_pos, {0, 0}, k_.render));
            }
        }
        Float4 color_and_weight{0.0F, 0.0F, 0.0F, 0.0F};
        const Float2 base_sample_offset{src_unjittered_pos[0] - src_output_pos[0],
                                        src_unjittered_pos[1] - src_output_pos[1]};
        const float kernel_reactive_factor =
            std::max(reactive_factor, params.is_new_sample ? 1.0F : 0.0F);
        const float kernel_bias_max = max_kernel_weight() * (1.0F - kernel_reactive_factor);
        const float kernel_bias_min = std::max(1.0F, (1.0F + kernel_bias_max) * 0.3F);
        const float kernel_bias_factor =
            std::max(0.0F, std::max(0.25F * params.depth_clip_factor, kernel_reactive_factor));
        const float kernel_bias = lerp(kernel_bias_max, kernel_bias_min, kernel_bias_factor);
        const float rectification_curve_bias =
            lerp(-2.0F, -3.0F, saturate(params.hr_velocity / 50.0F));
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const std::size_t index =
                    static_cast<std::size_t>(col) + (static_cast<std::size_t>(row) * 4);
                const Int2 col_row{flip_col ? (3 - col) : col, flip_row ? (3 - row) : row};
                const Float2 offset{static_cast<float>(offset_tl[0] + col_row[0]),
                                    static_cast<float>(offset_tl[1] + col_row[1])};
                const Float2 src_sample_offset{base_sample_offset[0] + offset[0],
                                               base_sample_offset[1] + offset[1]};
                const Int2 sample_pos{src_input_pos[0] + offset_tl[0] + col_row[0],
                                      src_input_pos[1] + offset_tl[1] + col_row[1]};
                const float on_screen = is_on_screen(sample_pos, k_.render) ? 1.0F : 0.0F;
                const Float2 biased{src_sample_offset[0] * kernel_bias,
                                    src_sample_offset[1] * kernel_bias};
                const float weight = on_screen * lanczos2_approx_sq((biased[0] * biased[0]) +
                                                                    (biased[1] * biased[1]));
                const Float3& sample = samples.at(index);
                color_and_weight = {color_and_weight[0] + (sample[0] * weight),
                                    color_and_weight[1] + (sample[1] * weight),
                                    color_and_weight[2] + (sample[2] * weight),
                                    color_and_weight[3] + weight};
                const float offset_sq = (src_sample_offset[0] * src_sample_offset[0]) +
                                        (src_sample_offset[1] * src_sample_offset[1]);
                const float box_weight = slash::exp(rectification_curve_bias * offset_sq);
                box.add_sample(row == 0 && col == 0, sample, box_weight);
            }
        }
        box.compute_variance_box_data();
        color_and_weight[3] *= color_and_weight[3] > kEpsilon ? 1.0F : 0.0F;
        if (color_and_weight[3] > kEpsilon) {
            Float3 rgb = mul3({color_and_weight[0], color_and_weight[1], color_and_weight[2]},
                              1.0F / color_and_weight[3]);
            color_and_weight[3] *= kUpsampleLanczosWeightScale;
            rgb = clamp3(rgb, box.aabb_min, box.aabb_max);
            color_and_weight = {rgb[0], rgb[1], rgb[2], color_and_weight[3]};
        }
        return color_and_weight;
    }

    static void accumulate(Float3& history_color, Float3 accumulation, Float4 upsampled) {
        accumulation = max3({kEpsilon, kEpsilon, kEpsilon},
                            add3(accumulation, {upsampled[3], upsampled[3], upsampled[3]}));
        const Float3 color =
            rgb_to_ycocg(tonemap(ycocg_to_rgb({upsampled[0], upsampled[1], upsampled[2]})));
        history_color = rgb_to_ycocg(tonemap(ycocg_to_rgb(history_color)));
        const Float3 alpha{upsampled[3] / accumulation[0], upsampled[3] / accumulation[1],
                           upsampled[3] / accumulation[2]};
        history_color = lerp3(history_color, color, alpha);
        history_color = ycocg_to_rgb(history_color);
        history_color = inverse_tonemap(history_color);
    }

    void rectify_history(const Params& params, const RectificationBox& box, Float3& history_color,
                         Float3& accumulation, float lock_contribution_this_frame,
                         float luma_instability_factor) const {
        const float scale_factor_influence =
            std::min(20.0F, power_of(1.0F / std::abs(k_.downscale[0] * k_.downscale[1]), 3.0F));
        const float velocity_factor = saturate(params.hr_velocity / 20.0F);
        const float box_scale_t =
            std::max(params.depth_clip_factor, std::max(params.accumulation_mask, velocity_factor));
        const float box_scale = lerp(scale_factor_influence, 1.0F, box_scale_t);
        const Float3 scaled_box_vec = mul3(box.vec, box_scale);
        Float3 box_min = sub3(box.center, scaled_box_vec);
        Float3 box_max = add3(box.center, scaled_box_vec);
        box_min = max3(box.aabb_min, box_min);
        box_max = min3(box.aabb_max, box_max);
        bool outside = false;
        for (std::size_t c = 0; c < 3; ++c) {
            outside = outside || box_min.at(c) > history_color.at(c) ||
                      history_color.at(c) > box_max.at(c);
        }
        if (outside) {
            const Float3 clamped = clamp3(history_color, box_min, box_max);
            float history_contribution =
                std::max(luma_instability_factor, lock_contribution_this_frame);
            const float reactive_contribution =
                1.0F - power_of(params.dilated_reactive_factor, 1.0F / 2.0F);
            history_contribution *= reactive_contribution;
            history_color = lerp3(clamped, history_color, saturate(history_contribution));
            const Float3 accumulation_min = min3(accumulation, {0.1F, 0.1F, 0.1F});
            accumulation = lerp3(accumulation_min, accumulation, saturate(history_contribution));
        }
    }

    [[nodiscard]] Float2 finalize_lock_status(const Params& params, Float2 lock_status,
                                              float upsampled_weight) const {
        const Float2 next{params.hr_uv[0] - params.motion_vector[0],
                          params.hr_uv[1] - params.motion_vector[1]};
        if (!is_uv_inside(next)) {
            lock_status[kLockLifetimeRemaining] = 0.0F;
        } else {
            const float decrease_max = k_.phases * kAverageLanczosWeightPerFrame;
            const float decrease = upsampled_weight / decrease_max;
            lock_status[kLockLifetimeRemaining] =
                std::max(0.0F, lock_status[kLockLifetimeRemaining] - decrease);
        }
        return lock_status;
    }

    [[nodiscard]] static Float3 base_accumulation_weight(const Params& params,
                                                         float this_frame_reactive_factor,
                                                         bool in_motion_last_frame,
                                                         float upsampled_weight) {
        float base = kMaxAccumulationLanczosWeight * (params.is_existing_sample ? 1.0F : 0.0F) *
                     (1.0F - this_frame_reactive_factor) * (1.0F - params.depth_clip_factor);
        base = std::min(base, lerp(base, upsampled_weight * 10.0F,
                                   std::max(in_motion_last_frame ? 1.0F : 0.0F,
                                            saturate(params.hr_velocity * 10.0F))));
        base = std::min(base, lerp(base, upsampled_weight, saturate(params.hr_velocity / 20.0F)));
        return {base, base, base};
    }

    [[nodiscard]] float luma_instability_factor(const Params& params, const RectificationBox& box,
                                                float this_frame_reactive_factor,
                                                float luminance_diff, Float4& luma_out) const {
        constexpr float kUnormThreshold = 1.0F / 255.0F;
        float current = box.center[0];
        current = current / (1.0F + std::max(0.0F, current));
        current = round_nearest(current * 255.0F) / 255.0F;
        const bool sample_luma_history =
            std::max(std::max(params.depth_clip_factor, params.accumulation_mask), luminance_diff) <
                0.1F &&
            !params.is_new_sample;
        Float4 history = sample_luma_history ? in_.history_luma->linear(params.reprojected_hr_uv)
                                             : Float4{0.0F, 0.0F, 0.0F, 0.0F};
        float instability = 0.0F;
        const float diffs0 = current - history[0];
        float smallest = std::abs(diffs0);
        if (smallest >= kUnormThreshold) {
            const auto sign = [](float v) {
                if (v > 0.0F) {
                    return 1.0F;
                }
                return v < 0.0F ? -1.0F : 0.0F;
            };
            for (std::size_t i = 1; i <= 3; ++i) {
                const float diffs1 = current - history.at(i);
                if (sign(diffs0) == sign(diffs1)) {
                    smallest = std::min(smallest, std::abs(diffs1));
                }
            }
            const float box_size_factor = power_of(saturate(box.vec[0] / 0.1F), 6.0F);
            instability = (smallest != std::abs(diffs0) ? 1.0F : 0.0F) * box_size_factor;
            instability = instability > kUnormThreshold ? 1.0F : 0.0F;
            instability *= 1.0F - std::max(params.accumulation_mask,
                                           power_of(this_frame_reactive_factor, 1.0F / 6.0F));
        }
        history[3] = history[2];
        history[2] = history[1];
        history[1] = history[0];
        history[0] = current;
        luma_out = history;
        return instability * (history[3] != 0.0F ? 1.0F : 0.0F);
    }

    [[nodiscard]] static float temporal_reactive_factor_for(const Params& params,
                                                            float temporal_reactive_factor) {
        float factor = std::min(0.99F, temporal_reactive_factor);
        factor = std::max(factor, lerp(factor, 0.4F, saturate(params.hr_velocity)));
        factor = std::max(factor * factor, std::max(params.depth_clip_factor * 0.1F,
                                                    params.dilated_reactive_factor));
        factor = params.is_new_sample ? 1.0F : factor;
        if (saturate(params.hr_velocity * 10.0F) >= 1.0F) {
            factor = std::max(kEpsilon, factor) * -1.0F;
        }
        return factor;
    }

    const Constants& k_;
    const AccumulateInputs& in_;
};

// ffxApproximateReciprocalMedium.
float rcp_medium(float a) {
    const auto b = slash::bit_cast<float>(0x7ef19fffU - slash::bit_cast<std::uint32_t>(a));
    return b * ((-b * a) + 2.0F);
}

float rcas_lobe(float mn4, float mx4) {
    const float hit_max_den = (4.0F * mn4) - 4.0F;
    if (mx4 <= 0.0F) {
        return (1.0F - mx4) / hit_max_den;
    }
    const float hit_min = mn4 / (4.0F * mx4);
    if (hit_max_den == 0.0F) {
        return -hit_min;
    }
    return std::max(-hit_min, (1.0F - mx4) / hit_max_den);
}

// fsr2_rcas.hlsl.
Float3 rcas(const Constants& k, const Plane& color, const Int2& sp) {
    if (k.copy) {
        return color.rgb(sp);
    }
    constexpr float kRcasLimit = 0.25F - (1.0F / 16.0F);
    const auto load = [&](const Int2& p) { return prepare_rgb(color.rgb(p), k.exposure, 1.0F); };
    const Float3 b = load({sp[0], sp[1] - 1});
    const Float3 d = load({sp[0] - 1, sp[1]});
    const Float3 e = load(sp);
    const Float3 f = load({sp[0] + 1, sp[1]});
    const Float3 h = load({sp[0], sp[1] + 1});
    const auto luma2 = [](const Float3& c) { return (c[2] * 0.5F) + ((c[0] * 0.5F) + c[1]); };
    const float b_l = luma2(b);
    const float d_l = luma2(d);
    const float e_l = luma2(e);
    const float f_l = luma2(f);
    const float h_l = luma2(h);
    float nz = (0.25F * b_l) + (0.25F * d_l) + (0.25F * f_l) + (0.25F * h_l) - e_l;
    const float range = std::max(std::max(std::max(b_l, d_l), std::max(e_l, f_l)), h_l) -
                        std::min(std::min(std::min(b_l, d_l), std::min(e_l, f_l)), h_l);
    nz = saturate(std::abs(nz) * rcp_medium(range));
    nz = (-0.5F * nz) + 1.0F;
    const Float3 mn4 = min3(min3(b, d), min3(f, h));
    const Float3 mx4 = max3(max3(b, d), max3(f, h));
    const float lobe_r = rcas_lobe(mn4[0], mx4[0]);
    const float lobe_g = rcas_lobe(mn4[1], mx4[1]);
    const float lobe_b = rcas_lobe(mn4[2], mx4[2]);
    float lobe = std::max(-kRcasLimit, std::min(std::max(std::max(lobe_r, lobe_g), lobe_b), 0.0F)) *
                 k.sharpness;
    lobe *= nz;
    const float rcp_l = rcp_medium((4.0F * lobe) + 1.0F);
    Float3 pix{};
    for (std::size_t c = 0; c < 3; ++c) {
        pix.at(c) =
            ((lobe * b.at(c)) + (lobe * d.at(c)) + (lobe * h.at(c)) + (lobe * f.at(c)) + e.at(c)) *
            rcp_l;
    }
    return unprepare_rgb(pix, k.exposure);
}

} // namespace

std::array<std::uint32_t, 2> fsr2_render_size(const Scene3D& scene, std::uint32_t width,
                                              std::uint32_t height) {
    const float scale = scene.fsr2 ? scene.fsr2->scale : 1.0F;
    const auto scaled = [&](std::uint32_t size) {
        return static_cast<std::uint32_t>(std::max(static_cast<float>(size) * scale, 1.0F));
    };
    return {scaled(width), scaled(height)};
}

std::uint32_t fsr2_phase_count(std::uint32_t render_width, std::uint32_t display_width) {
    const float ratio = static_cast<float>(display_width) / static_cast<float>(render_width);
    return static_cast<std::uint32_t>(8.0F * ratio * ratio);
}

std::array<float, 2> camera_jitter(std::uint64_t frame, std::uint32_t phases, std::uint32_t width,
                                   std::uint32_t height) {
    const auto phase = static_cast<std::uint32_t>(frame % std::max(phases, 1U));
    return {halton(phase, 2) / static_cast<float>(width),
            halton(phase, 3) / static_cast<float>(height)};
}

std::array<std::uint32_t, 2> fsr2_luma_mip_size(std::uint32_t render_width,
                                                std::uint32_t render_height) {
    return {std::max(render_width / 32U, 1U), std::max(render_height / 32U, 1U)};
}

Fsr2Uniforms fsr2_uniforms(const Fsr2Frame& frame) {
    Fsr2Uniforms u;
    u.sizes = {static_cast<std::int32_t>(frame.render_width),
               static_cast<std::int32_t>(frame.render_height),
               static_cast<std::int32_t>(frame.display_width),
               static_cast<std::int32_t>(frame.display_height)};
    // The jitter in render pixels with y down, as AMD's jitterOffset: clip space's y is up.
    u.jitter_scale = {
        frame.jitter[0] * static_cast<float>(frame.render_width) * 0.5F,
        -frame.jitter[1] * static_cast<float>(frame.render_height) * 0.5F,
        static_cast<float>(frame.render_width) / static_cast<float>(frame.display_width),
        static_cast<float>(frame.render_height) / static_cast<float>(frame.display_height)};
    // AMD's setupDeviceDepthToViewSpaceDepthParams, from the projection itself: its depth row
    // maps view space depth z (negative ahead) to clip space as A z + B, over -z for a
    // perspective camera.
    const Mat4& p = frame.projection;
    const bool ortho = p[11] == 0.0F;
    const float a = p[10];
    const float b = p[14];
    u.view_depth = {ortho ? b : -a, ortho ? -a : b, 1.0F / p[0], 1.0F / p[5]};
    // FsrRcasCon on Godot's sharpness, which turns sharpening off at 0.
    const float sharpness = std::clamp(1.0F - (frame.sharpness / 2.0F), 0.0F, 1.0F);
    const float con = sharpness > 1e-6F
                          ? slash::exp(-((-2.0F * sharpness) + 2.0F) * std::numbers::ln2_v<float>)
                          : 0.0F;
    u.frame = {0.0F, frame.phases, static_cast<float>(frame.frame_index), con};
    const auto mip = fsr2_luma_mip_size(frame.render_width, frame.render_height);
    u.options = {static_cast<std::int32_t>(mip[0]), static_cast<std::int32_t>(mip[1]),
                 sharpness > 1e-6F ? 0 : 1, ortho ? 1 : 0};
    // Godot's (correction * prev_proj) * prev_transform.affine_inverse() * cur_transform *
    // (correction * cur_proj).inverse(), worked out in doubles.
    const std::array<double, 16> to_world = inverse_matrix(frame.view_projection);
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += static_cast<double>(frame.previous.at((k * 4) + row)) *
                       to_world.at((column * 4) + k);
            }
            u.reprojection.at(column).at(row) = static_cast<float>(sum);
        }
    }
    return u;
}

std::array<float, 2> derive_motion_vector(const std::array<Float4, 4>& reprojection,
                                          const std::array<float, 2>& uv, float depth) {
    // Clip space's y runs up and texture coordinates' down; depth is already clip space's.
    const float x = (uv[0] * 2.0F) - 1.0F;
    const float y = 1.0F - (uv[1] * 2.0F);
    Float4 previous{};
    for (std::size_t i = 0; i < 4; ++i) {
        previous.at(i) = (((reprojection[0].at(i) * x) + (reprojection[1].at(i) * y)) +
                          (reprojection[2].at(i) * depth)) +
                         reprojection[3].at(i);
    }
    const float px = previous[0] / previous[3];
    const float py = previous[1] / previous[3];
    return {(0.5F + (px * 0.5F)) - uv[0], (0.5F - (py * 0.5F)) - uv[1]};
}

void Fsr2Clock::advance(std::uint32_t target_phases) {
    if (!primed) {
        frames = 0;
        phases = static_cast<float>(target_phases);
        primed = true;
        return;
    }
    ++frames;
    const auto target = static_cast<float>(target_phases);
    if (target > phases) {
        phases += 1.0F;
    } else if (target < phases) {
        phases -= 1.0F;
    }
}

void run_fsr2(const Fsr2Uniforms& uniforms, const Fsr2Inputs& inputs, Fsr2History& history,
              std::span<float> out) {
    const Constants k(uniforms);
    const std::size_t render_pixels = pixels(k.render);
    const std::size_t display_pixels = pixels(k.display);
    if (k.frame_index == 0 || history.color.size() != display_pixels * 4 ||
        history.dilated_motion.size() != render_pixels * 2) {
        history.color.assign(display_pixels * 4, 0.0F);
        history.lock_status.assign(display_pixels * 2, 0.0F);
        history.luma_history.assign(display_pixels * 4, 0.0F);
        history.dilated_motion.assign(render_pixels * 2, 0.0F);
    }
    const Plane color{.size = k.render, .channels = 4, .data = inputs.color};
    const Plane depth{.size = k.render, .channels = 1, .data = inputs.depth};
    // LoadInputMotionVector with Godot's FFX_FSR2_OPTION_GODOT_DERIVE_INVALID_MOTION_VECTORS: a
    // pixel the prepass drew no motion vector for (the sky) gets one from its depth.
    std::vector<float> loaded(inputs.motion.begin(), inputs.motion.end());
    for (int y = 0; y < k.render[1]; ++y) {
        for (int x = 0; x < k.render[0]; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(k.render[0])) +
                static_cast<std::size_t>(x);
            if (loaded[at * 2] <= kInvalidMotion && loaded[(at * 2) + 1] <= kInvalidMotion) {
                const std::array<float, 2> uv{
                    (static_cast<float>(x) + 0.5F) / static_cast<float>(k.render[0]),
                    (static_cast<float>(y) + 0.5F) / static_cast<float>(k.render[1])};
                const std::array<float, 2> derived =
                    derive_motion_vector(uniforms.reprojection, uv, depth.r({x, y}));
                loaded[at * 2] = derived[0];
                loaded[(at * 2) + 1] = derived[1];
            }
        }
    }
    const Plane motion{.size = k.render, .channels = 2, .data = loaded};

    const std::vector<float> luma_mip = luma_pass(k, color);
    const Dilated dilated = dilate_pass(k, depth, motion, color);
    const Plane dilated_depth{.size = k.render, .channels = 1, .data = dilated.depth};
    const Plane dilated_motion{.size = k.render, .channels = 2, .data = dilated.motion};
    const Plane lock_luma{.size = k.render, .channels = 1, .data = dilated.lock_luma};
    const std::vector<std::uint32_t> reconstructed =
        reconstruct_pass(k, dilated_depth, dilated_motion);
    const Plane previous_dilated_motion{
        .size = k.render, .channels = 2, .data = history.dilated_motion};
    const Clipped clipped = depth_clip_pass(k, {.dilated_motion = &dilated_motion,
                                                .dilated_depth = &dilated_depth,
                                                .color = &color,
                                                .previous_dilated_motion = &previous_dilated_motion,
                                                .motion = &motion,
                                                .reconstructed = &reconstructed});
    const std::vector<float> new_locks = lock_pass(k, lock_luma);

    const Plane prepared{.size = k.render, .channels = 4, .data = clipped.prepared};
    const Plane reactive{.size = k.render, .channels = 2, .data = clipped.reactive};
    const Plane history_color{.size = k.display, .channels = 4, .data = history.color};
    const Plane history_lock_status{.size = k.display, .channels = 2, .data = history.lock_status};
    const Plane locks{.size = k.display, .channels = 1, .data = new_locks};
    const Plane mip{.size = k.luma_mip, .channels = 1, .data = luma_mip};
    const Plane history_luma{.size = k.display, .channels = 4, .data = history.luma_history};
    const AccumulateInputs accumulate_inputs{.dilated_motion = &dilated_motion,
                                             .prepared = &prepared,
                                             .reactive = &reactive,
                                             .history_color = &history_color,
                                             .history_lock_status = &history_lock_status,
                                             .new_locks = &locks,
                                             .luma_mip = &mip,
                                             .history_luma = &history_luma};
    const Accumulator accumulator(k, accumulate_inputs);
    std::vector<float> next_color(display_pixels * 4);
    std::vector<float> next_lock_status(display_pixels * 2);
    std::vector<float> next_luma(display_pixels * 4);
    for (int y = 0; y < k.display[1]; ++y) {
        for (int x = 0; x < k.display[0]; ++x) {
            const Accumulated a = accumulator.run({x, y});
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(k.display[0])) +
                static_cast<std::size_t>(x);
            for (std::size_t c = 0; c < 4; ++c) {
                next_color[(at * 4) + c] = round_half(a.color.at(c));
                next_luma[(at * 4) + c] = round_unorm8(a.luma_history.at(c));
            }
            next_lock_status[at * 2] = round_half(a.lock_status[0]);
            next_lock_status[(at * 2) + 1] = round_half(a.lock_status[1]);
        }
    }
    const Plane internal{.size = k.display, .channels = 4, .data = next_color};
    for (int y = 0; y < k.display[1]; ++y) {
        for (int x = 0; x < k.display[0]; ++x) {
            const Float3 pix = rcas(k, internal, {x, y});
            const std::size_t at =
                ((static_cast<std::size_t>(y) * static_cast<std::size_t>(k.display[0])) +
                 static_cast<std::size_t>(x)) *
                4;
            out[at] = round_half(pix[0]);
            out[at + 1] = round_half(pix[1]);
            out[at + 2] = round_half(pix[2]);
            out[at + 3] = 1.0F;
        }
    }
    history.color = std::move(next_color);
    history.lock_status = std::move(next_lock_status);
    history.luma_history = std::move(next_luma);
    history.dilated_motion = dilated.motion;
}

} // namespace slash::render::detail
