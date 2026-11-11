#pragma once

#include "slash/core/math.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

// Soft bodies' points, for drawing: a SoftBody3D's mesh draws where 3D physics put its points, as
// Godot's SoftBody3D sends its points to the rendering server each frame. physics3d::install sets
// the source, since scene3d doesn't depend on physics3d.
namespace slash::scene3d {

// Where a soft body's mesh is now: a position and a smooth normal for each vertex of the mesh,
// in world space. A Cloth3D's positions are its welded points in world space, with no normals,
// and what draws is its surfaces, each by its part and its index in the part's mesh, in the
// model's space (the entity's, scale included).
struct SoftBodyPose {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    struct Surface {
        std::size_t part = 0;
        std::size_t surface = 0;
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
    };
    std::vector<Surface> surfaces;
};

// The pose of an entity's soft body, or null when it has none yet.
using SoftBodySource =
    std::function<std::shared_ptr<const SoftBodyPose>(const world::World&, entt::entity)>;
void set_soft_body_source(world::World& world, SoftBodySource source);
[[nodiscard]] std::shared_ptr<const SoftBodyPose> soft_body_pose(const world::World& world,
                                                                 entt::entity entity);

// What keeps each SoftBody3D from being made, as Godot's SoftBody3D configuration warnings and
// its errors for pinned points out of range: a MeshInstance3D drawing one primitive mesh is
// needed, as models aren't simulated yet.
[[nodiscard]] std::vector<Problem> check_soft_bodies(const world::World& world);

} // namespace slash::scene3d
