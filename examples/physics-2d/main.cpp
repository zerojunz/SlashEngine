// 2D physics, as Godot's: main.scene.json has ground and walls (StaticBody2D), a hill whose
// outline is a concave CollisionPolygon2D, a one-way ledge, a crate (RigidBody2D), a lamp
// swinging on a PinJoint2D, three coins (Area2D) and the hero (CharacterBody2D).
//
// The hero's controls are plain C++ systems, as a game's would be: gravity, walking right, a
// jump when a wall is in the way, and coins taken when the hero touches them. Then the program
// asks the physics commands what is where, as an agent would.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace world = slash::world;

constexpr float kGravity = 980.0F;  // pixels per second squared
constexpr float kWalkSpeed = 90.0F; // pixels per second
constexpr float kJumpSpeed = 380.0F;
constexpr float kFlag = 420.0F; // where the hero stops

// Gravity off the floor, walking right to the flag, and a jump when a wall blocks the way, as
// Godot's CharacterBody2D template script does it. The physics2d system moves the hero by its
// velocity after this.
void control_hero(world::World& world, double seconds) {
    const entt::entity hero = world.find("hero");
    auto& body = world.ecs().get<world::CharacterBody2D>(hero);
    if (!body.on_floor) {
        body.velocity.y += kGravity * static_cast<float>(seconds);
    }
    const float x = world.ecs().get<world::Transform2D>(hero).position.x;
    body.velocity.x = x < kFlag ? kWalkSpeed : 0.0F;
    if (body.on_floor && body.on_wall) {
        body.velocity.y = -kJumpSpeed;
    }
}

// A coin the hero touches is taken, as Godot's get_overlapping_bodies would find it.
void take_coins(world::World& world, double /*seconds*/) {
    const entt::entity hero = world.find("hero");
    std::vector<entt::entity> taken;
    for (const entt::entity coin : world.ecs().view<world::Area2D>()) {
        const physics2d::Contacts contacts = physics2d::contacts(world, coin);
        if (std::ranges::find(contacts.bodies, hero) != contacts.bodies.end()) {
            taken.push_back(coin);
        }
    }
    for (const entt::entity coin : taken) {
        world.emit("coin_taken", world.id_of(coin), {});
        world.destroy(coin);
    }
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

void print_hero(const world::World& world) {
    const entt::entity hero = world.find("hero");
    const auto& transform = world.ecs().get<world::Transform2D>(hero);
    const auto& body = world.ecs().get<world::CharacterBody2D>(hero);
    std::printf("tick %llu: hero at (%.1f, %.1f)%s%s\n",
                static_cast<unsigned long long>(world.tick()), transform.position.x,
                transform.position.y, body.on_floor ? ", on the floor" : "",
                body.on_wall ? ", at a wall" : "");
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

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    // Gameplay systems run before the physics2d system, which steps at priority 950.
    session.world().add_system("control_hero", control_hero, {.doc = "Moves the hero."});
    session.world().add_system("take_coins", take_coins, {.doc = "Takes coins the hero touches."});
    call(session, "events.subscribe", R"({"kinds": ["coin_taken"]})");

    // Six seconds, a second at a time: the hero walks, jumps the crate and up through the ledge,
    // climbs the hill and jumps up its step.
    for (int second = 0; second < 6; ++second) {
        session.call("step", json::parse(R"({"ticks": 60})").value());
        print_hero(session.world());
    }
    call(session, "events.read", R"({"subscription": "events_1"})");

    // What is under the hero, and how far down? The ray starts inside the hero, which it
    // wouldn't hit anyway, but exclude says so.
    call(session, "physics.raycast",
         R"({"from": [420, 189], "to": [420, 300], "exclude": ["hero"]})");
    // What does the hero touch, and what is at a point?
    call(session, "physics.contacts", R"({"id": "hero"})");
    call(session, "physics.overlap", R"({"point": [150, 240]})");
    // What would a 40-pixel box over the ledge overlap? The ledge, and the coin there was taken.
    call(session, "physics.overlap",
         R"({"shape": {"size": [40, 40]}, "position": [190, 175], "collide_with_areas": true})");

    // The lamp swings, and its pin keeps it 40 pixels away.
    const world::World& world = session.world();
    const slash::Vec2 lamp = world.ecs().get<world::Transform2D>(world.find("lamp")).position;
    const slash::Vec2 pin = world.ecs().get<world::Transform2D>(world.find("lamp_pin")).position;
    std::printf("lamp at (%.1f, %.1f), %.1f pixels from its pin\n", lamp.x, lamp.y,
                std::hypot(lamp.x - pin.x, lamp.y - pin.y));
    return 0;
}
