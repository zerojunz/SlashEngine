#pragma once

#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace slash::world {
class World;
}

// Godot's primitive meshes, made as scene/resources/3d/primitive_meshes.cpp makes them: the same
// corners in the same order, with the same normals, tangents and texture coordinates, so a
// texture wraps a box or a sphere here as it does in Godot. Angles go through slash::cos_sin, so
// a mesh is the same bits on every platform.
namespace slash::scene3d {

// A mesh's corners and its triangles, every three indices one, for RenderDevice::create_mesh.
struct MeshData {
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices;
};

[[nodiscard]] MeshData box_mesh(const world::BoxGeometry& box);
[[nodiscard]] MeshData sphere_mesh(const world::SphereGeometry& sphere);
[[nodiscard]] MeshData cylinder_mesh(const world::CylinderGeometry& cylinder);
[[nodiscard]] MeshData capsule_mesh(const world::CapsuleGeometry& capsule);
[[nodiscard]] MeshData plane_mesh(const world::PlaneGeometry& plane);
[[nodiscard]] MeshData quad_mesh(const world::QuadGeometry& quad);
[[nodiscard]] MeshData prism_mesh(const world::PrismGeometry& prism);
[[nodiscard]] MeshData torus_mesh(const world::TorusGeometry& torus);

// One of the primitive mesh components.
using Primitive = std::variant<world::BoxGeometry, world::SphereGeometry, world::CylinderGeometry,
                               world::CapsuleGeometry, world::PlaneGeometry, world::QuadGeometry,
                               world::PrismGeometry, world::TorusGeometry>;

// Their component names, in the variant's order.
inline constexpr std::array<std::string_view, 8> kPrimitiveNames = {
    "BoxGeometry",   "SphereGeometry", "CylinderGeometry", "CapsuleGeometry",
    "PlaneGeometry", "QuadGeometry",   "PrismGeometry",    "TorusGeometry"};

// The entity's primitive mesh component, or nullopt for none; an error naming them when it has
// more than one.
[[nodiscard]] Result<std::optional<Primitive>> primitive_of(const world::World& world,
                                                            entt::entity entity);

// Fails for settings Godot refuses to make a mesh of: a torus whose radii are equal.
[[nodiscard]] Result<void> check_primitive(const Primitive& primitive);

// The mesh a primitive makes; fails as check_primitive does.
[[nodiscard]] Result<MeshData> make_mesh(const Primitive& primitive);

} // namespace slash::scene3d
