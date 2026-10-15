// UI and text, as Godot's Control, Label and ColorRect: main.scene.json is a pause screen over
// the game, a panel centered on the window holding a title in a distance field font with an
// outline, a paragraph that wraps, a quest line cut to one line with an ellipsis and a hint in
// the bottom-right corner, plus a score and a sign in the corners.
//
// This program writes the font file, checks the screen with ui.layout and text.measure as an
// agent would, with no screenshot, changes the score and the window size, catches a misspelled
// theme override, then draws the screen into a texture.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/text/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
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

// Games ship their own .ttf or .otf files; this one writes the engine's built-in font (Roboto)
// so the example has no binary file.
void write_font() {
    const std::span<const std::uint8_t> font = slash::text::default_font();
    check(slash::platform::write_file(
        "fonts/roboto.ttf",
        std::string_view(reinterpret_cast<const char*>(font.data()), font.size())));
    std::puts("wrote fonts/roboto.ttf");
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

    write_font();
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Where everything is in the 640x360 window, in drawing order: parents before children.
    // The body wrapped to the panel's width, the quest was cut to one line with "…", and the
    // sign's Chinese characters aren't in Roboto, so they are counted as missing (project.json
    // turns off fonts installed on the machine, which might have them).
    call(session, "ui.layout", "{}");

    // Will a bigger score fit? text.measure answers without changing the scene.
    call(session, "text.measure", R"({"text": "Score 99999", "font_size": 20})");
    call(session, "component.set",
         R"({"id": "score", "components": {"Label": {"text": "Score 99999"}}})");
    call(session, "ui.layout", R"({"ids": ["score"]})");

    // On a smaller window the anchors keep the panel centered and the sign in its corner.
    call(session, "ui.layout", R"({"ids": ["panel", "sign"], "viewport": [480, 270]})");

    // A theme override Label doesn't have is reported, with the name it was probably meant to be.
    call(
        session, "component.set",
        R"({"id": "hint", "components": {"Control": {"theme_override_colors": {"font_colour": [1, 1, 1, 1]}}}})");
    call(session, "ui.layout", R"({"ids": ["hint"]})");
    call(session, "undo", "{}");

    // Draw into a 640x360 texture. Controls draw over the world's sprites, each label as its
    // shadow, outline then text; glyphs that draw in a row from the same atlas page share a GPU
    // draw.
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
    std::printf("controls drawn: %zu, glyphs drawn: %zu, GPU draws: %llu\n",
                renderer.controls_drawn(), renderer.glyphs_drawn(),
                static_cast<unsigned long long>(device->stats().draws));
    return 0;
}
