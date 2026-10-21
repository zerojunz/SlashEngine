#pragma once

#include "checks.hpp"

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

// What every device shares to draw meshes: the uniform buffers of shaders/mesh.hlsl, laid out as
// its cbuffers (std140 too, since every member is 16 bytes), the order a frame draws its meshes
// in, and the CPU twins of the shader's two stages, which the software device draws with.
namespace slash::render::detail {

using Float4 = std::array<float, 4>;
using Float3 = std::array<float, 3>;

// The textures a mesh draw samples: albedo, metallic, roughness, emission and normal.
inline constexpr std::size_t kMeshTextures = 5;

// mesh.hlsl's Surface flags.
inline constexpr std::uint32_t kMeshUnshaded = 1;
inline constexpr std::uint32_t kMeshVertexColorAlbedo = 2;
inline constexpr std::uint32_t kMeshAlphaScissor = 4;
inline constexpr std::uint32_t kMeshEmissionTexture = 8;
inline constexpr std::uint32_t kMeshNormalMap = 16;
inline constexpr std::uint32_t kMeshSideCheck = 32;
inline constexpr std::uint32_t kMeshHasAlpha = 64;

// The vertex stage's uniforms: b0 and b1 in space1.
struct MeshCameraUniforms {
    Mat4 view_projection{};
};
struct MeshInstanceUniforms {
    Mat4 model{};
    Mat4 normal_model{}; // the inverse transpose of model's upper 3x3, in its upper 3x3
    Float4 uv1{};        // scale, then offset
};

// The fragment stage's uniforms: b0 and b1 in space3.
struct MeshSurfaceUniforms {
    Float4 albedo{};
    Float4 emission{};
    Float4 surface{}; // metallic, roughness, specular, normal scale
    Float4 metallic_channel{};
    Float4 roughness_channel{};
    std::array<std::uint32_t, 4> options{}; // flags, the scissor threshold's bits, layers, 0
};
struct MeshLightUniforms {
    Float4 position{};
    Float4 direction{};
    Float4 color{};
    Float4 params{};
};
struct MeshSceneUniforms {
    Float4 camera{}; // the camera's place, then how many lights
    Float4 ambient{};
    std::array<MeshLightUniforms, kMaxLights3D> lights{};
};

static_assert(sizeof(MeshInstanceUniforms) == 144);
static_assert(sizeof(MeshSurfaceUniforms) == 96);
static_assert(sizeof(MeshSceneUniforms) == 32 + (64 * kMaxLights3D));

// Godot's Color::srgb_to_linear, for colors the CPU converts before the shader sees them.
[[nodiscard]] float srgb_to_linear_exact(float value);

[[nodiscard]] MeshCameraUniforms mesh_camera_uniforms(const Scene3D& scene);
[[nodiscard]] MeshInstanceUniforms mesh_instance_uniforms(const Mat4& transform,
                                                          const Material3D& material);
[[nodiscard]] MeshSurfaceUniforms mesh_surface_uniforms(const Material3D& material,
                                                        std::uint32_t layers);
[[nodiscard]] MeshSceneUniforms mesh_scene_uniforms(const Scene3D& scene);

// The box around a mesh's positions, for sorting draws.
struct Bounds {
    Float3 low{};
    Float3 high{};
};
[[nodiscard]] Bounds mesh_bounds(std::span<const Vertex3D> vertices);

// A mesh draw a frame holds until it is sent.
struct MeshDraw {
    MeshId mesh = MeshId::None;
    Mat4 transform{};
    Material3D material;
    std::uint32_t layers = 1;
};

// The order to draw a frame's meshes in, as Godot's: opaque and alpha scissor ones nearest
// first, then alpha ones farthest first, each by how far its box's center is in front of the
// camera; draws as far keep the order they were made in. bounds(i) gives draw i's mesh's box.
template <typename BoundsOf>
[[nodiscard]] std::vector<std::size_t>
mesh_draw_order(std::span<const MeshDraw> draws, const Scene3D& scene, const BoundsOf& bounds);
// How far a draw's box's center is in front of the camera.
[[nodiscard]] float mesh_draw_depth(const MeshDraw& draw, const Bounds& bounds,
                                    const Scene3D& scene);

// The checks every device makes on a mesh's vertices and indices.
[[nodiscard]] Result<void> check_mesh(std::span<const Vertex3D> vertices,
                                      std::span<const std::uint32_t> indices);
// That a scene's numbers are usable and it has at most kMaxLights3D lights.
[[nodiscard]] Result<void> check_scene(const Scene3D& scene);
// That a material's numbers are usable, and its textures exist (known) and aren't the target.
template <typename Known>
[[nodiscard]] Result<void> check_material_3d(const Material3D& material, TextureId target,
                                             const Known& known);
[[nodiscard]] Error unknown_mesh(MeshId mesh);
[[nodiscard]] Error no_scene();
// What draw_mesh checks in a frame: that it has a scene, the mesh exists (mesh_known), the
// transform is finite and the material is usable.
template <typename Known>
[[nodiscard]] Result<void> check_mesh_draw(bool has_scene, bool mesh_known, MeshId mesh,
                                           const Mat4& transform, const Material3D& material,
                                           TextureId target, const Known& known);

// What the vertex stage leaves at a corner, as vs_main does: where it is in the world and in
// clip space, its normal, tangent and binormal, texture coordinate and color.
struct MeshCorner {
    Float4 clip{};
    Float3 world{};
    Float3 normal{};
    Float3 tangent{};
    Float3 binormal{};
    std::array<float, 2> uv{};
    Float4 color{};
};
[[nodiscard]] MeshCorner mesh_vertex(const MeshCameraUniforms& camera,
                                     const MeshInstanceUniforms& instance, const Vertex3D& vertex);

// What the fragment stage reads at a pixel: the corners' values there, which face it is on, and
// what each texture holds there.
struct MeshPixel {
    Float3 world{};
    Float3 normal{};
    Float3 tangent{};
    Float3 binormal{};
    Float4 color{};
    bool front = true;
    Float4 albedo_texel{1.0F, 1.0F, 1.0F, 1.0F};
    Float4 metallic_texel{1.0F, 1.0F, 1.0F, 1.0F};
    Float4 roughness_texel{1.0F, 1.0F, 1.0F, 1.0F};
    Float3 emission_texel{1.0F, 1.0F, 1.0F};
    Float3 normal_texel{1.0F, 1.0F, 1.0F};
};
// The pixel's color, as fs_main gives it, or nullopt where it discards the pixel.
[[nodiscard]] std::optional<Float4> shade_mesh(const MeshSurfaceUniforms& surface,
                                               const MeshSceneUniforms& scene,
                                               const MeshPixel& pixel);

template <typename BoundsOf>
std::vector<std::size_t> mesh_draw_order(std::span<const MeshDraw> draws, const Scene3D& scene,
                                         const BoundsOf& bounds) {
    std::vector<std::size_t> order(draws.size());
    std::vector<float> depth(draws.size());
    for (std::size_t i = 0; i < draws.size(); ++i) {
        order[i] = i;
        depth[i] = mesh_draw_depth(draws[i], bounds(i), scene);
    }
    const auto blended = [&](std::size_t i) {
        return draws[i].material.transparency == Transparency::Alpha;
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (blended(a) != blended(b)) {
            return !blended(a);
        }
        return blended(a) ? depth[a] > depth[b] : depth[a] < depth[b];
    });
    return order;
}

template <typename Known>
Result<void> check_material_3d(const Material3D& material, TextureId target, const Known& known) {
    const auto finite = [](std::initializer_list<float> values) {
        return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
    };
    const auto& m = material;
    if (!finite({m.albedo[0], m.albedo[1], m.albedo[2], m.albedo[3], m.metallic, m.roughness,
                 m.specular, m.emission[0], m.emission[1], m.emission[2], m.emission_energy,
                 m.normal_scale, m.alpha_scissor_threshold, m.uv1_scale[0], m.uv1_scale[1],
                 m.uv1_offset[0], m.uv1_offset[1]})) {
        return make_error(ErrorCode::InvalidArgument, "a 3D material's numbers are finite");
    }
    for (const MaterialTexture* slot :
         {&m.albedo_texture, &m.metallic_texture, &m.roughness_texture, &m.emission_texture,
          &m.normal_texture}) {
        if (slot->texture == TextureId::None) {
            continue;
        }
        if (!known(slot->texture)) {
            return unknown_texture(slot->texture);
        }
        if (slot->texture == target) {
            return target_in_use(slot->texture);
        }
    }
    return {};
}

template <typename Known>
Result<void> check_mesh_draw(bool has_scene, bool mesh_known, MeshId mesh, const Mat4& transform,
                             const Material3D& material, TextureId target, const Known& known) {
    if (!has_scene) {
        return no_scene();
    }
    if (!mesh_known) {
        return unknown_mesh(mesh);
    }
    if (!std::ranges::all_of(transform, [](float value) { return std::isfinite(value); })) {
        return make_error(ErrorCode::InvalidArgument, "a mesh's transform is finite");
    }
    return check_material_3d(material, target, known);
}

} // namespace slash::render::detail
