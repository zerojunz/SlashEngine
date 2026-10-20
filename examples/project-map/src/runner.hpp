#pragma once

#include "slash/world/world.hpp"

namespace game {

// Runs with the arrow keys and jumps with the jump action.
void move_runner(slash::world::World& world, double tick_seconds);

} // namespace game
