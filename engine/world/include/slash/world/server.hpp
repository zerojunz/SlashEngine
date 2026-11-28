#pragma once

// Whether a world runs in a dedicated server: an export whose preset sets dedicated_server, or a
// game started with --server. A server has no window, GPU or sound and ticks in real time until
// stopped, so game code checks this to skip what only players see and hear, and to host instead
// of join.
namespace slash::world {

class World;

void set_dedicated_server(World& world, bool server);
[[nodiscard]] bool is_dedicated_server(const World& world);

} // namespace slash::world
