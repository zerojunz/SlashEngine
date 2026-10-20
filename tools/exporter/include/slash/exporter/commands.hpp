#pragma once

#include "slash/command/command.hpp"

namespace slash::exporter {

// Adds export.run, which exports the session's project with a preset or a platform's defaults;
// gameplay.build, which compiles its gameplay code and swaps it into the session's world
// (SessionOptions::gameplay); and sdk.check and sdk.add for its SDK packages. Builds and
// downloads need the build and filesystem capabilities. Tools register them next to the built-in
// commands; a running game doesn't, since a build would stall its loop. A running game reloads
// the gameplay library when another program rebuilds it.
void register_export_commands(command::Registry& registry);

// Adds api.search, which finds C++ functions, types and macros in the engine's public headers
// (from the engine folder exports use) and the project's gameplay sources, with the examples
// that use them. It only reads, so tools register it in every session, running games included.
void register_api_commands(command::Registry& registry);

} // namespace slash::exporter
