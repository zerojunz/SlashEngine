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
#include "slash/world/canvas_layer.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
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

constexpr Rgba kClear{0, 0, 0, 255};
constexpr Rgba kRed{255, 0, 0, 255};
constexpr Rgba kGreen{0, 255, 0, 255};
constexpr Rgba kBlue{0, 0, 255, 255};
constexpr Rgba kWhite{255, 255, 255, 255};

// A world with the built-in components and the canvas installed, on a 160x90 screen, with
// 4x4 red, green, blue and white textures.
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
        canvas::Settings settings;
        settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        settings.texture_filter = render::Filter::Nearest;
        canvas::set_settings(world, settings);
        files.files["red.png"] = square_png(kRed, 4);
        files.files["green.png"] = square_png(kGreen, 4);
        files.files["blue.png"] = square_png(kBlue, 4);
        files.files["white.png"] = square_png(kWhite, 4);
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

    template <typename T>
    T& get(std::string_view id) {
        return world.ecs().get<T>(world.find(id));
    }

    [[nodiscard]] world::Canvases canvases() const {
        const world::Tree2D tree(world);
        return world::canvases(world, tree, kScreen);
    }

    render::Image capture() {
        auto captured = canvas::capture(world, assets);
        REQUIRE_MESSAGE(captured.ok(), (captured.ok() ? "" : captured.error().message));
        return std::move(captured).value().image;
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
            "name": "Layers", "main_scene": "main.scene.json", "version": "1.0.0",
            "window": {"width": 160, "height": 90}})")
                    .ok());
        REQUIRE(slash::platform::write_file(root + "/white.png", square_png(kWhite, 4)).ok());
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

    std::string refused(std::string_view name, std::string_view params) const {
        const command::Reply reply = session->call(name, parse(params));
        CHECK_FALSE(reply.ok);
        return reply.message;
    }
};

} // namespace

TEST_CASE("canvases stack as Godot's: by layer, the world first among layer 0") {
    Scene scene;
    // With no CanvasLayer, there is only the world.
    CHECK(scene.canvases().all().size() == 1);
    CHECK_FALSE(scene.canvases().layered());

    const entt::entity top = scene.create("top", R"({"CanvasLayer": {"layer": 2}})");
    const entt::entity hud = scene.create("hud", R"({"CanvasLayer": {}})");
    const entt::entity back = scene.create("back", R"({"CanvasLayer": {"layer": -1}})");
    const entt::entity same = scene.create("same", R"({"CanvasLayer": {"layer": 0}})");
    const entt::entity menu = scene.create("menu", R"({"CanvasLayer": {}})");
    const world::Canvases canvases = scene.canvases();
    std::vector<entt::entity> order;
    for (const world::Canvas& each : canvases.all()) {
        order.push_back(each.layer);
    }
    // Layers of one layer in the order they were made.
    CHECK(order == std::vector<entt::entity>{back, entt::null, same, hud, menu, top});
    CHECK(canvases.world_canvas() == 1);
    CHECK(canvases.all()[3].order == 1);
    // Holding nothing, they leave everything in the world.
    CHECK_FALSE(canvases.layered());

    // What a layer holds draws in it, with its Transform2D and Control children.
    scene.create("ship", R"({"Transform2D": {}})");
    scene.create("gun", R"({"Transform2D": {}})");
    scene.create("panel", R"({"Control": {}})");
    scene.create("label", R"({"Control": {}, "Label": {}})");
    scene.create("rock", R"({"Transform2D": {}})");
    scene.get<world::Transform2D>("ship").children = {scene.world.find("gun")};
    scene.get<world::Control>("panel").children = {scene.world.find("label")};
    scene.get<world::CanvasLayer>("hud").children = {scene.world.find("ship"),
                                                     scene.world.find("panel")};
    // An entity two layers hold draws in the first made.
    scene.get<world::CanvasLayer>("top").children = {scene.world.find("panel")};
    const world::Canvases held = scene.canvases();
    CHECK(held.layered());
    CHECK(held.of(scene.world.find("ship")) == 3);
    CHECK(held.of(scene.world.find("gun")) == 3);
    CHECK(held.of(scene.world.find("panel")) == 5);
    CHECK(held.of(scene.world.find("label")) == 5);
    CHECK(held.of(scene.world.find("rock")) == 1);
    CHECK(held.of(entt::null) == 1);

    // A loop of Control children ends.
    scene.get<world::Control>("label").children = {scene.world.find("panel")};
    scene.get<world::CanvasLayer>("top").children = {};
    scene.get<world::CanvasLayer>("hud").children = {scene.world.find("ship")};
    CHECK(scene.canvases().of(scene.world.find("label")) == 1);
}

TEST_CASE("canvas_layer_transform places a layer as Godot's CanvasLayer, following the camera") {
    world::CanvasLayer layer;
    layer.offset = {10.0F, 20.0F};
    layer.rotation = kPi / 2.0F;
    layer.scale = {2.0F, 2.0F};
    // A camera at (1000, 500), which shows it in the middle of the screen.
    world::Affine2D camera;
    camera.origin = {-920.0F, -455.0F};
    // Offset, rotation and scale, apart from the camera.
    const world::Affine2D own = world::canvas_layer_transform(layer, camera, kScreen);
    check_near(own.apply({0.0F, 0.0F}), {10.0F, 20.0F});
    check_near(own.apply({1.0F, 0.0F}), {10.0F, 22.0F});

    // Following the viewport, after the camera.
    layer = {};
    layer.offset = {10.0F, 0.0F};
    layer.follow_viewport_enabled = true;
    check_near(world::canvas_layer_transform(layer, camera, kScreen).apply({1000.0F, 500.0F}),
               {90.0F, 45.0F});
    // Then scaled around the middle of the screen.
    layer.follow_viewport_scale = 0.5F;
    check_near(world::canvas_layer_transform(layer, camera, kScreen).apply({1000.0F, 500.0F}),
               {85.0F, 45.0F});
    check_near(world::canvas_layer_transform(layer, camera, kScreen).apply({0.0F, 0.0F}),
               {-415.0F, -205.0F});
}

TEST_CASE("Parallax2D scrolls as Godot's _update_scroll") {
    world::Parallax2D parallax;
    parallax.scroll_scale = {0.5F, 1.0F};
    parallax.screen_offset = {100.0F, 0.0F};
    // Half the camera's scroll: 50 pixels behind where the camera moved to.
    check_near(world::parallax_position(parallax, {1.0F, 1.0F}, kScreen), {50.0F, 0.0F});
    parallax.scroll_offset = {7.0F, 3.0F};
    check_near(world::parallax_position(parallax, {1.0F, 1.0F}, kScreen), {57.0F, 3.0F});

    // A repeat wraps the scroll inside one repeat of its size, times the entity's scale.
    parallax.scroll_offset = {};
    parallax.repeat_size = {64.0F, 0.0F};
    parallax.screen_offset = {200.0F, 0.0F};
    check_near(world::parallax_position(parallax, {1.0F, 1.0F}, kScreen), {164.0F, 0.0F});
    check_near(world::parallax_position(parallax, {2.0F, 1.0F}, kScreen), {100.0F, 0.0F});

    // Limits keep the scroll inside them, less a screen.
    parallax = {};
    parallax.scroll_scale = {0.5F, 1.0F};
    parallax.limit_begin = {0.0F, -10000000.0F};
    parallax.limit_end = {200.0F, 10000000.0F};
    parallax.screen_offset = {100.0F, 0.0F};
    check_near(world::parallax_position(parallax, {1.0F, 1.0F}, kScreen), {80.0F, 0.0F});

    // Not following the viewport, it is placed on the screen instead of in the world.
    parallax = {};
    parallax.scroll_scale = {0.5F, 1.0F};
    parallax.screen_offset = {100.0F, 0.0F};
    parallax.follow_viewport = false;
    check_near(world::parallax_position(parallax, {1.0F, 1.0F}, kScreen), {-50.0F, 0.0F});
}

TEST_CASE("the parallax_2d system scrolls with the current camera and autoscrolls repeats") {
    Scene scene;
    scene.create("cam", R"({"Transform2D": {"position": [1000, 500]}, "Camera2D": {}})");
    scene.create("hills", R"({"Transform2D": {}, "Parallax2D": {"scroll_scale": [0.5, 1]}})");
    scene.create("clouds", R"({"Transform2D": {}, "Parallax2D": {"repeat_size": [64, 0],
                                                                 "autoscroll": [60, 0]}})");
    scene.create("still", R"({"Transform2D": {}, "Parallax2D": {"autoscroll": [60, 0],
                                                                "ignore_camera_scroll": true}})");
    scene.world.step(kTick);
    // The camera shows the world from (920, 455).
    check_near(scene.get<world::Parallax2D>("hills").screen_offset, {920.0F, 455.0F});
    check_near(scene.get<world::Transform2D>("hills").position, {460.0F, 0.0F});
    // Autoscroll moves a repeat's offset on, wrapped inside the repeat.
    check_near(scene.get<world::Parallax2D>("clouds").scroll_offset, {1.0F, 0.0F});
    check_near(scene.get<world::Transform2D>("clouds").position,
               {920.0F - std::fmod(920.0F - 1.0F, 64.0F), 0.0F});
    // Without a repeat it doesn't, and one that ignores the camera keeps its screen_offset.
    check_near(scene.get<world::Parallax2D>("still").scroll_offset, {0.0F, 0.0F});
    check_near(scene.get<world::Parallax2D>("still").screen_offset, {0.0F, 0.0F});
    for (int i = 0; i < 64; ++i) {
        scene.world.step(kTick);
    }
    check_near(scene.get<world::Parallax2D>("clouds").scroll_offset, {1.0F, 0.0F}, 1e-2F);
}

TEST_CASE("canvas layers draw in their order, over the camera's view") {
    Scene scene;
    // A red sprite in the middle of the world's view, a blue one under it in layer -1, a green
    // one at (20, 20) on the screen in a HUD.
    scene.create("cam", R"({"Transform2D": {"position": [1000, 1000]}, "Camera2D": {}})");
    scene.create("rock", R"({"Transform2D": {"position": [1000, 1000]},
                             "Sprite2D": {"texture": "red.png"}})");
    scene.create("sky", R"({"Transform2D": {"position": [80, 45], "scale": [4, 4]},
                            "Sprite2D": {"texture": "blue.png"}})");
    scene.create("heart", R"({"Transform2D": {"position": [20, 20]},
                              "Sprite2D": {"texture": "green.png"}})");
    scene.create("back", R"({"CanvasLayer": {"layer": -1, "children": ["sky"]}})");
    scene.create("hud", R"({"CanvasLayer": {"children": ["heart"]}})");
    render::Image image = scene.capture();
    CHECK(pixel(image, 80, 45) == kRed);
    CHECK(pixel(image, 85, 45) == kBlue);
    CHECK(pixel(image, 20, 20) == kGreen);
    CHECK(pixel(image, 40, 20) == kClear);

    // A CanvasModulate darkens only the world's canvas.
    scene.create("night", R"({"CanvasModulate": {"color": [0, 0, 0, 1]}})");
    image = scene.capture();
    CHECK(pixel(image, 80, 45) == kClear);
    CHECK(pixel(image, 85, 45) == kBlue);
    CHECK(pixel(image, 20, 20) == kGreen);
    scene.world.destroy(scene.world.find("night"));

    // The HUD's offset and scale move what it holds; an invisible layer draws nothing.
    scene.get<world::CanvasLayer>("hud").offset = {10.0F, 0.0F};
    scene.get<world::CanvasLayer>("hud").scale = {2.0F, 2.0F};
    scene.get<world::CanvasLayer>("back").visible = false;
    image = scene.capture();
    CHECK(pixel(image, 50, 40) == kGreen);
    CHECK(pixel(image, 20, 20) == kClear);
    CHECK(pixel(image, 85, 45) == kClear);

    // Following the viewport, the HUD moves with the camera.
    scene.get<world::CanvasLayer>("hud").offset = {};
    scene.get<world::CanvasLayer>("hud").scale = {1.0F, 1.0F};
    scene.get<world::CanvasLayer>("hud").follow_viewport_enabled = true;
    scene.get<world::Transform2D>("heart").position = {1010.0F, 1000.0F};
    image = scene.capture();
    CHECK(pixel(image, 90, 45) == kGreen);
}

TEST_CASE("lights shade the canvases their layer range holds, as Godot's") {
    Scene scene;
    // The camera puts world (1000, 1000) in the middle of the screen. White ground fills the
    // screen in a dark world; a HUD in layer 1, moved 10 pixels right, is dark too and holds a
    // badge at (20, 20), a chip at (60, 20) and a lamp, an 8x8 square of light, over the chip.
    scene.create("cam", R"({"Transform2D": {"position": [1000, 1000]}, "Camera2D": {}})");
    scene.create("night", R"({"CanvasModulate": {"color": [0, 0, 0, 1]}})");
    scene.create("ground", R"({"Transform2D": {"position": [1000, 1000], "scale": [40, 22.5]},
                               "Sprite2D": {"texture": "white.png"}})");
    scene.create("hud_night", R"({"CanvasModulate": {"color": [0, 0, 0, 1]}})");
    scene.create("badge", R"({"Transform2D": {"position": [20, 20]},
                              "Sprite2D": {"texture": "white.png"}})");
    scene.create("chip", R"({"Transform2D": {"position": [60, 20]},
                             "Sprite2D": {"texture": "white.png"}})");
    scene.create("lamp", R"({"Transform2D": {"position": [60, 20]},
                             "PointLight2D": {"texture": "white.png", "texture_scale": 2,
                                              "range_layer_min": 1, "range_layer_max": 1}})");
    scene.create("hud", R"({"CanvasLayer": {"offset": [10, 0],
                            "children": ["hud_night", "badge", "chip", "lamp"]}})");

    // Each canvas has its own CanvasModulate, and the lamp lights only layer 1: the chip under
    // it, not the ground around it.
    render::Image image = scene.capture();
    CHECK(pixel(image, 30, 20) == kClear); // the badge
    CHECK(pixel(image, 70, 20) == kWhite); // the chip
    CHECK(pixel(image, 73, 17) == kClear); // the ground in the lamp's light
    CHECK(pixel(image, 120, 60) == kClear);

    // Reaching layer 0 too, it lights the world where its canvas puts it on the screen.
    scene.get<world::PointLight2D>("lamp").range_layer_min = 0;
    image = scene.capture();
    CHECK(pixel(image, 73, 17) == kWhite);
    CHECK(pixel(image, 76, 17) == kClear); // past its light

    // A torch in the world lights layer 0 by default, so not the HUD, until its range takes 1.
    scene.create("torch", R"({"Transform2D": {"position": [1000, 1000]},
                              "PointLight2D": {"texture": "white.png", "texture_scale": 100}})");
    image = scene.capture();
    CHECK(pixel(image, 120, 60) == kWhite);
    CHECK(pixel(image, 30, 20) == kClear);
    scene.get<world::PointLight2D>("torch").range_layer_max = 1;
    image = scene.capture();
    CHECK(pixel(image, 30, 20) == kWhite);

    // A hidden layer's lights go out with it.
    scene.get<world::PointLight2D>("torch").enabled = false;
    scene.get<world::CanvasLayer>("hud").visible = false;
    image = scene.capture();
    CHECK(pixel(image, 73, 17) == kClear);

    // collect_lighting says how each canvas is lit, and places the lamp through its canvas.
    scene.get<world::CanvasLayer>("hud").visible = true;
    std::vector<canvas::LightProblem> problems;
    const world::Canvases canvases = scene.canvases();
    const canvas::SceneLighting lighting = canvas::collect_lighting(
        scene.world,
        [](const std::string&) -> slash::Result<const render::Image*> {
            static const render::Image white{
                .width = 4, .height = 4, .pixels = std::vector<std::uint8_t>(64, 255)};
            return &white;
        },
        {}, problems, nullptr, &canvases);
    REQUIRE(lighting.canvases.size() == 2);
    const canvas::CanvasLighting& world_lit = lighting.canvases[canvases.world_canvas()];
    const canvas::CanvasLighting& hud_lit = lighting.canvases[1 - canvases.world_canvas()];
    CHECK(world_lit.lit);
    CHECK(world_lit.layer == 0);
    CHECK_FALSE(world_lit.to_lights);
    CHECK(hud_lit.layer == 1);
    CHECK(hud_lit.ambient == std::array{0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(hud_lit.to_lights);
    // HUD (60, 20) is screen (70, 20), world (990, 975).
    const auto& m = *hud_lit.to_lights;
    CHECK((m[0] * 60.0F) + (m[1] * 20.0F) + m[2] == doctest::Approx(990.0F));
    CHECK((m[3] * 60.0F) + (m[4] * 20.0F) + m[5] == doctest::Approx(975.0F));
    REQUIRE(lighting.lights.size() == 1);
    CHECK(lighting.lights[0].light.position[0] == doctest::Approx(990.0F));
    CHECK(lighting.lights[0].light.position[1] == doctest::Approx(975.0F));
    CHECK(lighting.lights[0].light.layer_min == 0);
    CHECK(lighting.lights[0].light.layer_max == 1);
    CHECK(problems.empty());
}

TEST_CASE("light.sample samples an entity in its canvas") {
    const Project project("light_sample_layer_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "cam", "components": {
            "Transform2D": {"position": [1000, 1000]}, "Camera2D": {}}}},
        {"command": "entity.create", "params": {"id": "sun", "components": {
            "DirectionalLight2D": {"color": [0.5, 0.5, 0.5, 1], "range_layer_max": 2}}}},
        {"command": "entity.create", "params": {"id": "dusk", "components": {
            "CanvasModulate": {"color": [0.25, 0.25, 0.25, 1]}}}},
        {"command": "entity.create", "params": {"id": "badge", "components": {
            "Transform2D": {"position": [20, 20]}, "Sprite2D": {"texture": "white.png"}}}},
        {"command": "entity.create", "params": {"id": "hud", "components": {
            "CanvasLayer": {"layer": 2, "children": ["badge"]}}}}]})");
    // The world: the sun over the dusk. The badge's canvas has no CanvasModulate.
    CHECK(project.ok("light.sample", R"({"points": [[0, 0]]})") ==
          parse(R"({"lit": true, "ambient": [0.25, 0.25, 0.25], "points": [
              {"color": [0.75, 0.75, 0.75], "lights": [{"id": "sun", "color": [0.5, 0.5, 0.5]}]}
          ]})"));
    CHECK(project.ok("light.sample", R"({"id": "badge"})") ==
          parse(R"({"lit": true, "layer": "hud", "ambient": [1, 1, 1], "points": [
              {"color": [1.5, 1.5, 1.5], "lights": [{"id": "sun", "color": [0.5, 0.5, 0.5]}]}
          ]})"));
    // Out of the sun's range, the badge's canvas isn't lit at all.
    project.ok("component.set",
               R"({"id": "sun", "components": {"DirectionalLight2D": {"range_layer_max": 1}}})");
    CHECK(project.ok("light.sample", R"({"id": "badge"})") ==
          parse(R"({"lit": false, "layer": "hud", "ambient": [1, 1, 1],
                    "points": [{"color": [1, 1, 1], "lights": []}]})"));
}

TEST_CASE("a Parallax2D's repeat draws its children side by side, as Godot's") {
    Scene scene;
    // A camera with the world's origin at the screen's top left.
    scene.create("cam", R"({"Transform2D": {"position": [80, 45]}, "Camera2D": {}})");
    scene.create("tile", R"({"Transform2D": {"position": [10, 20]},
                             "Sprite2D": {"texture": "white.png"}})");
    scene.create("row", R"({"Transform2D": {"children": ["tile"]},
                            "Parallax2D": {"repeat_size": [40, 0]}})");
    scene.world.step(kTick);
    render::Image image = scene.capture();
    CHECK(pixel(image, 10, 20) == kWhite);
    CHECK(pixel(image, 50, 20) == kWhite);
    CHECK(pixel(image, 30, 20) == kClear);
    CHECK(pixel(image, 90, 20) == kClear);

    // repeat_times 3 adds one on each side.
    scene.get<world::Parallax2D>("row").repeat_times = 3;
    image = scene.capture();
    CHECK(pixel(image, 50, 20) == kWhite);
    CHECK(pixel(image, 90, 20) == kWhite);
    CHECK(pixel(image, 130, 20) == kClear);

    // frame.summary's contents count the copies as one sprite, drawn four times over.
    auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    canvas::FrameContents contents;
    canvas::CanvasOptions options;
    options.contents = &contents;
    REQUIRE(renderer.draw(scene.world, options).ok());
    REQUIRE(contents.items.size() == 1);
    CHECK(contents.items[0].count == 1);
    REQUIRE(contents.items[0].pieces.size() == 1);
    CHECK(contents.items[0].pieces[0].vertices.size() == 24);
}

TEST_CASE("a repeat that wraps between ticks isn't drawn sliding back across the screen") {
    Scene scene;
    scene.create("cam", R"({"Transform2D": {"position": [80, 45]}, "Camera2D": {}})");
    scene.create("tile", R"({"Transform2D": {"position": [10, 20]},
                             "Sprite2D": {"texture": "white.png"}})");
    scene.create("row", R"({"Transform2D": {"children": ["tile"]},
                            "Parallax2D": {"repeat_size": [40, 0], "autoscroll": [600, 0]}})");
    auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    canvas::FrameContents contents;
    canvas::CanvasOptions options;
    options.contents = &contents;
    const auto x_at = [&](double interpolation) {
        options.interpolation = interpolation;
        REQUIRE(renderer.draw(scene.world, options).ok());
        REQUIRE(contents.items.size() == 1);
        return contents.items[0].pieces[0].vertices[0].x;
    };
    const auto row_x = [&] { return scene.get<world::Transform2D>("row").position.x; };
    // 10 pixels a tick, wrapping round every 4 ticks.
    scene.world.step(kTick);
    int wraps = 0;
    for (int i = 0; i < 8; ++i) {
        renderer.before_tick(scene.world);
        const float last = row_x();
        scene.world.step(kTick);
        if (std::abs(row_x() - last) < 20.0F) {
            // Halfway through the tick, it is drawn halfway there.
            CHECK(x_at(1.0) - x_at(0.5) == doctest::Approx(5.0F));
        } else {
            // Wrapped round, it is drawn where it is, not sliding back across the repeat.
            CHECK(x_at(0.5) == doctest::Approx(x_at(1.0)));
            ++wraps;
        }
    }
    CHECK(wraps == 2);
}

TEST_CASE("controls in a canvas layer lay out, draw and take input in its canvas") {
    const Project project("canvas_layer_controls_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "bar", "components": {
            "Control": {"offset_right": 40, "offset_bottom": 10},
            "ColorRect": {"color": [0, 0, 1, 1]}}}},
        {"command": "entity.create", "params": {"id": "back", "components": {
            "Control": {"offset_right": 160, "offset_bottom": 90},
            "ColorRect": {"color": [0.2, 0.2, 0.2, 1]}}}},
        {"command": "entity.create", "params": {"id": "ok", "components": {
            "Control": {"offset_right": 20, "offset_bottom": 10}, "Button": {"text": ""}}}},
        {"command": "entity.create", "params": {"id": "rock", "components": {
            "Transform2D": {"position": [100, 60]}, "Sprite2D": {"texture": "white.png"}}}},
        {"command": "entity.create", "params": {"id": "under", "components": {
            "CanvasLayer": {"layer": -1, "children": ["back"]}}}},
        {"command": "entity.create", "params": {"id": "hud", "components": {
            "CanvasLayer": {"offset": [10, 20], "scale": [2, 2], "children": ["ok"]}}}}]})");
    // The HUD's button is placed by the layer; the world's bar stays on the screen.
    json::Value layout = project.ok("ui.layout");
    CHECK(compact(layout["controls"]) == compact(parse(R"([
        {"id": "back", "layer": "under", "rect": [0, 0, 160, 90]},
        {"id": "bar", "rect": [0, 0, 40, 10]},
        {"draw_mode": "normal", "id": "ok", "layer": "hud", "rect": [10, 20, 40, 20]}])")));
    // frame.summary lists them in drawing order: under the world, the world, over it.
    CHECK(compact(project.ok("frame.summary")["drawn"]) == compact(parse(R"([
        {"id": "back", "kind": "ColorRect", "layer": "under", "rect": [0, 0, 160, 90],
         "shown": 0.97},
        {"id": "rock", "kind": "Sprite2D", "rect": [98, 58, 4, 4]},
        {"id": "bar", "kind": "ColorRect", "rect": [0, 0, 40, 10]},
        {"id": "ok", "kind": "Button", "layer": "hud", "rect": [10, 20, 40, 20]}])")));

    // The mouse is over the button where the layer shows it, not where it would be unplaced.
    const auto hovered = [&](std::string_view at) {
        project.ok("input.inject", R"({"events": [{"type": "mouse_motion", "position": )" +
                                       std::string(at) + "}]}");
        project.ok("step");
        json::Value controls = project.ok("ui.layout", R"({"ids": ["ok"]})")["controls"];
        return controls.as_array()[0].find("hovered") != nullptr;
    };
    CHECK(hovered("[45, 35]"));
    CHECK_FALSE(hovered("[5, 5]"));

    // An invisible layer hides its controls.
    project.ok("component.set", R"({"id": "hud", "components": {
        "CanvasLayer": {"visible": false}}})");
    json::Value hidden = project.ok("ui.layout", R"({"ids": ["ok"]})");
    CHECK(*hidden["controls"].as_array()[0].find("visible") == json::Value(false));
}

TEST_CASE("canvas.convert turns window pixels into the world through the camera, and back") {
    const Project project("canvas_convert_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "cam", "components": {
            "Transform2D": {"position": [1000, 500]}, "Camera2D": {"zoom": [2, 2]}}}},
        {"command": "entity.create", "params": {"id": "heart", "components": {
            "Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "bar", "components": {
            "Control": {}}}},
        {"command": "entity.create", "params": {"id": "hud", "components": {
            "CanvasLayer": {"offset": [10, 0], "children": ["heart"]}}}}]})");
    CHECK(
        compact(project.ok("canvas.convert", R"({"points": [[80, 45], [0, 0]], "to": "world"})")) ==
        R"({"points":[[1000,500],[960,477.5]]})");
    CHECK(compact(project.ok("canvas.convert", R"({"points": [[1000, 500]], "to": "screen"})")) ==
          R"({"points":[[80,45]]})");
    // In the canvas an entity draws in: a layer's, named, or the screen for a world control.
    CHECK(compact(project.ok("canvas.convert",
                             R"({"points": [[10, 0]], "to": "world", "id": "heart"})")) ==
          R"({"layer":"hud","points":[[0,0]]})");
    CHECK(compact(project.ok("canvas.convert",
                             R"({"points": [[10, 5]], "to": "world", "id": "bar"})")) ==
          R"({"points":[[10,5]]})");
    CHECK(project.refused("canvas.convert", R"({"points": [[0, 0]], "to": "world", "id": "hart"})")
              .find("heart") != std::string::npos);
    CHECK_FALSE(project.session->call("canvas.convert", parse(R"({"points": [[0, 0]]})")).ok);

    // The same in C++, as Godot's get_global_mouse_position.
    const world::World& world = project.session->world();
    check_near(canvas::to_world(world, {80.0F, 45.0F}), {1000.0F, 500.0F});
    check_near(canvas::to_screen(world, {1000.0F, 500.0F}), {80.0F, 45.0F});
    check_near(canvas::to_world(world, {10.0F, 0.0F}, world.find("heart")), {0.0F, 0.0F});
}

TEST_CASE("scene validate reports CanvasLayer children that can't draw in it") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    const auto errors = world::validate_scene(components, parse(R"({
        "format": 1,
        "entities": {
            "hud": {"components": {"CanvasLayer": {"children": ["hud", "menu", "gun", "panel",
                                                                "label", null]}}},
            "menu": {"components": {"CanvasLayer": {"children": ["panel"]}}},
            "ship": {"components": {"Transform2D": {"children": ["gun"]}}},
            "gun": {"components": {"Transform2D": {}}},
            "panel": {"components": {"Control": {"children": ["label"]}}},
            "label": {"components": {"Control": {}}}
        }
    })"));
    std::vector<std::string> found;
    for (const json::ValidationError& error : errors) {
        found.push_back(error.path + ": " + error.message);
    }
    const std::string at = "/entities/hud/components/CanvasLayer/children/";
    CHECK(found ==
          std::vector<std::string>{
              at + "0: a canvas layer can't hold itself",
              at + R"(1: "menu" is a canvas layer; canvas layers can't hold each other)",
              at + R"(2: "gun" is already a child of "ship")",
              at + R"(4: "label" is already a child of "panel")",
              at + "5: children can't be null",
              R"(/entities/menu/components/CanvasLayer/children/0: "panel" is already in "hud")",
          });
}

TEST_CASE("entity.destroy takes a canvas layer's children and leaves their layer's list") {
    const Project project("canvas_layer_destroy_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "heart", "components": {
            "Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "score", "components": {"Control": {}}}},
        {"command": "entity.create", "params": {"id": "hud", "components": {
            "CanvasLayer": {"children": ["heart", "score"]}}}}]})");
    project.ok("entity.destroy", R"({"id": "heart"})");
    CHECK(compact(project.ok("entity.get",
                             R"({"id": "hud"})")["components"]["CanvasLayer"]["children"]) ==
          R"(["score"])");
    project.ok("undo");
    CHECK(compact(project.ok("entity.destroy", R"({"id": "hud"})")) ==
          R"({"descendants":["heart","score"],"destroyed":"hud"})");
    CHECK(compact(project.ok("world.query", "{}")) ==
          R"({"count":0,"entities":[],"truncated":false})");
}
