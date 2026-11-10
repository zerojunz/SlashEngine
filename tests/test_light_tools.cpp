// Light tools: Godot's light temperature and physical light units, 2D light temperature, and
// light.tune and lighting.check over 3D and 2D worlds.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/render/light.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

// A project of 64 by 36 pixels with project.json's extra settings and a white texture.
struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string name, std::string_view settings) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/project.json",
                                            R"({"format": 1, "name": "Lights",
            "main_scene": "main.scene.json", "version": "1.0.0",
            "window": {"width": 64, "height": 36})" +
                                                std::string(settings) + "}")
                    .ok());
        render::Image white;
        white.width = 4;
        white.height = 4;
        white.pixels.assign(std::size_t{4} * 4 * 4, 255);
        REQUIRE(slash::platform::write_file(root + "/white.png", render::encode_png(white).value())
                    .ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply fails(std::string_view name, std::string_view params, int code) {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        CHECK_MESSAGE(reply.code == code, reply.message);
        return reply;
    }

    // The color drawn at a pixel of the frame, from 0 to 255.
    std::array<int, 3> pixel(int x, int y) {
        auto drawn = slash::canvas::capture(session->world(), session->assets(), {});
        REQUIRE(drawn.ok());
        const render::Image& image = drawn.value().image;
        const std::size_t at =
            ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
        return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2]};
    }

    double mean() {
        return ok("lighting.check", R"({"scale": 1, "lights": false})")["exposure"]["mean"]
            .as_double();
    }
};

// A gray floor under a camera, lit by one omni light.
constexpr std::string_view kRoom = R"({"commands": [
    {"command": "entity.create", "params": {"id": "camera", "components": {
        "Camera3D": {"current": true},
        "Transform3D": {"position": [0, 2, 4], "rotation": [-0.45, 0, 0]}}}},
    {"command": "entity.create", "params": {"id": "floor", "components": {
        "MeshInstance3D": {}, "PlaneMesh": {"size": [20, 20]},
        "StandardMaterial3D": {"albedo_color": [0.6, 0.6, 0.6, 1]}, "Transform3D": {}}}},
    {"command": "entity.create", "params": {"id": "bulb", "components": {
        "OmniLight3D": {"omni_range": 6}, "Transform3D": {"position": [0, 1, 0]}}}}]})";

// The light at the origin, as light.sample sees it.
json::Value sampled(Project& project) {
    json::Value reply = project.ok("light.sample", R"({"points": [[0, 0]]})");
    return reply["points"].as_array()[0]["color"];
}

} // namespace

TEST_CASE("color temperatures run from warm to cool, and light units are Godot's") {
    const std::array<float, 3> candle = render::color_temperature(1900.0F);
    const std::array<float, 3> noon = render::color_temperature(6500.0F);
    const std::array<float, 3> sky = render::color_temperature(12000.0F);
    // The brightest channel is 1: red when warm, blue when cool.
    CHECK(candle[0] == 1.0F);
    CHECK(candle[1] < 1.0F);
    CHECK(candle[2] < candle[1]);
    CHECK(sky[2] == 1.0F);
    CHECK(sky[0] < sky[1]);
    // Daylight is nearly white.
    for (const float channel : noon) {
        CHECK(channel > 0.9F);
    }
    // The same on every machine: integer-free math only through slash::exp and ln.
    CHECK(render::color_temperature(1900.0F) == candle);
    // Tinting multiplies in linear light: white takes the temperature's color.
    const std::array<float, 3> tinted = render::tint_by_temperature({1, 1, 1}, 1900.0F);
    CHECK(tinted[0] == doctest::Approx(candle[0]).epsilon(1e-4));
    CHECK(tinted[2] == doctest::Approx(candle[2]).epsilon(1e-3));

    constexpr float kPi = std::numbers::pi_v<float>;
    CHECK(render::directional_light_units(kPi) == doctest::Approx(1.0F));
    CHECK(render::omni_light_units(4.0F * kPi * kPi) == doctest::Approx(1.0F));
    CHECK(render::spot_light_units(kPi * kPi) == doctest::Approx(1.0F));
    CHECK(render::exposure_normalization(100.0F) == doctest::Approx(100.0F / 3072007.0F));
}

TEST_CASE("physical light units brighten lights by their lumens and tint them by temperature") {
    // Without physical light units, temperature and lumens change nothing, as in Godot.
    Project plain("light_units_off_project", "");
    plain.ok("batch", kRoom);
    const double base = plain.mean();
    CHECK(base > 0.05);
    plain.ok(
        "component.set",
        R"({"id": "bulb", "components": {"OmniLight3D": {"light_temperature": 1900, "light_intensity_lumens": 5000}}})");
    CHECK(plain.mean() == base);

    Project units("light_units_on_project",
                  R"(, "rendering": {"lights_and_shadows": {"use_physical_light_units": true}})");
    units.ok("batch", kRoom);
    // 1000 lumens is about 25 times the energy of a light of energy 1.
    const double bright = units.mean();
    CHECK(bright > base * 2.0);
    // 4 pi squared lumens is energy 1, so the frame comes back to the plain one's at 6500 K,
    // nearly white.
    units.ok(
        "component.set",
        R"({"id": "bulb", "components": {"OmniLight3D": {"light_intensity_lumens": 39.4784176}}})");
    CHECK(units.mean() == doctest::Approx(base).epsilon(0.05));
    const std::array<int, 3> white = units.pixel(32, 30);
    units.ok("component.set",
             R"({"id": "bulb", "components": {"OmniLight3D": {"light_temperature": 1900}}})");
    const std::array<int, 3> warm = units.pixel(32, 30);
    CHECK(warm[0] > warm[2] + 20);
    CHECK(std::abs(white[0] - white[2]) < 12);

    // A camera's exposure sensitivity normalizes exposure: ISO 100 is far darker.
    units.ok("component.set",
             R"({"id": "camera", "components": {"CameraAttributesPractical": {}}})");
    CHECK(units.mean() < base * 0.5);
}

TEST_CASE("2D lights tint by their temperature when it is set") {
    Project project("light_temperature_2d_project", "");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "dark", "components": {
            "CanvasModulate": {"color": [0, 0, 0, 1]}}}},
        {"command": "entity.create", "params": {"id": "lamp", "components": {
            "Transform2D": {}, "PointLight2D": {"texture": "white.png", "texture_scale": 50}}}}]})");
    CHECK(sampled(project) == parse("[1, 1, 1]"));
    project.ok("component.set",
               R"({"id": "lamp", "components": {"PointLight2D": {"temperature": 1900}}})");
    const json::Array color = sampled(project).as_array();
    CHECK(color[0].as_double() == 1.0);
    CHECK(color[1].as_double() < 1.0);
    CHECK(color[2].as_double() < color[1].as_double());
}

TEST_CASE("light.tune scales 3D lights to a mean brightness and each light's share") {
    Project project("light_tune_3d_project", "");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "camera", "components": {
            "Camera3D": {"current": true},
            "Transform3D": {"position": [0, 2, 5], "rotation": [-0.35, 0, 0]}}}},
        {"command": "entity.create", "params": {"id": "ground", "components": {
            "MeshInstance3D": {}, "PlaneMesh": {"size": [20, 20]},
            "StandardMaterial3D": {"albedo_color": [0.6, 0.6, 0.6, 1]}, "Transform3D": {}}}},
        {"command": "entity.create", "params": {"id": "sun", "components": {
            "DirectionalLight3D": {}, "Transform3D": {"rotation": [-0.9, 0.3, 0]}}}},
        {"command": "entity.create", "params": {"id": "blaze", "components": {
            "OmniLight3D": {"light_energy": 60, "omni_range": 4},
            "Transform3D": {"position": [0, 0.5, 1]}}}}]})");
    // The blaze clips the ground under it.
    json::Value tuned = project.ok("light.tune", R"({"scale": 1, "mean": 0.3})");
    CHECK(tuned["before"]["clipped"].as_double() > 0.05);
    CHECK(tuned["converged"] == true);
    CHECK(tuned["after"]["clipped"].as_double() <= 0.02);
    CHECK(std::abs(tuned["after"]["mean"].as_double() - 0.3) <= 0.02);
    json::Array changes = tuned["changes"].as_array();
    REQUIRE(changes.size() == 2);
    CHECK(changes[0]["id"] == "blaze");
    CHECK(changes[0]["from"] == 60);
    CHECK(changes[0]["to"].as_double() < 60);
    // One undo step puts the energies back.
    project.ok("undo");
    CHECK(
        project.ok("entity.get",
                   R"({"id": "blaze", "components": ["OmniLight3D"]})")["components"]["OmniLight3D"]
                                                                       ["light_energy"] == 60);

    // Shares: the sun gives most of the light and the blaze little.
    tuned = project.ok("light.tune",
                       R"({"scale": 1, "shares": {"sun": 0.6}, "mean": 0.35, "iterations": 10})");
    CHECK(std::abs(tuned["after"]["shares"]["sun"].as_double() - 0.6) <= 0.05);

    // Only the named lights change, and names are checked.
    tuned = project.ok("light.tune", R"({"scale": 1, "ids": ["sun"], "mean": 0.2})");
    for (json::Value& change : tuned["changes"].as_array()) {
        CHECK(change["id"] == "sun");
    }
    CHECK(project.fails("light.tune", R"({"ids": ["sum"]})", command::code::kNotFound).message ==
          "/ids/0: no light that shines has the id \"sum\"; did you mean \"sun\"?");
    CHECK(project.fails("light.tune", R"({"shares": {"blaz": 0.5}})", command::code::kNotFound)
              .message == "/shares/blaz: no light that shines has the id \"blaz\"; did you mean "
                          "\"blaze\"?");
    project.fails("light.tune", R"({"mean": 2})", command::code::kInvalidParams);
}

TEST_CASE("lighting.check and light.tune look at a 2D world's lights") {
    Project project("light_tune_2d_project", "");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "night", "components": {
            "CanvasModulate": {"color": [0.05, 0.05, 0.05, 1]}}}},
        {"command": "entity.create", "params": {"id": "ground", "components": {
            "Transform2D": {"scale": [16, 9]},
            "Sprite2D": {"texture": "white.png", "centered": false}}}},
        {"command": "entity.create", "params": {"id": "torch", "components": {
            "Transform2D": {"position": [20, 18]},
            "PointLight2D": {"texture": "white.png", "texture_scale": 4, "energy": 0.2}}}},
        {"command": "entity.create", "params": {"id": "moon", "components": {
            "DirectionalLight2D": {"energy": 0.1}}}}]})");
    json::Value checked = project.ok("lighting.check", R"({"scale": 1})");
    json::Array lights = checked["lights"].as_array();
    REQUIRE(lights.size() == 2);
    CHECK(lights[0]["id"] == "moon");
    CHECK(lights[0]["type"] == "DirectionalLight2D");
    CHECK(lights[1]["id"] == "torch");
    CHECK(lights[1]["lit"].as_double() > 0.0);

    json::Value tuned = project.ok("light.tune", R"({"scale": 1, "mean": 0.4})");
    CHECK(tuned["before"]["mean"].as_double() < 0.4);
    CHECK(tuned["converged"] == true);
    CHECK(std::abs(tuned["after"]["mean"].as_double() - 0.4) <= 0.02);

    // A world with neither 3D meshes nor 2D lights has nothing to tune.
    Project empty("light_tune_empty_project", "");
    CHECK(empty.fails("light.tune", "{}", command::code::kInvalidParams).message ==
          "no light with an ID shines, so none can be tuned");
}
