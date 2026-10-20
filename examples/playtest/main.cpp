// Playtests and tuning sweeps: many headless runs of a game in one call.
//
// Is the jump high enough to clear the pit? A run or two by hand says little, since it depends on
// when the player jumps. playtest.run plays many copies of the game at once, each with a value
// from vary set, a bot pressing input actions, and end conditions, and answers with a summary per
// value: how the runs ended, how many ticks they took, the events they emitted and the values
// they ended with.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/input/input.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace game {

struct Runner {
    float speed = 192.0F;      // pixels per second while move_right is held
    float jump_speed = 400.0F; // upward speed a jump starts with
    float velocity_y = 0.0F;
    bool on_floor = true;
    int coins = 0;
};
SLASH_COMPONENT(Runner, speed, jump_speed, velocity_y, on_floor, coins)

struct Coin {
    int value = 1;
};
SLASH_COMPONENT(Coin, value)

} // namespace game

namespace {

namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace world = slash::world;

constexpr float kGravity = 1280.0F; // pixels per second squared, y down
constexpr float kPitStart = 300.0F; // no floor from here...
constexpr float kPitEnd = 420.0F;   // ...to here

// Jumps, runs, falls and lands on the floor, which has a pit in it; picks up coins it touches.
void run(world::World& w, double tick_seconds) {
    const auto seconds = static_cast<float>(tick_seconds);
    std::vector<entt::entity> taken;
    for (const entt::entity entity : w.ecs().view<world::Transform2D, game::Runner>()) {
        auto& at = w.ecs().get<world::Transform2D>(entity).position;
        auto& runner = w.ecs().get<game::Runner>(entity);
        if (runner.on_floor && input::is_action_just_pressed(w, "jump")) {
            runner.velocity_y = -runner.jump_speed;
            runner.on_floor = false;
            w.emit("jumped", w.id_of(entity));
        }
        if (input::is_action_pressed(w, "move_right")) {
            at.x += runner.speed * seconds;
        }
        const float before = at.y;
        runner.velocity_y += kGravity * seconds;
        at.y += runner.velocity_y * seconds;
        // Lands only coming down onto the floor, not from inside the pit.
        const bool over_floor = at.x < kPitStart || at.x >= kPitEnd;
        runner.on_floor = over_floor && at.y >= 0.0F && before <= 0.0F;
        if (runner.on_floor) {
            at.y = 0.0F;
            runner.velocity_y = 0.0F;
        }
        for (const entt::entity coin : w.ecs().view<world::Transform2D, game::Coin>()) {
            const auto& spot = w.ecs().get<world::Transform2D>(coin).position;
            if (std::abs(spot.x - at.x) < 3.0F && std::abs(spot.y - at.y) < 24.0F) {
                runner.coins += w.ecs().get<game::Coin>(coin).value;
                taken.push_back(coin);
            }
        }
    }
    for (const entt::entity coin : taken) {
        w.emit("coin_collected", w.id_of(coin));
        w.destroy(coin);
    }
}

// The game's own code, as its SLASH_GAME_MODULE function would be. Every run gets its systems.
void module(world::GameModule& code) {
    code.add_component<game::Runner>().set_doc("Runs right and jumps over the pit.");
    code.add_component<game::Coin>().set_doc("Picked up when touched.");
    code.add_system("run", run, {.doc = "Moves runners and picks up coins."});
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
    slash::gameplay::Host code(components, module);
    command::SessionOptions options;
    options.capture_logs = false;
    options.gameplay = &code;
    // The session, and each run, reads project.json's actions.
    command::Session session(components, commands, options);

    // The level, with the game's components added and not saved: runs start from the world as
    // it is now.
    call(session, "scene.open", R"({"path": "level.scene.json"})");
    call(session, "batch", R"({"commands": [
        {"command": "component.set", "params": {"id": "runner", "components": {"Runner": {}}}},
        {"command": "component.set", "params": {"id": "coin_1", "components": {"Coin": {}}}},
        {"command": "component.set",
         "params": {"id": "coin_2", "components": {"Coin": {"value": 5}}}},
        {"command": "component.set", "params": {"id": "coin_3", "components": {"Coin": {}}}}]})");

    // Three jump speeds, sixteen runs of each, with a bot that holds move_right and jumps at
    // random, differently in each run: 400 never clears the 120-pixel pit, and the higher the
    // jump, the more runs clear it. Ticks are 1/64 s so every value is exact.
    call(session, "playtest.run", R"({"vary": {"runner/Runner/jump_speed": [400, 500, 600]},
        "runs": 16, "bot": [{"action": "move_right"}, {"action": "jump", "chance": 0.04}],
        "end": {"cleared": {"with": ["Runner"],
                    "where": [{"component": "Transform2D", "field": "position.x", "op": ">=",
                               "value": 600}]},
                "fell": {"with": ["Runner"],
                    "where": [{"component": "Transform2D", "field": "position.y", "op": ">",
                               "value": 64}]}},
        "ticks": 400, "tick_seconds": 0.015625, "measure": ["runner/Runner/coins"]})");

    // A bot that jumps at the edge, in the 18 pixels before the pit, and plays the same every
    // run. Now the only question is the jump.
    call(session, "playtest.run", R"({"vary": {"runner/Runner/jump_speed": [400, 500]},
        "bot": [{"action": "move_right"},
                {"action": "jump", "when": {"with": ["Runner"], "where": [
                    {"component": "Runner", "field": "on_floor", "value": true},
                    {"component": "Transform2D", "field": "position.x", "op": ">=",
                     "value": 282},
                    {"component": "Transform2D", "field": "position.x", "op": "<",
                     "value": 300}]}}],
        "end": {"cleared": {"with": ["Runner"],
                    "where": [{"component": "Transform2D", "field": "position.x", "op": ">=",
                               "value": 600}]},
                "fell": {"with": ["Runner"],
                    "where": [{"component": "Transform2D", "field": "position.y", "op": ">",
                               "value": 64}]}},
        "ticks": 400, "tick_seconds": 0.015625,
        "measure": ["runner/Transform2D/position/x", "*/Coin"]})");

    // The runs had worlds of their own: this one is where it was.
    call(session, "entity.get", R"({"id": "runner", "components": ["Transform2D"],
        "omit_defaults": true})");

    // A misspelled action is caught before anything runs.
    call(session, "playtest.run", R"({"bot": [{"action": "jmup"}]})");
    return 0;
}
