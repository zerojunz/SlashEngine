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

[[nodiscard]] MeshData box_mesh(const world::BoxMesh& box);
[[nodiscard]] MeshData sphere_mesh(const world::SphereMesh& sphere);
[[nodiscard]] MeshData cylinder_mesh(const world::CylinderMesh& cylinder);
[[nodiscard]] MeshData capsule_mesh(const world::CapsuleMesh& capsule);
[[nodiscard]] MeshData plane_mesh(const world::PlaneMesh& plane);
[[nodiscard]] MeshData quad_mesh(const world::QuadMesh& quad);
[[nodiscard]] MeshData prism_mesh(const world::PrismMesh& prism);
[[nodiscard]] MeshData torus_mesh(const world::TorusMesh& torus);

// One of the primitive mesh components.
using Primitive =
    std::variant<world::BoxMesh, world::SphereMesh, world::CylinderMesh, world::CapsuleMesh,
                 world::PlaneMesh, world::QuadMesh, world::PrismMesh, world::TorusMesh>;

// Their component names, in the variant's order.
inline constexpr std::array<std::string_view, 8> kPrimitiveNames = {
    "BoxMesh",   "SphereMesh", "CylinderMesh", "CapsuleMesh",
    "PlaneMesh", "QuadMesh",   "PrismMesh",    "TorusMesh"};

// The entity's primitive mesh component, or nullopt for none; an error naming them when it has
// more than one.
[[nodiscard]] Result<std::optional<Primitive>> primitive_of(const world::World& world,
                                                            entt::entity entity);

// Fails for settings Godot refuses to make a mesh of: a torus whose radii are equal.
[[nodiscard]] Result<void> check_primitive(const Primitive& primitive);

// The mesh a primitive makes; fails as check_primitive does.
[[nodiscard]] Result<MeshData> make_mesh(const Primitive& primitive);

} // namespace slash::scene3d
