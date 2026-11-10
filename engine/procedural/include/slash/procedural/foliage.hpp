#pragma once

// Foliage while a game runs, after Unreal's Foliage tool and its grass that parts for characters:
// the types Foliage3D and Foliage2D groups name (world::FoliageType, read through the world's
// assets), the brush that paints instances of a type over the ground, how far each instance leans
// in the wind and from characters pushing through it, and the foliage system that springs pushed
// instances back. Drawing (scene3d, canvas), collision (physics3d, physics2d) and the foliage.*
// commands all ask here, so they agree.

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/procedural/scatter.hpp"
#include "slash/world/components.hpp"
#include "slash/world/foliage.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace slash::world {
class World;
}

namespace slash::procedural {

// Floats each instance takes in a group's instances: 3D x, y, z, rotation x, y, z (radians,
// Godot's YXZ) and scale; 2D x, y, rotation and scale, a negative scale mirroring it.
inline constexpr std::size_t kFoliageFloats3D = 7;
inline constexpr std::size_t kFoliageFloats2D = 4;

// One instance, in its Foliage3D's or Foliage2D's space.
struct FoliagePlace3D {
    Vec3 position;
    Vec3 rotation;
    float scale = 1.0F;
};
struct FoliagePlace2D {
    Vec2 position;
    float rotation = 0.0F;
    float scale = 1.0F; // negative: mirrored
};

[[nodiscard]] std::size_t foliage_count(const world::FoliageGroup3D& group);
[[nodiscard]] std::size_t foliage_count(const world::FoliageGroup2D& group);
[[nodiscard]] FoliagePlace3D foliage_place(const world::FoliageGroup3D& group, std::size_t index);
[[nodiscard]] FoliagePlace2D foliage_place(const world::FoliageGroup2D& group, std::size_t index);
// Appends an instance, each number rounded to a thousandth so scenes stay short.
void add_foliage(world::FoliageGroup3D& group, const FoliagePlace3D& place);
void add_foliage(world::FoliageGroup2D& group, const FoliagePlace2D& place);
void remove_foliage(world::FoliageGroup3D& group, std::size_t index);
void remove_foliage(world::FoliageGroup2D& group, std::size_t index);

// The type a .foliage.json file holds as the world's assets have it now, or nullptr with why in
// problem: kept while the file's version stays, so asking every frame is cheap.
[[nodiscard]] std::shared_ptr<const world::FoliageType>
foliage_type(const world::World& world, const std::string& path, std::string* problem = nullptr);

// A brush stroke: the instances of type it adds within radius of center (world space; 2D in x
// and y), standing on ground, at strength (0 to 1) of the type's density, spaced from the places
// of avoid and from each other. Seeded, so the same stroke over the same ground places the same.
struct FoliageStroke {
    Vec3 center;
    float radius = 1.0F;
    float strength = 1.0F;
    std::uint32_t seed = 0;
};
struct FoliagePainted {
    std::vector<FoliagePlace3D> places; // world space; 2D in x, y and rotation.z
    int wanted = 0;
    int slope = 0;
    int height = 0;
    int spacing = 0;
};
[[nodiscard]] FoliagePainted paint_foliage(const world::FoliageType& type,
                                           const FoliageStroke& stroke,
                                           std::span<const ScatterTriangle> ground,
                                           std::span<const Vec3> avoid);

// How far an instance leans, in radians: in 3D about the world's x and z as a lean toward
// (x, z), in 2D toward +x. It is the wind at its base times wind_bend, swayed by sway at
// sway_frequency with a phase of its own, plus the push of characters the foliage system
// springs, held within max_bend. index is the instance's in its group.
[[nodiscard]] Vec2 foliage_lean_3d(const world::World& world, entt::entity entity,
                                   std::size_t group, std::size_t index,
                                   const world::FoliageType& type, Vec3 base, Vec3 wind);
[[nodiscard]] float foliage_lean_2d(const world::World& world, entt::entity entity,
                                    std::size_t group, std::size_t index,
                                    const world::FoliageType& type, Vec2 base, Vec2 wind);

// Instances characters have pushed and not yet let go: their entity, group, index and lean
// from pushing alone (2D in x).
struct FoliagePush {
    entt::entity entity = entt::null;
    std::size_t group = 0;
    std::size_t index = 0;
    Vec2 lean;
};
[[nodiscard]] std::vector<FoliagePush> foliage_pushes(const world::World& world);

// Adds the foliage system (priority 970, after physics): CharacterBody3D, RigidBody3D and
// VehicleBody3D entities push the Foliage3D instances within a type's interaction_radius over
// (CharacterBody2D and RigidBody2D the Foliage2D ones), and each springs back by its stiffness
// and damping. Keeps what it needs in the world's context, reset with the world's state.
void install_foliage(world::World& world);

} // namespace slash::procedural
