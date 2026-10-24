#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds terrain.get, terrain.generate and terrain.brush, over slash/scene3d/terrain.hpp.
void register_terrain_commands(Registry& registry);

} // namespace slash::command
