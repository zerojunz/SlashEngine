#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds vehicle.place, vehicle.control, vehicle.tune, vehicle.check and vehicle.state, over
// .vehicle.json recipes (slash/world/vehicle.hpp) and the vehicles systems
// (slash/physics3d/vehicle_tool.hpp, slash/physics2d/vehicle_tool.hpp).
void register_vehicle_commands(Registry& registry);

} // namespace slash::command
