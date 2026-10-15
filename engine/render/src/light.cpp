#include "slash/render/light.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

// Keep in step with the lighting in shaders/material.hlsl.
namespace slash::render {
namespace {

using Float3 = std::array<float, 3>;
using Float4 = std::array<float, 4>;

constexpr std::array<std::string_view, 3> kLightModes = {"unshaded", "normal", "light_only"};
constexpr std::array<std::string_view, 3> kShadowFilters = {"none", "pcf5", "pcf13"};
constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;
constexpr auto kWidth = static_cast<std::int32_t>(kShadowMapWidth);

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 normalize3(const Float3& a) {
    const float size = std::sqrt(dot3(a, a));
    if (size <= 0.0F) {
        return {0.0F, 0.0F, 0.0F};
    }
    return {a[0] / size, a[1] / size, a[2] / size};
}

// Godot's 2D normal lighting: Lambert, plus Blinn-Phong with a specular map.
Float3 light_normal(const Float3& light_vec, const Float3& normal, const Float4& base,
                    const Float4& light_color, const std::optional<Float4>& specular) {
    const float n_dot_l = std::max(0.0F, dot3(normal, light_vec));
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = light_color.at(i) * base.at(i) * n_dot_l;
    }
    if (!specular) {
        return out;
    }
    const Float3 view{0.0F, 0.0F, 1.0F};
    const Float3 half =
        normalize3({view[0] + light_vec[0], view[1] + light_vec[1], view[2] + light_vec[2]});
    const float n_dot_v = std::max(dot3(normal, view), 0.0F);
    const float n_dot_h = std::max(dot3(normal, half), 0.0F);
    const float shininess = std::exp2((15.0F * (*specular)[3]) + 1.0F) * 0.25F;
    float blinn = std::pow(n_dot_h, shininess);
    blinn *= (shininess + 8.0F) * (1.0F / (8.0F * std::numbers::pi_v<float>));
    const float s = blinn / std::max(4.0F * n_dot_v * n_dot_l, 0.75F);
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) += specular->at(i) * light_color.at(i) * s;
    }
    return out;
}

// From 0 to 1: how much of the filter's samples an occluder hides the position from.
float shadow_amount(const Light& light, std::array<float, 2> position, const ShadowTexel& texel) {
    const ShadowCoord coord = shadow_coord(light, position);
    const bool wrap = light.kind == LightKind::Point;
    int taps = 0;
    if (light.shadow_filter == ShadowFilter::Pcf5) {
        taps = 2;
    } else if (light.shadow_filter == ShadowFilter::Pcf13) {
        taps = 6;
    }
    const float step = 1.0F + light.shadow_smooth;
    const float bias = 0.5F / light.shadow_far;
    float hidden = 0.0F;
    for (int k = -taps; k <= taps; ++k) {
        auto column =
            static_cast<std::int32_t>(std::floor(coord.column + (static_cast<float>(k) * step)));
        if (wrap) {
            column = ((column % kWidth) + kWidth) % kWidth;
        } else if (column < 0 || column >= kWidth) {
            continue;
        }
        const float stored = decode_shadow_depth(texel(
            static_cast<std::uint32_t>(column), static_cast<std::uint32_t>(light.shadow_row)));
        if (stored <= 1.0F && coord.depth > stored + bias &&
            (wrap || coord.depth - stored < light.shadow_reach)) {
            hidden += 1.0F;
        }
    }
    return hidden / static_cast<float>((2 * taps) + 1);
}

} // namespace

std::string_view light_mode_name(LightMode mode) {
    return kLightModes.at(static_cast<std::size_t>(mode));
}

std::optional<LightMode> parse_light_mode(std::string_view name) {
    for (std::size_t i = 0; i < kLightModes.size(); ++i) {
        if (kLightModes.at(i) == name) {
            return static_cast<LightMode>(i);
        }
    }
    return std::nullopt;
}

std::string_view shadow_filter_name(ShadowFilter filter) {
    return kShadowFilters.at(static_cast<std::size_t>(filter));
}

std::optional<ShadowFilter> parse_shadow_filter(std::string_view name) {
    for (std::size_t i = 0; i < kShadowFilters.size(); ++i) {
        if (kShadowFilters.at(i) == name) {
            return static_cast<ShadowFilter>(i);
        }
    }
    return std::nullopt;
}

ShadowCoord shadow_coord(const Light& light, std::array<float, 2> position) {
    constexpr auto kColumns = static_cast<float>(kShadowMapWidth);
    if (light.kind == LightKind::Point) {
        const float dx = position[0] - light.position[0];
        const float dy = position[1] - light.position[1];
        return {.column = ((std::atan2(dy, dx) / kTwoPi) + 0.5F) * kColumns,
                .depth = std::sqrt((dx * dx) + (dy * dy)) / light.shadow_far};
    }
    const float across = (position[0] * -light.position[1]) + (position[1] * light.position[0]);
    const float along = (position[0] * light.position[0]) + (position[1] * light.position[1]);
    return {.column = (across - light.shadow_origin[0]) / light.shadow_span * kColumns,
            .depth = (along - light.shadow_origin[1]) / light.shadow_far};
}

std::array<std::uint8_t, 4> encode_shadow_depth(float depth) {
    constexpr double kLevels = 16777216.0;
    const double scaled = std::floor(static_cast<double>(std::clamp(depth, 0.0F, 1.0F)) * kLevels);
    const auto bits = static_cast<std::uint32_t>(std::min(scaled, kLevels - 1.0));
    return {static_cast<std::uint8_t>(bits >> 16U), static_cast<std::uint8_t>((bits >> 8U) & 255U),
            static_cast<std::uint8_t>(bits & 255U), 255};
}

float decode_shadow_depth(const std::array<std::uint8_t, 4>& texel) {
    if (texel[3] < 128) {
        return 2.0F;
    }
    return ((static_cast<float>(texel[0]) * 65536.0F) + (static_cast<float>(texel[1]) * 256.0F) +
            static_cast<float>(texel[2])) /
           16777216.0F;
}

std::array<float, 3> map_normal(const std::array<float, 4>& texel, std::array<float, 2> u_axis,
                                std::array<float, 2> v_axis) {
    const float nx = (texel[0] * 2.0F) - 1.0F;
    const float ny = 1.0F - (texel[1] * 2.0F);
    const float nz = std::sqrt(std::max(0.0F, 1.0F - ((nx * nx) + (ny * ny))));
    return normalize3(
        {(u_axis[0] * nx) + (v_axis[0] * ny), (u_axis[1] * nx) + (v_axis[1] * ny), nz});
}

std::array<float, 4> shade(std::span<const Light> lights, const std::array<float, 4>& ambient,
                           const ShadeItem& item, const ShadePixel& pixel,
                           const AtlasSampler& atlas, const ShadowTexel& shadows,
                           std::vector<LightHit>* hits) {
    if (item.mode == LightMode::Unshaded) {
        return pixel.color;
    }
    const Float4 base = pixel.color;
    Float4 color = base;
    if (item.mode == LightMode::Normal) {
        for (std::size_t i = 0; i < 4; ++i) {
            color.at(i) *= ambient.at(i);
        }
    }
    const float x = pixel.position[0];
    const float y = pixel.position[1];
    float light_only_alpha = 0.0F;
    const std::size_t count = std::min(lights.size(), kMaxLights);
    for (std::size_t index = 0; index < count; ++index) {
        const Light& light = lights[index];
        if ((light.item_cull_mask & item.light_mask) == 0 || item.z_index < light.z_min ||
            item.z_index > light.z_max) {
            continue;
        }
        Float4 light_color = light.color;
        Float3 light_vec{};
        if (light.kind == LightKind::Point) {
            const auto& m = light.texture_transform;
            const float u = (m[0] * x) + (m[1] * y) + m[2];
            const float v = (m[3] * x) + (m[4] * y) + m[5];
            if (u < 0.0F || v < 0.0F || u >= 1.0F || v >= 1.0F) {
                continue;
            }
            const auto& rect = light.atlas_rect;
            const Float4 texel = atlas(rect[0] + (u * rect[2]), rect[1] + (v * rect[3]));
            for (std::size_t i = 0; i < 4; ++i) {
                light_color.at(i) *= texel.at(i);
            }
            light_vec = normalize3({light.position[0] - x, light.position[1] - y, light.height});
        } else {
            const float h = light.height;
            light_vec =
                normalize3({-light.position[0] * (1.0F - h), -light.position[1] * (1.0F - h), h});
        }
        if (pixel.normal) {
            const Float3 lit =
                light_normal(light_vec, *pixel.normal, base, light_color, pixel.specular);
            std::copy(lit.begin(), lit.end(), light_color.begin());
        } else {
            for (std::size_t i = 0; i < 3; ++i) {
                light_color.at(i) *= base.at(i);
            }
        }
        float shadow = 0.0F;
        if (light.shadow_row >= 0) {
            shadow = shadow_amount(light, pixel.position, shadows);
            Float4 shadow_color = light.shadow_color;
            shadow_color[3] *= light_color[3];
            for (std::size_t i = 0; i < 4; ++i) {
                light_color.at(i) += (shadow_color.at(i) - light_color.at(i)) * shadow;
            }
        }
        if (hits != nullptr) {
            hits->push_back({.light = index, .color = light_color, .shadow = shadow});
        }
        light_only_alpha += light_color[3];
        for (std::size_t i = 0; i < 3; ++i) {
            const float amount = light_color.at(i) * light_color[3];
            if (light.blend == BlendMode::Sub) {
                color.at(i) -= amount;
            } else if (light.blend == BlendMode::Mix) {
                color.at(i) += (light_color.at(i) - color.at(i)) * light_color[3];
            } else {
                color.at(i) += amount;
            }
        }
    }
    if (item.mode == LightMode::LightOnly) {
        color[3] *= light_only_alpha;
    }
    return color;
}

} // namespace slash::render
