#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds creature.place, creature.populate, creature.interact and creature.state, over
// .creature.json recipes (slash/world/creature.hpp) and the fauna system (slash/npc/fauna.hpp),
// and animation.preview and animation.tune, which measure and tune their animations' motion.
void register_creature_commands(Registry& registry);

} // namespace slash::command
