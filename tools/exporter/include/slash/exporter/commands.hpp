#pragma once

#include "slash/command/command.hpp"

namespace slash::exporter {

// Adds export.run, which exports the session's project with a preset or a platform's defaults.
// It needs the build and filesystem capabilities. Tools register it next to the built-in
// commands; a running game doesn't, since an export would stall its loop for minutes.
void register_export_commands(command::Registry& registry);

} // namespace slash::exporter
