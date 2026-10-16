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

float atan2(float y, float x) {
    if (std::isnan(x) || std::isnan(y)) {
        return 0.0F;
    }
    const double dx = x;
    const double dy = y;
    const double ax = std::fabs(dx);
    const double ay = std::fabs(dy);
    if (ax == 0.0 && ay == 0.0) {
        return 0.0F;
    }
    if (std::isinf(ax) || std::isinf(ay)) {
        // The direction of the infinite parts alone, keeping the signs of the others.
        const double fx = std::copysign(std::isinf(ax) ? 1.0 : 0.0, dx);
        const double fy = std::copysign(std::isinf(ay) ? 1.0 : 0.0, dy);
        return atan2(static_cast<float>(fy), static_cast<float>(fx));
    }
    // atan of t = min / max, from 0 to 1, halved twice with atan(t) = 2 atan(t / (1 +
    // sqrt(1 + t^2))) to below tan(pi / 16), then a Taylor series: only IEEE operations, which
    // give the same bits everywhere, and good to well past a float's last bit.
    const bool steep = ay > ax;
    const double t = steep ? ax / ay : ay / ax;
    double u = t;
    for (int i = 0; i < 2; ++i) {
        u = u / (1.0 + std::sqrt(1.0 + (u * u)));
    }
    const double u2 = u * u;
    double series = 1.0 / 17.0;
    for (int k = 15; k >= 1; k -= 2) {
        series = (1.0 / static_cast<double>(k)) - (u2 * series);
    }
    double angle = 4.0 * u * series;
    if (steep) {
        angle = (std::numbers::pi / 2.0) - angle;
    }
    if (dx < 0.0) {
        angle = std::numbers::pi - angle;
    }
    return static_cast<float>(std::copysign(angle, dy));
}

} // namespace slash
