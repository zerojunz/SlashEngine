#pragma once

#include "slash/core/math.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/scene3d/surface.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace slash::world {
class World;
}

// CSG shapes, as Godot 4.6's CSG module (modules/csg/csg_shape.cpp): each shape's faces are made
// as Godot makes them, combined with Manifold's booleans in Godot's order, and the root's result
// made into one surface per material with Godot's normals and tangents. Roots draw through
// Renderer, collide in physics3d when use_collision is on, and are parsed for navigation meshes.
namespace slash::scene3d {

// One of the CSG components.
using CsgShape =
    std::variant<world::CSGCombiner3D, world::CSGBox3D, world::CSGSphere3D, world::CSGCylinder3D,
                 world::CSGTorus3D, world::CSGPolygon3D, world::CSGMesh3D>;

// Their component names, in the variant's order.
inline constexpr std::array<std::string_view, 7> kCsgNames = {
    "CSGCombiner3D", "CSGBox3D",     "CSGSphere3D", "CSGCylinder3D",
    "CSGTorus3D",    "CSGPolygon3D", "CSGMesh3D"};

// The fields every CSG component has, as Godot's CSGShape3D (and MeshInstance3D's layers and
// cast_shadow).
struct CsgSettings {
    std::string operation = "union";
    bool calculate_tangents = true;
    bool use_collision = false;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    std::uint32_t layers = 1;
    std::string cast_shadow = "on";
};
[[nodiscard]] CsgSettings csg_settings(const CsgShape& shape);

// The entity's CSG component, or nullopt for none. An entity with more than one gets the first
// in kCsgNames' order; check_meshes reports it.
[[nodiscard]] std::optional<CsgShape> csg_shape_of(const world::World& world, entt::entity entity);

// Whether the entity is a CSG root: a CSG shape whose Transform3D parent isn't one, as Godot's
// is_root_shape.
[[nodiscard]] bool is_csg_root(const world::World& world, const world::Tree3D& tree,
                               entt::entity entity);

// The CSG roots in tree order, then those without a Transform3D in the order they were made.
[[nodiscard]] std::vector<entt::entity> csg_roots(const world::World& world,
                                                  const world::Tree3D& tree);

// A root's result.
struct CsgMesh {
    // A number no other mesh made in this process has, for keeping it on a device.
    std::uint64_t version = 0;
    // One surface per material, as Godot's root mesh has: smooth faces' normals shared at each
    // corner. A CSGMesh3D's model's own material names the CSGMesh3D as its entity.
    std::vector<MeshSurface> surfaces;
    // Every three corners a triangle facing out, as Godot's ConcavePolygonShape3D for
    // use_collision, in the root's space.
    std::shared_ptr<const std::vector<Vec3>> collision;
    std::size_t triangles = 0;
    Vec3 min; // where the faces reach in the root's space; zero for an empty result
    Vec3 max;
    // Shapes under the root that couldn't be made, and an empty result, as Godot's
    // configuration warning.
    std::vector<Problem> problems;
};

// The mesh a root makes. With install_csg it is kept until a shape under the root changes;
// without, it is made on each call, models read with the world's file reader.
[[nodiscard]] std::shared_ptr<const CsgMesh> csg_mesh(const world::World& world,
                                                      const world::Tree3D& tree, entt::entity root);

// A CSG shape and the shapes under it made in code rather than read from a world's entities, as
// model recipes (recipe.hpp) make them. Each shape is placed by local in its parent (the root's
// isn't used), drawn with material (Godot's default with none), and its problems are reported at
// where, with no entity, its fields' below it ("<where>/radius").
struct CsgTree {
    CsgShape shape;
    world::Affine3D local;
    std::optional<world::StandardMaterial3D> material;
    // A CSGMesh3D's meshes: a model's, placed as its parts are at rest, or a primitive mesh's.
    std::shared_ptr<const Model> model;
    std::optional<Primitive> primitive;
    // A CSGPolygon3D's curve in path mode, in the shape's own space.
    std::optional<world::Path3D> path;
    std::string where;
    std::vector<CsgTree> children;
};

// The mesh a tree makes, as a root shape's in a world.
[[nodiscard]] std::shared_ptr<const CsgMesh> csg_mesh(const CsgTree& root);

// Keeps each root's mesh in the world until it changes, reading CSGMesh3D models with load.
// Sessions and games call it through canvas::install.
void install_csg(world::World& world, ModelLoader load);

// A root with use_collision on, as Godot's static body of its root shape: its collision layer
// and mask and its triangles. Roots whose result is empty are left out.
struct CsgBody {
    entt::entity entity = entt::null;
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    std::shared_ptr<const std::vector<Vec3>> faces;
};
[[nodiscard]] std::vector<CsgBody> csg_bodies(const world::World& world, const world::Tree3D& tree);

// Every CSG shape that can't be made, in the order entities were made: more than one CSG
// component on an entity, a CSG shape with a MeshInstance3D, a CSGMesh3D whose mesh doesn't load,
// settings Godot refuses, faces that don't close and roots whose result is empty.
[[nodiscard]] std::vector<Problem> check_csg(const world::World& world);

// Godot's Triangulate::triangulate (core/math/triangulate.cpp): ear clipping of a simple
// polygon, every three indices a triangle; empty when it can't be done.
[[nodiscard]] std::vector<int> triangulate(const std::vector<Vec2>& contour);

} // namespace slash::scene3d
