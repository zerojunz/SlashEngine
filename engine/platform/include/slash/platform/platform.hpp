#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace slash::platform {

struct Config {
    std::string title = "SlashEngine";
    int width = 1280;
    int height = 720;
    // Run with no window or GPU: how agents and CI run games, tests and replays on a server.
    bool headless = false;
    // Make the window for OpenGL ES, for the opengl render driver. Always on the web, where
    // games draw with WebGL 2.
    bool opengl = false;
};

struct Events {
    bool quit_requested = false;
};

struct DisplayMode {
    int width = 0; // in screen coordinates; pixels are these times pixel_density
    int height = 0;
    float refresh_rate = 0.0F; // Hz, or 0 when the system doesn't say
    float pixel_density = 1.0F;
};

struct Display {
    std::uint32_t id = 0;
    std::string name;
    bool primary = false;
    bool has_window = false; // the game's window is on it
    int x = 0;               // bounds in screen coordinates
    int y = 0;
    int width = 0;
    int height = 0;
    float content_scale = 1.0F;     // the system's UI scale, e.g. 2 on a Retina display
    DisplayMode mode;               // the current mode
    std::vector<DisplayMode> modes; // its fullscreen modes, largest and fastest first
};

// A window's size in screen coordinates. On a high-density display it has more pixels than that.
struct WindowSize {
    int width = 0;
    int height = 0;
    // Its pixels, which the renderer draws.
    int pixel_width = 0;
    int pixel_height = 0;
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
    // Called with each event poll_events drains, e.g. to read input from it.
    void set_event_handler(std::function<void(const SDL_Event&)> handler);

    // Monotonic time in seconds since the platform started.
    [[nodiscard]] double now_seconds() const;

    [[nodiscard]] bool headless() const noexcept { return headless_; }

    // The main window, for the renderer to draw to; null when headless.
    [[nodiscard]] SDL_Window* window() const noexcept { return window_; }

    // The window's current size; zero when headless.
    [[nodiscard]] WindowSize window_size() const;

    // The SDL video driver in use (e.g. "cocoa", "wayland"), or "none" when headless.
    [[nodiscard]] std::string_view video_driver() const;

    // The connected displays, primary first; none when headless.
    [[nodiscard]] std::vector<Display> displays() const;

    // The system clipboard's text, as Godot's DisplayServer::clipboard_get and clipboard_set.
    // Headless, there is no clipboard: it reads empty and setting it does nothing.
    [[nodiscard]] std::string clipboard_text() const;
    void set_clipboard_text(const std::string& text) const;

private:
    Platform(SDL_Window* window, bool headless) noexcept;
    void shutdown() noexcept;

    SDL_Window* window_ = nullptr;
    std::function<void(const SDL_Event&)> event_handler_;
    bool headless_ = false;
    bool active_ = false;
};

// The platform the game was built for, as export presets name it: "macos", "ios", "android",
// "windows", "linux" or "web".
[[nodiscard]] std::string_view os_name();

} // namespace slash::platform
