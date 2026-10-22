#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds navigation.path, navigation.closest, navigation.random, navigation.map and
// navigation.agent, over slash/navigation/navigation.hpp.
void register_navigation_commands(Registry& registry);

} // namespace slash::command
