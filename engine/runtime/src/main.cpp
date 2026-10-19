// The entry point of every game executable. SDL's main callbacks drive the loop, so the same code
// runs on desktops, on phones and on the web, where the browser owns the main loop.
#define SDL_MAIN_USE_CALLBACKS 1

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/runtime/game.hpp"
#include "slash/world/game_module.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

// An export links the game's own code in (export/CMakeLists.txt); slash-game loads it instead.
#ifdef SLASH_GAMEPLAY_LINKED
extern "C" void slash_game_module(slash::world::GameModule& game);
#endif

namespace {

// Results such as the summary go to stdout, or to logcat on Android, where slash-cli export
// --test reads them.
void print_line(const std::string& text) {
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "SlashEngine", text.c_str());
#else
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
#endif
}

} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    for (const std::string& arg : args) {
        if (arg == "--help") {
            print_line(std::string(slash::runtime::kGameUsage));
            return SDL_APP_SUCCESS;
        }
    }
    auto options = slash::runtime::parse_game_args(args);
    if (!options) {
        slash::log::error("usage error", {{"detail", options.error().message}});
        return SDL_APP_FAILURE;
    }
#ifdef SLASH_GAMEPLAY_LINKED
    options.value().gameplay = slash_game_module;
#endif
    auto game = slash::runtime::Game::start(options.value());
    if (!game) {
        slash::log::error("could not start the game", {{"detail", game.error().message}});
        return SDL_APP_FAILURE;
    }
    if (!game.value()->headless()) {
        // Without a renderer to pace frames with vsync, iterate at the tick rate instead of
        // spinning. Browsers ignore this and use animation frames.
        const std::string rate = std::to_string(game.value()->project().tick_hz);
        SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, rate.c_str());
    }
    *appstate = game.value().release();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto* game = static_cast<slash::runtime::Game*>(appstate);
    return game->frame() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto* game = static_cast<slash::runtime::Game*>(appstate);
    if (event->type == SDL_EVENT_QUIT) {
        game->request_quit();
        return SDL_APP_SUCCESS;
    }
    game->handle_event(*event);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    const std::unique_ptr<slash::runtime::Game> game(static_cast<slash::runtime::Game*>(appstate));
    if (game != nullptr && result == SDL_APP_SUCCESS) {
        print_line(slash::json::write(game->summary(), slash::json::Style::Compact));
    }
}
