#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds lightmap.bake, over slash/scene3d/lightmap.hpp.
void register_lightmap_commands(Registry& registry);

} // namespace slash::command
