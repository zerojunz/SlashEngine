// Gravity zones and conveyors, as Godot's Area2D space overrides and StaticBody2D constant
// velocities. main.scene.json has two halves:
//
// - A factory: a conveyor belt (a StaticBody2D with constant_linear_velocity) carrying a crate
//   off its end, and the hero walking against the belt at its speed, so it stays where it is.
// - Space: an area that replaces gravity with none, a planet, and a second area of higher
//   priority that pulls toward the planet's center. Rocks beside and below the planet fall onto
//   it; a probe outside the pull floats.
//
// The hero's controls are a plain C++ system that adds the gravity where the hero is, as Godot's
// template script adds get_gravity(). Then the program asks physics.gravity what pulls each
// body, as an agent would, and turns space off.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace world = slash::world;

constexpr float kWalkSpeed = 60.0F; // pixels per second, the belt's speed

// Gravity off the floor, from the areas the hero is in, and walking left against the belt.
void control_hero(world::World& world, double seconds) {
    const entt::entity hero = world.find("hero");
    auto& body = world.ecs().get<world::CharacterBody2D>(hero);
    if (const auto gravity = physics2d::gravity(world, hero); gravity && !body.on_floor) {
        body.velocity = body.velocity + gravity->gravity * static_cast<float>(seconds);
    }
    body.velocity.x = -kWalkSpeed;
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

slash::Vec2 position(const world::World& world, std::string_view id) {
    return world.ecs().get<world::Transform2D>(world.find(id)).position;
}

void print_tick(const world::World& world) {
    const slash::Vec2 crate = position(world, "crate");
    const slash::Vec2 hero = position(world, "hero");
    const auto& body = world.ecs().get<world::CharacterBody2D>(world.find("hero"));
    std::printf("tick %llu: crate at (%.1f, %.1f); hero at (%.1f, %.1f)%s, carried at %.1f\n",
                static_cast<unsigned long long>(world.tick()), crate.x, crate.y, hero.x, hero.y,
                body.on_floor ? " on the floor" : "", body.platform_velocity.x);
    const slash::Vec2 planet = position(world, "planet");
    for (const std::string_view id : {"rock_left", "rock_below", "probe"}) {
        const slash::Vec2 at = position(world, id);
        std::printf("  %.*s at (%.1f, %.1f), %.1f from the planet's center\n",
                    static_cast<int>(id.size()), id.data(), at.x, at.y,
                    std::hypot(at.x - planet.x, at.y - planet.y));
    }
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

    // Three seconds: the belt carries the crate off its end while the hero walks in place, and
    // the rocks fall onto the planet.
    for (int second = 0; second < 3; ++second) {
        session.call("step", json::parse(R"({"ticks": 60})").value());
        print_tick(session.world());
    }

    // What pulls each body, and through which areas, highest priority first.
    call(session, "physics.gravity", R"({"ids": ["hero", "rock_below", "probe"]})");
    // The belt stays where it is: a ray down at its middle hits its top.
    call(session, "physics.raycast", R"({"from": [100, 150], "to": [100, 250]})");

    std::puts("-- with space's override off, the probe falls with the project's gravity");
    call(session, "component.set",
         R"({"id": "space", "components": {"Area2D": {"gravity_space_override": "disabled"}}})");
    session.call("step", json::parse(R"({"ticks": 60})").value());
    print_tick(session.world());
    call(session, "physics.gravity", R"({"ids": ["probe"]})");
    return 0;
}
