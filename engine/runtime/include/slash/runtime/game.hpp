#pragma once

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slash::runtime {

struct GameOptions {
    // The folder with project.json and the scenes. Empty means the "data" folder an export puts
    // next to the executable (inside the app bundle on macOS, the root on the web).
    std::string data_dir;
    bool headless = false;
    // Stop after this many ticks; 0 runs until the window closes. Headless runs must set it.
    std::uint64_t max_ticks = 0;
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

    // What ran: {"entities", "headless", "name", "quit_requested", "renderer", "scene",
    // "sprites", "ticks"}. "sprites" counts the sprites the last frame drew, or would have
    // drawn on the null device, so a headless run shows whether every sprite can draw.
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
    platform::Platform platform_;
    // Declared after the platform, so it lets go of the window before the window closes.
    std::unique_ptr<render::RenderDevice> renderer_;
    // Declared after the device, so its textures go before the device does.
    std::unique_ptr<canvas::CanvasRenderer> canvas_;
    std::unique_ptr<platform::Loop> loop_;
    bool draw_failed_ = false;
};

} // namespace slash::runtime
