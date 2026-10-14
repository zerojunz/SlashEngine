#pragma once

#include "slash/core/result.hpp"
#include "slash/platform/platform.hpp"

#include <cstdint>
#include <functional>

namespace slash::platform {

struct RunOptions {
    double tick_hz = 60.0;
    // Stop after this many ticks; 0 runs until quit. Headless runs must set it.
    std::uint64_t max_ticks = 0;
};

struct RunStats {
    std::uint64_t ticks = 0;
    bool quit_requested = false;
};

// Called once per fixed tick with the tick index (from 0) and the fixed tick length in seconds.
using TickFn = std::function<void(std::uint64_t tick, double tick_seconds)>;

// Runs the main loop. Windowed runs pace ticks against real time; headless runs execute ticks
// back to back with no sleeping, so a headless run of N ticks is fast and fully deterministic.
Result<RunStats> run(Platform& platform, const RunOptions& options, const TickFn& on_tick);

} // namespace slash::platform
