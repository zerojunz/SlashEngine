#include "slash/runtime/game.hpp"

#include "slash/animation/animation.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <array>
#include <charconv>
#include <system_error>
#include <utility>

namespace slash::runtime {

Result<GameOptions> parse_game_args(const std::vector<std::string>& args) {
    constexpr std::array<std::string_view, 3> kOptions{"--headless", "--ticks", "--data"};
    GameOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--ticks" && has_value) {
            const std::string& value = args[++i];
            const char* end = value.data() + value.size();
            const auto [ptr, ec] = std::from_chars(value.data(), end, options.max_ticks);
            if (ec != std::errc() || ptr != end) {
                return make_error(ErrorCode::InvalidArgument,
                                  "--ticks needs a whole number, got \"" + value + "\"");
            }
        } else if (arg == "--data" && has_value) {
            options.data_dir = args[++i];
        } else if (arg.starts_with("--")) {
            std::string message = "unknown option or missing value: " + arg;
            if (const std::string_view hint = closest_match(arg, kOptions); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return make_error(ErrorCode::InvalidArgument, std::move(message));
        }
    }
    if (options.headless && options.max_ticks == 0) {
        return make_error(ErrorCode::InvalidArgument, "--headless needs --ticks");
    }
    return options;
}

Result<std::unique_ptr<Game>> Game::start(const GameOptions& options) {
    const std::string data_dir =
        options.data_dir.empty() ? platform::base_path() + "data" : options.data_dir;
    auto project = load_project(data_dir);
    if (!project) {
        return project.error();
    }
    const std::string scene_path = join_path(data_dir, project.value().main_scene);
    auto text = platform::read_file(scene_path);
    if (!text) {
        return make_error(ErrorCode::NotFound,
                          "the main scene \"" + project.value().main_scene + "\" is missing");
    }
    auto scene = json::parse(text.value());
    if (!scene) {
        return make_error(ErrorCode::InvalidArgument,
                          project.value().main_scene + ": " + scene.error().message);
    }

    platform::Config config;
    config.title = project.value().name;
    config.width = project.value().window_width;
    config.height = project.value().window_height;
    config.headless = options.headless;
    auto platform = platform::Platform::create(config);
    if (!platform) {
        return platform.error();
    }

    auto game = std::unique_ptr<Game>(
        new Game(std::move(project).value(), std::move(platform).value(), data_dir));
    if (auto loaded = world::load_scene(game->world_, scene.value()); !loaded) {
        return make_error(loaded.error().code,
                          game->project_.main_scene + ": " + loaded.error().message);
    }
    auto loop = platform::Loop::create(game->platform_, {.tick_hz = game->project_.tick_hz,
                                                         .max_ticks = options.max_ticks,
                                                         .max_fps = game->project_.max_fps});
    if (!loop) {
        return loop.error();
    }
    game->loop_ = std::make_unique<platform::Loop>(std::move(loop).value());
    game->renderer_ =
        game->platform_.headless()
            ? render::create_null_device()
            : render::create_device_or_null({.window = game->platform_.window(),
                                             .driver = game->project_.rendering_driver,
                                             .debug = false});
    // The schema only allows known names, so the mode always parses.
    game->renderer_->set_vsync(
        render::parse_vsync_mode(game->project_.vsync_mode).value_or(render::VsyncMode::Enabled));
    game->canvas_ = std::make_unique<canvas::CanvasRenderer>(*game->renderer_, game->assets_);
    log::info("game started", {{"name", game->project_.name},
                               {"scene", game->project_.main_scene},
                               {"entities", game->world_.size()},
                               {"renderer", render::backend_name(game->renderer_->backend())}});
    return game;
}

Game::Game(Project project, platform::Platform platform, const std::string& data_dir)
    : project_(std::move(project)), assets_(asset::folder_source(data_dir)), world_(components_),
      platform_(std::move(platform)) {
    // The world only keeps a pointer to the registry, so filling it afterwards is fine.
    world::register_builtin_components(components_);
    world_.set_file_reader(
        [this](std::string_view path) { return assets_.read(path, world::kPrefabType); });
    canvas::install(world_, assets_);
    ui::set_settings(world_, {.viewport = {static_cast<float>(project_.window_width),
                                           static_cast<float>(project_.window_height)},
                              .theme = project_.gui_theme,
                              .allow_system_fallback = project_.allow_system_fallback});
    animation::install(world_, assets_);
    physics2d::install(world_, project_.physics_2d);
    input::install(world_, project_.input, project_.input_devices);
    platform_.set_event_handler([this](const SDL_Event& event) { handle_event(event); });
    if (!platform_.headless()) {
        // LineEdits cut, copy and paste with the system's clipboard.
        ui::set_clipboard_backend(
            world_,
            {.get = [this] { return platform_.clipboard_text(); },
             .set = [this](const std::string& text) { platform_.set_clipboard_text(text); }});
    }
}

Game::~Game() = default;

bool Game::frame() {
    const std::uint64_t ticks = loop_->stats().ticks;
    const bool running = loop_->frame([this](std::uint64_t /*tick*/, double tick_seconds) {
        canvas_->before_tick(world_);
        // Controls are laid out in the window drawn, so clicks land on what is shown.
        if (const platform::WindowSize window = platform_.window_size(); window.width > 0) {
            ui::set_viewport(world_,
                             {static_cast<float>(window.width), static_cast<float>(window.height)});
        }
        world_.step(tick_seconds);
        // The input method and on-screen keyboard follow the LineEdit being edited.
        if (!platform_.headless()) {
            input_reader_.set_text_input(platform_.window(), ui::text_input(world_));
        }
    });
    // The frame that runs the last tick is drawn too.
    if (running || loop_->stats().ticks != ticks) {
        draw();
    }
    return running;
}

void Game::draw() {
    const auto& [r, g, b, a] = project_.clear_color;
    const platform::WindowSize window = platform_.window_size();
    canvas::CanvasOptions options;
    options.clear = {.r = r, .g = g, .b = b, .a = a};
    // World pixels are the window's screen coordinates, on high-density displays too.
    options.width = static_cast<float>(window.width);
    options.height = static_cast<float>(window.height);
    // Headless frames run one tick each, so there is nothing between ticks to draw.
    options.interpolation =
        project_.interpolation && !platform_.headless() ? loop_->interpolation() : 1.0;
    options.texture_filter =
        project_.texture_filter == "nearest" ? render::Filter::Nearest : render::Filter::Linear;
    options.time =
        canvas::material_time(world_.tick(), 1.0 / loop_->tick_hz(), options.interpolation);
    auto drawn = canvas_->draw(world_, options);
    // One warning, not one per frame.
    if (!drawn && !draw_failed_) {
        draw_failed_ = true;
        log::warn("drawing a frame failed", {{"error", drawn.error().message}});
    }
}

void Game::handle_event(const SDL_Event& event) {
    input_reader_.read(event, world_);
}

void Game::request_quit() noexcept {
    loop_->request_quit();
}

json::Value Game::summary() const {
    json::Value summary;
    summary["name"] = project_.name;
    summary["scene"] = project_.main_scene;
    summary["entities"] = world_.size();
    summary["ticks"] = loop_->stats().ticks;
    summary["quit_requested"] = loop_->stats().quit_requested;
    summary["headless"] = platform_.headless();
    summary["renderer"] = render::backend_name(renderer_->backend());
    summary["sprites"] = canvas_->sprites_drawn();
    return summary;
}

} // namespace slash::runtime
