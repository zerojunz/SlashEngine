#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;
using slash::Vec2;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTick = 1.0F / 60.0F;
constexpr Vec2 kScreen{160.0F, 90.0F};

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

void check_near(Vec2 got, Vec2 want, float tolerance = 1e-3F) {
    CAPTURE(got.x);
    CAPTURE(got.y);
    CHECK(std::abs(got.x - want.x) <= tolerance);
    CHECK(std::abs(got.y - want.y) <= tolerance);
}

world::Transform2D at(Vec2 position, float rotation = 0.0F) {
    world::Transform2D transform;
    transform.position = position;
    transform.rotation = rotation;
    return transform;
}

// Where a world point shows on a 160x90 screen, through a camera with this state, its entity
// at the transform, moved on by delta seconds.
Vec2 shown(const world::Camera2D& camera, world::CameraState& state,
           const world::Transform2D& transform, Vec2 point, float delta = kTick) {
    return world::camera_transform(camera, state, transform, kScreen, delta).apply(point);
}

// Files in memory, for a world's assets.
struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

using Rgba = std::array<std::uint8_t, 4>;

// A size x size PNG of one color.
std::string square_png(Rgba color, std::uint32_t size) {
    render::Image image{.width = size, .height = size, .pixels = {}};
    for (std::uint32_t i = 0; i < size * size; ++i) {
        image.pixels.insert(image.pixels.end(), color.begin(), color.end());
    }
    return render::encode_png(image).value();
}

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

// A world with the built-in components and the canvas installed, on a 160x90 screen.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui = slash::ui::settings(world);
        ui.viewport = kScreen;
        slash::ui::set_settings(world, ui);
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value all = parse(components_json);
        for (const auto& [name, fields] : all.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), name << path << ": "
                                                << (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    world::Transform2D& transform(std::string_view id) {
        return world.ecs().get<world::Transform2D>(world.find(id));
    }
};

// A 160x90 project with a 4x4 white texture, and a session over it.
struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string folder) : root(std::move(folder)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1,
            "name": "Camera", "main_scene": "main.scene.json", "version": "1.0.0",
            "window": {"width": 160, "height": 90}})")
                    .ok());
        REQUIRE(
            slash::platform::write_file(root + "/white.png", square_png({255, 255, 255, 255}, 4))
                .ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }
    ~Project() { std::filesystem::remove_all(root); }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;

    json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

} // namespace

TEST_CASE("camera_transform shows the world as Godot's Camera2D does") {
    world::Camera2D camera;
    world::CameraState state;
    // The middle of the screen is at the camera, from its first update.
    check_near(shown(camera, state, at({500, 300}), {500, 300}), {80, 45});
    check_near(shown(camera, state, at({500, 300}), {510, 290}), {90, 35});
    CHECK_FALSE(state.first);

    // fixed_top_left puts the screen's top left corner there instead.
    camera.anchor_mode = "fixed_top_left";
    state = {};
    check_near(shown(camera, state, at({500, 300}), {500, 300}), {0, 0});

    // Zoom 2 shows things twice as big around the middle; 0 counts as 1.
    camera = {};
    camera.zoom = {2.0F, 2.0F};
    state = {};
    check_near(shown(camera, state, at({500, 300}), {510, 300}), {100, 45});
    camera.zoom = {0.0F, 0.5F};
    state = {};
    check_near(shown(camera, state, at({500, 300}), {510, 310}), {90, 50});

    // The offset moves the view.
    camera = {};
    camera.offset = {10.0F, -5.0F};
    state = {};
    check_near(shown(camera, state, at({500, 300}), {500, 300}), {70, 50});

    // The view stays upright unless it turns with the camera: turned a quarter clockwise, what
    // is below the camera shows to its right.
    camera = {};
    state = {};
    check_near(shown(camera, state, at({500, 300}, kPi / 2.0F), {500, 310}), {80, 55});
    camera.ignore_rotation = false;
    state = {};
    check_near(shown(camera, state, at({500, 300}, kPi / 2.0F), {500, 300}), {80, 45});
    check_near(shown(camera, state, at({500, 300}, kPi / 2.0F), {500, 310}), {90, 45});
}

TEST_CASE("limits keep the view inside them, and center it between limits narrower than it") {
    world::Camera2D camera;
    camera.limit_left = 0;
    camera.limit_top = 0;
    camera.limit_right = 1000;
    camera.limit_bottom = 200;
    world::CameraState state;
    // Near the top left corner, the corner of the world is the corner of the screen.
    check_near(shown(camera, state, at({10, 10}), {0, 0}), {0, 0});
    // Near the bottom right one, likewise.
    check_near(shown(camera, state, at({995, 195}), {1000, 200}), {160, 90});
    // In the middle the camera is free.
    check_near(shown(camera, state, at({500, 100}), {500, 100}), {80, 45});
    // A fixed_top_left screen stays inside too.
    camera.anchor_mode = "fixed_top_left";
    check_near(shown(camera, state, at({995, 195}), {1000, 200}), {160, 90});
    camera.anchor_mode = "drag_center";

    // Limits narrower than the screen center it between them, as Godot 4.5 does.
    camera.limit_right = 100;
    check_near(shown(camera, state, at({500, 100}), {50, 100}), {80, 45});

    // limit_enabled off lets the camera go anywhere.
    camera.limit_enabled = false;
    check_near(shown(camera, state, at({10, 10}), {10, 10}), {80, 45});
}

TEST_CASE("position smoothing closes a share of the gap each tick") {
    world::Camera2D camera;
    camera.position_smoothing_enabled = true;
    world::CameraState state;
    check_near(shown(camera, state, at({500, 300}), {500, 300}), {80, 45});
    // Godot's smoothing: speed times the tick's seconds of the way, 5 / 60 of 120 pixels.
    check_near(shown(camera, state, at({620, 300}), {510, 300}), {80, 45});
    // No time leaves it where it is.
    check_near(shown(camera, state, at({620, 300}), {510, 300}, 0.0F), {80, 45});
    check_near(shown(camera, state, at({620, 300}), {519.1667F, 300}), {80, 45});
    // reset_camera_smoothing's way: the smoothed position catches up.
    state.smoothed_camera_pos = state.camera_pos;
    check_near(shown(camera, state, at({620, 300}), {620, 300}), {80, 45});

    // With limit_smoothed, the limits push the camera before smoothing, so it glides to them.
    camera.limit_right = 600;
    camera.limit_smoothed = true;
    state = {};
    check_near(shown(camera, state, at({500, 300}), {500, 300}), {80, 45});
    // The camera wants 620, the limit holds it at 520, and it glides 5 / 60 of the way there.
    check_near(shown(camera, state, at({620, 300}), {501.6667F, 300}), {80, 45});
    CHECK(state.camera_pos.x == doctest::Approx(520.0F));
}

TEST_CASE("drag margins let the entity move before the camera follows") {
    world::Camera2D camera;
    camera.drag_horizontal_enabled = true;
    world::CameraState state;
    check_near(shown(camera, state, at({500, 300}), {500, 300}), {80, 45});
    // Within 16 pixels (0.2 of half the screen) the camera stays.
    check_near(shown(camera, state, at({510, 300}), {500, 300}), {80, 45});
    // Past it, the camera is pulled along at the margin.
    check_near(shown(camera, state, at({530, 300}), {514, 300}), {80, 45});
    check_near(shown(camera, state, at({500, 300}), {514, 300}), {80, 45});
    // Up and down it follows at once.
    check_near(shown(camera, state, at({500, 320}), {514, 320}), {80, 45});
    // A new drag offset moves the camera to it at once: right by the whole left margin.
    camera.drag_horizontal_offset = 1.0F;
    check_near(shown(camera, state, at({500, 320}), {516, 320}), {80, 45});
    check_near(shown(camera, state, at({500, 320}), {516, 320}), {80, 45});
}

TEST_CASE("rotation smoothing turns the view a share of the way each tick") {
    world::Camera2D camera;
    camera.ignore_rotation = false;
    camera.rotation_smoothing_enabled = true;
    world::CameraState state;
    (void)world::camera_transform(camera, state, at({0, 0}, 1.0F), kScreen, kTick);
    CHECK(state.camera_angle == doctest::Approx(5.0F / 60.0F));
    // Godot's lerp_angle turns the short way round.
    state.camera_angle = 3.0F;
    (void)world::camera_transform(camera, state, at({0, 0}, -3.0F), kScreen, 0.1F);
    CHECK(state.camera_angle == doctest::Approx(3.0F + (((2.0F * kPi) - 6.0F) * 0.5F)));
}

TEST_CASE("the first enabled Camera2D in drawing order is current, and the system moves it on") {
    Scene scene;
    CHECK((world::current_camera(scene.world, world::Tree2D(scene.world)) == entt::null));
    const entt::entity a = scene.create("a", R"({"Transform2D": {"position": [100, 100]},
                                                 "Camera2D": {"position_smoothing_enabled": true}})");
    const entt::entity b = scene.create("b", R"({"Transform2D": {"position": [300, 300]},
                                                 "Camera2D": {}})");
    // Without a Transform2D, a Camera2D is nowhere.
    scene.create("loose", R"({"Camera2D": {}})");
    const auto current = [&] {
        return world::current_camera(scene.world, world::Tree2D(scene.world));
    };
    CHECK((current() == a));
    scene.world.ecs().get<world::Camera2D>(a).enabled = false;
    CHECK((current() == b));
    // A child of a draws before b.
    const entt::entity c = scene.create("c", R"({"Transform2D": {"position": [10, 0]},
                                                 "Camera2D": {}})");
    scene.transform("a").children = {c};
    CHECK((current() == c));
    // The view is the current camera's, its child's global position at the middle.
    const world::CameraView view =
        world::camera_view(scene.world, world::Tree2D(scene.world), kScreen);
    CHECK((view.camera == c));
    check_near(view.canvas.apply({110, 100}), {80, 45});

    // Each tick the system moves only the current camera's smoothing on.
    scene.transform("a").children = {};
    scene.world.ecs().get<world::Camera2D>(a).enabled = true;
    scene.world.step(kTick);
    scene.transform("a").position = {220, 100};
    scene.transform("b").position = {420, 300};
    // Drawing between ticks doesn't move smoothing on.
    check_near(world::camera_view(scene.world, world::Tree2D(scene.world), kScreen)
                   .canvas.apply({100, 100}),
               {80, 45});
    scene.world.step(kTick);
    check_near(world::camera_view(scene.world, world::Tree2D(scene.world), kScreen)
                   .canvas.apply({110, 100}),
               {80, 45});
    // reset_camera_smoothing catches it up.
    world::reset_camera_smoothing(scene.world, a);
    check_near(world::camera_view(scene.world, world::Tree2D(scene.world), kScreen)
                   .canvas.apply({220, 100}),
               {80, 45});
    // A camera that was never current starts where its entity is.
    scene.world.ecs().get<world::Camera2D>(a).enabled = false;
    check_near(world::camera_view(scene.world, world::Tree2D(scene.world), kScreen)
                   .canvas.apply({420, 300}),
               {80, 45});
    // reset_state forgets where smoothing had brought a camera.
    scene.world.ecs().get<world::Camera2D>(a).enabled = true;
    scene.world.reset_state();
    check_near(world::camera_view(scene.world, world::Tree2D(scene.world), kScreen)
                   .canvas.apply({220, 100}),
               {80, 45});
}

TEST_CASE("frame.summary shows the world through the camera, with controls where they were") {
    const Project project("camera_summary_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "rock", "components": {
            "Transform2D": {"position": [1000, 500], "scale": [4, 4]},
            "Sprite2D": {"texture": "white.png"}}}},
        {"command": "entity.create", "params": {"id": "bar", "components": {
            "Control": {"offset_right": 40, "offset_bottom": 10},
            "ColorRect": {"color": [0, 0, 1, 1]}}}},
        {"command": "entity.create", "params": {"id": "cam", "components": {
            "Transform2D": {"position": [1000, 500]}, "Camera2D": {}}}}]})");
    CHECK(compact(project.ok("frame.summary")) == compact(parse(R"({"viewport": [160, 90],
        "camera": {"id": "cam", "rect": [920, 455, 160, 90]},
        "drawn": [{"id": "rock", "kind": "Sprite2D", "rect": [72, 37, 16, 16]},
                  {"id": "bar", "kind": "ColorRect", "rect": [0, 0, 40, 10]}]})")));

    // Zoomed in twice and moved, as an agent's edit shows at once.
    project.ok("component.set", R"({"id": "cam", "components": {
        "Transform2D": {"position": [1004, 500]}, "Camera2D": {"zoom": [2, 2]}}})");
    CHECK(compact(project.ok("frame.summary")) == compact(parse(R"({"viewport": [160, 90],
        "camera": {"id": "cam", "rect": [964, 477.5, 80, 45]},
        "drawn": [{"id": "rock", "kind": "Sprite2D", "rect": [56, 29, 32, 32]},
                  {"id": "bar", "kind": "ColorRect", "rect": [0, 0, 40, 10]}]})")));

    // With no camera, the window shows the world from [0, 0].
    project.ok("component.set", R"({"id": "cam", "components": {"Camera2D": {"enabled": false}}})");
    CHECK(compact(project.ok("frame.summary")) == compact(parse(R"({"viewport": [160, 90],
        "drawn": [{"id": "bar", "kind": "ColorRect", "rect": [0, 0, 40, 10]}],
        "offscreen": ["rock"]})")));

    // A smoothed camera glides after its entity over the ticks.
    project.ok("component.set", R"({"id": "cam", "components": {
        "Transform2D": {"position": [1000, 500]},
        "Camera2D": {"enabled": true, "zoom": [1, 1], "position_smoothing_enabled": true}}})");
    project.ok("step", R"({"ticks": 1})");
    project.ok("component.set",
               R"({"id": "cam", "components": {"Transform2D": {"position": [1060, 500]}}})");
    project.ok("step", R"({"ticks": 1})");
    CHECK(compact(project.ok("frame.summary", R"({"ids": ["rock"]})")) ==
          compact(parse(R"({"viewport": [160, 90],
        "camera": {"id": "cam", "rect": [925, 455, 160, 90]},
        "drawn": [{"id": "rock", "kind": "Sprite2D", "rect": [67, 37, 16, 16]}]})")));
}

TEST_CASE("capture draws the world through the camera, lit where its lights are") {
    Scene scene;
    canvas::Settings settings;
    settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 1.0F, .a = 1.0F};
    settings.texture_filter = render::Filter::Nearest;
    canvas::set_settings(scene.world, settings);
    scene.files.files["red.png"] = square_png({255, 0, 0, 255}, 8);
    scene.files.files["light.png"] = square_png({255, 255, 255, 255}, 8);
    // Two red sprites far from the origin, the first under a light, in the dark.
    scene.create("lit", R"({"Transform2D": {"position": [1000, 1000]},
                            "Sprite2D": {"texture": "red.png"}})");
    scene.create("dark", R"({"Transform2D": {"position": [1020, 1000]},
                             "Sprite2D": {"texture": "red.png"}})");
    scene.create("lamp", R"({"Transform2D": {"position": [1000, 1000]},
                             "PointLight2D": {"texture": "light.png"}})");
    scene.create("night", R"({"CanvasModulate": {"color": [0, 0, 0, 1]}})");
    scene.create("cam", R"({"Transform2D": {"position": [1000, 1000]}, "Camera2D": {}})");
    auto captured = canvas::capture(scene.world, scene.assets);
    REQUIRE(captured.ok());
    const render::Image& image = captured.value().image;
    CHECK(pixel(image, 80, 45) == Rgba{255, 0, 0, 255});
    CHECK(pixel(image, 100, 45) == Rgba{0, 0, 0, 255});
    CHECK(pixel(image, 80, 60) == Rgba{0, 0, 255, 255});

    // Collision shapes draw through the camera too.
    scene.create("box", R"({"Transform2D": {"position": [960, 990]},
                            "StaticBody2D": {}, "CollisionShape2D": {"size": [8, 8]}})");
    auto shapes = canvas::capture(scene.world, scene.assets, {.collisions = true});
    REQUIRE(shapes.ok());
    CHECK(shapes.value().shapes == 1);
    CHECK(pixel(shapes.value().image, 40, 35) != Rgba{0, 0, 255, 255});
    CHECK(pixel(shapes.value().image, 40, 25) == Rgba{0, 0, 255, 255});
}

TEST_CASE("frames between ticks draw the camera's view part of the way") {
    Scene scene;
    scene.files.files["white.png"] = square_png({255, 255, 255, 255}, 4);
    scene.create("rock", R"({"Transform2D": {"position": [1000, 500]},
                             "Sprite2D": {"texture": "white.png"}})");
    scene.create("cam", R"({"Transform2D": {"position": [1000, 500]}, "Camera2D": {}})");
    auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    canvas::FrameContents contents;
    canvas::CanvasOptions options;
    options.contents = &contents;
    renderer.before_tick(scene.world);
    scene.transform("cam").position = {1020, 500};
    scene.world.step(kTick);
    // Halfway, the camera is at 1010, so the rock is 10 pixels left of the middle.
    options.interpolation = 0.5;
    REQUIRE(renderer.draw(scene.world, options).ok());
    REQUIRE(contents.items.size() == 1);
    CHECK(contents.items[0].pieces[0].vertices[0].x == doctest::Approx(68.0F));
    CHECK((contents.camera.camera == scene.world.find("cam")));
    options.interpolation = 1.0;
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(contents.items[0].pieces[0].vertices[0].x == doctest::Approx(58.0F));
}
