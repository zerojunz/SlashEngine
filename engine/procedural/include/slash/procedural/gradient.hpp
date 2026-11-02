#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Gradients, as Godot's Gradient resource (scene/resources/gradient.h): colors at offsets from 0
// to 1, blended linearly, held constant or by cubic curves, in sRGB, linear sRGB or Oklab. Colors
// are sRGB with straight alpha, as Godot's.
//
//   {"points": [{"offset": 0, "color": [0, 0, 0.2, 1]}, {"offset": 1, "color": [1, 0.9, 0.6, 1]}],
//    "interpolation_mode": "cubic", "interpolation_color_space": "oklab"}
namespace slash::procedural {

// Godot's Gradient.InterpolationMode.
enum class GradientInterpolation : std::uint8_t { Linear, Constant, Cubic };
// Godot's Gradient.ColorSpace.
enum class GradientColorSpace : std::uint8_t { Srgb, LinearSrgb, Oklab };

struct GradientStop {
    float offset = 0.0F;
    Vec4 color{0.0F, 0.0F, 0.0F, 1.0F};
};

struct Gradient {
    // Godot's new Gradient: black at 0, white at 1. Kept sorted by offset.
    std::vector<GradientStop> points{{0.0F, {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {1.0F, {1.0F, 1.0F, 1.0F, 1.0F}}};
    GradientInterpolation interpolation_mode = GradientInterpolation::Linear;
    GradientColorSpace interpolation_color_space = GradientColorSpace::Srgb;

    // The color at an offset, as Godot's get_color_at_offset: the first or last point's color
    // outside them, black with no points. The same bits on every machine.
    [[nodiscard]] Vec4 color_at(float offset) const;
};

// JSON Schema for a gradient object.
[[nodiscard]] json::Value gradient_schema();

// Every error in a gradient object, each with the JSON Pointer of the bad value under at.
[[nodiscard]] std::vector<json::ValidationError> validate_gradient(const json::Value& doc,
                                                                   const std::string& at = "");

// The gradient a gradient object holds, its points sorted by offset (points at the same offset
// keep their order), or its first error.
[[nodiscard]] Result<Gradient> parse_gradient(const json::Value& doc);

// The gradient as a gradient object.
[[nodiscard]] json::Value gradient_to_json(const Gradient& gradient);

// Color conversions as Godot's Color makes them, the same bits on every machine: sRGB to linear
// and back (Color::srgb_to_linear, linear_to_srgb), and linear sRGB to Oklab and back
// (ok_color's linear_srgb_to_oklab and oklab_to_linear_srgb). Alpha passes through.
[[nodiscard]] Vec4 srgb_to_linear(Vec4 color);
[[nodiscard]] Vec4 linear_to_srgb(Vec4 color);
[[nodiscard]] Vec4 linear_to_oklab(Vec4 color);
[[nodiscard]] Vec4 oklab_to_linear(Vec4 color);

} // namespace slash::procedural
