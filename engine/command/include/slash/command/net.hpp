#pragma once

#include "slash/command/command.hpp"
#include "slash/net/http.hpp"

namespace slash::world {
class World;
} // namespace slash::world

namespace slash::command {

// Adds net.host, net.join, net.close, net.state and net.rpc, which put a session's (or a
// running game's) multiplayer on the network and inspect it, over slash/net/multiplayer.hpp;
// and net.http and net.http_fake, which list game code's HTTP requests and answer them.
void register_net_commands(Registry& registry);

// Gives a session's world HTTP for game code: answered from net.http_fake's fakes, then by
// network, if given.
void install_session_http(world::World& world, net::HttpSender network);

} // namespace slash::command
