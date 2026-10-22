// 3D joints and a car on Jolt, as Godot's: main.scene.json hangs a lamp from a pin and a bell from
// a cone twist joint, puts a door on a hinge and a drawer on a slider, and parks a car on four
// wheels.
//
// The program drives them as an agent would, with component.set: it turns on the door's motor,
// then gives the car throttle, steering and the brake, a few seconds at a time, and prints where
// everything got to.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

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

void step(command::Session& session, int ticks) {
    session.call("step", json::parse(R"({"ticks": )" + std::to_string(ticks) + "}").value());
}

const world::Transform3D& transform_of(const world::World& world, std::string_view id) {
    return world.ecs().get<world::Transform3D>(world.find(id));
}

void print_props(const world::World& world) {
    const world::Transform3D& lamp = transform_of(world, "lamp");
    const world::Transform3D& bell = transform_of(world, "bell");
    const world::Transform3D& door = transform_of(world, "door");
    const world::Transform3D& drawer = transform_of(world, "drawer");
    std::printf("  lamp at (%.2f, %.2f, %.2f), bell at (%.2f, %.2f, %.2f)\n", lamp.position.x,
                lamp.position.y, lamp.position.z, bell.position.x, bell.position.y,
                bell.position.z);
    std::printf("  door turned %.2f rad, drawer at x %.2f\n", door.rotation.y, drawer.position.x);
}

void print_car(const world::World& world) {
    const world::Transform3D& car = transform_of(world, "car");
    const slash::Vec3 velocity =
        world.ecs().get<world::RigidBody3D>(world.find("car")).linear_velocity;
    const float speed = std::sqrt((velocity.x * velocity.x) + (velocity.z * velocity.z));
    std::printf("tick %llu: car at (%.2f, %.2f, %.2f), heading %.2f rad, %.2f m/s\n",
                static_cast<unsigned long long>(world.tick()), car.position.x, car.position.y,
                car.position.z, car.rotation.y, speed);
    for (const std::string_view id : {"front_left", "rear_left"}) {
        const auto& wheel = world.ecs().get<world::VehicleWheel3D>(world.find(id));
        std::printf("  %.*s: %s, suspension %.2f m, %.0f rpm, grip %.2f\n",
                    static_cast<int>(id.size()), id.data(),
                    wheel.in_contact ? "on the ground" : "in the air", wheel.suspension_length,
                    wheel.rpm, wheel.skid_info);
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

    // A second: the car settles on its springs, the lamp and bell swing and the drawer slides
    // to its stop.
    step(session, 60);
    print_car(session.world());
    print_props(session.world());

    // The door's motor opens it until its upper limit stops it.
    call(session, "component.set",
         R"({"id": "door_hinge", "components": {"HingeJoint3D": {"motor_enabled": true}}})");
    // Throttle for two seconds: the rear wheels drive the car toward +z.
    call(session, "component.set",
         R"({"id": "car", "components": {"VehicleBody3D": {"engine_force": 60}}})");
    step(session, 120);
    print_car(session.world());

    // Steering left (toward +x) for two seconds, still on the throttle.
    call(session, "component.set",
         R"({"id": "car", "components": {"VehicleBody3D": {"steering": 0.3}}})");
    step(session, 120);
    print_car(session.world());

    // Off the throttle, wheels straight and the brake on: the car stops.
    call(
        session, "component.set",
        R"({"id": "car", "components": {"VehicleBody3D": {"engine_force": 0, "steering": 0, "brake": 2}}})");
    step(session, 120);
    print_car(session.world());
    print_props(session.world());
    return 0;
}
