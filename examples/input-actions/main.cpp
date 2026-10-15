// Input actions, as Godot's input map: project.json names what the game does (move_left, jump,
// move_to) and the keys, mouse buttons, gamepad buttons and sticks that do it, on top of Godot's
// built-in ui_* actions. The player system below reads actions, never keys, so a player can
// rebind them and an agent can press them by name.
//
// This program adds that system, then plays the game as an agent does over MCP or JSON-RPC:
// input.actions shows the bindings, input.inject sends a key, half a stick, a tap and a touch,
// step runs the ticks they apply at, and input.state and entity.get show what the game saw.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/input/input.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace game {

struct Player {
    int jumps = 0;
    slash::Vec2 target; // where the last click or tap was
};
SLASH_COMPONENT(Player, jumps, target)

} // namespace game

namespace {

namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace world = slash::world;

constexpr float kSpeed = 128.0F; // pixels per second with a key or the stick all the way over

// Moves the player by the move_* actions, counts jumps and remembers where move_to clicked.
void player(world::World& game_world, double tick_seconds) {
    // At most 1 long, with the actions' deadzone: half a stick past it moves at half speed.
    const slash::Vec2 direction =
        input::get_vector(game_world, "move_left", "move_right", "move_up", "move_down");
    const float step = kSpeed * static_cast<float>(tick_seconds);
    game_world.ecs().view<world::Transform2D, game::Player>().each(
        [&](world::Transform2D& transform, game::Player& state) {
            transform.position = transform.position + (direction * step);
            // Just pressed is true for one tick, so a jump never repeats while the key is held.
            if (input::is_action_just_pressed(game_world, "jump")) {
                ++state.jumps;
            }
            if (input::is_action_just_pressed(game_world, "move_to")) {
                state.target = input::mouse_position(game_world);
            }
        });
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
    components.add<game::Player>()
        .set_doc("The player's jumps and where they last clicked.")
        .describe("jumps", {.doc = "Times jumped."})
        .describe("target", {.doc = "Where the last click or tap was."});
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    // The session reads project.json's actions.
    command::Session session(components, commands, options);
    world::World& w = session.world();
    w.add_system("player", player);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    w.ecs().emplace<game::Player>(w.find("player"));
    call(session, "input.actions", R"({"names": ["move_right", "jump"]})");

    // Hold D for eight ticks of 1/64 s: 2 pixels a tick to the right.
    call(session, "input.inject",
         R"({"events": [{"type": "key", "physical_key": "D", "pressed": true}]})");
    call(session, "step", R"({"ticks": 8, "tick_seconds": 0.015625})");
    call(session, "input.state", "{}");
    call(session, "entity.get", R"({"id": "player", "omit_defaults": true})");

    // Let go of D, push the stick halfway up and tap Space, all before one tick. Halfway past
    // the 0.25 deadzone reads as 0.5; the tap is both just pressed and just released.
    call(session, "input.inject", R"({"events": [
        {"type": "key", "physical_key": "D", "pressed": false},
        {"type": "joypad_motion", "axis": "left_y", "axis_value": -0.625},
        {"type": "key", "physical_key": "Space", "pressed": true},
        {"type": "key", "physical_key": "Space", "pressed": false}]})");
    call(session, "step", R"({"tick_seconds": 0.015625})");
    call(session, "input.state", R"({"actions": ["jump", "move_right", "move_up"]})");
    call(session, "step", R"({"ticks": 3, "tick_seconds": 0.015625})");
    call(session, "entity.get", R"({"id": "player", "omit_defaults": true})");

    // A touch clicks the left mouse button too, as Godot's emulate_mouse_from_touch, so move_to
    // works on phones. Let go of the stick in the same call.
    call(session, "input.inject", R"({"events": [
        {"type": "joypad_motion", "axis": "left_y", "axis_value": 0},
        {"type": "screen_touch", "position": [40, 200], "pressed": true},
        {"type": "screen_touch", "position": [40, 200], "pressed": false}]})");
    call(session, "step", R"({"tick_seconds": 0.015625})");
    call(session, "entity.get", R"({"id": "player", "components": ["Player"]})");

    // A misspelled action is caught before anything is queued.
    call(session, "input.inject",
         R"({"events": [{"type": "action", "action": "jmup", "pressed": true}]})");

    // Gameplay code rebinds actions, e.g. from an options menu: K jumps too now. Actions can be
    // pressed by name, as the agent's {"type": "action"} events do.
    input::Event k_key;
    k_key.type = input::EventType::Key;
    k_key.device = input::kAllDevices; // from any keyboard
    k_key.key = input::Key::K;
    input::map(w)->action_add_event("jump", k_key);
    std::puts("rebound jump to K too");
    call(session, "input.inject", R"({"events": [
        {"type": "key", "key": "K", "pressed": true},
        {"type": "key", "key": "K", "pressed": false},
        {"type": "action", "action": "move_right", "strength": 0.625, "pressed": true}]})");
    call(session, "step", R"({"tick_seconds": 0.015625})");
    call(session, "input.state", "{}");
    call(session, "entity.get", R"({"id": "player", "omit_defaults": true})");
    return 0;
}
