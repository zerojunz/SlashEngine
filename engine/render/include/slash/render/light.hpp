#pragma once

#include "slash/render/material.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// 2D lighting, as Godot's: point and directional lights shade lit draws per pixel in the
// material shader (shaders/material.hlsl), after the draw's program has made its color, and
// occluders cast shadows through shadow maps the CPU fills, one row of kShadowMapWidth texels per
// shadowed light. shade runs the same lighting on the CPU, for checks and light.sample.
//
// A lit pixel starts from the color its program made (Godot's base color). Normal draws multiply
// it by the ambient color (their canvas's CanvasModulate); then each light whose cull mask, z
// range and layer range take the draw adds, subtracts or mixes in its color times the base color,
// darkened where the pixel's normal faces away from it and blended toward its shadow color where
// an occluder hides it.
namespace slash::render {

// How a draw takes the frame's lights, as Godot's light_mode.
enum class LightMode : std::uint8_t {
    Unshaded,  // lights and the ambient color leave it alone; draws are unshaded by default
    Normal,    // the ambient color tints it and lights shade it
    LightOnly, // like Normal without the ambient color, and visible only where lights reach
};

// "unshaded", "normal" or "light_only".
[[nodiscard]] std::string_view light_mode_name(LightMode mode);
// The mode with this name, or nullopt.
[[nodiscard]] std::optional<LightMode> parse_light_mode(std::string_view name);

enum class LightKind : std::uint8_t {
    Point,       // shines from a position through its texture, as PointLight2D
    Directional, // shines everywhere from one direction, as DirectionalLight2D
};

// How soft shadow edges are, as Godot's shadow filters: one shadow map texel, or the average of
// 5 or 13 neighbors.
enum class ShadowFilter : std::uint8_t { None, Pcf5, Pcf13 };

// "none", "pcf5" or "pcf13".
[[nodiscard]] std::string_view shadow_filter_name(ShadowFilter filter);
// The filter with this name, or nullopt.
[[nodiscard]] std::optional<ShadowFilter> parse_shadow_filter(std::string_view name);

// The most lights one frame takes.
inline constexpr std::size_t kMaxLights = 64;
// Texels across each shadowed light's row of the shadow map.
inline constexpr std::uint32_t kShadowMapWidth = 2048;

// One light, in the coordinates vertices use.
struct Light {
    LightKind kind = LightKind::Point;
    // Add, Sub or Mix, as Godot's light blend modes.
    BlendMode blend = BlendMode::Add;
    // RGB already multiplied by the light's energy, and alpha.
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    // Point: where the light is. Directional: the unit direction its light travels.
    std::array<float, 2> position{};
    // For normal maps. Point: pixels above the plane. Directional: from 0, along the plane, to 1,
    // straight down onto it.
    float height = 0.0F;
    // Point: the light texture's coordinate at (x, y) is u = [0] x + [1] y + [2] and
    // v = [3] x + [4] y + [5]; outside 0..1 the light doesn't reach.
    std::array<float, 6> texture_transform{};
    // Point: where the light texture is in the frame's light atlas, as [x, y, width, height]
    // from 0 to 1.
    std::array<float, 4> atlas_rect{0.0F, 0.0F, 1.0F, 1.0F};
    // Lights draws whose light mask shares a bit with it, z index from z_min to z_max and canvas
    // layer from layer_min to layer_max.
    std::uint32_t item_cull_mask = 1;
    std::int32_t z_min = -1024;
    std::int32_t z_max = 1024;
    std::int32_t layer_min = 0;
    std::int32_t layer_max = 0;
    // The light's row in the shadow map, or -1 for no shadows.
    std::int32_t shadow_row = -1;
    ShadowFilter shadow_filter = ShadowFilter::None;
    // Spreads the filter's samples by 1 + shadow_smooth texels.
    float shadow_smooth = 0.0F;
    // What shadowed pixels get instead of the light's color; alpha is times the light's.
    std::array<float, 4> shadow_color{0.0F, 0.0F, 0.0F, 0.0F};
    // The distance the row's depths are fractions of.
    float shadow_far = 1.0F;
    // Directional: where the row starts across the direction ([0], along (-dir.y, dir.x)) and
    // in depth along it ([1]), how far across it reaches, and how far a shadow reaches past its
    // occluder, as a fraction of shadow_far.
    std::array<float, 2> shadow_origin{};
    float shadow_span = 1.0F;
    float shadow_reach = 1.0F;

    bool operator==(const Light&) const = default;
};

// Where a point falls in its light's shadow row: the column, as a real number of texels (point
// lights go round from -pi, directional ones across), and its depth as a fraction of shadow_far.
struct ShadowCoord {
    float column = 0.0F;
    float depth = 0.0F;
};
[[nodiscard]] ShadowCoord shadow_coord(const Light& light, std::array<float, 2> position);

// A shadow map texel holding an occluder at this depth, from 0 to 1, as 24 bits.
[[nodiscard]] std::array<std::uint8_t, 4> encode_shadow_depth(float depth);
// A shadow map texel with no occluder.
inline constexpr std::array<std::uint8_t, 4> kNoShadow{0, 0, 0, 0};
// The depth a texel holds, or 2 for none.
[[nodiscard]] float decode_shadow_depth(const std::array<std::uint8_t, 4>& texel);

// A pixel of a normal map, OpenGL style (green up), turned into the frame's coordinates (y
// down, z toward the viewer) for a draw whose texture's u and v axes run along u_axis and v_axis.
[[nodiscard]] std::array<float, 3> map_normal(const std::array<float, 4>& texel,
                                              std::array<float, 2> u_axis,
                                              std::array<float, 2> v_axis);

// What a lit pixel needs from its draw.
struct ShadeItem {
    LightMode mode = LightMode::Normal;
    std::uint32_t light_mask = 1;
    std::int32_t z_index = 0;
    std::int32_t layer = 0; // the canvas layer it is drawn in
};

struct ShadePixel {
    std::array<float, 2> position{};
    // The color the draw's program made.
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    // From a normal map, or none: facing the viewer, and no light vectors.
    std::optional<std::array<float, 3>> normal;
    // From a specular map, times the draw's specular color (rgb) and shininess (a).
    std::optional<std::array<float, 4>> specular;
};

// The frame's light atlas at a coordinate, filtered as the GPU samples it.
using AtlasSampler = std::function<std::array<float, 4>(float u, float v)>;
// A texel of the frame's shadow map.
using ShadowTexel =
    std::function<std::array<std::uint8_t, 4>(std::uint32_t column, std::uint32_t row)>;

// How one light reached a pixel.
struct LightHit {
    std::size_t light = 0; // index into the lights
    // Its color after the base color, normal and shadow, before blending.
    std::array<float, 4> color{};
    // From 0, in the light, to 1, in shadow.
    float shadow = 0.0F;
};

// The color a lit pixel ends with, before the draw's blend mode, as the material shader computes
// it. Lists the lights that reached it in hits when given. Floating-point results may differ
// from a GPU's in the last bits.
[[nodiscard]] std::array<float, 4> shade(std::span<const Light> lights,
                                         const std::array<float, 4>& ambient, const ShadeItem& item,
                                         const ShadePixel& pixel, const AtlasSampler& atlas,
                                         const ShadowTexel& shadows,
                                         std::vector<LightHit>* hits = nullptr);

} // namespace slash::render
