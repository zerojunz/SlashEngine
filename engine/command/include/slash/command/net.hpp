#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds net.host, net.join, net.close, net.state and net.rpc, which put a session's (or a
// running game's) multiplayer on the network and inspect it, over slash/net/multiplayer.hpp.
void register_net_commands(Registry& registry);

} // namespace slash::command
