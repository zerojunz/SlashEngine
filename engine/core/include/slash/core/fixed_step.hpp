#pragma once

namespace slash {

// Turns variable frame times into a whole number of fixed simulation ticks, so the simulation
// advances identically no matter how fast frames arrive. This is what makes replays and headless
// runs deterministic.
class FixedStep {
public:
    // tick_hz must be positive. max_ticks_per_advance caps catch-up work after a long stall;
    // time beyond the cap is dropped instead of making the next frame even slower.
    explicit FixedStep(double tick_hz, int max_ticks_per_advance = 8);

    // Adds elapsed real time (negative values count as zero) and returns how many ticks to run.
    int advance(double elapsed_seconds);

    [[nodiscard]] double tick_seconds() const noexcept { return tick_seconds_; }

    // Fraction of a tick left in the accumulator, in [0, 1), for interpolating rendered state.
    [[nodiscard]] double alpha() const noexcept { return accumulator_ / tick_seconds_; }

private:
    double tick_seconds_;
    int max_ticks_per_advance_;
    double accumulator_ = 0.0;
};

} // namespace slash
