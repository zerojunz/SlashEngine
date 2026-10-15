#pragma once

#include "slash/core/result.hpp"

#include <string>
#include <string_view>

struct SDL_Window;

namespace slash::platform {

struct Config {
    std::string title = "SlashEngine";
    int width = 1280;
    int height = 720;
    // Run with no window or GPU: how agents and CI run games, tests and replays on a server.
    bool headless = false;
};

struct Events {
    bool quit_requested = false;
};

// Owns the SDL3 platform layer and, unless headless, the main window. Only one Platform may
// exist at a time.
class Platform {
public:
    static Result<Platform> create(const Config& config);

    Platform(Platform&& other) noexcept;
    Platform& operator=(Platform&& other) noexcept;
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    ~Platform();

    // Drains pending OS events. A closed window or Ctrl+C both report quit_requested.
    Events poll_events();

    // Monotonic time in seconds since the platform started.
    [[nodiscard]] double now_seconds() const;

    [[nodiscard]] bool headless() const noexcept { return headless_; }

    // The main window, for the renderer to draw to; null when headless.
    [[nodiscard]] SDL_Window* window() const noexcept { return window_; }

    // The SDL video driver in use (e.g. "cocoa", "wayland"), or "none" when headless.
    [[nodiscard]] std::string_view video_driver() const;

private:
    Platform(SDL_Window* window, bool headless) noexcept;
    void shutdown() noexcept;

    SDL_Window* window_ = nullptr;
    bool headless_ = false;
    bool active_ = false;
};

} // namespace slash::platform
