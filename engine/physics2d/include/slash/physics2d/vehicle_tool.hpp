#pragma once

// Vehicles in 2D, seen from the side, after Unreal's Chaos Vehicles and Godot's 2D car demos: the
// "vehicles_2d" system (priority 945, before the physics step) keeps each Vehicle2D's rig as its
// .vehicle.json recipe's side view says (slash/world/vehicle.hpp's vehicle_side_view): the
// RigidBody2D's mass and center of mass, a collision box child "<id>_collision", a Polygon2D child
// "<id>_<piece>" for each piece of its body, and for each wheel a RigidBody2D circle
// "<id>_wheel_<n>" held under it by a GrooveJoint2D ("<id>_wheel_<n>_groove") and sprung by a
// DampedSpringJoint2D ("<id>_wheel_<n>_spring"). It drives the wheels with the recipe's
// drivetrain from the controls, floats boats on their water (at y = -water_level pixels), flies
// aircraft, carries riders (VehicleRider) and hits characters and creatures as Vehicle3D does,
// pixels_per_meter big.

#include "slash/core/result.hpp"
#include "slash/world/vehicle.hpp"
#include "slash/world/world.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace slash::physics2d {

// Adds the vehicles_2d system to a world (install does).
void add_vehicle_system(world::World& world);

// A Vehicle2D's recipe with its overrides, as the system drives it.
[[nodiscard]] Result<std::shared_ptr<const world::VehicleRecipe>>
vehicle_recipe(const world::World& world, entt::entity vehicle);

// Seats someone (a CharacterBody2D, or anything with a Transform2D) in a Vehicle2D: in the seat
// named, or the first free one (the driver's first). Their body stops colliding while they ride.
[[nodiscard]] Result<std::string> enter_vehicle(world::World& world, entt::entity rider,
                                                entt::entity vehicle, std::string_view seat = {});

// Gets someone out of the Vehicle2D they ride, standing beside it, their collision as it was.
[[nodiscard]] Result<void> exit_vehicle(world::World& world, entt::entity rider);

} // namespace slash::physics2d
