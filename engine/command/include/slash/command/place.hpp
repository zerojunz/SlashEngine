#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds entity.place: scene assembly, placing 3D entities on the ground, apart from what is
// there and along a path, as agents build a level of library assets.
void register_place_commands(Registry& registry);

} // namespace slash::command
