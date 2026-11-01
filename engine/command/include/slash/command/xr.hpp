#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds xr.state and xr.set, which inspect a session's XR server and move its test headset, over
// slash/xr/xr.hpp.
void register_xr_commands(Registry& registry);

} // namespace slash::command
