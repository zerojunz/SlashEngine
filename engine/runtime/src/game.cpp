#include "slash/runtime/game.hpp"

#include "slash/animation/animation.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <SDL3/SDL_events.h>

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
    config.opengl = project.value().rendering_driver == "opengl";
    auto platform = platform::Platform::create(config);
    if (!platform) {
        return platform.error();
    }

    auto game = std::unique_ptr<Game>(
        new Game(std::move(project).value(), std::move(platform).value(), data_dir));
    if (options.gameplay != nullptr) {
        game->gameplay_ = std::make_unique<gameplay::Host>(game->components_, options.gameplay);
    } else if (!game->project_.gameplay_sources.empty()) {
        game->gameplay_ =
            std::make_unique<gameplay::Host>(game->components_, gameplay::library_path(data_dir));
    }
    if (game->gameplay_ != nullptr) {
        const gameplay::Load load = game->gameplay_->attach(game->world_);
        if (!load.error.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "the game's code did not load: " + load.error);
        }
        if (!game->gameplay_->loaded()) {
            return make_error(ErrorCode::NotFound, "the game's code is not built; build it with "
                                                   "\"slash-cli build\"");
        }
    }
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
    if (options.register_backends != nullptr) {
        // The SDK packages' backends, from the code an export generates for them.
        services::Backends backends;
        options.register_backends(backends);
        if (backends.ads || !backends.analytics.empty() || backends.purchases) {
            services::set_backends(game->world_, std::move(backends));
        }
    }
    log::info("game started", {{"name", game->project_.name},
                               {"scene", game->project_.main_scene},
                               {"entities", game->world_.size()},
                               {"renderer", render::backend_name(game->renderer_->backend())}});
    return game;
}

Game::Game(Project project, platform::Platform platform, const std::string& data_dir)
    : project_(std::move(project)), data_dir_(data_dir), assets_(asset::folder_source(data_dir)),
      world_(components_), platform_(std::move(platform)) {
    // Before anything uses the job system's threads: physics makes its Box2D world on them.
    if (!jobs::configure(project_.max_threads)) {
        log::warn("threading.max_threads is ignored: the job system has already started",
                  {{"max_threads", project_.max_threads}});
    }
    // The world only keeps a pointer to the registry, so filling it afterwards is fine.
    world::register_builtin_components(components_);
    world_.set_file_reader(
        [this](std::string_view path) { return assets_.read(path, world::kPrefabType); });
    canvas::install(world_, assets_);
    const auto& [r, g, b, a] = project_.clear_color;
    canvas::set_settings(world_, {.clear_color = {.r = r, .g = g, .b = b, .a = a},
                                  .texture_filter = project_.texture_filter == "nearest"
                                                        ? render::Filter::Nearest
                                                        : render::Filter::Linear,
                                  .tick_seconds = 1.0 / project_.tick_hz,
                                  .scaling = project_.scaling});
    ui::set_settings(world_, {.viewport = {static_cast<float>(project_.window_width),
                                           static_cast<float>(project_.window_height)},
                              .theme = project_.gui_theme,
                              .allow_system_fallback = project_.allow_system_fallback,
                              .tooltip_delay_sec = project_.tooltip_delay_sec,
                              .tooltip_position_offset = project_.tooltip_position_offset,
                              .drag_threshold = project_.drag_threshold});
    animation::install(world_, assets_);
    physics2d::install(world_, project_.physics_2d);
    input::install(world_, project_.input, project_.input_devices);
    audio::install(world_, assets_, project_.audio);
    // With no backends until an SDK package's glue code gives some.
    services::install(world_, project_.services);
    platform_.set_event_handler([this](const SDL_Event& event) { handle_event(event); });
    if (!platform_.headless()) {
        // Each tick's sound goes to the speakers; with no output device the game stays silent.
        auto output =
            audio::SdlOutput::open(project_.audio.mix_rate, project_.audio.output_latency);
        if (output) {
            audio_output_ = std::move(output).value();
            audio::set_sink(world_, audio_output_->sink());
        } else {
            log::warn("no sound", {{"error", output.error().message}});
        }
        if (project_.audio.enable_input) {
            // The microphone opens when a microphone player turns input on.
            audio_input_ = std::make_unique<audio::SdlInput>();
            audio::set_input_device(world_, audio_input_.get());
        }
        // LineEdits cut, copy and paste with the system's clipboard.
        ui::set_clipboard_backend(
            world_,
            {.get = [this] { return platform_.clipboard_text(); },
             .set = [this](const std::string& text) { platform_.set_clipboard_text(text); }});
    }
}

Game::~Game() {
    // Before SDL quits, as backends written in Java or Objective-C still call through it.
    services::stop(world_);
    audio::set_sink(world_, nullptr);
    audio::set_input_device(world_, nullptr);
}

bool Game::frame() {
    if (gameplay_ != nullptr) {
        (void)gameplay_->poll();
    }
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
    // The render scale scales the window's pixels.
    options.pixel_width = static_cast<std::uint32_t>(window.pixel_width);
    options.pixel_height = static_cast<std::uint32_t>(window.pixel_height);
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
    // A phone may stop the game once it is in the background, so analytics events go now.
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_TERMINATING) {
        services::flush_analytics(world_);
    }
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
    json::Value backends = services::backends_json(world_);
    if (backends.find("ads")->as_string() != "none" ||
        !backends.find("analytics")->as_array().empty() ||
        backends.find("purchases")->as_string() != "none") {
        summary["services"] = std::move(backends);
    }
    return summary;
}

} // namespace slash::runtime
