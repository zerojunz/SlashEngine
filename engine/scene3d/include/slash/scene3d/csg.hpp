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

// CSG shapes: each shape's faces are made as
// Manifold's booleans combine them, and the root's result
// is made into one surface per material. Roots draw through
// Renderer, collide in physics3d when use_collision is on, and are parsed for navigation meshes.
namespace slash::scene3d {

// One of the CSG components.
using CsgShape = std::variant<world::SolidGroup3D, world::SolidBox3D, world::SolidSphere3D,
                              world::SolidCylinder3D, world::SolidTorus3D, world::SolidExtrusion3D,
                              world::SolidMesh3D>;

// Their component names, in the variant's order.
inline constexpr std::array<std::string_view, 7> kCsgNames = {
    "SolidGroup3D", "SolidBox3D",       "SolidSphere3D", "SolidCylinder3D",
    "SolidTorus3D", "SolidExtrusion3D", "SolidMesh3D"};

// The fields every CSG component has (and Model3D's layers and
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

// Whether the entity is a CSG root: a CSG shape whose Transform3D parent isn't one.
[[nodiscard]] bool is_csg_root(const world::World& world, const world::Tree3D& tree,
                               entt::entity entity);

// The CSG roots in tree order, then those without a Transform3D in the order they were made.
[[nodiscard]] std::vector<entt::entity> csg_roots(const world::World& world,
                                                  const world::Tree3D& tree);

// A root's result.
struct CsgMesh {
    // A number no other mesh made in this process has, for keeping it on a device.
    std::uint64_t version = 0;
    // One surface per material: smooth faces' normals shared at each
    // corner. A SolidMesh3D's model's own material names the SolidMesh3D as its entity.
    std::vector<MeshSurface> surfaces;
    // Every three corners a triangle facing out, for
    // use_collision, in the root's space.
    std::shared_ptr<const std::vector<Vec3>> collision;
    std::size_t triangles = 0;
    Vec3 min; // where the faces reach in the root's space; zero for an empty result
    Vec3 max;
    // Shapes under the root that couldn't be made, and an empty result, as warnings.
    std::vector<Problem> problems;
};

// The mesh a root makes. With install_csg it is kept until a shape under the root changes;
// without, it is made on each call, models read with the world's file reader.
[[nodiscard]] std::shared_ptr<const CsgMesh> csg_mesh(const world::World& world,
                                                      const world::Tree3D& tree, entt::entity root);

// A CSG shape and the shapes under it made in code rather than read from a world's entities, as
// model recipes (recipe.hpp) make them. Each shape is placed by local in its parent (the root's
// isn't used), drawn with material (the default material with none), and its problems are reported
// at where, with no entity, its fields' below it ("<where>/radius").
struct CsgTree {
    CsgShape shape;
    world::Affine3D local;
    std::optional<world::Surface3D> material;
    // A SolidMesh3D's meshes: a model's, placed as its parts are at rest, or a primitive mesh's.
    std::shared_ptr<const Model> model;
    std::optional<Primitive> primitive;
    // A SolidExtrusion3D's curve in path mode, in the shape's own space.
    std::optional<world::Spline3D> path;
    std::string where;
    std::vector<CsgTree> children;
};

// The mesh a tree makes, as a root shape's in a world.
[[nodiscard]] std::shared_ptr<const CsgMesh> csg_mesh(const CsgTree& root);

// Keeps each root's mesh in the world until it changes, reading SolidMesh3D models with load.
// Sessions and games call it through canvas::install.
void install_csg(world::World& world, ModelLoader load);

// A root with use_collision on, as a static body of its root shape: its collision layer
// and mask and its triangles. Roots whose result is empty are left out.
struct CsgBody {
    entt::entity entity = entt::null;
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    std::shared_ptr<const std::vector<Vec3>> faces;
};
[[nodiscard]] std::vector<CsgBody> csg_bodies(const world::World& world, const world::Tree3D& tree);

// Every CSG shape that can't be made, in the order entities were made: more than one CSG
// component on an entity, a CSG shape with a Model3D, a SolidMesh3D whose mesh doesn't load,
// unsupported settings, faces that don't close and roots whose result is empty.
[[nodiscard]] std::vector<Problem> check_csg(const world::World& world);

// Ear clipping of a simple
// polygon, every three indices a triangle; empty when it can't be done.
[[nodiscard]] std::vector<int> triangulate(const std::vector<Vec2>& contour);

} // namespace slash::scene3d
