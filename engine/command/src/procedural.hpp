#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds texture.generate, over slash/procedural/texture.hpp.
void register_procedural_commands(Registry& registry);

} // namespace slash::command
