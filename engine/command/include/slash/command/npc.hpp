#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds npc.plan, over slash/npc/npc.hpp.
void register_npc_commands(Registry& registry);

} // namespace slash::command
