#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The shapes a model collides as (Model::collision), as Godot's importer makes them: from its
// nodes' collision hints (apply_import_hints), or generated as its "Generate > Physics" options
// do, with V-HACD for convex decomposition. A MeshInstance3D whose model has them is a static
// body, as the StaticBody3D Godot's importer adds, or adds them to the body it is on.
namespace slash::scene3d {

// The convex hull of points, with at most max_points corners (0 for any number), as V-HACD's
// QuickHull makes it, its faces in Godot's winding; no points for points that span no volume.
[[nodiscard]] ModelCollision convex_hull(std::span<const Vec3> points, std::size_t max_points = 0);

// How convex_decompose splits a mesh, Godot's MeshConvexDecompositionSettings as V-HACD 4 takes
// them: at most max_hulls hulls of at most max_points corners each, the mesh voxelized into about
// resolution voxels (Godot's default).
struct DecomposeOptions {
    std::size_t max_hulls = 16;
    std::size_t max_points = 32;
    std::uint32_t resolution = 10000;
};

// A mesh (indices three corners each over points) split into convex hulls, as Godot's
// Mesh::convex_decompose does with V-HACD; the same hulls on every platform. None for a mesh
// with no volume.
[[nodiscard]] std::vector<ModelCollision> convex_decompose(std::span<const Vec3> points,
                                                           std::span<const std::uint32_t> indices,
                                                           const DecomposeOptions& options = {});

// The shapes a model's whole mesh makes, as Godot's mesh import "Generate > Physics" shape
// types: "trimesh" its triangles, "convex" their convex hull (of at most options.max_points
// corners) and "decompose" convex_decompose's hulls. They replace the model's collision.
[[nodiscard]] Result<std::size_t> generate_collision(Model& model, std::string_view kind,
                                                     const DecomposeOptions& options = {});

// A model's collision as CollisionShape3Ds in its own space: convex_polygon shapes of hulls'
// points, and concave_polygon shapes of trimeshes' faces.
[[nodiscard]] std::vector<world::CollisionShape3D> collision_shapes(const Model& model);

// The shapes of the model an entity's MeshInstance3D draws, or null when it draws none or its
// model has no collision. With install_model_collision they are kept until the model changes;
// without, the model is read with the world's file reader on each call.
[[nodiscard]] std::shared_ptr<const std::vector<world::CollisionShape3D>>
model_shapes(const world::World& world, entt::entity entity);

// Keeps each model's shapes for model_shapes, the models read with models.
void install_model_collision(world::World& world, ModelLoader models);

// A MeshInstance3D whose model has collision, in storage order, with its shapes: physics3d
// makes it a static body of them, layer and mask 1, unless it is a body itself or the child of
// one, which takes them as it takes a CollisionShape3D.
struct ModelBody {
    entt::entity entity = entt::null;
    std::shared_ptr<const std::vector<world::CollisionShape3D>> shapes;
};
[[nodiscard]] std::vector<ModelBody> model_bodies(const world::World& world);

} // namespace slash::scene3d
