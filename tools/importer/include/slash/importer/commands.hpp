#pragma once

#include "slash/command/command.hpp"

namespace slash::importer {

// Adds project.import, which converts a game made with another engine, in a folder inside the
// session's project, into the project. It needs the filesystem capability.
void register_import_commands(command::Registry& registry);

} // namespace slash::importer
