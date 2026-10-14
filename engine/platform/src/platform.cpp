#include "slash/platform/platform.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

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
    : window_(std::exchange(other.window_, nullptr)), headless_(other.headless_),
      active_(std::exchange(other.active_, false)) {}

Platform& Platform::operator=(Platform&& other) noexcept {
    if (this != &other) {
        shutdown();
        window_ = std::exchange(other.window_, nullptr);
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
    }
    return events;
}

double Platform::now_seconds() const {
    return static_cast<double>(SDL_GetTicksNS()) / 1e9;
}

std::string_view Platform::video_driver() const {
    if (headless_) {
        return "none";
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? driver : "unknown";
}

} // namespace slash::platform
