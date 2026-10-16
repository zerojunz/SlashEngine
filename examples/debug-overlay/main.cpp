// The debug overlay and collision shapes: what a person watching a running game sees, and what
// an agent sees of the same shapes in a captured frame.
//
// The scene has ground, a hill whose outline is a concave CollisionPolygon2D, a one-way ledge, a
// crate that falls onto it, a coin (an area) and a disabled gate. An agent first captures a frame
// with the shapes drawn, as frame.capture's "collisions" draws them. Then a person runs the game
// as slash-cli run --debug-overlay shows it: the host hands each input event to the overlay
// first, runs the overlay each tick before the game, and draws its panel over the frame. The
// person moves the mouse over the crate, pauses, steps twice, turns the shapes on and plays on.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/debug/overlay.hpp"
#include "slash/input/input.hpp"
#include "slash/render/device.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace debug = slash::debug;
namespace input = slash::input;
namespace json = slash::json;
namespace render = slash::render;
namespace ui = slash::ui;
namespace world = slash::world;
using slash::Vec2;

constexpr double kTick = 1.0 / 60.0;

// A running game, as slash-cli run has it: the session with play and pause, a canvas (on the
// null device, which counts what it would draw) and the overlay.
struct Host {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;
    std::unique_ptr<render::RenderDevice> device = render::create_null_device();
    std::unique_ptr<canvas::CanvasRenderer> renderer;
    std::unique_ptr<debug::Overlay> overlay;

    Host() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
        renderer = std::make_unique<canvas::CanvasRenderer>(*device, session->assets());
        overlay = std::make_unique<debug::Overlay>(session->assets());
        debug::register_commands(commands, *overlay);
    }

    // Prints the call as an agent would make it, then the reply.
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        const command::Reply reply = session->call(name, value);
        std::puts(
            json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                .c_str());
    }

    [[nodiscard]] Vec2 viewport() const { return ui::settings(session->world()).viewport; }

    // A window event: the overlay sees it first, and the game gets what it doesn't take.
    void send(const input::Event& event) const {
        if (!overlay->handle(event)) {
            input::push(session->world(), event);
        }
    }

    // One tick of the host's loop and the frame after it.
    void frame() const {
        overlay->update(*session, {.fps = 60.0, .frame_ms = 16.7, .tick_seconds = kTick}, *renderer,
                        viewport());
        if (session->playing()) {
            session->world().step(kTick);
        }
        canvas::CanvasOptions options;
        options.collisions = overlay->collisions();
        options.overlay = overlay->world();
        (void)renderer->draw(session->world(), options);
    }

    void frames(int count) const {
        for (int i = 0; i < count; ++i) {
            frame();
        }
    }

    // The mouse moving to a point.
    void move(Vec2 to) const {
        input::Event motion;
        motion.type = input::EventType::MouseMotion;
        motion.position = to;
        send(motion);
    }

    // A click on one of the panel's buttons, where it is on screen.
    void click(std::string_view button) const {
        const world::World& panel = *overlay->world();
        const ui::Layout placed = ui::layout(panel, *ui::fonts(panel), viewport());
        const ui::Placed* control = placed.find(panel.find(button));
        input::Event press;
        press.type = input::EventType::MouseButton;
        press.button = input::MouseButton::Left;
        press.position = control->transform.apply(control->size * 0.5F);
        press.pressed = true;
        send(press);
        input::Event release = press;
        release.pressed = false;
        send(release);
    }

    void key(input::Key key, bool pressed) const {
        input::Event event;
        event.type = input::EventType::Key;
        event.key = key;
        event.physical_key = key;
        event.pressed = pressed;
        send(event);
    }

    // What the panel shows.
    void print_panel() const {
        for (const std::string& line : overlay->lines()) {
            std::printf("  | %s\n", line.c_str());
        }
    }
};

} // namespace

int main() {
    const Host host;
    host.call("scene.open", R"({"path": "main.scene.json"})");

    std::puts("-- an agent looks at the collision shapes in a captured frame");
    host.call("step", R"({"ticks": 45})");
    host.call("frame.capture", R"({"points": [[360, 204], [600, 40]]})");
    // The crate on the ledge, the ledge's one-way arrow, the hill, the coin, the disabled gate
    // and the sky.
    host.call("frame.capture", R"({"collisions": true,
                                   "points": [[360, 204], [360, 236], [480, 300], [240, 260],
                                              [600, 280], [600, 40]]})");

    std::puts("-- a person watching the game presses F3");
    host.session->set_playing(true);
    host.key(input::Key::F3, true);
    host.key(input::Key::F3, false);
    host.frames(2);
    host.print_panel();

    std::puts("-- the mouse moves over the crate");
    host.move({360.0F, 204.0F});
    host.frames(1);
    host.print_panel();

    std::puts("-- Pause, then Step twice");
    host.click("play");
    host.frames(10);
    host.print_panel();
    host.click("step");
    host.frames(1);
    host.click("step");
    host.frames(1);
    host.print_panel();
    host.call("history.get", "{}");

    std::puts("-- Shapes, then Play");
    host.click("shapes");
    host.frames(2);
    host.click("play");
    host.frames(2);
    host.print_panel();

    std::puts("-- an agent hides the panel and leaves the shapes on");
    host.call("debug.overlay", R"({"visible": false})");
    host.frames(1);
    std::printf("shapes drawn: %zu\n", host.renderer->shapes_drawn());
    return 0;
}
