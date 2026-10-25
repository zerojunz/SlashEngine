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
#include <string>
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

// The textures a mesh draw samples: albedo, metallic, roughness, emission and normal, then six
// only terrain shading samples, the first of which holds a VoxelGI atlas and the last a lightmap
// (mesh_texture_slots).
inline constexpr std::size_t kMeshTextures = 11;

// mesh.hlsl's Surface flags.
inline constexpr std::uint32_t kMeshUnshaded = 1;
inline constexpr std::uint32_t kMeshVertexColorAlbedo = 2;
inline constexpr std::uint32_t kMeshAlphaScissor = 4;
inline constexpr std::uint32_t kMeshEmissionTexture = 8;
inline constexpr std::uint32_t kMeshNormalMap = 16;
inline constexpr std::uint32_t kMeshSideCheck = 32;
inline constexpr std::uint32_t kMeshHasAlpha = 64;
inline constexpr std::uint32_t kMeshDisableFog = 128;
inline constexpr std::uint32_t kMeshLightmap = 256;
inline constexpr std::uint32_t kMeshLightmapCapture = 512;

// mesh.hlsl's terrain flags, in MeshSurfaceUniforms::options' w: terrain shading, depth blending,
// and from kMeshTerrainNormals up, which layers have a normal_roughness texture.
inline constexpr std::uint32_t kMeshTerrain = 1;
inline constexpr std::uint32_t kMeshTerrainDepthBlending = 2;
inline constexpr std::uint32_t kMeshTerrainNormals = 16;

// The vertex stage's uniforms: b0 and b1 in space1. A frame with TAA draws with its camera
// jittered, and its prepass works out each pixel's motion from where the vertex is without the
// jitter and where it was in the frame before.
struct MeshCameraUniforms {
    Mat4 view_projection{};
    Mat4 previous_view_projection{}; // the frame before's, without its jitter
    Float4 jitter{};                 // in clip space, in x and y; view_projection has it
};
struct MeshInstanceUniforms {
    Mat4 model{};
    Mat4 normal_model{};   // the inverse transpose of model's upper 3x3, in its upper 3x3
    Float4 uv1{};          // scale, then offset
    Float4 lightmap_uv{};  // the lightmap's rect of uv2: offset, then scale
    Mat4 previous_model{}; // model in the frame before
};

// A VoxelGI a draw traces, as mesh.hlsl's Surface holds it.
struct MeshVoxelGIUniforms {
    std::array<Float4, 3> rows{}; // the world to its cells' space, a row each
    Float4 size{};                // texels along x, y and z at level 0, then how many levels
    Float4 params{};              // energy, bias, normal bias, 1 to blend with the ambient light
    // Each level's first slice's left and top texels in the atlas and its slices a row, then 0.
    std::array<Float4, kMaxVoxelGILevels> levels{};
};

// The fragment stage's uniforms: b0 and b1 in space3.
struct MeshSurfaceUniforms {
    Float4 albedo{};
    Float4 emission{};
    Float4 surface{}; // metallic, roughness, specular, normal scale
    Float4 metallic_channel{};
    Float4 roughness_channel{};
    // Flags, the scissor threshold's bits, layers, the terrain flags.
    std::array<std::uint32_t, 4> options{};
    Float4 terrain_uv{};      // each ground layer's tiling: the maps' resolution over its uv_scale
    Float4 terrain_opacity{}; // how much the color map tints each ground layer
    Float4 lightmap{};        // the lightmap's exposure, then 1 for the bicubic filter
    std::array<Float4, 9> capture{}; // the light probes' spherical harmonics, RGB
    Float4 voxel_gi{};               // how many of voxel_gis it traces, then 0
    std::array<MeshVoxelGIUniforms, 2> voxel_gis{};
};

// A draw's textures in mesh.hlsl's slots. A StandardMaterial3D's albedo, metallic, roughness,
// emission and normal textures, the rest None but the sixth, its VoxelGIs' atlas, and the last, its
// lightmap; with terrain shading,
// the color map, splat map and normal map, then the four layers' albedo_bump and their
// normal_roughness.
[[nodiscard]] std::array<MaterialTexture, kMeshTextures>
mesh_texture_slots(const Material3D& material);
// Where slot samples for a pixel at uv: the ground layers' slots tile by terrain_uv, the rest
// sample at uv.
[[nodiscard]] std::array<float, 2> mesh_texture_uv(const MeshSurfaceUniforms& surface,
                                                   std::size_t slot, std::array<float, 2> uv);

struct MeshLightUniforms {
    Float4 position{};
    Float4 direction{};
    Float4 color{};
    Float4 params{};
    // Its slot in MeshShadowUniforms' directional or positional, or -1; then 1 when it is baked,
    // then its volumetric fog energy.
    Float4 shadow{};
};
struct MeshSceneUniforms {
    Float4 camera{}; // the camera's place, then how many lights there are in lights
    Float4 ambient{};
    Float4 tonemap{};   // exposure, white, the tonemapper, 1 to tonemap (0 when post passes do)
    Float4 fog_color{}; // linear, times its energy; then 0 for no fog, 1 exponential, 2 depth
    Float4 fog{};       // density, sun scatter, height, height density
    Float4 fog_depth{}; // depth fog's begin, end and curve
    // 1 for ambient light from the radiance, 1 for reflections of it, the background's energy,
    // ambient light's sky contribution.
    Float4 sky{};
    std::array<Float4, 3> sky_rows{}; // the world to the sky's space, a row each
    // Forward+'s clusters (LightClusters::uniform): tiles across, a tile's words of light bits,
    // depth slices per unit of view depth, 0.
    Float4 cluster{};
    // 1 when the frame draws SSAO, its light affect and its AO channel affect.
    Float4 ssao{};
    std::array<Float4, 3> view_rows{}; // the world to the camera's space, a row each, for normals
    // Forward+'s reflection probes: how many, the level the roughest surfaces read, the exposure.
    Float4 reflections{};
    // 1 when meshes trace their VoxelGIs (Forward+, and not a reflection probe's capture), then 1
    // for high quality.
    Float4 voxel_gi{};
    // Forward+'s decals (set_cluster_uniforms): how many, the filter (DecalFilter), the atlas's
    // width and height in texels.
    Float4 decal_info{};
    Float4 screen{}; // the target's width and height in pixels, then 0, 0
    // The camera's view-projection without TAA's jitter: its x, y and w rows, from which decals
    // work out how far their textures move across a pixel.
    std::array<Float4, 3> clip_rows{};
    // Forward+'s volumetric fog (VolumetricFog3D): 1 when the frame has it, one over its length,
    // one over its detail spread and its sky affect; then the volume's froxels across, down and
    // deep, and 0.
    Float4 volumetric_fog{};
    Float4 fog_volume{};
    // Every light drawn in Compatibility; in Forward+, only the directional ones.
    std::array<MeshLightUniforms, kMaxLights3D> lights{};
};

// A probe as mesh.hlsl's probes buffer holds it: t18 in space2 (t5 in Metal, t2 in WebGPU's group
// 0).
struct MeshProbeUniforms {
    std::array<Float4, 3> rows{}; // the world to the probe's space, a row each
    Float4 extents{};             // half the box's size, then the intensity
    Float4 offset{};              // origin_offset, then the blend distance
    Float4 ambient{};             // the ambient color, linear, times its energy; then the mode
    Float4 options{};             // 1 to box project, the cube, the reflection mask, 0
};
static_assert(sizeof(MeshProbeUniforms) == 112);

// A decal as mesh.hlsl's decals buffer holds it: t19 in space2 (t6 in Metal, t3 in WebGPU's group
// 0). Its space runs x and z from 0 to 1 across its box and y from -1 to 1 up it.
struct MeshDecalUniforms {
    std::array<Float4, 3> rows{}; // the world to its space, a row each
    // Its albedo, normal, ORM and emission textures' rects in the atlas: left, top, width and
    // height, as fractions of its size; all 0 for none.
    std::array<Float4, 4> rects{};
    Float4 modulate{};                   // linear, its alpha times the distance fade
    Float4 normal{};                     // its up in the world, then the normal fade
    std::array<Float4, 3> normal_rows{}; // its turn, orthonormalized, a row each
    Float4 params{}; // emission energy times the distance fade, albedo mix, upper and lower fade
    std::array<std::uint32_t, 4> options{}; // its cull mask, then 0
};
static_assert(sizeof(MeshDecalUniforms) == 224);
struct DecalAtlas; // in decal.hpp

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
    Float4 position{}; // the light's place, then its range
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

static_assert(sizeof(MeshCameraUniforms) == 144);
static_assert(sizeof(MeshInstanceUniforms) == 224);
static_assert(sizeof(MeshVoxelGIUniforms) == 240);
static_assert(sizeof(MeshSurfaceUniforms) == 784);
static_assert(sizeof(MeshSceneUniforms) == 384 + (80 * kMaxLights3D));
static_assert(sizeof(MeshDirectionalShadowUniforms) == 320);
static_assert(sizeof(MeshPositionalShadowUniforms) == 576);
static_assert(sizeof(MeshShadowUniforms) ==
              32 + (320 * kMaxDirectionalShadows) + (576 * kMaxPositionalShadows));
static_assert(sizeof(MeshShadowPassUniforms) == 144);

// Godot's Color::srgb_to_linear, for colors the CPU converts before the shader sees them.
[[nodiscard]] float srgb_to_linear_exact(float value);

// Where a camera matrix's camera is in the world: the inverse of the matrix applied to the origin.
[[nodiscard]] Float3 camera_place(const Mat4& camera);
// The scene's camera, with no jitter and as the frame before's.
[[nodiscard]] MeshCameraUniforms mesh_camera_uniforms(const Scene3D& scene);
// A draw's instance, which was at previous in the frame before (at transform without one).
[[nodiscard]] MeshInstanceUniforms mesh_instance_uniforms(const Mat4& transform,
                                                          const Material3D& material);
[[nodiscard]] MeshInstanceUniforms
mesh_instance_uniforms(const Mat4& transform, const Material3D& material, const Mat4& previous);
[[nodiscard]] MeshSurfaceUniforms mesh_surface_uniforms(const Material3D& material,
                                                        std::uint32_t layers);
// The scene's uniforms: in Compatibility its first kMaxLights3D lights, in Forward+ its
// directional ones, which every pixel takes; its omni and spot lights are in its LightClusters.
[[nodiscard]] MeshSceneUniforms mesh_scene_uniforms(const Scene3D& scene);
// A light as the shaders take it, with its shadow slot or -1.
[[nodiscard]] MeshLightUniforms mesh_light_uniforms(const Light3D& light, int shadow_slot);
// How many of the scene's lights a frame draws, from the first: all of them in Forward+, at most
// kMaxLights3D in Compatibility.
[[nodiscard]] std::size_t lights_drawn(const Scene3D& scene);

// Forward+'s clusters, as Godot's ClusterBuilderRD makes them, but on the CPU, so the software
// device's are the same: the screen in tiles kClusterSize pixels across and each tile's depth in
// kClusterSlices slices from the camera to the far plane. cells holds, for each tile in rows from
// the top left, words words with bit i of word i / 32 set for each light that may reach the
// tile, then for each slice the first of those lights in it in its low 16 bits and one past the
// last in its high 16 bits (0 for none); then, when the frame has decals, the same for them in
// decal_words words. The lights are the omni and spot ones, nearest the camera first, so that each
// slice's lights are a short range, and the decals in place_decals' order.
inline constexpr std::uint32_t kClusterSize = 32;
inline constexpr std::uint32_t kClusterSlices = 32;
struct LightClusters {
    std::uint32_t width = 0; // the target's size in pixels
    std::uint32_t height = 0;
    std::uint32_t tiles_across = 0;
    std::uint32_t tiles_down = 0;
    std::uint32_t words = 1;       // the lights over 32, rounded up, and at least 1
    std::uint32_t decal_words = 0; // the decals over 32, rounded up
    float slices_per_depth = 0.0F;
    std::vector<MeshLightUniforms> lights;
    std::vector<MeshDecalUniforms> decals;
    std::vector<std::uint32_t> cells;

    // MeshSceneUniforms::cluster: tiles across, words, slices per unit of depth, decal words.
    [[nodiscard]] Float4 uniform() const;
    // The words each tile takes in cells.
    [[nodiscard]] std::uint32_t stride() const;
};
// The far plane's distance from the camera for a projection.
[[nodiscard]] float far_plane(const Mat4& projection);
// The clusters of a scene drawn to a target width by height pixels, with its decals when it has
// an atlas of their textures. A light reaches every tile and slice its range's sphere could
// cover, rounded out, and a decal those its box's corners could, so that every pixel either
// reaches finds it.
[[nodiscard]] LightClusters build_clusters(const Scene3D& scene, std::uint32_t width,
                                           std::uint32_t height,
                                           const DecalAtlas* decal_atlas = nullptr);

// range_mask in shaders/mesh.hlsl: the bits of word of a tile's bits for the lights or decals from
// first up to end.
[[nodiscard]] std::uint32_t range_mask(std::uint32_t word, std::uint32_t first, std::uint32_t end);

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
    Mat4 previous{};       // transform in the frame before, for TAA's motion vectors
    std::uint32_t lod = 0; // the mesh's own triangles, or one of its LODs'
};

// Each light's shadow slot, as MeshLightUniforms::shadow holds it: drawn lights (lights_drawn)
// with shadow set take the directional or positional slots in order while there are some; the
// rest, and lights without shadows, have -1.
[[nodiscard]] std::vector<int> shadow_slots(const Scene3D& scene);

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
// The inverse of a matrix, in doubles; the identity for one that has none.
[[nodiscard]] std::array<double, 16> inverse_matrix(const Mat4& m);

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

// The checks every device makes on a mesh's vertices, indices and LODs.
[[nodiscard]] Result<void> check_mesh(std::span<const Vertex3D> vertices,
                                      std::span<const std::uint32_t> indices,
                                      std::span<const std::span<const std::uint32_t>> lods = {});
// Where a mesh's triangles, or one of its LODs', are in its index buffer.
struct IndexRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};
// A mesh's index buffer: its indices (0, 1, 2 and on for a mesh without them), then each LOD's;
// ranges gets where each starts, the mesh's first.
[[nodiscard]] std::vector<std::uint32_t>
mesh_index_buffer(std::size_t vertex_count, std::span<const std::uint32_t> indices,
                  std::span<const std::span<const std::uint32_t>> lods,
                  std::vector<IndexRange>& ranges);
// Fails unless lod names a mesh's own triangles (0) or one of its lod_count LODs.
[[nodiscard]] Result<void> check_lod(std::uint32_t lod, std::size_t lod_count);
// The checks every device makes on new vertices for a mesh of count vertices (update_mesh).
[[nodiscard]] Result<void> check_mesh_update(std::span<const Vertex3D> vertices, std::size_t count);
// That a scene's numbers are usable and it has at most kMaxDirectionalLights3D directional lights
// and kMaxClusteredLights3D omni and spot lights.
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

// Fails unless a draw's previous transform, when it has one, is finite.
[[nodiscard]] Result<void> check_previous_transform(const std::optional<Mat4>& previous);

// What the vertex stage leaves at a corner, as vs_main does: where it is in the world and in
// clip space, its normal, tangent and binormal, texture coordinate and color, and for TAA's
// prepass, where it is in clip space without the jitter and where it was in the frame before.
struct MeshCorner {
    Float4 clip{};
    Float3 world{};
    Float3 normal{};
    Float3 tangent{};
    Float3 binormal{};
    std::array<float, 2> uv{};
    Float4 color{};
    std::array<float, 2> uv2{}; // in the lightmap
    Float4 motion_clip{};
    Float4 previous_clip{};
};
// A corner whose vertex was at previous, in the mesh's space, in the frame before (at its
// position without one).
[[nodiscard]] MeshCorner mesh_vertex(const MeshCameraUniforms& camera,
                                     const MeshInstanceUniforms& instance, const Vertex3D& vertex);
[[nodiscard]] MeshCorner mesh_vertex(const MeshCameraUniforms& camera,
                                     const MeshInstanceUniforms& instance, const Vertex3D& vertex,
                                     const Float3& previous);

// What the fragment stage reads at a pixel: where it is in the target, the corners' values there,
// which face it is on, and what each texture holds there.
struct MeshPixel {
    std::uint32_t x = 0; // from the target's left
    std::uint32_t y = 0; // from its top
    Float3 world{};
    Float3 normal{};
    Float3 tangent{};
    Float3 binormal{};
    Float4 color{};
    bool front = true;
    // Each slot's texel, at mesh_texture_uv.
    std::array<Float4, kMeshTextures> texels{white_texels()};
    // The lightmap's light there, as lightmap_light gives it, when the draw has one.
    Float3 lightmap{};
    // The draw's VoxelGI atlas, when it traces VoxelGIs.
    const Image* voxel_gi = nullptr;

    [[nodiscard]] static constexpr std::array<Float4, kMeshTextures> white_texels() {
        std::array<Float4, kMeshTextures> out{};
        out.fill({1.0F, 1.0F, 1.0F, 1.0F});
        return out;
    }
};
// A shadow atlas on the CPU: depths in rows from the top, size texels across.
struct ShadowAtlas {
    std::span<const float> depths;
    std::uint32_t size = 0;

    // SampleCmpLevelZero with a linear GREATER_OR_EQUAL comparison sampler that clamps to the
    // edge: how much of the four texels around (u, v) reference is at least.
    [[nodiscard]] float compare(float u, float v, float reference) const;
};
struct CubeImage; // in sky_shading.hpp

// The maps the fragment stage samples besides the material's textures: the shadow atlases, the
// radiance (none when the scene's meshes aren't lit by it), and in Forward+ the clusters.
struct MeshMaps {
    const MeshShadowUniforms* uniforms = nullptr;
    ShadowAtlas directional;
    ShadowAtlas positional;
    const CubeImage* radiance = nullptr;
    const LightClusters* clusters = nullptr;
    // Forward+'s screen-space ambient occlusion, a value a pixel of the target in rows from the
    // top, when the scene's uniforms say the frame draws it, and its screen-space reflections, four
    // (RGB premultiplied, then how much of the sky's they replace) when they say it draws them.
    std::span<const float> ao;
    std::span<const float> ssr;
    std::uint32_t ao_width = 0; // the target's width, for both
    // Forward+'s reflection probes, when the scene's uniforms say the frame has some, and each
    // atlas cube's levels.
    std::span<const MeshProbeUniforms> probes;
    const std::vector<CubeImage>* probe_cubes = nullptr;
    // Forward+'s decals' atlas, when the clusters have decals.
    const DecalAtlas* decal_atlas = nullptr;
    // Forward+'s volumetric fog (sample_fog_volume), when the scene's uniforms say the frame has
    // it.
    std::span<const Float4> fog;
};

// A half-float texture's texels, as floats: RGBA, in rows from the top.
struct FloatImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;
};
// The image's RGBA at uv, filtered linearly and clamped to its edges, as a linear sampler that
// clamps reads it.
[[nodiscard]] Float4 sample_linear(const FloatImage& image, std::array<float, 2> uv);
// lightmap_light in shaders/mesh.hlsl: the lightmap's light at uv, bicubic or bilinear by the
// surface's lightmap uniform, times its exposure.
[[nodiscard]] Float3 lightmap_light(const MeshSurfaceUniforms& surface, const FloatImage& image,
                                    std::array<float, 2> uv);
// capture_light in shaders/mesh.hlsl: the light probes' light along a world normal.
[[nodiscard]] Float3 capture_light(const MeshSurfaceUniforms& surface, const Float3& n);

// What a pixel's VoxelGIs give it, as trace_voxel_gis in shaders/mesh.hlsl (Godot's process_gi):
// ambient light and reflections, linear, each with how much of the sky's it replaces in alpha.
struct VoxelGILight {
    Float4 ambient{};
    Float4 reflection{};
};
// Traces the surface's VoxelGIs through atlas from a point in the world with a shading normal,
// the direction toward the camera and a roughness, as Godot's gi.glsl: cones for the ambient
// light (four of 45 degrees, or six narrower ones for high quality) and one along the reflection,
// each VoxelGI weighed by how far the point is inside its box.
[[nodiscard]] VoxelGILight trace_voxel_gis(const MeshSurfaceUniforms& surface, bool high_quality,
                                           const Image& atlas, const Float3& world,
                                           const Float3& normal, const Float3& view,
                                           float roughness);

// A VoxelGI as mesh.hlsl's Surface and volumetric_fog.hlsl's Fog hold it.
[[nodiscard]] MeshVoxelGIUniforms voxel_gi_uniforms(const VoxelGIProbe& probe);

// The light a VoxelGI gives volumetric fog at a point of the world, as Godot's
// volumetric_fog_process.glsl gathers it: each level at the point, front to back by how solid the
// ones before are, times its energy; nothing outside its box.
[[nodiscard]] Float3 fog_voxel_gi_light(const MeshVoxelGIUniforms& gi, const Image& atlas,
                                        const Float3& world);

// The pixel's color, as fs_main gives it, or nullopt where it discards the pixel.
[[nodiscard]] std::optional<Float4> shade_mesh(const MeshSurfaceUniforms& surface,
                                               const MeshSceneUniforms& scene, const MeshMaps& maps,
                                               const MeshPixel& pixel);
// fs_main in shaders/mesh_normal.hlsl, Forward+'s normal-roughness prepass: the pixel's normal in
// the camera's space (scaled from -1 to 1 into 0 to 1) and its roughness times 127 / 255, or
// nullopt where it discards the pixel.
[[nodiscard]] std::optional<Float4> mesh_normal_roughness(const MeshSurfaceUniforms& surface,
                                                          const MeshSceneUniforms& scene,
                                                          const MeshPixel& pixel);
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
    if (!finite({m.albedo[0],
                 m.albedo[1],
                 m.albedo[2],
                 m.albedo[3],
                 m.metallic,
                 m.roughness,
                 m.specular,
                 m.emission[0],
                 m.emission[1],
                 m.emission[2],
                 m.emission_energy,
                 m.normal_scale,
                 m.alpha_scissor_threshold,
                 m.uv1_scale[0],
                 m.uv1_scale[1],
                 m.uv1_offset[0],
                 m.uv1_offset[1],
                 m.lightmap.uv_rect[0],
                 m.lightmap.uv_rect[1],
                 m.lightmap.uv_rect[2],
                 m.lightmap.uv_rect[3],
                 m.lightmap.exposure})) {
        return make_error(ErrorCode::InvalidArgument, "a 3D material's numbers are finite");
    }
    for (const std::array<float, 3>& coefficient : m.capture.sh) {
        if (!finite({coefficient[0], coefficient[1], coefficient[2]})) {
            return make_error(ErrorCode::InvalidArgument,
                              "a 3D material's light probe capture is finite");
        }
    }
    if (const VoxelGIShading& gi = m.voxel_gi; gi.atlas != TextureId::None) {
        if (gi.count > gi.probes.size()) {
            return make_error(ErrorCode::InvalidArgument, "a 3D material traces at most " +
                                                              std::to_string(gi.probes.size()) +
                                                              " VoxelGIs");
        }
        for (std::size_t i = 0; i < gi.count; ++i) {
            const VoxelGIProbe& probe = gi.probes.at(i);
            if (!std::ranges::all_of(probe.to_cell, [](float v) { return std::isfinite(v); }) ||
                !finite({probe.energy, probe.bias, probe.normal_bias}) || probe.bias < 0.0F) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a VoxelGI's numbers are finite and its bias at least 0");
            }
            if (probe.levels == 0 || probe.levels > kMaxVoxelGILevels ||
                std::ranges::any_of(
                    probe.size, [](std::uint32_t v) { return v == 0 || v > kMaxVoxelGICells; })) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a VoxelGI has 1 to " + std::to_string(kMaxVoxelGILevels) +
                                      " levels and 1 to " + std::to_string(kMaxVoxelGICells) +
                                      " cells along each axis");
            }
        }
    }
    if (m.terrain.enabled) {
        if (m.terrain.resolution == 0 || m.terrain.resolution > kMaxTextureSize) {
            return make_error(ErrorCode::InvalidArgument,
                              "a terrain's resolution is 1 to " + std::to_string(kMaxTextureSize));
        }
        for (const TerrainLayer& layer : m.terrain.layers) {
            if (!finite({layer.uv_scale, layer.colormap_opacity}) || !(layer.uv_scale > 0.0F)) {
                return make_error(ErrorCode::InvalidArgument,
                                  "a terrain layer's uv_scale is above 0 and its numbers finite");
            }
        }
    }
    for (const MaterialTexture& slot : mesh_texture_slots(m)) {
        if (slot.texture == TextureId::None) {
            continue;
        }
        if (!known(slot.texture)) {
            return unknown_texture(slot.texture);
        }
        if (slot.texture == target) {
            return target_in_use(slot.texture);
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
