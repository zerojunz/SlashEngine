// 3D physics on Jolt, as Godot's: main.scene.json has a floor, a step, a ramp and a conveyor
// belt (StaticBody3D), a ball, a parcel and three crates (RigidBody3D), three coins (Area3D) and
// the hero (CharacterBody3D).
//
// The hero's controls are plain C++ systems, as a game's would be: gravity, walking along x, a
// jump when a wall is in the way, and coins taken when the hero touches them. Then the program
// asks the physics commands what is where, as an agent would.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace physics3d = slash::physics3d;
namespace world = slash::world;

constexpr float kGravity = 9.8F;   // meters per second squared
constexpr float kWalkSpeed = 3.0F; // meters per second
constexpr float kJumpSpeed = 4.5F;
constexpr float kFlag = 14.0F; // where the hero stops

// Gravity off the floor, walking to the flag, and a jump when a wall blocks the way, as Godot's
// CharacterBody3D template script does it. The physics3d system moves the hero by its velocity
// after this.
void control_hero(world::World& world, double seconds) {
    const entt::entity hero = world.find("hero");
    auto& body = world.ecs().get<world::CharacterBody3D>(hero);
    if (!body.on_floor) {
        body.velocity.y -= kGravity * static_cast<float>(seconds);
    }
    const float x = world.ecs().get<world::Transform3D>(hero).position.x;
    body.velocity.x = x < kFlag ? kWalkSpeed : 0.0F;
    if (body.on_floor && body.on_wall) {
        body.velocity.y = kJumpSpeed;
    }
}

// A coin the hero touches is taken, as Godot's get_overlapping_bodies would find it.
void take_coins(world::World& world, double /*seconds*/) {
    const entt::entity hero = world.find("hero");
    std::vector<entt::entity> taken;
    for (const entt::entity coin : world.ecs().view<world::Area3D>()) {
        const physics3d::Contacts contacts = physics3d::contacts(world, coin);
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

void print_position(const world::World& world, std::string_view id) {
    const auto& transform = world.ecs().get<world::Transform3D>(world.find(id));
    std::printf("  %.*s at (%.2f, %.2f, %.2f)\n", static_cast<int>(id.size()), id.data(),
                transform.position.x, transform.position.y, transform.position.z);
}

void print_hero(const world::World& world) {
    const entt::entity hero = world.find("hero");
    const auto& transform = world.ecs().get<world::Transform3D>(hero);
    const auto& body = world.ecs().get<world::CharacterBody3D>(hero);
    std::printf("tick %llu: hero at (%.2f, %.2f, %.2f)%s%s\n",
                static_cast<unsigned long long>(world.tick()), transform.position.x,
                transform.position.y, transform.position.z, body.on_floor ? ", on the floor" : "",
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
    // Gameplay systems run before the physics3d system, which steps at priority 950.
    session.world().add_system("control_hero", control_hero, {.doc = "Moves the hero."});
    session.world().add_system("take_coins", take_coins, {.doc = "Takes coins the hero touches."});
    call(session, "events.subscribe", R"({"kinds": ["coin_taken", "body_entered"]})");

    // Six seconds, a second at a time: the hero walks, takes the coins, jumps onto the step and
    // walks off it to the flag.
    for (int second = 0; second < 6; ++second) {
        session.call("step", json::parse(R"({"ticks": 60})").value());
        print_hero(session.world());
    }
    // The ball rolled down the ramp, the belt carried the parcel off its far end onto the floor
    // and the crates stayed stacked.
    for (const std::string_view id : {"ball", "parcel", "crate_1", "crate_2", "crate_3"}) {
        print_position(session.world(), id);
    }
    call(session, "events.read", R"({"subscription": "events_1"})");

    // What is under the hero, and how far down?
    call(session, "physics.raycast",
         R"({"from": [14, 2, 0], "to": [14, -2, 0], "exclude": ["hero"]})");
    // What does the hero touch, and what is at a point?
    call(session, "physics.contacts", R"({"id": "hero"})");
    call(session, "physics.overlap", R"({"point": [6, 2.5, 3]})");
    // What would a sphere a meter across, sunk a little into the floor at the foot of the stack,
    // touch?
    call(session, "physics.overlap",
         R"({"shape": {"shape": "sphere", "radius": 0.5}, "position": [6, 0.4, 2.2]})");
    // The parcel's gravity, and the project's damping.
    call(session, "physics.gravity", R"({"ids": ["parcel"]})");
    return 0;
}
