#pragma once

// Recorded simulations, as Unreal's Chaos Cache Manager records them (Godot has none): while a
// world records, 3D and 2D physics add where each recorded rigid body is after every step, and
// physics.cache writes the recording as an animation of their transforms, which an
// AnimationPlayer, or a cinematic's animation track, plays back the same every time.

#include "slash/core/math.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::world {

class World;

// Where a body was at a time since the recording started: its Transform3D's position and
// rotation, or a Transform2D's as (x, y, 0) and (rotation, 0, 0).
struct CacheSample {
    double time = 0.0;
    Vec3 position;
    Vec3 rotation;
};

struct CacheTrack {
    int dimensions = 3;
    std::vector<CacheSample> samples;
    bool destroyed = false; // it went before the recording stopped
};

class PhysicsCache {
public:
    // Records the given entities, or every rigid body when empty, from start_tick on.
    PhysicsCache(std::set<entt::entity> only, std::uint64_t start_tick)
        : only_(std::move(only)), start_tick_(start_tick) {}

    [[nodiscard]] bool records(entt::entity entity) const {
        return only_.empty() || only_.contains(entity);
    }
    // Adds where an entity is after the step of a tick seconds long.
    void add(const std::string& id, int dimensions, std::uint64_t tick, double seconds,
             Vec3 position, Vec3 rotation);
    // Adds where an entity is when the recording starts.
    void add_start(const std::string& id, int dimensions, Vec3 position, Vec3 rotation);

    [[nodiscard]] std::map<std::string, CacheTrack>& tracks() noexcept { return tracks_; }
    [[nodiscard]] const std::map<std::string, CacheTrack>& tracks() const noexcept {
        return tracks_;
    }
    [[nodiscard]] std::uint64_t start_tick() const noexcept { return start_tick_; }

private:
    std::set<entt::entity> only_;
    std::uint64_t start_tick_ = 0;
    std::map<std::string, CacheTrack> tracks_;
};

// Starts recording a world's rigid bodies (those in only, or all), replacing a recording.
PhysicsCache& start_physics_cache(World& world, std::set<entt::entity> only);
// The recording, or nullptr when the world isn't recording.
[[nodiscard]] PhysicsCache* physics_cache(World& world);
// Stops recording.
void stop_physics_cache(World& world);

} // namespace slash::world
