#include "slash/world/physics_cache.hpp"

#include "slash/world/world.hpp"

#include <memory>
#include <utility>

namespace slash::world {

void PhysicsCache::add(const std::string& id, int dimensions, std::uint64_t tick, double seconds,
                       Vec3 position, Vec3 rotation) {
    CacheTrack& track = tracks_[id];
    track.dimensions = dimensions;
    // The step of the recording's first tick ends one tick in.
    const double time = static_cast<double>(tick + 1 - start_tick_) * seconds;
    if (!track.samples.empty() && track.samples.back().time >= time) {
        track.samples.back() = {.time = time, .position = position, .rotation = rotation};
        return;
    }
    track.samples.push_back({.time = time, .position = position, .rotation = rotation});
}

void PhysicsCache::add_start(const std::string& id, int dimensions, Vec3 position, Vec3 rotation) {
    CacheTrack& track = tracks_[id];
    track.dimensions = dimensions;
    track.samples.insert(track.samples.begin(),
                         {.time = 0.0, .position = position, .rotation = rotation});
}

PhysicsCache& start_physics_cache(World& world, std::set<entt::entity> only) {
    auto cache = std::make_shared<PhysicsCache>(std::move(only), world.tick());
    world.ecs().ctx().insert_or_assign(cache);
    return *cache;
}

PhysicsCache* physics_cache(World& world) {
    auto* cache = world.ecs().ctx().find<std::shared_ptr<PhysicsCache>>();
    return cache != nullptr ? cache->get() : nullptr;
}

void stop_physics_cache(World& world) {
    world.ecs().ctx().erase<std::shared_ptr<PhysicsCache>>();
}

} // namespace slash::world
