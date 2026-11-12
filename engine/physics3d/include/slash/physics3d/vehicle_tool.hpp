#pragma once

// Vehicles in 3D, after Unreal's Chaos Vehicles: the "vehicles_3d" system (priority 945, before
// the physics step) keeps each Vehicle3D's rig as its .vehicle.json recipe says (the RigidBody3D's
// mass and center of mass, a VehicleBody3D with a VehicleWheel3D child "<id>_wheel_<n>" per wheel
// for the kinds that roll, each drawn by a "<id>_wheel_<n>_tire" child), drives it with the
// recipe's drivetrain (slash/world/vehicle.hpp's drive_step) from its controls, skid-steers
// tracked vehicles, floats boats on their water level with thrust and a rudder, flies aircraft by
// lift, drag and control surfaces, keeps bikes upright, carries its riders (VehicleRider) in their
// seats, and hits characters and creatures it runs into by their contact impulse: a vehicle_hit
// event, a push, damage by the recipe and a knockdown for a Ragdoll3D hit hard enough.

#include "slash/core/result.hpp"
#include "slash/world/vehicle.hpp"
#include "slash/world/world.hpp"

#include <memory>
#include <string_view>

namespace slash::physics3d {

// Adds the vehicles_3d system to a world (install does).
void add_vehicle_system(world::World& world);

// A Vehicle3D's recipe with its overrides, as the system drives it.
[[nodiscard]] Result<std::shared_ptr<const world::VehicleRecipe>>
vehicle_recipe(const world::World& world, entt::entity vehicle);

// Seats someone (a CharacterBody3D, or anything with a Transform3D) in a vehicle: in the seat
// named, or the first free one (the driver's first). Their body stops colliding while they ride.
// Fails when the seat is taken or there is none free.
[[nodiscard]] Result<std::string> enter_vehicle(world::World& world, entt::entity rider,
                                                entt::entity vehicle, std::string_view seat = {});

// Gets someone out of the vehicle they ride, beside their seat's door (or their seat), their
// body's collision as it was.
[[nodiscard]] Result<void> exit_vehicle(world::World& world, entt::entity rider);

} // namespace slash::physics3d
