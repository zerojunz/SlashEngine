#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/process.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/game.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace runtime = slash::runtime;
namespace platform = slash::platform;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

std::vector<std::string> error_paths(std::string_view project) {
    std::vector<std::string> paths;
    for (const auto& error : runtime::validate_project(parse(project))) {
        paths.push_back(error.path);
    }
    return paths;
}

} // namespace

TEST_CASE("a minimal project gets the defaults") {
    auto project = runtime::parse_project(
        parse(R"({"format": 1, "name": "Space Pong 2", "main_scene": "scenes/main.scene.json"})"));
    REQUIRE(project.ok());
    CHECK(project.value().version == "0.1.0");
    CHECK(project.value().window_width == 1280);
    CHECK(project.value().tick_hz == doctest::Approx(60.0));
    CHECK(project.value().binary_name() == "SpacePong2");
    CHECK(project.value().bundle_id() == "com.example.spacepong2");
    CHECK(project.value().clear_color == std::array<float, 4>{0.3F, 0.3F, 0.3F, 1.0F});
    CHECK(project.value().rendering_driver == "auto");
    CHECK(project.value().vsync_mode == "enabled");
    CHECK(project.value().max_fps == 0);
    CHECK(project.value().gui_theme.empty());

    project = runtime::parse_project(parse(R"({"format": 1, "name": "?!", "id": "org.me.game",
        "main_scene": "a.scene.json", "window": {"width": 320, "vsync_mode": "mailbox"},
        "tick_hz": 30, "max_fps": 120, "rendering": {"clear_color": [0, 0.5, 1, 1],
        "driver": "null"}, "gui": {"theme": "ui/game.theme.json"}})"));
    REQUIRE(project.ok());
    CHECK(project.value().gui_theme == "ui/game.theme.json");
    CHECK(project.value().vsync_mode == "mailbox");
    CHECK(project.value().max_fps == 120);
    CHECK(project.value().clear_color == std::array<float, 4>{0.0F, 0.5F, 1.0F, 1.0F});
    CHECK(project.value().rendering_driver == "null");
    CHECK(project.value().binary_name() == "Game");
    CHECK(project.value().bundle_id() == "org.me.game");
    CHECK(project.value().window_width == 320);
    CHECK(project.value().window_height == 720);
    CHECK(project.value().tick_hz == doctest::Approx(30.0));
}

TEST_CASE("project files are checked field by field") {
    const std::string base = R"("format": 1, "name": "Pong", "main_scene": "main.scene.json")";
    CHECK(error_paths("{" + base + "}").empty());
    CHECK(error_paths(R"({"format": 1, "main_scene": "main.scene.json"})") ==
          std::vector<std::string>{""});
    CHECK(error_paths("{" + base + R"(, "id": "pong"})") == std::vector<std::string>{"/id"});
    CHECK(error_paths("{" + base + R"(, "id": "com.example.pong!"})") ==
          std::vector<std::string>{"/id"});
    CHECK(error_paths("{" + base + R"(, "version": "1.0"})") ==
          std::vector<std::string>{"/version"});
    CHECK(error_paths("{" + base + R"(, "window": {"width": 0}})") ==
          std::vector<std::string>{"/window/width"});
    CHECK(error_paths("{" + base + R"(, "tick_hz": 0})") == std::vector<std::string>{"/tick_hz"});
    CHECK(error_paths("{" + base + R"(, "window": {"vsync_mode": "on"}})") ==
          std::vector<std::string>{"/window/vsync_mode"});
    CHECK(error_paths("{" + base + R"(, "max_fps": 1001})") ==
          std::vector<std::string>{"/max_fps"});
    CHECK(error_paths("{" + base + R"(, "max_fps": -1})") == std::vector<std::string>{"/max_fps"});
    CHECK(error_paths("{" + base + R"(, "rendering": {"clear_color": [0, 0, 2, 1]}})") ==
          std::vector<std::string>{"/rendering/clear_color/2"});
    CHECK(error_paths("{" + base + R"(, "rendering": {"clear_color": [0, 0, 0]}})") ==
          std::vector<std::string>{"/rendering/clear_color"});
    CHECK(error_paths("{" + base + R"(, "rendering": {"driver": "opengl"}})") ==
          std::vector<std::string>{"/rendering/driver"});
    CHECK(error_paths("{" + base + R"(, "gui": {"theme": "../game.theme.json"}})") ==
          std::vector<std::string>{"/gui/theme"});
    CHECK(error_paths("{" + base + R"(, "gui": {"theme": "game.json"}})") ==
          std::vector<std::string>{"/gui/theme"});
    CHECK(error_paths("{" + base + R"(, "gui": {"theme": ""}})").empty());
    for (const std::string_view scene :
         {"/abs.scene.json", "../up.scene.json", "a/./b.scene.json", "c:/x.scene.json",
          "dir\\\\x.scene.json", "scenes/main.json"}) {
        CHECK(error_paths(R"({"format": 1, "name": "Pong", "main_scene": ")" + std::string(scene) +
                          R"("})") == std::vector<std::string>{"/main_scene"});
    }

    const auto errors = runtime::validate_project(parse("{" + base + R"(, "tickhz": 30})"));
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().path == "/tickhz");
    CHECK(errors.front().message.find("did you mean \"tick_hz\"") != std::string::npos);

    auto parsed = runtime::parse_project(parse("{" + base + R"(, "version": "x"})"));
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().message.find("/version") != std::string::npos);
}

TEST_CASE("new projects are valid") {
    const json::Value project = runtime::new_project("My Game", "scenes/main.scene.json");
    CHECK(runtime::validate_project(project).empty());
    CHECK(runtime::validate_project(runtime::project_schema()).size() > 0);
}

TEST_CASE("game arguments") {
    auto options = runtime::parse_game_args({"--headless", "--ticks", "30", "--data", "game"});
    REQUIRE(options.ok());
    CHECK(options.value().headless);
    CHECK(options.value().max_ticks == 30);
    CHECK(options.value().data_dir == "game");

    // Launchers add their own single-dash arguments; those are ignored.
    CHECK(runtime::parse_game_args({"-NSDocumentRevisionsDebugMode", "YES"}).ok());
    CHECK_FALSE(runtime::parse_game_args({"--headless"}).ok());
    CHECK_FALSE(runtime::parse_game_args({"--ticks", "ten"}).ok());
    auto typo = runtime::parse_game_args({"--tick", "3"});
    REQUIRE_FALSE(typo.ok());
    CHECK(typo.error().message.find("did you mean \"--ticks\"") != std::string::npos);
}

TEST_CASE("a game loads its main scene and runs headless") {
    auto game = runtime::Game::start(
        {.data_dir = SLASH_TEST_DATA_DIR "/game", .headless = true, .max_ticks = 25});
    REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
    CHECK(game.value()->project().name == "Test Game");
    int frames = 0;
    while (game.value()->frame()) {
        ++frames;
    }
    CHECK(frames == 24);
    const json::Value summary = game.value()->summary();
    CHECK(summary.find("ticks")->as_int() == 25);
    CHECK(summary.find("entities")->as_int() == 3);
    CHECK(summary.find("scene")->as_string() == "scenes/main.scene.json");
    CHECK(summary.find("renderer")->as_string() == "null"); // headless
    CHECK(game.value()->renderer().stats().frames == 0);
    // spawn_point extends prefabs/marker.prefab.json.
    const auto& world = game.value()->world();
    CHECK(world.ecs().get<slash::world::Transform2D>(world.find("spawn_point")).position.y ==
          100.0F);
    CHECK_FALSE(summary.find("quit_requested")->as_bool());
    CHECK_FALSE(game.value()->frame());
}

TEST_CASE("a game starts with the project's V-Sync mode and frame cap") {
    const std::string root = "runtime_frame_settings";
    REQUIRE(platform::create_directories(root).ok());
    REQUIRE(
        platform::write_file(root + "/main.scene.json", R"({"entities": {}, "format": 1})").ok());
    REQUIRE(platform::write_file(root + "/project.json",
                                 R"({"format": 1, "name": "G", "main_scene": "main.scene.json",
                                     "window": {"vsync_mode": "disabled"}, "max_fps": 30})")
                .ok());
    auto game = runtime::Game::start({.data_dir = root, .headless = true, .max_ticks = 1});
    REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
    CHECK(game.value()->loop().max_fps() == 30);
    CHECK(game.value()->renderer().vsync() == slash::render::VsyncMode::Disabled);
}

TEST_CASE("a headless game plays animations and counts the sprites it would draw") {
    const std::string root = "runtime_sprites";
    REQUIRE(platform::create_directories(root).ok());
    slash::render::Image image;
    image.width = 4;
    image.height = 2;
    image.pixels.assign(4 * 2 * 4, 255);
    REQUIRE(
        platform::write_file(root + "/hero.png", slash::render::encode_png(image).value()).ok());
    REQUIRE(platform::write_file(root + "/hero.frames.json",
                                 R"({"format": 1, "animations": {"default": {
                                     "texture": "hero.png", "hframes": 2, "fps": 30}}})")
                .ok());
    REQUIRE(platform::write_file(root + "/main.scene.json", R"({"entities": {
        "hero": {"components": {"Transform2D": {}, "AnimatedSprite2D": {
            "sprite_frames": "hero.frames.json"}}},
        "rock": {"components": {"Transform2D": {}, "Sprite2D": {"texture": "hero.png"}}},
        "ghost": {"components": {"Transform2D": {}, "Sprite2D": {"texture": "gone.png"}}}
    }, "format": 1})")
                .ok());
    REQUIRE(platform::write_file(root + "/project.json",
                                 R"({"format": 1, "name": "G", "main_scene": "main.scene.json"})")
                .ok());
    auto game = runtime::Game::start({.data_dir = root, .headless = true, .max_ticks = 3});
    REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
    while (game.value()->frame()) {
    }
    // The hero and the rock; the ghost's texture is missing, so it is left out with a warning.
    CHECK(game.value()->summary().find("sprites")->as_int() == 2);
    // Three ticks of 1/60 s at 30 frames per second: frame 1, halfway through.
    const auto& world = game.value()->world();
    const entt::entity hero = world.find("hero");
    CHECK(world.ecs().get<slash::world::AnimatedSprite2D>(hero).frame == 1);
    CHECK(world.ecs().get<slash::world::Sprite2D>(hero).frame == 1);
}

TEST_CASE("a game reads SDL's events as the project's input actions") {
    const std::string root = "runtime_input";
    REQUIRE(platform::create_directories(root).ok());
    REQUIRE(
        platform::write_file(root + "/main.scene.json", R"({"entities": {}, "format": 1})").ok());
    REQUIRE(platform::write_file(root + "/project.json",
                                 R"({"format": 1, "name": "G", "main_scene": "main.scene.json",
                                     "input": {"jump": {"events": [
                                         {"type": "key", "key": "Space"}]}}})")
                .ok());
    auto game = runtime::Game::start({.data_dir = root, .headless = true, .max_ticks = 3});
    REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
    const auto& world = game.value()->world();

    // Events SDL hands to the app's event callback...
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_SPACE;
    key.key.scancode = SDL_SCANCODE_SPACE;
    key.key.down = true;
    game.value()->handle_event(key);
    CHECK(game.value()->frame());
    CHECK(slash::input::is_action_just_pressed(world, "jump"));
    CHECK(slash::input::is_action_pressed(world, "ui_accept"));

    // ...and those the loop drains itself.
    key.type = SDL_EVENT_KEY_UP;
    key.key.down = false;
    REQUIRE(SDL_PushEvent(&key));
    CHECK(game.value()->frame());
    CHECK(slash::input::is_action_just_released(world, "jump"));
}

TEST_CASE("a game explains what is missing") {
    auto missing =
        runtime::Game::start({.data_dir = "no_such_game", .headless = true, .max_ticks = 1});
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("slash-cli init") != std::string::npos);

    const std::string root = "runtime_test_game";
    REQUIRE(platform::create_directories(root).ok());
    REQUIRE(platform::write_file(root + "/project.json",
                                 R"({"format": 1, "name": "G", "main_scene": "gone.scene.json"})")
                .ok());
    auto no_scene = runtime::Game::start({.data_dir = root, .headless = true, .max_ticks = 1});
    REQUIRE_FALSE(no_scene.ok());
    CHECK(no_scene.error().message.find("gone.scene.json") != std::string::npos);
}

TEST_CASE("a loop can be driven one frame at a time") {
    auto created = platform::Platform::create({.headless = true});
    REQUIRE(created.ok());
    auto loop = platform::Loop::create(created.value(), {.tick_hz = 30.0, .max_ticks = 3});
    REQUIRE(loop.ok());
    int ticks = 0;
    CHECK(loop.value().frame([&](std::uint64_t, double) { ++ticks; }));
    CHECK(loop.value().frame([&](std::uint64_t, double) { ++ticks; }));
    CHECK_FALSE(loop.value().frame([&](std::uint64_t, double) { ++ticks; }));
    CHECK_FALSE(loop.value().frame([&](std::uint64_t, double) { ++ticks; }));
    CHECK(ticks == 3);
    CHECK(loop.value().finished());

    auto stopped = platform::Loop::create(created.value(), {.tick_hz = 30.0, .max_ticks = 100});
    REQUIRE(stopped.ok());
    stopped.value().request_quit();
    CHECK_FALSE(stopped.value().frame(nullptr));
    CHECK(stopped.value().stats().quit_requested);
    CHECK(stopped.value().stats().ticks == 0);
}

TEST_CASE("processes run with their output logged") {
    const std::string log = "process_test.log";
    std::remove(log.c_str());
    platform::ProcessOptions options;
    options.log_path = log;
    options.environment = {{"SLASH_TEST_VALUE", "hello"}};
    options.tail_lines = 2;
    auto result = platform::run_process({SLASH_TEST_CMAKE, "-E", "echo", "one"}, options);
    REQUIRE(result.ok());
    CHECK(result.value().exit_code == 0);
    CHECK(result.value().tail == "one");

    // The environment reaches the child, and nonzero exits are results, not errors.
    result =
        platform::run_process({SLASH_TEST_CMAKE, "-P", SLASH_TEST_DATA_DIR "/fail.cmake"}, options);
    REQUIRE(result.ok());
    CHECK(result.value().exit_code != 0);
    CHECK(result.value().tail.find("value is hello") != std::string::npos);
    auto logged = platform::read_file(log);
    REQUIRE(logged.ok());
    CHECK(logged.value().find("one") != std::string::npos);
    CHECK(logged.value().find("value is hello") != std::string::npos);

    CHECK_FALSE(platform::run_process({"slash-no-such-program-anywhere"}).ok());
    CHECK_FALSE(platform::run_process({}).ok());
}

TEST_CASE("environment variables are read") {
    CHECK(platform::environment_variable("SLASH_TEST_UNSET_VARIABLE").empty());
    REQUIRE(SDL_setenv_unsafe("SLASH_TEST_VARIABLE", "set", 1) == 0);
    CHECK(platform::environment_variable("SLASH_TEST_VARIABLE") == "set");
    REQUIRE(SDL_unsetenv_unsafe("SLASH_TEST_VARIABLE") == 0);
    CHECK(platform::environment_variable("SLASH_TEST_VARIABLE").empty());
}
