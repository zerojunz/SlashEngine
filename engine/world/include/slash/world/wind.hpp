#pragma once

// Wind, after Unreal's WindDirectionalSource: the wind field a world's WindSource3Ds blow and the
// clock their gusts keep. Godot has no wind but Area3D's for soft bodies, which physics3d keeps.

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <vector>

namespace slash::world {

class World;
class Tree2D;
class Tree3D;

// The wind everywhere in a world: the sum of its enabled WindSource3Ds, each placed by its
// entity's global transform when the field was made. Gusts come and go with the time and sweep
// along with the wind, and turbulence wanders across it, from a noise of whole numbers hashed
// with each source's ID, so the wind is the same bits on every machine.
class WindField {
public:
    WindField() = default;
    WindField(const World& world, const Tree3D& tree);

    // The wind at a place, in meters per second, time seconds after the wind began (wind_time).
    [[nodiscard]] Vec3 at(Vec3 position, double time) const;
    [[nodiscard]] bool empty() const noexcept { return sources_.empty(); }

private:
    struct Source {
        WindSource3D settings;
        Vec3 origin;
        Vec3 direction; // which way directional wind blows, as a unit vector
        std::uint32_t seed = 0;
    };
    std::vector<Source> sources_;
};

// The same in a 2D world, from its WindSource2Ds, in pixels per second.
class WindField2D {
public:
    WindField2D() = default;
    WindField2D(const World& world, const Tree2D& tree);

    [[nodiscard]] Vec2 at(Vec2 position, double time) const;
    [[nodiscard]] bool empty() const noexcept { return sources_.empty(); }

private:
    struct Source {
        WindSource2D settings;
        Vec2 origin;
        Vec2 direction;
        std::uint32_t seed = 0;
    };
    std::vector<Source> sources_;
};

// Seconds a world's wind has blown: the wind system adds each tick's length, from 0 when it was
// added or the world's state was last reset. 0 in a world without it.
[[nodiscard]] double wind_time(const World& world);

// Adds the wind system, which counts wind_time, unless the world has it.
void add_wind_system(World& world);

} // namespace slash::world
