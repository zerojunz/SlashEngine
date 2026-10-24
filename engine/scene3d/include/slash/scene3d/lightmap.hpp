#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <map>
#include <memory>
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

// A .lightmap.json file, as Godot's LightmapGIData: the texels' file, the unwraps of the meshes it
// lights by their hash (lightmap_mesh_hash; a model's parts' surfaces are unwrapped together) and
// each mesh's place.
struct LightmapData {
    std::string texture; // its .hdr file: the slices, each width by height, top to bottom
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t slices = 0;
    float baked_exposure = 1.0F; // the exposure it was baked with
    std::map<std::string, LightmapUnwrap, std::less<>> meshes;
    std::vector<LightmapUser> users;

    friend bool operator==(const LightmapData&, const LightmapData&) = default;
};

// The JSON Schema for .lightmap.json files.
[[nodiscard]] json::Value lightmap_schema();
// Every error in a document, each with its JSON Pointer: those of the schema, and users whose
// mesh, slice or rectangle isn't there, and unwraps whose vertices and triangles don't match.
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
// meshes and lights it took, and what it left out.
struct LightmapBake {
    LightmapData data;
    render::HdrImage texels;
    std::size_t meshes = 0;
    std::size_t lights = 0;
    std::vector<Problem> problems;
};

// Bakes meshes lit by lights and environment (a panorama as render::ambient_panorama makes, or
// empty for none) with settings, as Godot's LightmapperRD::bake on the CPU: each mesh unwrapped
// (meshes of equal hashes once), the unwraps packed into slices, and every texel traced, on pool's
// threads where given. The same texels on every platform and for any number of threads.
[[nodiscard]] Result<LightmapBake> bake_lightmap(const std::vector<LightmapMesh>& meshes,
                                                 const std::vector<LightmapLight>& lights,
                                                 const render::HdrImage& environment,
                                                 const LightmapSettings& settings,
                                                 jobs::Pool* pool = nullptr);

// The images and models a bake reads materials from, by project path.
struct LightmapSources {
    std::function<Result<std::shared_ptr<const render::Image>>(const std::string& path)> images;
    ModelLoader models;
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
