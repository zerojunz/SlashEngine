#pragma once

#include "slash/core/fixed_step.hpp"
#include "slash/core/result.hpp"
#include "slash/platform/platform.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace slash::platform {

// The most frames per second a frame cap can ask for.
inline constexpr int kMaxFps = 1000;

struct RunOptions {
    double tick_hz = 60.0;
    // Stop after this many ticks; 0 runs until quit. Headless runs must set it.
    std::uint64_t max_ticks = 0;
    // Frames per second at most, from 0 (no cap, Godot's default) to kMaxFps. Windowed runs wait
    // between frames to keep to it, and on the web the browser calls frames at this rate;
    // headless runs never wait.
    int max_fps = 0;
};

struct RunStats {
    std::uint64_t ticks = 0;
    bool quit_requested = false;
};

// How evenly frames arrive, over the last kWindow frames.
struct FrameStats {
    static constexpr std::size_t kWindow = 120;

    std::uint64_t frames = 0; // frames run
    double fps = 0.0;
    double frame_ms_avg = 0.0; // time from one frame's start to the next
    double frame_ms_min = 0.0;
    double frame_ms_max = 0.0;
    // Frames that ran more than one tick to catch up, because the frame before took longer than
    // a tick: the hitches a player sees.
    std::uint64_t catch_up_frames = 0;
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

    // Changes the frame cap from the next frame; see RunOptions::max_fps. Returns false and
    // changes nothing if max_fps is out of range.
    bool set_max_fps(int max_fps) noexcept;
    [[nodiscard]] int max_fps() const noexcept { return options_.max_fps; }

    [[nodiscard]] const RunStats& stats() const noexcept { return stats_; }
    [[nodiscard]] FrameStats frame_stats() const;
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] double tick_hz() const noexcept { return options_.tick_hz; }
    [[nodiscard]] bool headless() const noexcept { return platform_->headless(); }

    // How far real time is into the next tick, from 0 to 1, for drawing state interpolated
    // between the last two ticks. Always 0 headless, where frames and ticks are one to one.
    [[nodiscard]] double interpolation() const noexcept;

private:
    Loop(Platform& platform, const RunOptions& options);
    bool tick(const TickFn& on_tick, double tick_seconds);
    // Waits as long as the frame cap asks, then returns the time the frame starts.
    double pace();
    void apply_frame_cap() const;
    void record_frame(double start, int ticks);

    Platform* platform_;
    RunOptions options_;
    FixedStep step_;
    double last_seconds_;
    double next_frame_;         // when the frame cap last aimed a frame to start
    double frame_start_ = -1.0; // when the last frame started, or -1 before the first
    std::array<float, FrameStats::kWindow> frame_ms_{};
    std::uint64_t frames_ = 0;
    std::uint64_t catch_up_frames_ = 0;
    RunStats stats_;
    bool finished_ = false;
};

// Called once per frame, after its ticks, e.g. to draw it. A renderer waiting for the display
// paces the frames; without one, windowed runs with no frame cap yield 1 ms per frame.
using FrameFn = std::function<void()>;

// Runs a loop to the end on this thread. Windowed runs yield between frames instead of spinning.
RunStats run(Loop& loop, const TickFn& on_tick, const FrameFn& on_frame = {});

// Creates a loop and runs it to the end.
Result<RunStats> run(Platform& platform, const RunOptions& options, const TickFn& on_tick,
                     const FrameFn& on_frame = {});

} // namespace slash::platform
