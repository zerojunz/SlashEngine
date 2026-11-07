#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds lighting.check, which measures a 3D frame's exposure and what each light lights.
void register_lighting_commands(Registry& registry);

} // namespace slash::command
