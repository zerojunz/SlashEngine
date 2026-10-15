#include "slash/core/math.hpp"

#include <cmath>
#include <numbers>

namespace slash {

Vec2 cos_sin(float radians) {
    if (!std::isfinite(radians)) {
        return {1.0F, 0.0F};
    }
    // A quarter turn count and what is left, from -pi/4 to pi/4, in double so the result is
    // good to a float's last bit; then Taylor series, which IEEE arithmetic gives the same bits
    // everywhere.
    constexpr double kQuarter = std::numbers::pi / 2.0;
    const double quarters = std::nearbyint(static_cast<double>(radians) / kQuarter);
    const double r = static_cast<double>(radians) - (quarters * kQuarter);
    const double r2 = r * r;
    const double s =
        r * (1.0 +
             (r2 * (-1.0 / 6.0 + (r2 * (1.0 / 120.0 + (r2 * (-1.0 / 5040.0 + (r2 / 362880.0))))))));
    const double c =
        1.0 +
        (r2 * (-0.5 + (r2 * (1.0 / 24.0 +
                             (r2 * (-1.0 / 720.0 + (r2 * (1.0 / 40320.0 - (r2 / 3628800.0)))))))));
    const auto quadrant = static_cast<int>(std::fmod(quarters, 4.0) + 4.0) % 4;
    const auto fc = static_cast<float>(c);
    const auto fs = static_cast<float>(s);
    switch (quadrant) {
    case 1:
        return {-fs, fc};
    case 2:
        return {-fc, -fs};
    case 3:
        return {fs, -fc};
    default:
        return {fc, fs};
    }
}

} // namespace slash
