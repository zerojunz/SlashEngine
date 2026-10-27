#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <climits>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::asset {
class Assets;
}
namespace slash::jobs {
class Pool;
}
namespace slash::world {
class World;
}

// Baked lightmaps, as Godot 4.6's LightmapGI (scene/3d/lightmap_gi.cpp) baked by its GPU
// lightmapper (modules/lightmapper_rd), here on the CPU. Each static mesh is unwrapped into
// lightmap UVs with xatlas, as Godot's "Unwrap UV2 for Lightmap/AO" unwraps it; the meshes' places
// are packed into an atlas; and every texel's light is traced: direct light from the baked lights
// with soft shadows, bounces between surfaces and light from the environment, then denoised,
// dilated and blended across seams. The result is a .lightmap.json file and the .hdr file of its
// texels, which lit meshes then take their ambient light from (render::LightmapShading).
namespace slash::scene3d {

inline constexpr std::string_view kLightmapSuffix = ".lightmap.json";

// Godot's default lightmap texel size when it unwraps a mesh: 0.2 units a texel.
inline constexpr float kLightmapTexelSize = 0.2F;

// A mesh unwrapped into lightmap UVs, as Godot's ArrayMesh::lightmap_unwrap with xatlas: its
// triangles cut into charts, laid flat and packed into width by height texels at the texel size,
// one texel between charts. Vertices are the source's, split where charts meet.
struct LightmapUnwrap {
    std::vector<std::uint32_t> xref;       // each vertex's source vertex
    std::vector<std::array<float, 2>> uv2; // each vertex's lightmap UV, from 0 to 1
    std::vector<std::uint32_t> indices;    // its triangles, in the source's order
    std::uint32_t width = 0;               // Godot's lightmap_size_hint, in texels
    std::uint32_t height = 0;

    friend bool operator==(const LightmapUnwrap&, const LightmapUnwrap&) = default;
};

// The mesh unwrapped, the same on every platform. A mesh without a triangle of any area can't be.
[[nodiscard]] Result<LightmapUnwrap> unwrap_lightmap(std::span<const render::Vertex3D> vertices,
                                                     std::span<const std::uint32_t> indices,
                                                     float texel_size = kLightmapTexelSize);

// What an unwrap of a mesh depends on, as 16 hex digits: its positions, normals and triangles.
// A baked lightmap names its meshes so, and a mesh whose hash differs since is baked again.
[[nodiscard]] std::string lightmap_mesh_hash(std::span<const render::Vertex3D> vertices,
                                             std::span<const std::uint32_t> indices);

// One mesh's place in a baked lightmap, as Godot's LightmapGIData user: a static MeshInstance3D's
// mesh, or one part of its model.
struct LightmapUser {
    std::string entity;     // the entity's ID
    std::uint32_t part = 0; // the model's part, 0 for a primitive mesh
    std::string mesh;       // its unwrap, in LightmapData::meshes
    std::uint32_t slice = 0;
    // Where its texels are in the slice: offset x, y, then scale x, y, as Godot's uv_scale.
    std::array<float, 4> uv_rect{0.0F, 0.0F, 1.0F, 1.0F};

    friend bool operator==(const LightmapUser&, const LightmapUser&) = default;
};

// The light reaching a point from every direction, as Godot's light probes keep it: nine
// spherical harmonics coefficients (l0, then l1 for y, z and x, then l2 for xy, yz, 3z²-1, xz and
// x²-y², Godot's order), each linear RGB.
using LightmapSh = std::array<std::array<float, 3>, 9>;

// A leaf of a probe BSP tree with no tetrahedron.
inline constexpr std::int32_t kLightmapBspEmpty = INT32_MIN;

// A node of the BSP tree that finds the tetrahedron of light probes around a point, as Godot's
// LightmapGI::BSPNode: a point with normal·p > d goes over it, others under. Each side is another
// node's index (always later), -(tetrahedron + 1) for a leaf or kLightmapBspEmpty.
struct LightmapBspNode {
    std::array<float, 4> plane{0.0F, 0.0F, 0.0F, 0.0F}; // normal x, y, z, then d
    std::int32_t over = kLightmapBspEmpty;
    std::int32_t under = kLightmapBspEmpty;

    friend bool operator==(const LightmapBspNode&, const LightmapBspNode&) = default;
};

// A lightmap's light probes, as Godot's LightmapGIData capture data, which meshes that move take
// their ambient light from: the probes' places and light in the LightmapGI's space, the Delaunay
// tetrahedra between them, the BSP tree over those, and the bounds they fill. None when points is
// empty.
struct LightmapProbes {
    std::array<float, 3> bounds_position{0.0F, 0.0F, 0.0F};
    std::array<float, 3> bounds_size{0.0F, 0.0F, 0.0F};
    bool interior = false; // meshes inside take only interior lightmaps' light, as Godot's
    std::vector<std::array<float, 3>> points;
    std::vector<LightmapSh> sh; // each point's light
    std::vector<std::array<std::uint32_t, 4>> tetrahedra;
    std::vector<LightmapBspNode> bsp;

    friend bool operator==(const LightmapProbes&, const LightmapProbes&) = default;
};

// A .lightmap.json file, as Godot's LightmapGIData: the texels' file, the unwraps of the meshes it
// lights by their hash (lightmap_mesh_hash; a model's parts' surfaces are unwrapped together),
// each mesh's place and its light probes.
struct LightmapData {
    std::string texture; // its .hdr file: the slices, each width by height, top to bottom
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t slices = 0;
    float baked_exposure = 1.0F; // the exposure it was baked with
    std::map<std::string, LightmapUnwrap, std::less<>> meshes;
    std::vector<LightmapUser> users;
    LightmapProbes probes;

    friend bool operator==(const LightmapData&, const LightmapData&) = default;
};

// The JSON Schema for .lightmap.json files.
[[nodiscard]] json::Value lightmap_schema();
// Every error in a document, each with its JSON Pointer: those of the schema, and users whose
// mesh, slice or rectangle isn't there, unwraps whose vertices and triangles don't match, and
// probes whose light, tetrahedra or BSP tree don't fit their points.
[[nodiscard]] std::vector<json::ValidationError> validate_lightmap(const json::Value& doc);
// The lightmap in a document, or its first error.
[[nodiscard]] Result<LightmapData> parse_lightmap(const json::Value& doc);
// The document of a lightmap, in canonical form: unwraps' arrays as base64 of little-endian
// values.
[[nodiscard]] json::Value lightmap_json(const LightmapData& data);

// Registers .lightmap.json files with assets as LightmapData, and .hdr files as render::HdrImage.
// Called by add_asset_types.
void add_lightmap_types(asset::Assets& assets);

// The light a bake traces, as Godot's LightmapGI properties (world::LightmapGI's docs).
struct LightmapSettings {
    std::uint32_t rays = 128; // a texel's rays: Godot's quality, from 32 (low) to 2048 (ultra)
    std::uint32_t bounces = 3;
    float bounce_indirect_energy = 1.0F;
    bool use_texture_for_bounces = true;
    bool use_denoiser = true;
    float denoiser_strength = 0.1F;
    std::uint32_t denoiser_range = 10;
    float bias = 0.0005F;
    float texel_scale = 1.0F;
    std::uint32_t max_texture_size = 16384;
    float supersampling = 1.0F; // supersampling_factor when supersampling, else 1
    float exposure = 1.0F;      // the camera attributes' exposure multiplier
    // A device that traces the light on its GPU (RenderDevice::trace_lightmap), as Godot's
    // LightmapperRD does; with none, or one that can't, the CPU traces it.
    render::RenderDevice* device = nullptr;
};

// A point on a mesh's surface, where a bake asks what it is made of: its texture coordinate
// and its vertex color, linear.
struct LightmapPoint {
    std::array<float, 2> uv{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
};

// A mesh a bake lights: its triangles, as in its own space, where the world puts it, and what
// each part of it is made of.
struct LightmapMesh {
    std::string entity;
    std::uint32_t part = 0;
    world::Affine3D transform;              // where it is in the world
    std::vector<render::Vertex3D> vertices; // in its own space
    std::vector<std::uint32_t> indices;
    float texel_scale = 1.0F; // gi_lightmap_texel_scale
    // A run of its triangles drawn with one material: albedo (linear RGB, alpha) and emission
    // (linear, times its energy) at a point, as Godot bakes a material into UV2 space, and the
    // faces it culls.
    struct Surface {
        std::uint32_t first_index = 0; // the triangles from here to the next surface's
        std::function<std::array<float, 4>(const LightmapPoint& at)> albedo;
        std::function<std::array<float, 3>(const LightmapPoint& at)> emission;
        render::CullMode cull = render::CullMode::Back;
    };
    std::vector<Surface> surfaces;
};

// A light a bake traces, as Godot's LightmapperRD::Light: color linear, size a directional
// light's tangent of its angular distance, else its radius.
struct LightmapLight {
    render::LightType type = render::LightType::Directional;
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    float energy = 1.0F;
    float indirect_energy = 1.0F; // times its light in bounces
    std::array<float, 3> position{};
    std::array<float, 3> direction{0.0F, 0.0F, -1.0F};
    float range = 5.0F;
    float attenuation = 1.0F;
    float spot_angle = 45.0F; // degrees
    float spot_attenuation = 1.0F;
    float size = 0.0F;
    float shadow_blur = 1.0F;
    bool static_bake = false; // its direct light is in the lightmap too, not only its bounces
};

// What a bake makes: the lightmap's data (its texture path left for the caller) and texels, the
// meshes and lights it took, and what it left out. The low-level bake leaves the probes' places
// to its caller and gives their light in probe_sh.
struct LightmapBake {
    LightmapData data;
    render::HdrImage texels;
    std::size_t meshes = 0;
    std::size_t lights = 0;
    std::vector<LightmapSh> probe_sh;
    std::vector<Problem> problems;
    bool traced_on_gpu = false; // whether LightmapSettings::device traced its light
};

// Where a bake traces light probes, as LightmapperRD::add_probe, each traced with rays rays: the
// points in the world, and the space their light is kept in (only its axes count, as a
// LightmapGI's rotation), so that a probe's light turns with its LightmapGI.
struct LightmapProbeRequest {
    std::vector<std::array<float, 3>> points;
    world::Affine3D space;
    std::uint32_t rays = 256; // Godot's medium_quality_probe_ray_count
};

// Bakes meshes lit by lights and environment (a panorama as render::ambient_panorama makes, or
// empty for none) with settings, as Godot's LightmapperRD::bake on the CPU: each mesh unwrapped
// (meshes of equal hashes once), the unwraps packed into slices, and every texel traced, then the
// probes, on pool's threads where given. The same texels and probes on every platform and for any
// number of threads. With settings' device, its GPU traces the direct light, bounces and probes
// (traced_on_gpu): the same light but for rounding, and the noise that follows from it.
[[nodiscard]] Result<LightmapBake>
bake_lightmap(const std::vector<LightmapMesh>& meshes, const std::vector<LightmapLight>& lights,
              const render::HdrImage& environment, const LightmapSettings& settings,
              jobs::Pool* pool = nullptr, const LightmapProbeRequest& probes = {});

// Where a LightmapGI's light probes go, as LightmapGI::bake places them, from its meshes'
// triangles (three points each) and the LightmapProbes' places, all in its space: bounds around
// both, grown a little; the LightmapProbes' places first, then the bounds' 8 corners, and when
// subdiv isn't 0 the corners of the cells of an octree subdiv cells along its longest side that
// triangles touch, leaving out those within half a cell of a LightmapProbe. The bounds come back
// in probes' bounds_position and bounds_size, the places in its points.
[[nodiscard]] LightmapProbes place_lightmap_probes(std::span<const std::array<float, 3>> triangles,
                                                   std::span<const std::array<float, 3>> user,
                                                   int subdiv);

// The Delaunay tetrahedra between points, as Godot's Delaunay3D::tetrahedralize: Bowyer-Watson
// over the points scaled into a unit cube and quantized, with its acceleration grid, flat
// tetrahedra left out. Where Godot tests circumspheres in 128-bit fixed point with a tolerance,
// which lets tetrahedra overlap when points lie in flat layers or on a grid, the test here is
// exact. The same on every platform.
[[nodiscard]] std::vector<std::array<std::uint32_t, 4>>
tetrahedralize(std::span<const std::array<float, 3>> points);

// The BSP tree that finds the tetrahedron around a point, as LightmapGI::_compute_bsp_tree: each
// node split by the tetrahedra's face that divides them most evenly, scored on pool's threads
// where given. Empty for fewer than two tetrahedra.
[[nodiscard]] std::vector<LightmapBspNode>
lightmap_bsp_tree(std::span<const std::array<float, 3>> points,
                  std::span<const std::array<std::uint32_t, 4>> tetrahedra,
                  jobs::Pool* pool = nullptr);

// The light at a point in a LightmapGI's space, as LightStorage::lightmap_tap_sh_light: the
// probes of the tetrahedron its BSP tree finds blended by the point's barycentric coordinates,
// each clamped from 0 to 1, or nothing where the tree finds none.
[[nodiscard]] LightmapSh tap_lightmap_probes(const LightmapProbes& probes,
                                             std::array<float, 3> point);

// sh turned by a rotation (only basis's axes, orthonormalized, count), with John Hable's code as
// Godot's Basis::rotate_sh, its coefficients' signs matched to Godot's lightmapper.
void rotate_sh(LightmapSh& sh, const world::Affine3D& basis);

// A lightmap's probes where they are drawn, for lightmap_capture: its LightmapGI's place in the
// world, and the camera's exposure over the one it was baked with.
struct LightmapCaptureSource {
    const LightmapProbes* probes = nullptr;
    world::Affine3D transform;
    float exposure = 1.0F;
};

// The light a moving mesh centered at center (in the world) takes, as Godot's
// _update_instance_lightmap_captures: each lightmap's probes whose bounds hold it tapped and
// turned into the world, blended by how far inside it is, interior lightmaps standing in for the
// others. nullopt when no lightmap's bounds hold it.
[[nodiscard]] std::optional<LightmapSh>
lightmap_capture(std::span<const LightmapCaptureSource> lightmaps, std::array<float, 3> center);

// The images and models a bake reads materials from, by project path.
struct LightmapSources {
    std::function<Result<std::shared_ptr<const render::Image>>(const std::string& path)> images;
    ModelLoader models;
    render::RenderDevice* device = nullptr; // LightmapSettings::device
};

// Bakes the lightmap of a LightmapGI entity, as Godot's LightmapGI::bake: the static
// MeshInstance3Ds (gi_mode "static") visible under the entity's parent (everything for a root),
// lit by the lights there whose light_bake_mode isn't "disabled", and the environment its
// environment_mode names, on the world's pool. Skinned model parts are left out, with a problem.
[[nodiscard]] Result<LightmapBake> bake_lightmap(const world::World& world, entt::entity entity,
                                                 const LightmapSources& sources);

// Each LightmapGI's problems, for scene validate and exports: camera_attributes naming no
// CameraAttributesPractical, and a light_data file that isn't a .lightmap.json file, doesn't
// read, isn't valid or whose texture doesn't read.
[[nodiscard]] std::vector<Problem> check_lightmaps(const world::World& world);

// The lightmap path a LightmapGI's .hdr file takes beside its .lightmap.json file:
// "level.lightmap.json" has "level.lightmap.hdr".
[[nodiscard]] std::string lightmap_texture_path(std::string_view data_path);

} // namespace slash::scene3d
