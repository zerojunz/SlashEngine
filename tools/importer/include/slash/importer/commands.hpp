#pragma once

#include "slash/command/command.hpp"

namespace slash::importer {

struct ImportCommandOptions {
    // Whether project.import may unpack a Unity build, which runs AssetRipper for minutes. A
    // running game turns it off, since that would stall its loop.
    bool unpack_builds = true;
};

// Adds project.import, which converts a game made with another engine, in a folder inside the
// session's project, into the project. It needs the filesystem capability, and unpacking a Unity
// build the build capability too.
void register_import_commands(command::Registry& registry, ImportCommandOptions options = {});

} // namespace slash::importer
