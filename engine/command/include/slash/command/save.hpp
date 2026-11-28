#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds save.list, save.read, save.write and save.delete, over the project's save folder
// (slash/platform/save_data.hpp: <project>/.slash/saves/), where game code saves in sessions and
// headless runs.
void register_save_commands(Registry& registry);

} // namespace slash::command
