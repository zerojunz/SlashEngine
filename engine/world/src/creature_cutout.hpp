#pragma once

// What the character_2d system calls to keep Creature2Ds' cut-outs as it keeps Character2Ds'
// (src/creature.cpp).

#include "slash/world/world.hpp"

namespace slash::world::detail {

// Gives the world the state creatures' cut-outs keep, once.
void prepare_creature_cutouts(World& world);
// Makes each Creature2D's bone children, or makes them again when its recipe changes, and
// mirrors it by its flip_h.
void keep_creature_cutouts(World& world);

} // namespace slash::world::detail
