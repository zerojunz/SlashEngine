// Replays: a run recorded into a .replay.json file plays back exactly, so a player's bug report
// or an agent's play session can be stepped through again, inspected at any tick and kept as a
// test.
//
// The game is a hero who walks and jumps by input actions (project.json), with a crate and a
// coin in the way. The first session records a run while the player presses keys between ticks,
// as a window's events would arrive. The second, an agent's, plays it back, stops at the jump to
// look, then seeks the moment the coin was taken and the tick before it. The third has a lower
// jump, and the replay says where the run first went another way.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace world = slash::world;

constexpr float kGravity = 980.0F;  // pixels per second squared
constexpr float kWalkSpeed = 90.0F; // pixels per second
constexpr double kTick = 1.0 / 60.0;

// A session running the game, with the hero's controls as a plain C++ system.
struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Game(float jump_speed) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
        session->world().add_system(
            "control_hero",
            [jump_speed](world::World& w, double seconds) {
                const entt::entity hero = w.find("hero");
                if (hero == entt::null) {
                    return;
                }
                auto& body = w.ecs().get<world::CharacterBody2D>(hero);
                body.velocity.x = input::get_axis(w, "move_left", "move_right") * kWalkSpeed;
                if (!body.on_floor) {
                    body.velocity.y += kGravity * static_cast<float>(seconds);
                } else if (input::is_action_just_pressed(w, "jump")) {
                    body.velocity.y = -jump_speed;
                }
            },
            {.doc = "Walks and jumps the hero by its actions."});
        session->world().add_system(
            "take_coin",
            [](world::World& w, double /*seconds*/) {
                const entt::entity coin = w.find("coin");
                if (coin == entt::null) {
                    return;
                }
                const physics2d::Contacts contacts = physics2d::contacts(w, coin);
                if (std::ranges::find(contacts.bodies, w.find("hero")) != contacts.bodies.end()) {
                    w.destroy(coin);
                }
            },
            {.doc = "Takes the coin when the hero touches it."});
    }

    // Prints the call as an agent would make it, then the reply.
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        const command::Reply reply = session->call(name, value);
        json::Value shown = reply.ok ? reply.value : json::Value(reply.message);
        // Which components differ, without their values.
        json::Value* changes = shown.find("changes");
        if (json::Value* changed = changes != nullptr ? changes->find("changed") : nullptr;
            changed != nullptr) {
            for (auto& [id, entity] : changed->as_object()) {
                json::Array types;
                for (const auto& [type, fields] : entity.as_object()) {
                    types.emplace_back(type);
                }
                entity = std::move(types);
            }
        }
        std::puts(json::write(shown, json::Style::Compact).c_str());
    }

    void print_hero() const {
        const world::World& w = session->world();
        const entt::entity hero = w.find("hero");
        const auto& transform = w.ecs().get<world::Transform2D>(hero);
        const auto& body = w.ecs().get<world::CharacterBody2D>(hero);
        std::printf("tick %llu: hero at (%.1f, %.1f)%s, coin %s\n",
                    static_cast<unsigned long long>(w.tick()), transform.position.x,
                    transform.position.y, body.on_floor ? " on the floor" : " in the air",
                    w.find("coin") == entt::null ? "taken" : "still there");
    }
};

// What a window would queue for a key.
void key(world::World& w, input::Key physical_key, bool pressed) {
    input::Event event;
    event.type = input::EventType::Key;
    event.physical_key = physical_key;
    event.key = physical_key;
    event.pressed = pressed;
    input::push(w, event);
}

// The host's loop: queued input, then a tick.
void play(world::World& w, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        w.step(kTick);
    }
}

} // namespace

int main() {
    std::puts("-- the player's session records a run");
    Game player(380.0F);
    player.call("scene.open", R"({"path": "main.scene.json"})");
    player.call("replay.record", R"({"path": "replays/run.replay.json"})");
    world::World& w = player.session->world();
    play(w, 20); // the hero lands
    key(w, input::Key::D, true);
    play(w, 30); // walks right into the crate
    key(w, input::Key::Space, true);
    play(w, 2);
    key(w, input::Key::Space, false);
    play(w, 48); // jumps the crate and takes the coin
    key(w, input::Key::D, false);
    key(w, input::Key::A, true);
    play(w, 30); // turns back
    key(w, input::Key::A, false);
    play(w, 20);
    player.print_hero();
    player.call("replay.stop", "{}");

    // The file: the start, the ticks in runs of one length, what happened between them, a
    // hash of the world every 60 ticks and the end.
    const json::Value replay =
        json::parse(slash::platform::read_file("replays/run.replay.json").value()).value();
    std::printf("the replay holds %zu step run(s), %zu timeline entries and %zu checks\n",
                replay.find("steps")->size(), replay.find("timeline")->size(),
                replay.find("checks")->size());
    std::printf("its first entry: %s\n",
                json::write(replay.find("timeline")->as_array()[0], json::Style::Compact).c_str());

    std::puts("-- an agent's session plays it back");
    const Game agent(380.0F);
    agent.call("replay.run", R"({"path": "replays/run.replay.json"})");
    agent.print_hero();
    // Back to the moment of the jump, to look around.
    agent.call("replay.run", R"({"path": "replays/run.replay.json", "tick": 55})");
    agent.print_hero();
    agent.call("input.state", R"({"actions": ["jump", "move_right"]})");
    // The moment the coin went: the first tick with no Area2D left, then one tick back.
    agent.call("replay.seek", R"({"with": ["Area2D"], "until": "none"})");
    agent.print_hero();
    agent.call("replay.seek", R"({"ticks": -1})");
    agent.print_hero();

    // The hero can't clear the crate now: the replay says where it first differs, and what
    // differs at the end.
    std::puts("-- a session with a lower jump");
    const Game changed(150.0F);
    changed.call("replay.run", R"({"path": "replays/run.replay.json"})");
    changed.print_hero();
    return 0;
}
