#pragma once

#include "slash/command/command.hpp"

namespace slash::exporter {

// Adds export.run, which exports the session's project with a preset or a platform's defaults,
// and gameplay.build, which compiles its gameplay code and swaps it into the session's world
// (SessionOptions::gameplay). They need the build and filesystem capabilities. Tools register
// them next to the built-in commands; a running game doesn't, since a build would stall its loop.
// A running game reloads the gameplay library when another program rebuilds it.
void register_export_commands(command::Registry& registry);

} // namespace slash::exporter
