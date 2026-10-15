#include "slash/platform/platform.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <utility>

namespace slash::platform {
namespace {

bool g_platform_alive = false;

} // namespace

Result<Platform> Platform::create(const Config& config) {
    if (g_platform_alive) {
        return make_error(ErrorCode::InvalidArgument, "a Platform already exists");
    }
    if (!config.headless && (config.width <= 0 || config.height <= 0)) {
        return make_error(ErrorCode::InvalidArgument, "window width and height must be positive");
    }

    const SDL_InitFlags flags =
        config.headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    if (!SDL_Init(flags)) {
        return make_error(ErrorCode::PlatformFailure,
                          std::string("SDL_Init failed: ") + SDL_GetError());
    }

    SDL_Window* window = nullptr;
    if (!config.headless) {
        window = SDL_CreateWindow(config.title.c_str(), config.width, config.height,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window == nullptr) {
            std::string message = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
            SDL_Quit();
            return make_error(ErrorCode::PlatformFailure, std::move(message));
        }
        // Gamepads are optional: a game still runs with the keyboard if they can't start.
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            log::warn("gamepads are unavailable", {{"error", SDL_GetError()}});
        }
    }

    Platform platform(window, config.headless);
    log::info("platform started", {{"headless", config.headless},
                                   {"video_driver", platform.video_driver()},
                                   {"width", config.width},
                                   {"height", config.height}});
    return platform;
}

Platform::Platform(SDL_Window* window, bool headless) noexcept
    : window_(window), headless_(headless), active_(true) {
    g_platform_alive = true;
}

Platform::Platform(Platform&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)),
      event_handler_(std::move(other.event_handler_)), headless_(other.headless_),
      active_(std::exchange(other.active_, false)) {}

Platform& Platform::operator=(Platform&& other) noexcept {
    if (this != &other) {
        shutdown();
        window_ = std::exchange(other.window_, nullptr);
        event_handler_ = std::move(other.event_handler_);
        headless_ = other.headless_;
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

Platform::~Platform() {
    shutdown();
}

void Platform::shutdown() noexcept {
    if (!active_) {
        return;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
    active_ = false;
    g_platform_alive = false;
}

Events Platform::poll_events() {
    Events events;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            events.quit_requested = true;
        }
        if (event_handler_) {
            event_handler_(event);
        }
    }
    return events;
}

void Platform::set_event_handler(std::function<void(const SDL_Event&)> handler) {
    event_handler_ = std::move(handler);
}

double Platform::now_seconds() const {
    return static_cast<double>(SDL_GetTicksNS()) / 1e9;
}

std::vector<Display> Platform::displays() const {
    std::vector<Display> out;
    if (headless_) {
        return out;
    }
    const auto to_mode = [](const SDL_DisplayMode& mode) {
        return DisplayMode{.width = mode.w,
                           .height = mode.h,
                           .refresh_rate = mode.refresh_rate,
                           .pixel_density = mode.pixel_density};
    };
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    const SDL_DisplayID with_window = window_ != nullptr ? SDL_GetDisplayForWindow(window_) : 0;
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        const SDL_DisplayID id = ids[i];
        Display display;
        display.id = id;
        const char* name = SDL_GetDisplayName(id);
        display.name = name != nullptr ? name : "";
        display.primary = id == primary;
        display.has_window = id == with_window;
        if (SDL_Rect bounds; SDL_GetDisplayBounds(id, &bounds)) {
            display.x = bounds.x;
            display.y = bounds.y;
            display.width = bounds.w;
            display.height = bounds.h;
        }
        display.content_scale = SDL_GetDisplayContentScale(id);
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id); mode != nullptr) {
            display.mode = to_mode(*mode);
        }
        int mode_count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &mode_count);
        for (int m = 0; modes != nullptr && m < mode_count; ++m) {
            display.modes.push_back(to_mode(*modes[m]));
        }
        // The list and its modes are one allocation.
        SDL_free(reinterpret_cast<void*>(modes));
        out.push_back(std::move(display));
    }
    SDL_free(ids);
    std::stable_partition(out.begin(), out.end(), [](const Display& d) { return d.primary; });
    return out;
}

WindowSize Platform::window_size() const {
    WindowSize size;
    if (window_ != nullptr && !SDL_GetWindowSize(window_, &size.width, &size.height)) {
        size = {};
    }
    return size;
}

std::string_view Platform::video_driver() const {
    if (headless_) {
        return "none";
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? driver : "unknown";
}

std::string Platform::clipboard_text() const {
    if (headless_) {
        return {};
    }
    char* text = SDL_GetClipboardText();
    std::string out = text != nullptr ? text : "";
    SDL_free(text);
    return out;
}

void Platform::set_clipboard_text(const std::string& text) const {
    if (!headless_ && !SDL_SetClipboardText(text.c_str())) {
        log::warn("the clipboard was not set", {{"error", SDL_GetError()}});
    }
}

} // namespace slash::platform
