#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace json = slash::json;
namespace world = slash::world;
namespace command = slash::command;
namespace render = slash::render;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// A 4x4 texture: opaque white, or opaque only in its left half.
void write_texture(const std::string& path, bool half) {
    render::Image image{.width = 4, .height = 4, .pixels = {}};
    image.pixels.assign(std::size_t{4} * 4 * 4, 255);
    if (half) {
        for (std::size_t y = 0; y < 4; ++y) {
            for (std::size_t x = 2; x < 4; ++x) {
                image.pixels[(((y * 4) + x) * 4) + 3] = 0;
            }
        }
    }
    REQUIRE(slash::platform::write_file(path, render::encode_png(image).value()).ok());
}

// A 160x90 project with two textures, and a session over it.
struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string folder) : root(std::move(folder)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1,
            "name": "Summary", "main_scene": "main.scene.json", "version": "1.0.0",
            "window": {"width": 160, "height": 90}})")
                    .ok());
        write_texture(root + "/white.png", false);
        write_texture(root + "/half.png", true);
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

    command::Reply fails(std::string_view name, std::string_view params, int code) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        CHECK_MESSAGE(reply.code == code, reply.message);
        return reply;
    }
};

// A sprite of a 4x4 texture, scaled to size pixels square and centered on x, y.
std::string sprite(std::string_view id, std::string_view texture, float x, float y, float size,
                   std::string_view extra = "") {
    const float scale = size / 4.0F;
    return R"({"command": "entity.create", "params": {"id": ")" + std::string(id) +
           R"(", "components": {"Transform2D": {"position": [)" + std::to_string(x) + ", " +
           std::to_string(y) + R"(], "scale": [)" + std::to_string(scale) + ", " +
           std::to_string(scale) + R"(]}, "Sprite2D": {"texture": ")" + std::string(texture) +
           "\"" + std::string(extra) + "}}}}";
}

} // namespace

TEST_CASE("frame.summary says what shows, what is covered, offscreen or clear, and why not") {
    const Project project("frame_summary_project");
    project.ok("batch",
               R"({"commands": [)" + sprite("ground", "white.png", 80, 80, 20) + ", " +
                   sprite("hero", "white.png", 40, 40, 16) + ", " +
                   sprite("crate", "white.png", 48, 40, 16, R"(, "z_index": 1)") + ", " +
                   sprite("ghost", "white.png", 100, 40, 8, R"(, "modulate": [1, 1, 1, 0])") +
                   ", " + sprite("coin", "white.png", 300, 40, 8) + ", " +
                   sprite("lamp", "white.png", 140, 20, 16) + ", " +
                   sprite("shade", "half.png", 140, 20, 16, R"(, "z_index": 1)") + ", " +
                   sprite("torch", "gone.png", 10, 10, 8) + "]}");
    // The ground is 20 pixels square, scaled up only on x from here on.
    project.ok("component.set",
               R"({"id": "ground", "components": {"Transform2D": {"scale": [40, 5]}}})");

    const json::Value summary = project.ok("frame.summary");
    CHECK(compact(summary) == compact(parse(R"({
        "viewport": [160, 90],
        "drawn": [
            {"id": "ground", "kind": "Sprite2D", "rect": [0, 70, 160, 20]},
            {"id": "hero", "kind": "Sprite2D", "rect": [32, 32, 16, 16], "shown": 0.5},
            {"id": "lamp", "kind": "Sprite2D", "rect": [132, 12, 16, 16], "shown": 0.5},
            {"id": "crate", "kind": "Sprite2D", "rect": [40, 32, 16, 16]},
            {"id": "shade", "kind": "Sprite2D", "rect": [132, 12, 16, 16]}],
        "offscreen": ["coin"],
        "clear": ["ghost"],
        "problems": [{"id": "torch", "path": "/components/Sprite2D/texture",
                      "message": "asset \"gone.png\" could not be read: no such file"}]})")));
}

TEST_CASE("frame.summary reads controls and maps the frame, by region, IDs and a limit") {
    const Project project("frame_summary_map_project");
    project.ok("batch",
               R"({"commands": [)" + sprite("wall", "white.png", 20, 45, 40) + ", " +
                   sprite("hidden", "white.png", 130, 20, 16) + ", " +
                   sprite("seen", "white.png", 130, 68, 16) + ", " +
                   sprite("spin", "white.png", 60, 70, 16) + ", " +
                   sprite("sheet", "white.png", 80, 40, 8, R"(, "hframes": 2, "frame": 3)") +
                   R"(,
        {"command": "entity.create", "params": {"id": "panel", "components": {
            "Control": {"offset_left": 100, "offset_right": 160, "offset_bottom": 45},
            "ColorRect": {"color": [0, 0, 0, 0.9]}}}},
        {"command": "entity.create", "params": {"id": "glass", "components": {
            "Control": {"offset_left": 100, "offset_top": 45, "offset_right": 160,
                        "offset_bottom": 90},
            "ColorRect": {"color": [1, 1, 1, 0.6]}}}},
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Control": {"offset_left": 40, "offset_right": 100, "offset_bottom": 20},
            "Label": {"text": "Hello world", "visible_characters": 5}}}},
        {"command": "entity.create", "params": {"id": "go", "components": {
            "Control": {"offset_left": 4, "offset_top": 70, "offset_right": 34,
                        "offset_bottom": 86},
            "Button": {"text": "Go", "disabled": true}}}}]})");
    project.ok("component.set",
               R"({"id": "spin", "components": {"Transform2D": {"rotation": 0.7853982}}})");

    // A sprite under a panel of alpha 0.9 is covered; under one of 0.6 it shows. The disabled
    // button is 30 pixels high, so a third of it is below the window. The title shows the five
    // characters visible_characters lets it. Glass took "g" first, reading across, so the button
    // is "G".
    CHECK(compact(project.ok("frame.summary", R"({"map": [8, 4]})")) == compact(parse(R"({
        "viewport": [160, 90],
        "drawn": [
            {"id": "wall", "kind": "Sprite2D", "rect": [0, 25, 40, 40]},
            {"id": "seen", "kind": "Sprite2D", "rect": [122, 60, 16, 16]},
            {"id": "spin", "kind": "Sprite2D", "rect": [48.7, 58.7, 22.6, 22.6]},
            {"id": "panel", "kind": "ColorRect", "rect": [100, 0, 60, 45]},
            {"id": "glass", "kind": "ColorRect", "rect": [100, 45, 60, 45]},
            {"id": "title", "kind": "Label", "rect": [40, 0, 60, 22], "text": "Hello"},
            {"id": "go", "kind": "Button", "rect": [4, 70, 30, 30], "shown": 0.67,
             "text": "Go", "draw_mode": "disabled"}],
        "covered": ["hidden"],
        "problems": [{"id": "sheet", "path": "/components/Sprite2D",
                      "message": "frame 3 is outside the sheet's 2 frames, numbered from 0"}],
        "map": {"legend": {"G": "go", "g": "glass", "p": "panel", "w": "wall"},
                "rows": [".....ppp", "ww...ppp", "ww...ggg", "GG...ggg"]}})")));

    // A region: only what shows in it, its map, and nothing offscreen.
    CHECK(compact(project.ok("frame.summary", R"({"region": [0, 20, 40, 50], "map": [4, 5]})")) ==
          compact(parse(R"({"viewport": [160, 90],
              "drawn": [{"id": "wall", "kind": "Sprite2D", "rect": [0, 25, 40, 40]}],
              "problems": [{"id": "sheet", "path": "/components/Sprite2D",
                            "message": "frame 3 is outside the sheet's 2 frames, numbered from 0"}],
              "map": {"legend": {"w": "wall"}, "rows": ["wwww", "wwww", "wwww", "wwww", "wwww"]}})")));

    // IDs and a limit.
    CHECK(compact(project.ok("frame.summary", R"({"ids": ["hidden", "seen"]})")) ==
          compact(parse(R"({"viewport": [160, 90], "covered": ["hidden"],
              "drawn": [{"id": "seen", "kind": "Sprite2D", "rect": [122, 60, 16, 16]}]})")));
    const json::Value limited = project.ok("frame.summary", R"({"limit": 2})");
    CHECK(limited.find("drawn")->as_array().size() == 2);
    CHECK(limited.find("truncated")->as_bool());

    // Mistakes.
    CHECK(project
              .fails("frame.summary", R"({"region": [150, 0, 20, 20]})",
                     command::code::kInvalidArgument)
              .message == "/region: [x, y, width, height] inside the 160x90 window");
    CHECK(project.fails("frame.summary", R"({"ids": ["wal"]})", command::code::kNotFound).message ==
          "/ids/0: no entity has the id \"wal\"; did you mean \"wall\"?");
    project.fails("frame.summary", R"({"map": [0, 4]})", command::code::kInvalidParams);
    project.fails("frame.summary", R"({"map": [8]})", command::code::kInvalidParams);
}
