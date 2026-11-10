#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A look measured from a picture, so a game's style can be kept as numbers and drift from it
// found without looking: the storyboard's style references keep one from what they were made of,
// and style.check measures new frames the same way. Integer math, then doubles from whole sums
// and correctly rounded square roots, so a picture measures the same everywhere.
namespace slash::storyboard {

struct Swatch {
    std::array<std::uint8_t, 3> color{};
    double share = 0.0; // of the measured pixels
};

struct Fingerprint {
    // At most kMaxSwatches colors the pixels gather into, the most used first.
    std::vector<Swatch> palette;
    // Luma (Rec. 709 weights over the sRGB values, 0 to 1) at the 5th, 50th and 95th
    // percentiles: where the shadows, the middle and the highlights sit.
    std::array<double, 3> value{};
    // RMS contrast: the standard deviation of luma.
    double contrast = 0.0;
    // The mean of each pixel's HSV saturation.
    double saturation = 0.0;
    // The mean of red less blue: above 0 warm, below 0 cool.
    double warmth = 0.0;
    // The share of pixels on an edge (Sobel over luma), and how many pixels wide edges are.
    double edges = 0.0;
    double line_weight = 0.0;
    // The share of pixels the same as their right and lower neighbors, give or take one step of
    // luma: high for flat or cel shading and pixel art, low for smooth shading and texture.
    double flat = 0.0;
    // A model's albedo texels per meter (scene3d::check_geometry), when the picture is of one.
    std::optional<double> texel_density;
    // Pixels measured: those at least half opaque.
    std::size_t pixels = 0;
};

inline constexpr std::size_t kMaxSwatches = 8;

// Measures RGBA8 pixels, width * height * 4 bytes, rows top first.
[[nodiscard]] Fingerprint fingerprint(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                      std::uint32_t height);

// The measures, in the order drift reports them: "palette", "value", "contrast", "saturation",
// "warmth", "edges", "line_weight", "flat" and "texel_density".
[[nodiscard]] const std::vector<std::string_view>& fingerprint_measures();
// The drift a measure may show before a check fails, unless a reference's tolerances say.
[[nodiscard]] double default_tolerance(std::string_view measure);

// A fingerprint as a style entry keeps it: {"palette": {"#rrggbb": share}, "value": [low, mid,
// high], "contrast", ...}, each number rounded to 4 places, so what is kept is what is compared.
[[nodiscard]] json::Value to_json(const Fingerprint& fingerprint);
// Reads to_json's form back; an error names the bad member.
[[nodiscard]] Result<Fingerprint> fingerprint_from_json(const json::Value& value);
// JSON Schema for to_json's form.
[[nodiscard]] json::Value fingerprint_schema();
// JSON Schema for a style entry's tolerances: a number per measure.
[[nodiscard]] json::Value tolerances_schema();

// How far now is from a reference in each measure both have, by measure name:
// - palette: half the sum, over both palettes, of each swatch's share times its distance to the
//   nearest swatch of the other, in RGB over 255;
// - value: the largest change of the three percentiles;
// - texel_density: the change over the reference's;
// - the rest: the change.
// Rounded to 4 places.
[[nodiscard]] json::Object drift(const Fingerprint& reference, const Fingerprint& now);
// A measure's value in words, for messages: "0.41", "[0.1, 0.4, 0.8]" or the palette's colors.
[[nodiscard]] std::string describe_measure(const Fingerprint& fingerprint,
                                           std::string_view measure);

// A fingerprint in a few words for a prompt: "mostly #50714b and #7399d9, cool, mid-toned, low
// contrast, muted, flat shading".
[[nodiscard]] std::string describe_look(const Fingerprint& fingerprint);

} // namespace slash::storyboard
