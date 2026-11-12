#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds character.generate, character.check and character.place, over .character.json recipes
// (slash/world/character.hpp) and the models they build (slash/scene3d/character.hpp).
void register_character_commands(Registry& registry);

} // namespace slash::command
