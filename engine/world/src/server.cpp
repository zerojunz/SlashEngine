#include "slash/world/server.hpp"

#include "slash/world/world.hpp"

namespace slash::world {
namespace {

struct DedicatedServer {
    bool on = false;
};

} // namespace

void set_dedicated_server(World& world, bool server) {
    world.ecs().ctx().insert_or_assign(DedicatedServer{.on = server});
}

bool is_dedicated_server(const World& world) {
    const auto* found = world.ecs().ctx().find<DedicatedServer>();
    return found != nullptr && found->on;
}

} // namespace slash::world
