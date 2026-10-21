#pragma once

#include "slash/command/command.hpp"

// The asset library's commands: asset.search, asset.generate, asset.refine, asset.info,
// asset.use, asset.feedback, asset.check, asset.sync and asset.add. A session's library is
// SessionOptions::library_dir, or library::default_dir(); the team's is the one
// library::open_team finds (slash/library/team.hpp).
namespace slash::library {

void register_commands(command::Registry& registry);

} // namespace slash::library
