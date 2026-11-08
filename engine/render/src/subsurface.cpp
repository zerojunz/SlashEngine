#include "subsurface.hpp"

#include "forward_post.hpp"

#include "slash/core/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace slash::render {

SubsurfaceSettings subsurface_settings_from_json(const json::Value& value) {
    SubsurfaceSettings out;
    if (!value.is_object()) {
        return out;
    }
    if (const json::Value* found = value.find("quality"); found != nullptr && found->is_string()) {
        for (std::size_t i = 0; i < kSubsurfaceQualityNames.size(); ++i) {
            if (found->as_string() == kSubsurfaceQualityNames.at(i)) {
                out.quality = static_cast<SubsurfaceQuality>(i);
            }
        }
    }
    const auto number = [&](std::string_view key, float low, float high, float& field) {
        if (const json::Value* found = value.find(key); found != nullptr && found->is_number()) {
            const double read = found->as_double();
            if (read >= low && read <= high) {
                field = static_cast<float>(read);
            }
        }
    };
    number("scale", 0.0F, 1.0F, out.scale);
    number("depth_scale", 0.0F, 1.0F, out.depth_scale);
    return out;
}

} // namespace slash::render

namespace slash::render::detail {

namespace {

struct Tap {
    float weight;
    float offset;
    std::array<float, 3> skin;
};

// Jimenez's kernels as Godot's subsurface_scattering.glsl holds them: each sample's weight, its
// offset and its weights for skin, from the center out, for 11, 17 and 25 samples across.
constexpr std::array<Tap, 6> kKernel11{{
    {0.560479F, 0.0F, {0.560479F, 0.669086F, 0.784728F}},
    {0.0771802F, 0.08F, {0.0771802F, 0.113491F, 0.0793803F}},
    {0.0821904F, 0.32F, {0.0821904F, 0.0358608F, 0.0209261F}},
    {0.03639F, 0.72F, {0.03639F, 0.0130999F, 0.00643685F}},
    {0.0192831F, 1.28F, {0.0192831F, 0.00282018F, 0.00084214F}},
    {0.00471691F, 2.0F, {0.00471691F, 0.000184771F, 5.07565e-005F}},
}};
constexpr std::array<Tap, 9> kKernel17{{
    {0.536343F, 0.0F, {0.536343F, 0.624624F, 0.748867F}},
    {0.0324462F, 0.03125F, {0.0324462F, 0.0656718F, 0.0532821F}},
    {0.0582416F, 0.125F, {0.0582416F, 0.0659959F, 0.0411329F}},
    {0.0571056F, 0.28125F, {0.0571056F, 0.0287432F, 0.0172844F}},
    {0.0347317F, 0.5F, {0.0347317F, 0.0151085F, 0.00871983F}},
    {0.0216301F, 0.78125F, {0.0216301F, 0.00794618F, 0.00376991F}},
    {0.0144609F, 1.125F, {0.0144609F, 0.00317269F, 0.00106399F}},
    {0.0100386F, 1.53125F, {0.0100386F, 0.000914679F, 0.000275702F}},
    {0.00317394F, 2.0F, {0.00317394F, 0.000134823F, 3.77269e-005F}},
}};
constexpr std::array<Tap, 13> kKernel25{{
    {0.530605F, 0.0F, {0.530605F, 0.613514F, 0.739601F}},
    {0.0211412F, 0.0208333F, {0.0211412F, 0.0459286F, 0.0378196F}},
    {0.0402784F, 0.0833333F, {0.0402784F, 0.0657244F, 0.04631F}},
    {0.0493588F, 0.1875F, {0.0493588F, 0.0367726F, 0.0219485F}},
    {0.0410172F, 0.333333F, {0.0410172F, 0.0199899F, 0.0118481F}},
    {0.0263642F, 0.520833F, {0.0263642F, 0.0119715F, 0.00684598F}},
    {0.017924F, 0.75F, {0.017924F, 0.00711691F, 0.00347194F}},
    {0.0128496F, 1.02083F, {0.0128496F, 0.00356329F, 0.00132016F}},
    {0.0094389F, 1.33333F, {0.0094389F, 0.00139119F, 0.000416598F}},
    {0.00700976F, 1.6875F, {0.00700976F, 0.00049366F, 0.000151938F}},
    {0.00500364F, 2.08333F, {0.00500364F, 0.00020094F, 5.28848e-005F}},
    {0.00333804F, 2.52083F, {0.00333804F, 7.85443e-005F, 1.2945e-005F}},
    {0.000973794F, 3.0F, {0.000973794F, 1.11862e-005F, 9.43437e-007F}},
}};

std::span<const Tap> kernel_of(std::uint32_t samples) {
    if (samples == 13) {
        return kKernel25;
    }
    if (samples == 9) {
        return kKernel17;
    }
    return kKernel11;
}

std::array<float, 3> weight_of(const Tap& tap, bool skin) {
    return skin ? tap.skin : std::array<float, 3>{tap.weight, tap.weight, tap.weight};
}

float lerp(float a, float b, float t) {
    return a + (t * (b - a));
}

struct Source {
    std::span<const float> pixels;
    int width;
    int height;

    [[nodiscard]] Float4 load(int x, int y) const {
        const int cx = std::clamp(x, 0, width - 1);
        const int cy = std::clamp(y, 0, height - 1);
        const std::size_t at = (static_cast<std::size_t>(cy) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(cx)) *
                               4;
        return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
    }

    // source_linear: filtered linearly at uv, clamped to the edges.
    [[nodiscard]] Float4 linear(float u, float v) const {
        const float px = (u * static_cast<float>(width)) - 0.5F;
        const float py = (v * static_cast<float>(height)) - 0.5F;
        const float bx = std::floor(px);
        const float by = std::floor(py);
        const float fx = px - bx;
        const float fy = py - by;
        const int x = static_cast<int>(bx);
        const int y = static_cast<int>(by);
        const Float4 a = load(x, y);
        const Float4 b = load(x + 1, y);
        const Float4 c = load(x, y + 1);
        const Float4 d = load(x + 1, y + 1);
        Float4 out{};
        for (std::size_t i = 0; i < 4; ++i) {
            out.at(i) = lerp(lerp(a.at(i), b.at(i), fx), lerp(c.at(i), d.at(i), fx), fy);
        }
        return out;
    }
};

// Godot's do_filter: the samples one way from the pixel, until one has no strength.
void filter(const Source& source, std::span<const Tap> kernel, std::array<float, 3>& color,
            std::array<float, 3>& divisor, float u, float v, float step_x, float step_y,
            bool skin) {
    for (std::size_t i = 1; i < kernel.size(); ++i) {
        const float offset = kernel[i].offset;
        const Float4 sampled = source.linear(u + (offset * step_x), v + (offset * step_y));
        if (std::abs(sampled[3]) < 0.001F) {
            break;
        }
        const std::array<float, 3> w = weight_of(kernel[i], skin);
        for (std::size_t c = 0; c < 3; ++c) {
            color.at(c) += sampled.at(c) * w.at(c);
            divisor.at(c) += w.at(c);
        }
    }
}

} // namespace

bool uses_subsurface(const Scene3D& scene) {
    return scene.method == RenderMethod::ForwardPlus &&
           scene.subsurface.quality != SubsurfaceQuality::Disabled && !scene.capture &&
           !scene.path_trace;
}

bool draws_subsurface(const Material3D& material) {
    return material.subsurface > 0.0F && !material.unshaded &&
           material.transparency != Transparency::Alpha && material.shader == ShaderId::None;
}

std::uint32_t subsurface_samples(SubsurfaceQuality quality) {
    switch (quality) {
    case SubsurfaceQuality::Medium:
        return 9;
    case SubsurfaceQuality::High:
        return 13;
    default:
        return 6;
    }
}

SubsurfaceUniforms subsurface_uniforms(const Scene3D& scene, std::uint32_t width,
                                       std::uint32_t height, bool down) {
    const std::array<float, 16>& m = scene.projection;
    const bool orthogonal = m[11] == 0.0F;
    // Godot's unit_size: the projection of Plane(1, 0, -1, 1), its x over its w.
    const float x = m[0] - m[8] + m[12];
    const float w = m[3] - m[11] + m[15];
    const float unit_size = w != 0.0F ? x / w : 0.0F;
    SubsurfaceUniforms out;
    out.size = {width, height, down ? 1U : 0U, orthogonal ? 1U : 0U};
    out.params = {unit_size, scene.subsurface.scale, scene.subsurface.depth_scale,
                  static_cast<float>(subsurface_samples(scene.subsurface.quality))};
    out.projection = {m[10], m[11], m[14], m[15]};
    return out;
}

std::vector<float> run_subsurface_pass(const SubsurfaceUniforms& uniforms,
                                       std::span<const float> source, std::span<const float> depth,
                                       std::span<const float> diffuse) {
    const auto width = static_cast<int>(uniforms.size[0]);
    const auto height = static_cast<int>(uniforms.size[1]);
    const bool down = uniforms.size[2] != 0;
    const bool orthogonal = uniforms.size[3] != 0;
    const auto samples = static_cast<std::uint32_t>(uniforms.params[3]);
    const std::span<const Tap> kernel = kernel_of(samples);
    const Source from{.pixels = source, .width = width, .height = height};
    std::vector<float> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                static_cast<std::size_t>(x);
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
            Float4 base = from.load(x, y);
            const float strength = std::abs(base[3]);
            if (strength > 0.0F) {
                const float r = depth[pixel];
                const float view_z = ((r * uniforms.projection[3]) - uniforms.projection[2]) /
                                     (uniforms.projection[0] - (r * uniforms.projection[1]));
                const float depth_scale =
                    orthogonal ? uniforms.params[0] : uniforms.params[0] / -view_z;
                const float scale = lerp(uniforms.params[1], depth_scale, uniforms.params[2]);
                const float along = scale * strength / 3.0F;
                const float step_x = down ? 0.0F : along;
                const float step_y = down ? along : 0.0F;
                const bool skin = base[3] < 0.0F;
                std::array<float, 3> divisor = weight_of(kernel[0], skin);
                std::array<float, 3> color{base[0] * divisor[0], base[1] * divisor[1],
                                           base[2] * divisor[2]};
                filter(from, kernel, color, divisor, u, v, step_x, step_y, skin);
                filter(from, kernel, color, divisor, u, v, -step_x, -step_y, skin);
                for (std::size_t c = 0; c < 3; ++c) {
                    base.at(c) = color.at(c) / divisor.at(c);
                }
            }
            float* to = &out[pixel * 4];
            if (!down) {
                std::copy(base.begin(), base.end(), to);
                continue;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                to[c] = strength > 0.0F ? base.at(c) - diffuse[(pixel * 4) + c] : 0.0F;
            }
            to[3] = 0.0F;
        }
    }
    return out;
}

std::vector<float> run_subsurface(const Scene3D& scene, std::uint32_t width, std::uint32_t height,
                                  std::span<const float> diffuse, std::span<const float> depth) {
    std::vector<float> across = run_subsurface_pass(
        subsurface_uniforms(scene, width, height, false), diffuse, depth, diffuse);
    for (float& value : across) {
        value = round_half(value);
    }
    return run_subsurface_pass(subsurface_uniforms(scene, width, height, true), across, depth,
                               diffuse);
}

} // namespace slash::render::detail
