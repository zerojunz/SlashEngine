#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds ragdoll.generate, ragdoll.hit and ragdoll.tune, over slash/scene3d/ragdoll.hpp.
void register_ragdoll_commands(Registry& registry);

} // namespace slash::command
