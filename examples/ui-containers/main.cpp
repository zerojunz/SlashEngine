// UI containers, themes and the controls that draw boxes and textures, as Godot's: main.scene.json
// is an inventory screen. A CenterContainer keeps a PanelContainer in the middle of the window;
// in it a VBoxContainer stacks a title, a ProgressBar, a GridContainer of item slots with
// TextureRect icons, a FlowContainer of tags and a hint. A NinePatchRect tooltip in the corner
// holds a MarginContainer. ui/game.theme.json, the project's theme, styles all of it.
//
// This program writes the two textures, checks the screen with ui.layout and ui.theme as an
// agent would, changes the bar and the grid, catches a misspelled style override, then draws the
// screen into a texture.

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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

void write_png(const std::string& path, const render::Image& image) {
    const auto png = render::encode_png(image);
    if (!png) {
        check(png.error());
    }
    check(slash::platform::write_file(path, png.value()));
    std::printf("wrote %s\n", path.c_str());
}

// Games ship their own images; this example draws its two so it has no binary file. The gem is
// white, so each icon's self_modulate colors it.
void write_textures() {
    render::Image gem{.width = 16, .height = 16, .pixels = {}};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int distance = std::abs((2 * x) - 15) + std::abs((2 * y) - 15);
            std::uint8_t shade = x + y < 15 ? 255 : 190;
            if (distance > 12) {
                shade = 120;
            }
            const std::uint8_t alpha = distance <= 15 ? 255 : 0;
            gem.pixels.insert(gem.pixels.end(), {shade, shade, shade, alpha});
        }
    }
    write_png("ui/gem.png", gem);

    // A frame with a 2-pixel border and cut corners, for a nine-patch with 6-pixel margins.
    render::Image frame{.width = 24, .height = 24, .pixels = {}};
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            const bool corner = (x == 0 || x == 23) && (y == 0 || y == 23);
            const bool border = x < 2 || y < 2 || x > 21 || y > 21;
            if (corner) {
                frame.pixels.insert(frame.pixels.end(), {0, 0, 0, 0});
            } else if (border) {
                frame.pixels.insert(frame.pixels.end(), {200, 190, 150, 255});
            } else {
                frame.pixels.insert(frame.pixels.end(), {30, 28, 40, 230});
            }
        }
    }
    write_png("ui/frame.png", frame);
}

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.value;
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    write_textures();
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Where the containers put things. The center container keeps the frame in the middle of the
    // 640x360 window, the grid is 4 slots of 64 wide with 6 between, centered in the box, and the
    // tags wrap onto a second line.
    call(session, "ui.layout",
         R"({"ids": ["frame", "title", "health", "grid", "slot_0", "icon_0", "slot_7", "tags",)"
         R"( "tag_5", "hint", "tooltip_text"]})");

    // Where a control's look comes from: the slot's panel from the theme's "Slot" variation, the
    // title's size and color from "Header", the rest of its items from the default theme.
    call(session, "ui.theme", R"({"id": "slot_0", "items": ["styles"]})");
    call(session, "ui.theme", R"({"id": "title", "items": ["colors", "font_sizes"]})");

    // The bar shows its value; the grid in one row widens the frame, which stays centered.
    call(session, "component.set",
         R"({"id": "health", "components": {"ProgressBar": {"value": 35}}})");
    call(session, "component.set",
         R"({"id": "grid", "components": {"GridContainer": {"columns": 8}}})");
    call(session, "ui.layout", R"({"ids": ["frame", "health", "grid", "slot_7"]})");

    // A style override its type doesn't have is reported, with the name it was probably meant to
    // be.
    call(
        session, "component.set",
        R"({"id": "slot_1", "components": {"Control": {"theme_override_styles": {"panal": {"bg_color": [1, 0, 0, 1]}}}}})");
    call(session, "ui.layout", R"({"ids": ["slot_1"]})");
    call(session, "undo", "{}");

    // Draw into a 640x360 texture: the style boxes, the icons and the nine-patch, then text.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 640,
                                                           .height = 360,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(session.world(), {.target = target,
                                          .clear = render::kDefaultClearColor,
                                          .width = 0.0F,
                                          .height = 0.0F,
                                          .interpolation = 1.0,
                                          .texture_filter = render::Filter::Linear,
                                          .time = 0.0}));
    std::printf("controls drawn: %zu, boxes drawn: %zu, glyphs drawn: %zu, GPU draws: %llu\n",
                renderer.controls_drawn(), renderer.boxes_drawn(), renderer.glyphs_drawn(),
                static_cast<unsigned long long>(device->stats().draws));
    return 0;
}
