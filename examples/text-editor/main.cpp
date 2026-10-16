// A text field of several lines, as Godot's TextEdit: main.scene.json is a letter to write, a
// TextEdit that wraps its lines and highlights the caret's, with a word count under it. The
// word_count system below reads the TextEdit's text_changed events, as gameplay code does.
//
// This program writes the letter as an agent does over MCP or JSON-RPC: a click, typed text and
// new lines with input.inject, a double click to select a word and type over it, undo and redo,
// a tab, and the wheel to scroll, reading the events with events.read and the field with
// ui.layout. Then it draws the letter into a texture.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace ui = slash::ui;
namespace world = slash::world;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// Counts the letter's words each time it changes. It runs after the gui system, so it sees this
// tick's events.
void word_count(world::World& w, double /*tick_seconds*/) {
    for (const ui::UiEvent& event : ui::events(w)) {
        if (event.kind != "text_changed" || event.id != "letter") {
            continue;
        }
        const std::string& letter = w.ecs().get<world::TextEdit>(w.find("letter")).text;
        int words = 0;
        bool in_word = false;
        for (const char c : letter) {
            const bool space = c == ' ' || c == '\n' || c == '\t';
            if (!space && !in_word) {
                ++words;
            }
            in_word = !space;
        }
        w.ecs().get<world::Label>(w.find("count")).text =
            std::to_string(words) + (words == 1 ? " word" : " words");
    }
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    world::World& w = session.world();
    w.add_system("word_count", word_count);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    call(session, "events.subscribe", R"({"kinds": ["focus_entered", "text_changed"]})");

    // Empty, it shows its placeholder.
    call(session, "ui.layout", R"({"ids": ["letter"]})");

    // A click puts the caret in it; typed text goes in at the caret, ui_text_newline (Enter)
    // starts a line, and lines too wide for the field wrap at a word.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "left", "position": [160, 80], "pressed": true},
        {"type": "mouse_button", "button": "left", "position": [160, 80], "pressed": false},
        {"type": "text", "text": "Dear Mira,"},
        {"type": "action", "action": "ui_text_newline", "pressed": true},
        {"type": "text", "text": "The harbor bell rang twice tonight. Meet me at the old lighthouse before the tide turns."}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "ui.layout", R"({"ids": ["letter", "count"]})");

    // A double click selects a word, and typing replaces it. Undo takes back the replacement in
    // one step, and redo puts it back.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "left", "position": [143, 81], "pressed": true},
        {"type": "mouse_button", "button": "left", "position": [143, 81], "pressed": false},
        {"type": "mouse_button", "button": "left", "position": [143, 81], "pressed": true,
         "double_click": true},
        {"type": "mouse_button", "button": "left", "position": [143, 81], "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["letter"]})");
    call(session, "input.inject", R"({"events": [{"type": "text", "text": "tolled"}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["letter"]})");
    call(session, "input.inject",
         R"({"events": [{"type": "action", "action": "ui_undo", "pressed": true}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["letter"]})");
    call(session, "input.inject",
         R"({"events": [{"type": "action", "action": "ui_redo", "pressed": true}]})");
    call(session, "step", "{}");

    // More lines than fit: a scroll bar shows and the view follows the caret down. Tab types a
    // tab (tab_input_mode is on), as wide as four spaces.
    call(session, "input.inject", R"({"events": [
        {"type": "action", "action": "ui_text_caret_document_end", "pressed": true},
        {"type": "action", "action": "ui_text_newline", "pressed": true},
        {"type": "action", "action": "ui_text_newline", "pressed": true},
        {"type": "key", "key": "Tab", "pressed": true},
        {"type": "text", "text": "Bring the lantern."},
        {"type": "action", "action": "ui_text_newline", "pressed": true},
        {"type": "text", "text": "Kai"}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["letter", "count"]})");

    // The wheel scrolls three lines a notch, without moving the caret.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "wheel_up", "position": [160, 80], "pressed": true}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["letter"]})");
    call(session, "entity.get", R"({"id": "letter", "components": ["TextEdit"]})");

    // Draw into a 320x240 texture: the field's box, the caret's line, the text, the caret and
    // the scroll bar.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 320,
                                                           .height = 240,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(w, {.target = target,
                            .clear = render::kDefaultClearColor,
                            .width = 0.0F,
                            .height = 0.0F,
                            .interpolation = 1.0,
                            .texture_filter = render::Filter::Linear,
                            .time = 0.0}));
    std::printf("controls drawn: %zu, boxes drawn: %zu, glyphs drawn: %zu\n",
                renderer.controls_drawn(), renderer.boxes_drawn(), renderer.glyphs_drawn());
    return 0;
}
