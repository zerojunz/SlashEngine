#include "taa.hpp"

#include "forward_post.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace slash::render {

bool use_taa(const json::Value& rendering) {
    const json::Value* found = rendering.is_object() ? rendering.find("anti_aliasing") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("quality") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("use_taa") : nullptr;
    return found != nullptr && found->is_bool() && found->as_bool();
}

} // namespace slash::render

// The CPU twin of shaders/taa.hlsl, as Godot 4.6's taa_resolve.glsl (MIT, Copyright (c) 2016-2022
// Panos Karabelas, from Spartan Engine). Keep the two in step.
namespace slash::render::detail {

namespace {

using Float2 = std::array<float, 2>;

constexpr float kFltMin = 0.00000001F;
constexpr float kFltMax = 32767.0F;
constexpr float kRpc9 = 0.11111111111F;
constexpr float kRpc16 = 0.0625F;
// Scales the weight of a pixel whose motion changed by more than the threshold.
constexpr float kDisocclusionScale = 0.01F;
// The 3x3 neighborhood, in Godot's order.
constexpr std::array<std::array<int, 2>, 9> kOffsets3x3{{
    {-1, -1},
    {0, -1},
    {1, -1},
    {-1, 0},
    {0, 0},
    {1, 0},
    {-1, 1},
    {0, 1},
    {1, 1},
}};
constexpr Float3 kLumCoeff{0.299F, 0.587F, 0.114F};

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Float3 mul(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
Float3 mul(const Float3& a, const Float3& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}
Float3 clamp3(const Float3& v, const Float3& low, const Float3& high) {
    return {std::clamp(v[0], low[0], high[0]), std::clamp(v[1], low[1], high[1]),
            std::clamp(v[2], low[2], high[2])};
}
float mix(float a, float b, float t) {
    return a + ((b - a) * t);
}
Float3 mix3(const Float3& a, const Float3& b, float t) {
    return {mix(a[0], b[0], t), mix(a[1], b[1], t), mix(a[2], b[2], t)};
}

Float3 reinhard(const Float3& hdr) {
    return {hdr[0] / (hdr[0] + 1.0F), hdr[1] / (hdr[1] + 1.0F), hdr[2] / (hdr[2] + 1.0F)};
}
Float3 reinhard_inverse(const Float3& sdr) {
    return {sdr[0] / (1.0F - sdr[0]), sdr[1] / (1.0F - sdr[1]), sdr[2] / (1.0F - sdr[2])};
}

// The frame's textures, loaded at texel coordinates kept inside them.
struct Frame {
    const TaaInputs* inputs = nullptr;
    int width = 0;
    int height = 0;

    [[nodiscard]] std::size_t at(int x, int y) const {
        return (static_cast<std::size_t>(std::clamp(y, 0, height - 1)) *
                static_cast<std::size_t>(width)) +
               static_cast<std::size_t>(std::clamp(x, 0, width - 1));
    }
    [[nodiscard]] Float3 color(int x, int y) const {
        const std::size_t i = at(x, y) * 4;
        return {inputs->color[i], inputs->color[i + 1], inputs->color[i + 2]};
    }
    [[nodiscard]] Float3 history(int x, int y) const {
        const std::size_t i = at(x, y) * 4;
        return {inputs->history[i], inputs->history[i + 1], inputs->history[i + 2]};
    }
    [[nodiscard]] float depth(int x, int y) const { return inputs->depth[at(x, y)]; }
    [[nodiscard]] Float2 velocity(int x, int y) const {
        const std::size_t i = at(x, y) * 2;
        return {inputs->velocity[i], inputs->velocity[i + 1]};
    }
    [[nodiscard]] Float2 last_velocity(int x, int y) const {
        const std::size_t i = at(x, y) * 2;
        return {inputs->last_velocity[i], inputs->last_velocity[i + 1]};
    }

    // The history filtered linearly at uv, clamped to its edges, as Godot's linear sampler with
    // repeat disabled reads it.
    [[nodiscard]] Float3 history_linear(const Float2& uv) const {
        const float x = (uv[0] * static_cast<float>(width)) - 0.5F;
        const float y = (uv[1] * static_cast<float>(height)) - 0.5F;
        const float x0 = std::floor(x);
        const float y0 = std::floor(y);
        const float fx = x - x0;
        const float fy = y - y0;
        const int ix = static_cast<int>(x0);
        const int iy = static_cast<int>(y0);
        const Float3 top = mix3(history(ix, iy), history(ix + 1, iy), fx);
        const Float3 bottom = mix3(history(ix, iy + 1), history(ix + 1, iy + 1), fx);
        return mix3(top, bottom, fy);
    }
};

// Godot's sample_catmull_rom_9: the history at uv, filtered with a Catmull-Rom spline over 4x4
// texels in nine linear reads.
Float3 sample_catmull_rom_9(const Frame& frame, const Float2& uv, const Float2& resolution) {
    Float3 result{0.0F, 0.0F, 0.0F};
    std::array<Float2, 3> positions{};
    std::array<Float2, 3> weights{};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const float sample_pos = uv.at(axis) * resolution.at(axis);
        const float tex_pos1 = std::floor(sample_pos - 0.5F) + 0.5F;
        const float f = sample_pos - tex_pos1;
        const float w0 = f * (-0.5F + (f * (1.0F - (0.5F * f))));
        const float w1 = 1.0F + (f * f * (-2.5F + (1.5F * f)));
        const float w2 = f * (0.5F + (f * (2.0F - (1.5F * f))));
        const float w3 = f * f * (-0.5F + (0.5F * f));
        const float w12 = w1 + w2;
        const float offset12 = w2 / (w1 + w2);
        positions.at(0).at(axis) = (tex_pos1 - 1.0F) / resolution.at(axis);
        positions.at(1).at(axis) = (tex_pos1 + offset12) / resolution.at(axis);
        positions.at(2).at(axis) = (tex_pos1 + 2.0F) / resolution.at(axis);
        weights.at(0).at(axis) = w0;
        weights.at(1).at(axis) = w12;
        weights.at(2).at(axis) = w3;
    }
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const Float3 texel =
                frame.history_linear({positions.at(column)[0], positions.at(row)[1]});
            result = add(result, mul(texel, weights.at(column)[0] * weights.at(row)[1]));
        }
    }
    return {std::max(result[0], 0.0F), std::max(result[1], 0.0F), std::max(result[2], 0.0F)};
}

// Godot's clip_aabb, from Playdead's "Temporal Reprojection Anti-Aliasing".
Float3 clip_aabb(const Float3& aabb_min, const Float3& aabb_max, const Float3& p, const Float3& q) {
    Float3 r = sub(q, p);
    const Float3 rmax = sub(aabb_max, p);
    const Float3 rmin = sub(aabb_min, p);
    for (std::size_t c = 0; c < 3; ++c) {
        if (r.at(c) > rmax.at(c) + kFltMin) {
            r = mul(r, rmax.at(c) / r.at(c));
        }
    }
    for (std::size_t c = 0; c < 3; ++c) {
        if (r.at(c) < rmin.at(c) - kFltMin) {
            r = mul(r, rmin.at(c) / r.at(c));
        }
    }
    return add(p, r);
}

float length2(const Float2& v) {
    return std::sqrt((v[0] * v[0]) + (v[1] * v[1]));
}

float luminance(const Float3& color) {
    return std::max(
        (color[0] * kLumCoeff[0]) + (color[1] * kLumCoeff[1]) + (color[2] * kLumCoeff[2]), 0.0001F);
}

Float3 resolve_pixel(const Frame& frame, const TaaUniforms& uniforms, int x, int y) {
    const Float2 resolution{static_cast<float>(uniforms.size[0]),
                            static_cast<float>(uniforms.size[1])};
    const Float2 uv{(static_cast<float>(x) + 0.5F) / resolution[0],
                    (static_cast<float>(y) + 0.5F) / resolution[1]};
    const Float2 velocity = frame.velocity(x, y);
    const Float2 uv_reprojected{uv[0] + velocity[0], uv[1] + velocity[1]};
    Float3 color_input = frame.color(x, y);
    Float3 color_history = sample_catmull_rom_9(frame, uv_reprojected, resolution);

    // get_closest_pixel_velocity_3x3: the motion of the neighbor of least depth. With reversed
    // depth, as Godot 4.6 has it, that is the farthest.
    float min_depth = 1.0F;
    std::array<int, 2> min_pos{x, y};
    for (const auto& offset : kOffsets3x3) {
        const float depth = frame.depth(x + offset[0], y + offset[1]);
        if (depth < min_depth) {
            min_depth = depth;
            min_pos = {x + offset[0], y + offset[1]};
        }
    }
    const Float2 velocity_closest = frame.velocity(min_pos[0], min_pos[1]);

    // clip_history_3x3: variance clipping to the neighborhood, its box shrinking with motion.
    Float3 color_avg{0.0F, 0.0F, 0.0F};
    Float3 color_avg2{0.0F, 0.0F, 0.0F};
    for (const auto& offset : kOffsets3x3) {
        const Float3 sample = frame.color(x + offset[0], y + offset[1]);
        color_avg = add(color_avg, sample);
        color_avg2 = add(color_avg2, mul(sample, sample));
    }
    color_avg = mul(color_avg, kRpc9);
    color_avg2 = mul(color_avg2, kRpc9);
    // smoothstep(0.02, 0.0, length), written out.
    const float t = std::clamp((length2(velocity_closest) - 0.02F) / (0.0F - 0.02F), 0.0F, 1.0F);
    const float box_size = mix(0.0F, uniforms.params[1], t * t * (3.0F - (2.0F * t)));
    Float3 dev{};
    for (std::size_t c = 0; c < 3; ++c) {
        dev.at(c) =
            std::sqrt(std::abs(color_avg2.at(c) - (color_avg.at(c) * color_avg.at(c)))) * box_size;
    }
    const Float3 color_min = sub(color_avg, dev);
    const Float3 color_max = add(color_avg, dev);
    color_history =
        clip_aabb(color_min, color_max, clamp3(color_avg, color_min, color_max), color_history);
    color_history = clamp3(color_history, {kFltMin, kFltMin, kFltMin}, {kFltMax, kFltMax, kFltMax});

    float blend_factor = kRpc16;
    {
        const float factor_screen = uv_reprojected[0] < 0.0F || uv_reprojected[1] < 0.0F ||
                                            uv_reprojected[0] > 1.0F || uv_reprojected[1] > 1.0F
                                        ? 1.0F
                                        : 0.0F;
        // get_factor_disocclusion: more of the frame where the motion changed.
        const Float2 previous =
            frame.last_velocity(static_cast<int>(uv_reprojected[0] * resolution[0]),
                                static_cast<int>(uv_reprojected[1] * resolution[1]));
        const Float2 change{(previous[0] * resolution[0]) - (velocity[0] * resolution[0]),
                            (previous[1] * resolution[1]) - (velocity[1] * resolution[1])};
        const float disocclusion = length2(change) - uniforms.params[0];
        const float factor_disocclusion = std::clamp(disocclusion * kDisocclusionScale, 0.0F, 1.0F);
        blend_factor = std::clamp(blend_factor + factor_screen + factor_disocclusion, 0.0F, 1.0F);
    }

    color_history = reinhard(color_history);
    color_input = reinhard(color_input);
    // Less of the frame where it flickers against the history.
    const float lum_color = luminance(color_input);
    const float lum_history = luminance(color_history);
    float diff =
        std::abs(lum_color - lum_history) / std::max(lum_color, std::max(lum_history, 1.001F));
    diff = 1.0F - diff;
    diff = diff * diff;
    blend_factor = mix(0.0F, blend_factor, diff);
    return reinhard_inverse(mix3(color_history, color_input, blend_factor));
}

} // namespace

float halton(std::uint32_t index, std::uint32_t base) {
    float f = 1.0F;
    float r = 0.0F;
    while (index > 0) {
        f = f / static_cast<float>(base);
        r = r + (f * static_cast<float>(index % base));
        index = index / base;
    }
    return (r * 2.0F) - 1.0F;
}

std::array<float, 2> taa_jitter(std::uint64_t frame, std::uint32_t width, std::uint32_t height) {
    const auto phase = static_cast<std::uint32_t>(frame % kTaaPhases);
    return {halton(phase, 2) / static_cast<float>(width),
            halton(phase, 3) / static_cast<float>(height)};
}

Mat4 jittered(const Mat4& projection, const std::array<float, 2>& jitter) {
    Mat4 out = projection;
    for (std::size_t column = 0; column < 4; ++column) {
        const float w = projection.at((column * 4) + 3);
        out.at(column * 4) += jitter[0] * w;
        out.at((column * 4) + 1) += jitter[1] * w;
    }
    return out;
}

MeshCameraUniforms taa_camera_uniforms(const Scene3D& scene, const std::array<float, 2>& jitter,
                                       const Mat4& previous) {
    return {.view_projection = multiply(jittered(scene.projection, jitter), scene.camera),
            .previous_view_projection = previous,
            .jitter = {jitter[0], jitter[1], 0.0F, 0.0F}};
}

std::array<float, 2> motion_vector(const Float4& clip, const Float4& previous) {
    const Float2 position{clip[0] / clip[3], clip[1] / clip[3]};
    const Float2 prev_position{previous[0] / previous[3], previous[1] / previous[3]};
    // Clip space's y runs up and texture coordinates' down.
    return {(prev_position[0] * 0.5F) - (position[0] * 0.5F),
            (position[1] * 0.5F) - (prev_position[1] * 0.5F)};
}

TaaUniforms taa_uniforms(std::uint32_t width, std::uint32_t height) {
    constexpr float kBaseVariance = 1.1F;
    constexpr float kBaseVarianceMin = 0.75F;
    constexpr float kBaseVarianceMax = 1.0F;
    // 1080p taken as the baseline, as most commonly used.
    const float variance_scale = 1080.0F / static_cast<float>(height);
    return {
        .size = {width, height, 0, 0},
        .params = {2.5F,
                   std::clamp(kBaseVariance * variance_scale, kBaseVarianceMin, kBaseVarianceMax),
                   0.0F, 0.0F}};
}

void run_taa(const TaaUniforms& uniforms, const TaaInputs& inputs, std::span<float> out) {
    const Frame frame{.inputs = &inputs,
                      .width = static_cast<int>(uniforms.size[0]),
                      .height = static_cast<int>(uniforms.size[1])};
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const Float3 result = resolve_pixel(frame, uniforms, x, y);
            const std::size_t at = frame.at(x, y) * 4;
            out[at] = round_half(result[0]);
            out[at + 1] = round_half(result[1]);
            out[at + 2] = round_half(result[2]);
            out[at + 3] = 1.0F;
        }
    }
}

} // namespace slash::render::detail
