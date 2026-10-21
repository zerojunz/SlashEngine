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
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// What every device shares to draw meshes: the uniform buffers of shaders/mesh.hlsl, laid out as
// its cbuffers (std140 too, since every member is 16 bytes), the order a frame draws its meshes
// in, and the CPU twins of the shader's two stages, which the software device draws with.
namespace slash::render::detail {

using Float4 = std::array<float, 4>;
using Float3 = std::array<float, 3>;

// A value's bits as another type of the same size. std::bit_cast would do, but the glibc 2.31
// sysroot Linux exports build against has libstdc++ 10, which lacks it.
template <typename To, typename From>
[[nodiscard]] To same_bits(From value) {
    static_assert(sizeof(To) == sizeof(From));
    To out{};
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

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
    Float4 shadow{}; // its slot in MeshShadowUniforms' directional or positional, or -1
};
struct MeshSceneUniforms {
    Float4 camera{}; // the camera's place, then how many lights
    Float4 ambient{};
    Float4 tonemap{}; // exposure, white, the tonemapper, 1 to tonemap (0 when post passes do)
    std::array<MeshLightUniforms, kMaxLights3D> lights{};
};

// The shadows the fragment stage samples: b2 in space3. Matrices take a point in the world to its
// shadow atlas: x and y its texture coordinate there, z the depth to compare.
struct MeshDirectionalShadowUniforms {
    std::array<Mat4, 4> matrices{}; // one a cascade
    Float4 splits{};                // the view depth each cascade reaches
    Float4 normal_bias{};           // each cascade's, in world units
    Float4 params{};                // fade from and to (view z), opacity, how many cascades
    Float4 options{};               // 1 blends cascades, an atlas texel, 0, 0
};
struct MeshPositionalShadowUniforms {
    Mat4 light{};                  // the world to the light's own space, for an omni's faces
    std::array<Mat4, 6> faces{};   // one a cube face for an omni light; a spot light's first
    std::array<Float4, 6> rects{}; // each face's tile, its texture coordinates' low and high
    Float4 params{};   // normal bias over distance (world units), opacity, an atlas texel, faces
    Float4 position{}; // the light's place
};
struct MeshShadowUniforms {
    Float4 camera_z{}; // the camera matrix's z row: a point's view z is its dot with (point, 1)
    Float4 filters{};  // samples a directional and a spot shadow takes: 1, 5 or 13
    std::array<MeshDirectionalShadowUniforms, kMaxDirectionalShadows> directional{};
    std::array<MeshPositionalShadowUniforms, kMaxPositionalShadows> positional{};
};

// A shadow pass's vertex stage: b0 in space1 of shaders/shadow.hlsl, whose b1 is
// MeshInstanceUniforms and whose fragment stage takes MeshSurfaceUniforms.
struct MeshShadowPassUniforms {
    Mat4 view{};       // the world to the pass's camera space
    Mat4 projection{}; // its camera space to clip space, reversed depth
    Float4 options{};  // the bias, 1 pushes by it along the ray (omni), 1 clamps to near (pancake)
};

static_assert(sizeof(MeshInstanceUniforms) == 144);
static_assert(sizeof(MeshSurfaceUniforms) == 96);
static_assert(sizeof(MeshSceneUniforms) == 48 + (80 * kMaxLights3D));
static_assert(sizeof(MeshDirectionalShadowUniforms) == 320);
static_assert(sizeof(MeshPositionalShadowUniforms) == 576);
static_assert(sizeof(MeshShadowUniforms) ==
              32 + (320 * kMaxDirectionalShadows) + (576 * kMaxPositionalShadows));
static_assert(sizeof(MeshShadowPassUniforms) == 144);

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
    ShadowCasting shadows = ShadowCasting::On;
};

// Each light's shadow slot, as MeshLightUniforms::shadow holds it: lights with shadow set take the
// directional or positional slots in order while there are some; the rest, and lights without
// shadows, have -1.
[[nodiscard]] std::array<int, kMaxLights3D> shadow_slots(const Scene3D& scene);

// A rect of a shadow atlas, in texels from its top left.
struct AtlasRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// One view of the meshes a frame draws into a shadow atlas: a cascade of a directional light or a
// face of an omni or spot light.
struct ShadowPass {
    bool positional = false; // into the positional atlas, else the directional one
    std::size_t light = 0;   // its place in the scene's lights
    AtlasRect rect;
    MeshShadowPassUniforms uniforms;
    bool reverse_cull = false;
    std::uint32_t caster_mask = 0xFFFFFFFFU;
};

// The shadow passes a frame draws and what its forward pass samples, as Godot's Compatibility
// renderer lays them out: each directional light a share of its atlas (split as
// _get_directional_shadow_rect splits it), its cascades fitted to slices of the view as
// RendererSceneCull::_light_instance_setup_directional_shadow fits them, and each omni or spot
// light square tiles of the other atlas, all of one size.
struct ShadowPlan {
    std::uint32_t directional_size = 0; // 0: no pass draws into it
    std::uint32_t positional_size = 0;
    std::vector<ShadowPass> passes;
    std::unique_ptr<MeshShadowUniforms> uniforms = std::make_unique<MeshShadowUniforms>();
};
// The plan for a scene, its atlases at most max_size texels across.
[[nodiscard]] ShadowPlan plan_shadows(const Scene3D& scene, std::uint32_t max_size);

// Whether a pass draws a mesh: one that casts, isn't blended, is on a layer its light's casters
// are, and whose box isn't wholly outside the pass's view (past its sides or far plane; casters
// nearer the light than its near plane still cast, through the pancake).
[[nodiscard]] bool shadow_pass_draws(const ShadowPass& pass, const MeshDraw& draw,
                                     const Bounds& bounds);
// The faces a pass culls for a draw.
[[nodiscard]] CullMode shadow_cull(const ShadowPass& pass, const MeshDraw& draw);

// vs_main in shaders/shadow.hlsl: where a corner lands in a shadow pass's clip space.
[[nodiscard]] Float4 shadow_vertex(const MeshShadowPassUniforms& pass,
                                   const MeshInstanceUniforms& instance, const Vertex3D& vertex);

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
// A shadow atlas on the CPU: depths in rows from the top, size texels across.
struct ShadowAtlas {
    std::span<const float> depths;
    std::uint32_t size = 0;

    // SampleCmpLevelZero with a linear GREATER_OR_EQUAL comparison sampler that clamps to the
    // edge: how much of the four texels around (u, v) reference is at least.
    [[nodiscard]] float compare(float u, float v, float reference) const;
};
// What the fragment stage samples shadows from.
struct MeshShadows {
    const MeshShadowUniforms* uniforms = nullptr;
    ShadowAtlas directional;
    ShadowAtlas positional;
};

// The pixel's color, as fs_main gives it, or nullopt where it discards the pixel.
[[nodiscard]] std::optional<Float4> shade_mesh(const MeshSurfaceUniforms& surface,
                                               const MeshSceneUniforms& scene,
                                               const MeshShadows& shadows, const MeshPixel& pixel);
// fs_main in shaders/shadow.hlsl: whether a shadow pass keeps a pixel, as the alpha scissor does.
[[nodiscard]] bool shadow_keeps(const MeshSurfaceUniforms& surface, const Float4& albedo_texel,
                                const Float4& color);

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
