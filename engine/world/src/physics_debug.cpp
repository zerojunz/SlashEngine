#include "slash/world/physics_debug.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <memory>

namespace slash::world {

PhysicsDebugStep& PhysicsDebug::add(std::uint64_t tick, int dimensions) {
    const auto same = [&](const PhysicsDebugStep& step) { return step.dimensions == dimensions; };
    if (keep_ > 0 && static_cast<std::size_t>(std::ranges::count_if(steps_, same)) >= keep_) {
        steps_.erase(std::ranges::find_if(steps_, same));
    }
    PhysicsDebugStep& step = steps_.emplace_back();
    step.tick = tick;
    step.dimensions = dimensions;
    return step;
}

void record_physics(World& world, std::size_t keep) {
    auto& ctx = world.ecs().ctx();
    if (keep == 0) {
        ctx.erase<std::shared_ptr<PhysicsDebug>>();
        return;
    }
    ctx.insert_or_assign(std::make_shared<PhysicsDebug>(keep));
}

const PhysicsDebug* physics_debug(const World& world) {
    const auto* record = world.ecs().ctx().find<std::shared_ptr<PhysicsDebug>>();
    return record != nullptr ? record->get() : nullptr;
}

PhysicsDebug* physics_debug(World& world) {
    auto* record = world.ecs().ctx().find<std::shared_ptr<PhysicsDebug>>();
    return record != nullptr ? record->get() : nullptr;
}

} // namespace slash::world
