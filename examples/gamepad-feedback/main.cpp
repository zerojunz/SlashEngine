// Gamepad feedback: rumble, motion aiming and button prompts that match the controller.
//
// The game system below fires on the fire action and rumbles the gamepad that pressed it
// (input::rumble), turns the camera by the gyroscope while aim is held (input::gamepad_gyro),
// and writes the prompt for jump that a hint on screen would show (input::action_prompt), which
// follows whatever the player last used: a key, the mouse or a gamepad, labelled for its kind.
//
// This program plays it as an agent does over MCP or JSON-RPC: input.inject plugs in a
// controller, presses its buttons and tilts it, step runs the ticks, and input.state and
// input.actions show what a player would feel and see. In a window, SdlReader reports real
// controllers the same way and plays the rumble on them.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace world = slash::world;

// What the game keeps: the camera's turn in radians and the hint it shows.
struct Game {
    float yaw = 0.0F;
    std::string hint;
};
Game game;

void play(world::World& game_world, double tick_seconds) {
    const input::LastInput last = input::last_input(game_world);
    // A shot kicks the gamepad that fired it: both motors for a fifth of a second, and the
    // trigger's own motor where it has one.
    if (input::is_action_just_pressed(game_world, "fire") &&
        last.kind == input::InputKind::Gamepad) {
        input::rumble(game_world, last.device,
                      {.low = 0.75F, .high = 0.5F, .right_trigger = 1.0F, .seconds = 0.2F});
    }
    // Aiming turns with the controller: the gyroscope's turn rate about its y axis.
    if (input::is_action_pressed(game_world, "aim") && last.kind == input::InputKind::Gamepad) {
        game.yaw +=
            input::gamepad_gyro(game_world, last.device).y * static_cast<float>(tick_seconds);
    }
    if (const auto prompt = input::action_prompt(game_world, "jump")) {
        game.hint = "Press " + prompt->label.label + " to jump (" + prompt->label.glyph + ")";
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

void show() {
    std::printf("hint: %s; yaw %.3f\n", game.hint.c_str(), static_cast<double>(game.yaw));
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
    session.world().add_system("play", play);
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Before any gamepad, the hint is for the keyboard.
    call(session, "step", R"({"tick_seconds": 0.0625})");
    show();

    // A DualSense plugs in and the player presses Cross: the hint follows.
    call(session, "input.inject", R"({"events": [
        {"type": "joypad_connection", "connected": true, "name": "DualSense Wireless Controller",
         "gamepad": "ps5", "can_rumble": true, "has_motion": true},
        {"type": "joypad_button", "button": "a", "pressed": true},
        {"type": "joypad_button", "button": "a", "pressed": false}]})");
    call(session, "step", R"({"tick_seconds": 0.0625})");
    show();

    // R2 fires: the gamepad rumbles for 0.2 s, and four ticks of 1/16 s later it is still.
    call(session, "input.inject",
         R"({"events": [{"type": "joypad_motion", "axis": "trigger_right", "axis_value": 1}]})");
    call(session, "step", R"({"tick_seconds": 0.0625})");
    call(session, "input.state", R"({"actions": ["fire"]})");
    call(session, "input.inject",
         R"({"events": [{"type": "joypad_motion", "axis": "trigger_right", "axis_value": 0}]})");
    call(session, "step", R"({"ticks": 4, "tick_seconds": 0.0625})");
    call(session, "input.state", R"({"actions": ["fire"]})");

    // Holding L2 and turning the controller at 2 radians a second for a quarter second aims a
    // half radian over.
    call(session, "input.inject", R"({"events": [
        {"type": "joypad_motion", "axis": "trigger_left", "axis_value": 1},
        {"type": "joypad_sensor", "gyro": [0, 2, 0], "accel": [0, 9.81, 0]}]})");
    call(session, "step", R"({"ticks": 4, "tick_seconds": 0.0625})");
    show();

    // The prompts a menu would show, for the controller in use and for others.
    call(session, "input.actions", R"({"names": ["aim", "fire", "jump"], "prompt": "last"})");
    call(session, "input.actions", R"({"names": ["jump"], "prompt": "switch_pro"})");

    // The mouse takes over, and the controller is unplugged.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "left", "position": [10, 10], "pressed": true},
        {"type": "joypad_connection", "connected": false}]})");
    call(session, "step", R"({"tick_seconds": 0.0625})");
    show();
    call(session, "input.state", R"({"actions": ["aim"]})");
    return 0;
}
