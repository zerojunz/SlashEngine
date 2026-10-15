#include "slash/platform/run.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace slash::platform {

Result<Loop> Loop::create(Platform& platform, const RunOptions& options) {
    if (!(options.tick_hz > 0.0) || !std::isfinite(options.tick_hz)) {
        return make_error(ErrorCode::InvalidArgument, "tick_hz must be a positive number");
    }
    if (platform.headless() && options.max_ticks == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "headless runs need max_ticks, or they would never end");
    }
    if (options.max_fps < 0 || options.max_fps > kMaxFps) {
        return make_error(ErrorCode::InvalidArgument,
                          "max_fps must be from 0 (no cap) to " + std::to_string(kMaxFps));
    }
    return Loop(platform, options);
}

Loop::Loop(Platform& platform, const RunOptions& options)
    : platform_(&platform), options_(options), step_(options.tick_hz),
      last_seconds_(platform.now_seconds()), next_frame_(last_seconds_) {
    apply_frame_cap();
}

void Loop::apply_frame_cap() const {
#ifdef __EMSCRIPTEN__
    // The browser calls each frame, at the display's rate unless SDL is told another rate, and
    // waiting inside a frame would freeze the page. So on the web the cap is that rate.
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, std::to_string(options_.max_fps).c_str());
#endif
}

bool Loop::frame(const TickFn& on_tick) {
    if (finished_) {
        return false;
    }
    if (platform_->poll_events().quit_requested) {
        request_quit();
        return false;
    }
    if (platform_->headless()) {
        record_frame(platform_->now_seconds(), 1);
        return tick(on_tick, step_.tick_seconds());
    }
    const double now = pace();
    const int due = step_.advance(now - last_seconds_);
    last_seconds_ = now;
    record_frame(now, due);
    for (int i = 0; i < due; ++i) {
        if (!tick(on_tick, step_.tick_seconds())) {
            return false;
        }
    }
    return true;
}

double Loop::pace() {
    double now = platform_->now_seconds();
#ifdef __EMSCRIPTEN__
    return now; // see apply_frame_cap
#else
    if (options_.max_fps == 0) {
        return now;
    }
    // As Godot does: aim each frame one period after the last aim, but never more than a period
    // behind or ahead of now, so a stall doesn't cause a burst of fast frames after it.
    const double period = 1.0 / options_.max_fps;
    next_frame_ += period;
    if (now < next_frame_) {
        SDL_DelayPrecise(static_cast<Uint64>((next_frame_ - now) * 1e9));
        now = platform_->now_seconds();
    }
    next_frame_ = std::clamp(next_frame_, now - period, now + period);
    return now;
#endif
}

void Loop::record_frame(double start, int ticks) {
    if (frame_start_ >= 0.0) {
        frame_ms_.at(frames_ % frame_ms_.size()) = static_cast<float>((start - frame_start_) * 1e3);
        ++frames_;
    }
    frame_start_ = start;
    if (ticks > 1) {
        ++catch_up_frames_;
    }
}

FrameStats Loop::frame_stats() const {
    FrameStats stats;
    // The first frame has no frame before it to measure from.
    stats.frames = frame_start_ >= 0.0 ? frames_ + 1 : 0;
    stats.catch_up_frames = catch_up_frames_;
    const std::size_t count = std::min<std::size_t>(frames_, frame_ms_.size());
    if (count == 0) {
        return stats;
    }
    double sum = 0.0;
    stats.frame_ms_min = frame_ms_[0];
    stats.frame_ms_max = frame_ms_[0];
    for (std::size_t i = 0; i < count; ++i) {
        const double ms = frame_ms_.at(i);
        sum += ms;
        stats.frame_ms_min = std::min(stats.frame_ms_min, ms);
        stats.frame_ms_max = std::max(stats.frame_ms_max, ms);
    }
    stats.frame_ms_avg = sum / static_cast<double>(count);
    stats.fps = stats.frame_ms_avg > 0.0 ? 1000.0 / stats.frame_ms_avg : 0.0;
    return stats;
}

bool Loop::set_max_fps(int max_fps) noexcept {
    if (max_fps < 0 || max_fps > kMaxFps) {
        return false;
    }
    options_.max_fps = max_fps;
    next_frame_ = platform_->now_seconds();
    apply_frame_cap();
    return true;
}

double Loop::interpolation() const noexcept {
    return platform_->headless() ? 0.0 : step_.alpha();
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

RunStats run(Loop& loop, const TickFn& on_tick, const FrameFn& on_frame) {
    while (loop.frame(on_tick)) {
        if (on_frame) {
            on_frame(); // a renderer waits for the display here, unless V-Sync is off
        } else if (!loop.headless() && loop.max_fps() == 0) {
            // Nothing paces the frames, so yield instead of spinning.
            SDL_Delay(1);
        }
    }
    const RunStats& stats = loop.stats();
    log::info("run finished", {{"ticks", stats.ticks},
                               {"quit_requested", stats.quit_requested},
                               {"headless", loop.headless()}});
    return stats;
}

Result<RunStats> run(Platform& platform, const RunOptions& options, const TickFn& on_tick,
                     const FrameFn& on_frame) {
    auto loop = Loop::create(platform, options);
    if (!loop) {
        return loop.error();
    }
    return run(loop.value(), on_tick, on_frame);
}

} // namespace slash::platform
