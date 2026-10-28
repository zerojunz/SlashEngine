#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds model.cleanup, over slash/scene3d/cleanup.hpp, and model.retarget, over
// slash/scene3d/humanoid.hpp.
void register_model_cleanup_commands(Registry& registry);

} // namespace slash::command
