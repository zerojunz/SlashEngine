#pragma once

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/input/sdl.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/runtime/project.hpp"
#include "slash/services/services.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

union SDL_Event;

namespace slash::gameplay {
class Host;
} // namespace slash::gameplay

namespace slash::runtime {

struct GameOptions {
    // The folder with project.json and the scenes. Empty means the "data" folder an export puts
    // next to the executable (inside the app bundle on macOS, the root on the web).
    std::string data_dir;
    bool headless = false;
    // Stop after this many ticks; 0 runs until the window closes. Headless runs must set it.
    std::uint64_t max_ticks = 0;
    // The game's own code linked in, as exports link it: the function SLASH_GAME_MODULE defines.
    // Without it, a project with gameplay sources loads the library slash-cli build makes, and
    // loads it again when it is rebuilt.
    world::GameModuleFunction gameplay = nullptr;
    // Fills in the backends the services run on, at start: the slash_sdk_register function an
    // export writes for its SDK packages' glue code. Without it the services have no backends.
    void (*register_backends)(services::Backends& backends) = nullptr;
};

inline constexpr std::string_view kGameUsage =
    "Options:\n"
    "  --headless       Run with no window or GPU (needs --ticks)\n"
    "  --ticks <n>      Stop after n fixed ticks and print a JSON summary\n"
    "  --data <dir>     Folder with project.json (default: the exported data folder)\n"
    "  --help           Show this message\n";

// Parses a game executable's arguments (without the program name). Unknown "--" options are
// errors; other arguments are ignored, since some launchers add their own (Xcode, Finder).
[[nodiscard]] Result<GameOptions> parse_game_args(const std::vector<std::string>& args);

// A running game: a project's main scene loaded into a world, ticked on the platform's loop and
// drawn to its window.
class Game {
public:
    [[nodiscard]] static Result<std::unique_ptr<Game>> start(const GameOptions& options);

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    Game(Game&&) = delete;
    Game& operator=(Game&&) = delete;
    ~Game();

    // Runs one frame. Returns false once the run is over.
    bool frame();
    void request_quit() noexcept;
    // Reads input from an OS event SDL hands to the app; the platform hands the rest over itself.
    // Going to the background sends the analytics backends' batched events.
    void handle_event(const SDL_Event& event);

    // What ran: {"entities", "headless", "name", "quit_requested", "renderer", "scene",
    // "sprites", "ticks"}, and "services" (services::backends_json) when the services run on
    // backends. "sprites" counts the sprites the last frame drew, or would have drawn on the
    // null device, so a headless run shows whether every sprite can draw.
    [[nodiscard]] json::Value summary() const;

    [[nodiscard]] const Project& project() const noexcept { return project_; }
    [[nodiscard]] world::World& world() noexcept { return world_; }
    // Assets from the data folder, for gameplay code; prefab files are read through it too.
    [[nodiscard]] asset::Assets& assets() noexcept { return assets_; }
    [[nodiscard]] bool headless() const noexcept { return platform_.headless(); }
    // The device drawing to the window: the null device when headless or no GPU backend works.
    // Its set_vsync changes V-Sync while the game runs, e.g. from an options menu.
    [[nodiscard]] render::RenderDevice& renderer() noexcept { return *renderer_; }
    // The run loop: its set_max_fps changes the frame cap, and frame_stats says how evenly
    // frames arrive.
    [[nodiscard]] platform::Loop& loop() noexcept { return *loop_; }

private:
    Game(Project project, platform::Platform platform, const std::string& data_dir);
    void draw();

    Project project_;
    asset::Assets assets_;
    world::ComponentRegistry components_;
    world::World world_;
    std::unique_ptr<gameplay::Host> gameplay_;
    platform::Platform platform_;
    // Declared after the platform, so it closes the gamepads before SDL quits.
    input::SdlReader input_reader_;
    // Declared after the platform, so it lets go of the window before the window closes.
    std::unique_ptr<render::RenderDevice> renderer_;
    // Declared after the device, so its textures go before the device does.
    std::unique_ptr<canvas::CanvasRenderer> canvas_;
    std::unique_ptr<platform::Loop> loop_;
    // The speakers and the microphone, in a window; declared after the platform, so they close
    // before SDL quits.
    std::unique_ptr<audio::SdlOutput> audio_output_;
    std::unique_ptr<audio::SdlInput> audio_input_;
    bool draw_failed_ = false;
};

} // namespace slash::runtime
