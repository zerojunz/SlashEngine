#pragma once

// Flesh on a character's 3D model (slash/world/flesh.hpp): the flesh_3d system steps the
// muscles of each Flesh3D's .character.json model from where its posed bones are, and the
// renderer moves the model's skinned vertices by them, so skin bulges over working muscles and
// jiggles as the body moves. A model drawn as "::muscles" shows the muscles themselves move.

#include "slash/scene3d/skeleton.hpp"
#include "slash/world/flesh.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace slash::scene3d {

// How far flesh moves each vertex of a model's skinned surfaces, in the model's space after
// skinning: last tick's and this tick's, to ease between as drawing does.
struct FleshPose {
    struct Surface {
        std::size_t part = 0;
        std::size_t surface = 0;
        std::vector<Vec3> before;
        std::vector<Vec3> now;
    };
    std::vector<Surface> surfaces;
};

// Adds the flesh_3d system (priority 957, after physics and ragdolls), its models loaded with
// load.
void install_flesh(world::World& world, ModelLoader load);
// An entity's flesh pose, or null where it has none.
[[nodiscard]] std::shared_ptr<const FleshPose> flesh_pose(const world::World& world,
                                                          entt::entity entity);
// An entity's flesh, or empty where it has none yet.
[[nodiscard]] world::FleshView flesh_3d(const world::World& world, entt::entity entity);

} // namespace slash::scene3d
