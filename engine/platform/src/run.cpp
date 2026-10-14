#include "slash/platform/run.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

#include <cmath>

namespace slash::platform {

Result<Loop> Loop::create(Platform& platform, const RunOptions& options) {
    if (!(options.tick_hz > 0.0) || !std::isfinite(options.tick_hz)) {
        return make_error(ErrorCode::InvalidArgument, "tick_hz must be a positive number");
    }
    if (platform.headless() && options.max_ticks == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "headless runs need max_ticks, or they would never end");
    }
    return Loop(platform, options);
}

Loop::Loop(Platform& platform, const RunOptions& options)
    : platform_(&platform), options_(options), step_(options.tick_hz),
      last_seconds_(platform.now_seconds()) {}

bool Loop::frame(const TickFn& on_tick) {
    if (finished_) {
        return false;
    }
    if (platform_->poll_events().quit_requested) {
        request_quit();
        return false;
    }
    if (platform_->headless()) {
        return tick(on_tick, step_.tick_seconds());
    }
    const double now = platform_->now_seconds();
    const int due = step_.advance(now - last_seconds_);
    last_seconds_ = now;
    for (int i = 0; i < due; ++i) {
        if (!tick(on_tick, step_.tick_seconds())) {
            return false;
        }
    }
    return true;
}

bool Loop::tick(const TickFn& on_tick, double tick_seconds) {
    if (on_tick) {
        on_tick(stats_.ticks, tick_seconds);
    }
    ++stats_.ticks;
    if (options_.max_ticks != 0 && stats_.ticks >= options_.max_ticks) {
        finished_ = true;
    }
    return !finished_;
}

void Loop::request_quit() noexcept {
    stats_.quit_requested = true;
    finished_ = true;
}

Result<RunStats> run(Platform& platform, const RunOptions& options, const TickFn& on_tick) {
    auto loop = Loop::create(platform, options);
    if (!loop) {
        return loop.error();
    }
    while (loop.value().frame(on_tick)) {
        if (!platform.headless()) {
            // No renderer yet to pace frames with vsync, so yield instead of spinning.
            SDL_Delay(1);
        }
    }
    const RunStats& stats = loop.value().stats();
    log::info("run finished", {{"ticks", stats.ticks},
                               {"quit_requested", stats.quit_requested},
                               {"headless", platform.headless()}});
    return stats;
}

} // namespace slash::platform
