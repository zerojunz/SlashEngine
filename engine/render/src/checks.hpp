#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Checks both devices make, so the null device fails exactly where a GPU device would.
namespace slash::render::detail {

inline constexpr std::uint32_t kMaxTextureSize = 16384;
// The most vertices one frame can draw: 1.4 million triangles.
inline constexpr std::size_t kMaxFrameVertices = std::size_t{1} << 22;

[[nodiscard]] Result<void> check_texture_desc(const TextureDesc& desc);
// rgba must be empty (when allow_empty) or exactly width * height * 4 bytes.
[[nodiscard]] Result<void> check_pixels(const TextureDesc& desc, std::span<const std::uint8_t> rgba,
                                        bool allow_empty);
// in_frame is how many vertices the frame has drawn so far.
[[nodiscard]] Result<void> check_vertices(std::span<const Vertex2D> vertices, std::size_t in_frame);
[[nodiscard]] Error unknown_texture(TextureId texture);
[[nodiscard]] Error not_a_render_target(TextureId texture);
[[nodiscard]] Error target_in_use(TextureId texture);
[[nodiscard]] Error no_frame(std::string_view call);
[[nodiscard]] Error frame_in_progress(std::string_view call);
[[nodiscard]] std::array<std::uint8_t, 4> to_rgba8(const Color& color);

// The material's program, and that its textures exist (known says whether one does) and aren't
// the frame's target.
template <typename Known>
[[nodiscard]] Result<void> check_material(const Material& material, TextureId target,
                                          const Known& known) {
    const DistanceField& field = material.field;
    if (!(field.pixel_range >= 0.0F) || !(field.outline >= 0.0F) ||
        !std::isfinite(field.pixel_range) || !std::isfinite(field.outline)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a distance field's pixel range and outline are 0 or more");
    }
    if (field.pixel_range > 0.0F && !material.program.code.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a material with a program can't draw a distance field too");
    }
    if (!material.program.code.empty()) {
        if (auto checked = check_program(material.program); !checked) {
            return checked;
        }
    }
    const auto check = [&](TextureId texture) -> Result<void> {
        if (texture == TextureId::None) {
            return {};
        }
        if (!known(texture)) {
            return unknown_texture(texture);
        }
        if (texture == target) {
            return target_in_use(texture);
        }
        return {};
    };
    for (const MaterialTexture& slot : material.textures) {
        if (auto checked = check(slot.texture); !checked) {
            return checked;
        }
    }
    if (auto checked = check(material.shading.normal_map); !checked) {
        return checked;
    }
    return check(material.shading.specular_map);
}

// That the frame's lighting textures still exist when a draw needs them.
template <typename Known>
[[nodiscard]] Result<void> check_lighting_textures(const Lighting& lighting, const Known& known) {
    for (const TextureId texture : {lighting.atlas, lighting.shadow_map}) {
        if (texture != TextureId::None && !known(texture)) {
            return unknown_texture(texture);
        }
    }
    return {};
}

// The frame's lights, and that their textures exist and aren't the frame's target. size gives a
// known texture's width and height.
template <typename Known, typename Size>
[[nodiscard]] Result<void> check_lighting(const Lighting& lighting, TextureId target,
                                          const Known& known, const Size& size) {
    if (lighting.lights.size() > kMaxLights) {
        return make_error(ErrorCode::InvalidArgument,
                          "a frame takes up to " + std::to_string(kMaxLights) + " lights, not " +
                              std::to_string(lighting.lights.size()));
    }
    for (std::size_t i = 0; i < lighting.lights.size(); ++i) {
        const Light& light = lighting.lights[i];
        if (light.blend != BlendMode::Add && light.blend != BlendMode::Sub &&
            light.blend != BlendMode::Mix) {
            return make_error(ErrorCode::InvalidArgument,
                              "light " + std::to_string(i) + " blends with " +
                                  std::string(blend_mode_name(light.blend)) +
                                  "; lights add, sub or mix");
        }
        if (light.shadow_row >= 0 && (!(light.shadow_far > 0.0F) || !(light.shadow_span > 0.0F))) {
            return make_error(ErrorCode::InvalidArgument,
                              "light " + std::to_string(i) +
                                  " has shadows, so its shadow_far and shadow_span are above 0");
        }
    }
    for (const TextureId texture : {lighting.atlas, lighting.shadow_map}) {
        if (texture == TextureId::None) {
            continue;
        }
        if (!known(texture)) {
            return unknown_texture(texture);
        }
        if (texture == target) {
            return target_in_use(texture);
        }
    }
    for (std::size_t i = 0; i < lighting.lights.size(); ++i) {
        const std::int32_t row = lighting.lights[i].shadow_row;
        if (row < 0) {
            continue;
        }
        if (lighting.shadow_map == TextureId::None) {
            return make_error(ErrorCode::InvalidArgument, "light " + std::to_string(i) +
                                                              " has shadows but the frame has no "
                                                              "shadow map");
        }
        const auto [width, height] = size(lighting.shadow_map);
        if (width != kShadowMapWidth || static_cast<std::uint32_t>(row) >= height) {
            return make_error(ErrorCode::InvalidArgument,
                              "light " + std::to_string(i) + "'s shadow row " +
                                  std::to_string(row) + " is outside the " + std::to_string(width) +
                                  "x" + std::to_string(height) + " shadow map, which is " +
                                  std::to_string(kShadowMapWidth) + " texels wide");
        }
    }
    return {};
}

} // namespace slash::render::detail
