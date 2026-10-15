// Buttons, focus and hover, as Godot's: main.scene.json is a title menu. A column holds a Play
// button, three toggle buttons in one button_group (at most one is on), a TextureButton that
// mutes the sound, a disabled Online button and a Quit button whose shortcut is ui_cancel
// (Escape). The menu system below reads the buttons' UI events, as gameplay code does.
//
// This program writes the TextureButton's three textures, checks the menu with ui.layout, then
// works it as an agent does over MCP or JSON-RPC: it hovers and clicks with input.inject, moves
// focus with the arrow keys, taps with a finger, presses Escape and calls ui.press, reading the
// UI events with events.read. Then it draws the menu into a texture.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace game {

// What the menu chose.
struct Menu {
    std::string difficulty = "normal";
    bool sound = true;
    bool playing = false;
    bool quitting = false;
};
SLASH_COMPONENT(Menu, difficulty, sound, playing, quitting)

} // namespace game

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

void write_png(const std::string& path, const render::Image& image) {
    const auto png = render::encode_png(image);
    if (!png) {
        check(png.error());
    }
    check(slash::platform::write_file(path, png.value()));
    std::printf("wrote %s\n", path.c_str());
}

// Games ship their own images; this example draws its three so it has no binary file: a
// speaker, the speaker crossed out, and a ring drawn over the button while it has focus.
void write_textures() {
    constexpr int kSize = 24;
    const auto speaker = [](int x, int y) {
        const bool box = x >= 3 && x < 9 && y >= 8 && y < 16;
        const bool cone = x >= 9 && x < 15 && std::abs((2 * y) - 23) <= (x - 9) * 2 + 8;
        return box || cone;
    };
    render::Image on{.width = kSize, .height = kSize, .pixels = {}};
    render::Image muted{.width = kSize, .height = kSize, .pixels = {}};
    render::Image ring{.width = kSize, .height = kSize, .pixels = {}};
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const bool shape = speaker(x, y);
            const bool wave = x >= 17 && x < 19 && y >= 7 && y < 17;
            const bool cross = x >= 15 && x < 23 &&
                               (std::abs(x - y - 3) <= 0 || std::abs((x - 15) - (16 - y)) <= 0);
            const std::uint8_t lit = shape || wave ? 255 : 0;
            on.pixels.insert(on.pixels.end(), {240, 240, 240, lit});
            if (cross && y >= 8 && y < 16) {
                muted.pixels.insert(muted.pixels.end(), {230, 70, 60, 255});
            } else {
                const std::uint8_t dim = shape ? 255 : 0;
                muted.pixels.insert(muted.pixels.end(), {150, 150, 150, dim});
            }
            const bool edge = x < 2 || y < 2 || x >= kSize - 2 || y >= kSize - 2;
            const std::uint8_t outline = edge ? 255 : 0;
            ring.pixels.insert(ring.pixels.end(), {250, 210, 80, outline});
        }
    }
    check(slash::platform::create_directories("ui"));
    write_png("ui/speaker.png", on);
    write_png("ui/muted.png", muted);
    write_png("ui/ring.png", ring);
}

// Reads the menu's UI events: which difficulty is on, whether the sound is muted, and whether
// Play or Quit was pressed. It runs after the gui system, so it sees this tick's events.
void menu(world::World& w, double /*tick_seconds*/) {
    auto* state = w.ecs().try_get<game::Menu>(w.find("screen"));
    if (state == nullptr) {
        return;
    }
    for (const ui::UiEvent& event : ui::events(w)) {
        const entt::entity button = w.find(event.id);
        if (event.kind == "toggled" && button != entt::null) {
            const auto* toggle = w.ecs().try_get<world::Button>(button);
            if (toggle != nullptr && toggle->button_group == "difficulty" && event.toggled_on) {
                state->difficulty = event.id;
            }
            if (event.id == "sound") {
                state->sound = !event.toggled_on;
            }
        }
    }
    state->playing = state->playing || ui::was_pressed(w, w.find("play"));
    state->quitting = state->quitting || ui::was_pressed(w, w.find("quit"));
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
    components.add<game::Menu>()
        .set_doc("What the title menu chose.")
        .describe("difficulty", {.doc = "The difficulty button that is on."})
        .describe("sound", {.doc = "Sound on."})
        .describe("playing", {.doc = "Play was pressed."})
        .describe("quitting", {.doc = "Quit was pressed."});
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    world::World& w = session.world();
    w.add_system("menu", menu);

    write_textures();
    call(session, "scene.open", R"({"path": "main.scene.json"})");
    w.ecs().emplace<game::Menu>(w.find("screen"));
    call(session, "events.subscribe",
         R"({"kinds": ["mouse_entered", "mouse_exited", "focus_entered", "focus_exited",)"
         R"( "button_down", "button_up", "pressed", "toggled"]})");

    // Where the buttons are and how each draws: Normal is on, so it draws pressed; Online is
    // disabled. The texture button is as big as its texture.
    call(session, "ui.layout",
         R"({"ids": ["play", "easy", "normal", "hard", "sound", "online", "quit"]})");

    // Move the mouse onto Hard and click it, all before one tick. The click gives it focus
    // (hidden until a key moves it), turns it on and Normal off.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_motion", "position": [228, 116]},
        {"type": "mouse_button", "button": "left", "position": [228, 116], "pressed": true},
        {"type": "mouse_button", "button": "left", "position": [228, 116], "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "ui.layout", R"({"ids": ["normal", "hard"]})");

    // The arrow keys move focus to the closest button that way, and show it: Left to Normal,
    // Up to Play. Enter presses the button with focus.
    call(session, "input.inject", R"({"events": [
        {"type": "key", "key": "Left", "pressed": true},
        {"type": "key", "key": "Left", "pressed": false},
        {"type": "key", "key": "Up", "pressed": true},
        {"type": "key", "key": "Up", "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "input.inject", R"({"events": [
        {"type": "key", "key": "Enter", "pressed": true},
        {"type": "key", "key": "Enter", "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");

    // A finger taps the speaker: it toggles on, which mutes the sound, and draws muted.png.
    call(session, "input.inject", R"({"events": [
        {"type": "screen_touch", "position": [72, 150], "pressed": true},
        {"type": "screen_touch", "position": [72, 150], "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");

    // Escape is Quit's shortcut, which lights the button up for a moment. ui.press presses a
    // button by ID, as a click would: Easy turns on and Hard off.
    call(session, "input.inject", R"({"events": [
        {"type": "key", "key": "Escape", "pressed": true},
        {"type": "key", "key": "Escape", "pressed": false}]})");
    call(session, "ui.press", R"({"id": "easy"})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "ui.layout", R"({"ids": ["easy", "hard", "sound", "quit"]})");
    call(session, "entity.get", R"({"id": "screen", "components": ["Menu"]})");

    // A disabled button can't be pressed, and a Label can't take focus.
    call(session, "ui.press", R"({"id": "online"})");
    call(session, "ui.focus", R"({"id": "title"})");
    call(session, "ui.focus", R"({"id": "play"})");

    // Draw into a 320x240 texture: style boxes per state, the focus outline and the textures.
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
    std::printf("controls drawn: %zu, boxes drawn: %zu, glyphs drawn: %zu, GPU draws: %llu\n",
                renderer.controls_drawn(), renderer.boxes_drawn(), renderer.glyphs_drawn(),
                static_cast<unsigned long long>(device->stats().draws));
    return 0;
}
