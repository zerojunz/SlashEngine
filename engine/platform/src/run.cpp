#include "slash/platform/run.hpp"

#include "slash/core/fixed_step.hpp"
#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

#include <cmath>

namespace slash::platform {
namespace {

RunStats run_headless(Platform& platform, const RunOptions& options, const TickFn& on_tick) {
    const double tick_seconds = 1.0 / options.tick_hz;
    RunStats stats;
    while (stats.ticks < options.max_ticks) {
        if (platform.poll_events().quit_requested) {
            stats.quit_requested = true;
            break;
        }
        if (on_tick) {
            on_tick(stats.ticks, tick_seconds);
        }
        ++stats.ticks;
    }
    return stats;
}

RunStats run_windowed(Platform& platform, const RunOptions& options, const TickFn& on_tick) {
    FixedStep step(options.tick_hz);
    RunStats stats;
    double last = platform.now_seconds();
    for (;;) {
        if (platform.poll_events().quit_requested) {
            stats.quit_requested = true;
            break;
        }
        const double now = platform.now_seconds();
        const int ticks = step.advance(now - last);
        last = now;
        for (int i = 0; i < ticks; ++i) {
            if (on_tick) {
                on_tick(stats.ticks, step.tick_seconds());
            }
            ++stats.ticks;
            if (options.max_ticks != 0 && stats.ticks >= options.max_ticks) {
                return stats;
            }
        }
        // No renderer yet to pace frames with vsync, so yield instead of spinning.
        SDL_Delay(1);
    }
    return stats;
}

} // namespace

Result<RunStats> run(Platform& platform, const RunOptions& options, const TickFn& on_tick) {
    if (!(options.tick_hz > 0.0) || !std::isfinite(options.tick_hz)) {
        return make_error(ErrorCode::InvalidArgument, "tick_hz must be a positive number");
    }
    if (platform.headless() && options.max_ticks == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "headless runs need max_ticks, or they would never end");
    }

    const RunStats stats = platform.headless() ? run_headless(platform, options, on_tick)
                                               : run_windowed(platform, options, on_tick);
    log::info("run finished", {{"ticks", stats.ticks},
                               {"quit_requested", stats.quit_requested},
                               {"headless", platform.headless()}});
    return stats;
}

} // namespace slash::platform
