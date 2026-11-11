// Reflection2D: a 2D reflection that shows what its canvas drew before it upside down about its
// top edge, rippled, faded and tinted.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
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

// A project of 64 by 36 pixels over black, with a white texture.
struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/project.json",
                                            R"({"format": 1, "name": "Water",
            "main_scene": "main.scene.json", "version": "1.0.0",
            "window": {"width": 64, "height": 36},
            "rendering": {"clear_color": [0, 0, 0, 1]}})")
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

    render::Image frame() {
        auto drawn = slash::canvas::capture(session->world(), session->assets(), {});
        REQUIRE(drawn.ok());
        return std::move(drawn).value().image;
    }
};

// The color at a pixel of an image, from 0 to 255.
std::array<int, 3> at(const render::Image& image, int x, int y) {
    const std::size_t i =
        ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
    return {image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]};
}

// The first column from the left where a row of an image is redder than half.
int red_column(const render::Image& image, int y) {
    for (int x = 0; x < static_cast<int>(image.width); ++x) {
        if (at(image, x, y)[0] > 128) {
            return x;
        }
    }
    return -1;
}

// A red bar 32 pixels wide from y 4 to 12, and still water from y 18 down.
constexpr std::string_view kPond = R"({"commands": [
    {"command": "entity.create", "params": {"id": "boat", "components": {
        "Sprite2D": {"texture": "white.png", "modulate": [1, 0, 0, 1]},
        "Transform2D": {"position": [32, 8], "scale": [8, 2]}}}},
    {"command": "entity.create", "params": {"id": "water", "components": {
        "Reflection2D": {"size": [64, 18], "opacity": 1, "fade": 0},
        "Transform2D": {"position": [0, 18]}}}}]})";

} // namespace

TEST_CASE("a reflection shows what draws before it upside down below its line") {
    Project project("reflection_2d_project");
    project.ok("batch", kPond);
    const render::Image image = project.frame();
    // The bar is 6 to 14 pixels above the line, so its reflection 6 to 14 below it.
    CHECK(at(image, 32, 8) == std::array{255, 0, 0});
    CHECK(at(image, 32, 28) == std::array{255, 0, 0});
    CHECK(at(image, 32, 26) == std::array{255, 0, 0});
    CHECK(at(image, 32, 22) == std::array{0, 0, 0}); // between the line and the reflection
    CHECK(at(image, 32, 34) == std::array{0, 0, 0});
    CHECK(at(image, 8, 28) == std::array{0, 0, 0}); // beside the bar

    // Faded with depth and tinted: half as strong at the line, less below, and only its green.
    project.ok("component.set", R"({"id": "water", "components": {"Reflection2D": {
        "opacity": 0.5, "fade": 1, "tint": [1, 1, 1, 1]}}})");
    const render::Image faded = project.frame();
    CHECK(at(faded, 32, 26)[0] > at(faded, 32, 30)[0]);
    CHECK(at(faded, 32, 26)[0] < 128);
    CHECK(at(faded, 32, 30)[0] > 0);
    project.ok("component.set", R"({"id": "water", "components": {"Reflection2D": {
        "opacity": 1, "fade": 0, "tint": [0, 1, 1, 1]}}})");
    CHECK(at(project.frame(), 32, 28) == std::array{0, 0, 0});

    // Hidden, it draws nothing.
    project.ok("component.set",
               R"({"id": "water", "components": {"Reflection2D": {"visible": false}}})");
    CHECK(at(project.frame(), 32, 28) == std::array{0, 0, 0});
}

TEST_CASE("a reflection shows only what draws before it, and what draws after covers it") {
    Project project("reflection_2d_order_project");
    project.ok("batch", kPond);
    // The bar above the water in draw order: not reflected.
    project.ok("component.set", R"({"id": "boat", "components": {"Sprite2D": {"z_index": 1}}})");
    CHECK(at(project.frame(), 32, 28) == std::array{0, 0, 0});
    project.ok("component.set", R"({"id": "boat", "components": {"Sprite2D": {"z_index": 0}}})");
    // A green reed drawn after the water stands over its reflection.
    project.ok("entity.create", R"({"id": "reed", "components": {
        "Sprite2D": {"texture": "white.png", "modulate": [0, 1, 0, 1]},
        "Transform2D": {"position": [32, 28], "scale": [1, 1]}}})");
    const render::Image image = project.frame();
    CHECK(at(image, 32, 28) == std::array{0, 255, 0});
    CHECK(at(image, 40, 28) == std::array{255, 0, 0});
}

TEST_CASE("a reflection ripples sideways in waves that move down it") {
    Project project("reflection_2d_ripple_project");
    project.ok("batch", kPond);
    project.ok("component.set", R"({"id": "water", "components": {"Reflection2D": {
        "ripple_amplitude": 3, "ripple_wavelength": 8, "ripple_speed": 1}}})");
    const render::Image still = project.frame();
    // The bar's left edge at 16, shifted by the waves, differently at different depths.
    const int upper = red_column(still, 25);
    const int lower = red_column(still, 29);
    CHECK(upper >= 13);
    CHECK(upper <= 19);
    CHECK(lower >= 13);
    CHECK(lower <= 19);
    CHECK(upper != lower);
    // Half a second later the waves have moved half a wave down, so they lean the other way.
    project.ok("step", R"({"ticks": 30})");
    const render::Image later = project.frame();
    CHECK(red_column(later, 25) != upper);
    CHECK(red_column(later, 29) != lower);
}

TEST_CASE("frame.summary lists a reflection among what was drawn") {
    Project project("reflection_2d_summary_project");
    project.ok("batch", kPond);
    const std::string summary = json::write(project.ok("frame.summary"));
    CHECK(summary.find("Reflection2D") != std::string::npos);
    CHECK(summary.find("\"water\"") != std::string::npos);
}
