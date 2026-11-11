#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds physics.debug, over slash/world/physics_debug.hpp (the visual debugger's record of
// contacts, joints and fields per step), and physics.cache, over slash/world/physics_cache.hpp
// (simulations recorded as animations), in 3D and 2D.
void register_physics_debug_commands(Registry& registry);

} // namespace slash::command
