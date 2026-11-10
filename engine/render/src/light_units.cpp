// Color temperature and physical light units, as Godot's Light3D and CameraAttributes.

#include "slash/core/math.hpp"
#include "slash/render/light.hpp"

#include <algorithm>
#include <array>
#include <numbers>

namespace slash::render {
namespace {

// Godot's Color::linear_to_srgb for one channel, through slash::exp and ln.
float to_srgb(float c) {
    if (c < 0.0031308F) {
        return 12.92F * c;
    }
    if (c >= 1.0F) {
        return 1.0F; // exactly, where exp and ln round
    }
    return (1.055F * slash::exp(slash::ln(c) / 2.4F)) - 0.055F;
}

// Godot's Color::srgb_to_linear for one channel.
float to_linear(float c) {
    if (c < 0.04045F) {
        return c * (1.0F / 12.92F);
    }
    return slash::exp(2.4F * slash::ln((c + 0.055F) * (1.0F / 1.055F)));
}

} // namespace

std::array<float, 3> color_temperature(float kelvin) {
    // Krystek's approximation of the Planckian locus in CIE 1960 UCS (u, v), as Godot's
    // Light3D::get_correlated_color.
    const float t = kelvin;
    const float t2 = t * t;
    const float u = (0.860117757F + (1.54118254e-4F * t) + (1.28641212e-7F * t2)) /
                    (1.0F + (8.42420235e-4F * t) + (7.08145163e-7F * t2));
    // NOLINTNEXTLINE(modernize-use-std-numbers): Krystek's fit, not 1 / pi
    const float v = (0.317398726F + (4.22806245e-5F * t) + (4.20481691e-8F * t2)) /
                    (1.0F - (2.89741816e-5F * t) + (1.61456053e-7F * t2));
    // To xyY, then XYZ with Y at 1.
    const float d = (2.0F * u) - (8.0F * v) + 4.0F;
    const float x = 3.0F * u / d;
    const float y = 2.0F * v / d;
    const float a = 1.0F / std::max(y, 1e-5F);
    const float big_x = x * a;
    const float big_z = (1.0F - x - y) * a;
    // XYZ to linear sRGB.
    std::array<float, 3> linear{
        (3.2404542F * big_x) - 1.5371385F - (0.4985314F * big_z),
        (-0.9692660F * big_x) + 1.8760108F + (0.0415560F * big_z),
        (0.0556434F * big_x) - 0.2040259F + (1.0572252F * big_z),
    };
    const float most = std::max(1e-5F, std::max({linear[0], linear[1], linear[2]}));
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = to_srgb(std::clamp(linear.at(i) / most, 0.0F, 1.0F));
    }
    return out;
}

std::array<float, 3> tint_by_temperature(const std::array<float, 3>& color, float kelvin) {
    const std::array<float, 3> tint = color_temperature(kelvin);
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = to_srgb(std::max(to_linear(color.at(i)) * to_linear(tint.at(i)), 0.0F));
    }
    return out;
}

float directional_light_units(float lux) {
    return lux / std::numbers::pi_v<float>;
}

float omni_light_units(float lumens) {
    constexpr float kPi = std::numbers::pi_v<float>;
    return lumens / (4.0F * kPi * kPi);
}

float spot_light_units(float lumens) {
    // Godot keeps a spot light's intensity whatever its cone, to keep it easy to control.
    constexpr float kPi = std::numbers::pi_v<float>;
    return lumens / (kPi * kPi);
}

float exposure_normalization(float sensitivity) {
    return sensitivity / 3072007.0F;
}

} // namespace slash::render
