// Text fields, as Godot's LineEdit: main.scene.json is a sign-up form with a name field (a clear
// button and at most 20 characters), a secret password field and an age field that asks for the
// number keyboard. The profile system below reads the fields' UI events, as gameplay code does.
//
// This program clicks into the name field and types as an agent does over MCP or JSON-RPC:
// characters with input.inject's "text" events, an input method's composition with "ime"
// events, editing actions (ui_text_select_all, ui_copy, ui_undo) and keys (Enter, Tab), reading
// the UI events with events.read and the fields with ui.layout. Then it draws the form into a
// texture.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

namespace game {

// What the form was filled in with.
struct Profile {
    std::string name;
    int age = 0;
};
SLASH_COMPONENT(Profile, name, age)

} // namespace game

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace text = slash::text;
namespace ui = slash::ui;
namespace world = slash::world;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

void write_font(const std::string& path, std::span<const std::uint8_t> bytes) {
    check(slash::platform::write_file(
        path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    std::printf("wrote %s\n", path.c_str());
}

// Games ship real fonts. So that it has no binary file, this example writes Roboto, the
// engine's built-in font, and a placeholder font whose glyphs are boxes for the Japanese the
// input method types.
void write_fonts() {
    write_font("fonts/roboto.ttf", text::default_font());
    write_font("fonts/japanese.ttf", text::box_font(U"みら未来", "Japanese Boxes"));
}

// Reads the form's UI events: the name when it is submitted (Enter), the age as it changes. It
// runs after the gui system, so it sees this tick's events.
void profile(world::World& w, double /*tick_seconds*/) {
    auto* state = w.ecs().try_get<game::Profile>(w.find("form"));
    if (state == nullptr) {
        return;
    }
    for (const ui::UiEvent& event : ui::events(w)) {
        if (event.kind == "text_submitted" && event.id == "name") {
            state->name = event.text;
            w.ecs().get<world::Label>(w.find("status")).text = "Hi, " + event.text;
        }
        if (event.kind == "text_changed" && event.id == "age") {
            int age = 0;
            std::from_chars(event.text.data(), event.text.data() + event.text.size(), age);
            state->age = age;
        }
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

// What the field being edited asks of the platform: games pass it to SdlReader::set_text_input
// each frame, which turns on the input method and shows the on-screen keyboard.
void print_text_input(const world::World& w) {
    const slash::input::TextInput request = ui::text_input(w);
    if (!request.active) {
        std::puts("text input: off");
        return;
    }
    std::printf("text input: on, field at %g,%g %gx%g, caret %d px in, keyboard %s%s\n",
                request.position.x, request.position.y, request.size.x, request.size.y,
                request.caret, request.keyboard.c_str(), request.show_keyboard ? " (shown)" : "");
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    components.add<game::Profile>()
        .set_doc("What the sign-up form was filled in with.")
        .describe("name", {.doc = "The name submitted."})
        .describe("age", {.doc = "The age typed."});
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    world::World& w = session.world();
    w.add_system("profile", profile);

    write_fonts();
    call(session, "scene.open", R"({"path": "main.scene.json"})");
    w.ecs().emplace<game::Profile>(w.find("form"));
    call(session, "events.subscribe",
         R"({"kinds": ["focus_entered", "focus_exited", "editing_toggled", "text_changed",)"
         R"( "text_submitted", "text_change_rejected"]})");

    // Empty fields show their placeholders.
    call(session, "ui.layout", R"({"ids": ["name", "password", "age"]})");

    // A click into the name field gives it focus and starts editing it; typed text goes in at
    // the caret, with one text_changed for the tick.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "left", "position": [100, 56], "pressed": true},
        {"type": "mouse_button", "button": "left", "position": [100, 56], "pressed": false},
        {"type": "text", "text": "Mira "}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    print_text_input(w);

    // An input method (Japanese here) composes text that shows at the caret, the part being
    // converted underlined, until it commits it as typed text.
    call(session, "input.inject",
         R"({"events": [{"type": "ime", "text": "みら", "selection": [0, 2]}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["name"]})");
    call(session, "input.inject", R"({"events": [
        {"type": "ime", "text": ""},
        {"type": "text", "text": "未来"}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");

    // Editing actions work as their shortcuts do (Ctrl+A and Ctrl+C here, Command on a Mac).
    call(session, "input.inject", R"({"events": [
        {"type": "action", "action": "ui_text_select_all", "pressed": true},
        {"type": "action", "action": "ui_copy", "pressed": true}]})");
    call(session, "step", "{}");
    call(session, "ui.clipboard", "{}");
    call(session, "ui.layout", R"({"ids": ["name"]})");

    // Enter submits the name and stops editing; the profile system greets.
    call(session, "input.inject", R"({"events": [
        {"type": "key", "key": "Enter", "pressed": true},
        {"type": "key", "key": "Enter", "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");

    // Tab moves on and starts editing the password field, which shows dots.
    call(session, "input.inject", R"({"events": [
        {"type": "key", "key": "Tab", "pressed": true},
        {"type": "key", "key": "Tab", "pressed": false},
        {"type": "text", "text": "hunter2"}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["password"]})");

    // The age field takes three characters and rejects the rest; undo takes back what a tick
    // typed.
    call(session, "input.inject", R"({"events": [
        {"type": "key", "key": "Tab", "pressed": true},
        {"type": "key", "key": "Tab", "pressed": false},
        {"type": "text", "text": "1234"}]})");
    call(session, "step", "{}");
    print_text_input(w);
    call(session, "input.inject", R"({"events": [
        {"type": "action", "action": "ui_undo", "pressed": true},
        {"type": "text", "text": "34"}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "entity.get", R"({"id": "form", "components": ["Profile"]})");
    call(session, "ui.layout", R"({"ids": ["status"]})");

    // Draw into a 320x240 texture: each field's box, the focus outline, the text and the caret.
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
