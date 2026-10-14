#include "slash/core/fixed_step.hpp"

#include "slash/core/assert.hpp"

#include <cmath>

namespace slash {

FixedStep::FixedStep(double tick_hz, int max_ticks_per_advance)
    : tick_seconds_(1.0 / tick_hz), max_ticks_per_advance_(max_ticks_per_advance) {
    SLASH_ASSERT(tick_hz > 0.0 && std::isfinite(tick_hz), "tick_hz must be positive");
    SLASH_ASSERT(max_ticks_per_advance > 0, "max_ticks_per_advance must be positive");
}

int FixedStep::advance(double elapsed_seconds) {
    if (elapsed_seconds > 0.0) {
        accumulator_ += elapsed_seconds;
    }
    int ticks = 0;
    while (accumulator_ >= tick_seconds_ && ticks < max_ticks_per_advance_) {
        accumulator_ -= tick_seconds_;
        ++ticks;
    }
    if (accumulator_ >= tick_seconds_) {
        // Over the catch-up cap: drop the backlog but keep the partial tick.
        accumulator_ = std::fmod(accumulator_, tick_seconds_);
    }
    return ticks;
}

} // namespace slash
