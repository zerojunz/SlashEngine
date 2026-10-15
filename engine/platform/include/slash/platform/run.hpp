#pragma once

#include "slash/core/fixed_step.hpp"
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

// The main loop, one frame at a time, for hosts that own the loop themselves: SDL's main
// callbacks on the web and on phones call frame() once per iteration. Windowed frames run the
// ticks that real time says are due; headless frames run one tick each, with no waiting, so a
// headless run of N ticks is fast and fully deterministic.
class Loop {
public:
    [[nodiscard]] static Result<Loop> create(Platform& platform, const RunOptions& options);

    // Polls events and runs the ticks that are due. Returns false once the run is over, because
    // max_ticks ran or a quit was requested; later calls do nothing.
    bool frame(const TickFn& on_tick);

    // Ends the run, e.g. when the host saw a quit event itself.
    void request_quit() noexcept;

    [[nodiscard]] const RunStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] double tick_hz() const noexcept { return options_.tick_hz; }

private:
    Loop(Platform& platform, const RunOptions& options);
    bool tick(const TickFn& on_tick, double tick_seconds);

    Platform* platform_;
    RunOptions options_;
    FixedStep step_;
    double last_seconds_;
    RunStats stats_;
    bool finished_ = false;
};

// Called once per frame, after its ticks, e.g. to draw it.
using FrameFn = std::function<void()>;

// Runs a Loop to the end on this thread. Windowed runs yield between frames instead of spinning.
Result<RunStats> run(Platform& platform, const RunOptions& options, const TickFn& on_tick,
                     const FrameFn& on_frame = {});

} // namespace slash::platform
