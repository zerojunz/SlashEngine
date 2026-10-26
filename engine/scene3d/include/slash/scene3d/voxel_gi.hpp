#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstdint>
#include <entt/entity/entity.hpp>
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
struct VoxelGI;
} // namespace slash::world

// VoxelGI, as Godot 4.6's VoxelGI (scene/3d/voxel_gi.cpp): a box of the world voxelized into an
// octree of cells, each holding the albedo, emission and normal of the static meshes in it
// (scene/3d/voxelizer.cpp), baked into a .voxelgi.json file. Lights are injected into the cells as
// Godot's voxel_gi.glsl injects them on the GPU, here on the CPU on the job pool, whenever the
// lights change: direct light with shadows marched through the cells' distance field, a second
// bounce traced through the first, and the light averaged up the octree. Each level's light is a
// level of the 3D texture lit meshes trace cones through for their ambient light and reflections
// (render::VoxelGIShading).
namespace slash::scene3d {

inline constexpr std::string_view kVoxelGISuffix = ".voxelgi.json";

// A cell as Godot's VoxelGIData keeps its data cells: its place in its level's cells (x, y << 11,
// z << 21), its albedo and how much of it is solid (RGBA8, linear; only the deepest cells have an
// albedo), its emission (RGBE9995) and its normal (three snorm8).
using VoxelGICell = std::array<std::uint32_t, 4>;

// A .voxelgi.json file, as Godot's VoxelGIData: the box it voxelized in its VoxelGI's space, its
// cells along each axis at the deepest level, the exposure its emission was baked with, and its
// cells, level by level from the root (level_counts of each), each level sorted by x, then y, then
// z, as Godot's Voxelizer sorts them.
struct VoxelGIData {
    std::array<float, 3> bounds_position{0.0F, 0.0F, 0.0F};
    std::array<float, 3> bounds_size{0.0F, 0.0F, 0.0F};
    std::array<std::uint32_t, 3> octree_size{0, 0, 0};
    float baked_exposure = 1.0F;
    std::vector<std::uint32_t> level_counts;
    std::vector<VoxelGICell> cells;

    friend bool operator==(const VoxelGIData&, const VoxelGIData&) = default;
};

// The JSON Schema for .voxelgi.json files.
[[nodiscard]] json::Value voxel_gi_schema();
// Every error in a document, each with its JSON Pointer: those of the schema, octree sizes that
// aren't what a VoxelGI makes, level counts that don't add up to the cells, and cells out of order,
// outside their level or without a parent.
[[nodiscard]] std::vector<json::ValidationError> validate_voxel_gi(const json::Value& doc);
// The data in a document, or its first error.
[[nodiscard]] Result<VoxelGIData> parse_voxel_gi(const json::Value& doc);
// The document of the data, in canonical form: its cells as base64 of little-endian values.
[[nodiscard]] json::Value voxel_gi_json(const VoxelGIData& data);

// Registers .voxelgi.json files with assets as VoxelGIData. Called by add_asset_types.
void add_voxel_gi_types(asset::Assets& assets);

// The cells along each axis of a VoxelGI's box of size at subdiv (6 for "64" up to 9 for "512"),
// as Godot's VoxelGI::get_estimated_cell_size: 2^subdiv along its longest side, halved along each
// other side while half the longest side still covers it.
[[nodiscard]] std::array<std::uint32_t, 3> voxel_gi_octree_size(std::array<float, 3> size,
                                                                int subdiv);
// The depth of the octree a VoxelGI of subdiv "64", "128", "256" or "512" bakes: 6 to 9, or 0 for
// any other name.
[[nodiscard]] int voxel_gi_subdiv(std::string_view name);
// The transform from a VoxelGI's space to its cells', as Godot's to_cell_xform: the box's corner
// at 0, one unit a cell.
[[nodiscard]] world::Affine3D voxel_gi_to_cell(const VoxelGIData& data);

// What a voxelized surface is made of, as Godot's Voxelizer bakes a material: its albedo and its
// emission, linear, as kVoxelGIMaterialSize texels square, the textures shrunk with Godot's cubic
// filter and read at a point's texture coordinate wrapped, and the faces a moving mesh's material
// hides when it is plotted (the voxelizer takes every face).
inline constexpr std::uint32_t kVoxelGIMaterialSize = 128;
struct VoxelGIMaterial {
    std::vector<std::array<float, 3>> albedo;
    std::vector<std::array<float, 3>> emission; // times its energy and the bake's exposure
    render::CullMode cull_mode = render::CullMode::Back;
};
// A StandardMaterial3D as a VoxelGI bakes it (Voxelizer::_get_material_cache): its albedo color
// times its albedo texture, and when emission_enabled, its emission color plus its emission
// texture, both times emission_energy_multiplier and exposure, and its cull_mode. Images left out
// are not there.
[[nodiscard]] VoxelGIMaterial voxel_gi_material(const world::StandardMaterial3D& standard,
                                                const render::Image* albedo,
                                                const render::Image* emission, float exposure);

// A mesh a bake voxelizes: its triangles in its own space, where it is in the VoxelGI's space, and
// what each run of them is made of.
struct VoxelGIMesh {
    world::Affine3D transform;
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices;
    struct Surface {
        std::uint32_t first_index = 0; // the triangles from here to the next surface's
        std::shared_ptr<const VoxelGIMaterial> material;
    };
    std::vector<Surface> surfaces;
};

// Voxelizes meshes in a VoxelGI's box of size (centered on its origin) at subdiv, as Godot's
// Voxelizer: each triangle that touches the box goes down an octree of cells to the deepest level,
// each cell there takes its albedo, emission and normal from a 4x4 scan of it, then the cells are
// sorted and the levels above take how much of them is solid. exposure is the baked_exposure the
// materials' emission has. The same cells on every platform.
[[nodiscard]] VoxelGIData voxelize(std::span<const VoxelGIMesh> meshes, std::array<float, 3> size,
                                   int subdiv, float exposure = 1.0F);

// A cell's missing child.
inline constexpr std::uint32_t kVoxelGIEmpty = 0xFFFFFFFF;

// What injecting light needs besides the cells: each cell's children (bit 0 of their index is x,
// bit 1 y and bit 2 z), the first cell of each level and one past the last, and the distance from
// each place at the deepest level to the nearest cell there, as Godot's get_sdf_3d_image: 0 for a
// cell, else 1 more than the distance in cells, up to 255.
struct VoxelGIOctree {
    std::vector<std::array<std::uint32_t, 8>> children;
    std::vector<std::uint32_t> level_offsets;
    std::vector<std::uint8_t> distance_field; // x fastest, then y, then z
};
// The octree of valid data (validate_voxel_gi), on pool's threads where given.
[[nodiscard]] VoxelGIOctree voxel_gi_octree(const VoxelGIData& data, jobs::Pool* pool = nullptr);

// A light injected into a VoxelGI's cells, as Godot's VoxelGILight: in the cells' space, energy
// light_energy times light_indirect_energy, color linear, radius its range in cells, direction
// the way it shines.
struct VoxelGILight {
    render::LightType type = render::LightType::Directional;
    float energy = 1.0F;
    float radius = 0.0F;
    float attenuation = 1.0F;
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    float cos_spot_angle = 0.0F;
    float inv_spot_attenuation = 1.0F;
    std::array<float, 3> position{0.0F, 0.0F, 0.0F};
    std::array<float, 3> direction{0.0F, 0.0F, -1.0F};
    bool has_shadow = false;

    friend bool operator==(const VoxelGILight&, const VoxelGILight&) = default;
};

// How lights are injected, as a VoxelGI's settings: the range light is stored over, how much light
// spreads up the octree, and whether a second bounce is traced.
struct VoxelGISettings {
    float dynamic_range = 2.0F;
    float propagation = 0.5F;
    bool use_two_bounces = true;

    friend bool operator==(const VoxelGISettings&, const VoxelGISettings&) = default;
};

// A VoxelGI's light as its 3D texture: level 0 has octree_size texels, each next level half as
// many along each axis (at least 1), as many levels as the octree has; RGBA8 texels, x fastest,
// then y, then z, each the cell's light over dynamic_range and how much of the cell is solid, and
// 0 where there's no cell.
struct VoxelGITexture {
    std::vector<std::array<std::uint32_t, 3>> sizes;
    std::vector<std::vector<std::uint8_t>> levels;
};
// The light of lights in the cells, as Godot's VoxelGIInstance::update: each deepest cell's light
// (MODE_COMPUTE_LIGHT), averaged up the octree (MODE_UPDATE_MIPMAPS) and written into the texture
// (MODE_WRITE_TEXTURE); then with use_two_bounces again with each deepest cell's light plus what
// six cones traced through that texture bring it (MODE_SECOND_BOUNCE). Cells are lit on pool's
// threads where given; the same texels on every platform and for any number of threads.
[[nodiscard]] VoxelGITexture light_voxel_gi(const VoxelGIData& data, const VoxelGIOctree& octree,
                                            std::span<const VoxelGILight> lights,
                                            const VoxelGISettings& settings,
                                            jobs::Pool* pool = nullptr);

// The meshes of a MeshInstance3D entity as a VoxelGI takes them, each in the entity's own space:
// its primitive mesh, or each part of its model at rest, with its StandardMaterial3D or else the
// model's own materials, read through sources (lightmap.hpp's) with their emission at exposure.
// What can't be read is a problem.
[[nodiscard]] std::vector<VoxelGIMesh>
voxel_gi_meshes(const world::World& world, entt::entity entity, const LightmapSources& sources,
                float exposure, std::vector<Problem>& problems);

// Moving meshes plotted into a VoxelGI's light, as Godot's dynamic objects
// (VoxelGIInstance::update): each object (its transform into the VoxelGI's space) is drawn from
// the six faces of its box into maps 16 times finer than the cells, its nearest surfaces' albedo,
// normal and emission lit by lights as the cells are (MODE_DYNAMIC_LIGHTING), then halved level by
// level and blended over each level of texture from level 0 (MODE_DYNAMIC_SHRINK_PLOT). Pixels
// are lit on pool's threads where given; the same texels on every platform and for any number of
// threads. A texture that isn't the data's light is left alone.
void plot_voxel_gi_objects(VoxelGITexture& texture, const VoxelGIData& data,
                           const VoxelGIOctree& octree, std::span<const VoxelGIMesh> objects,
                           std::span<const VoxelGILight> lights, const VoxelGISettings& settings,
                           jobs::Pool* pool = nullptr);

// A VoxelGI's texture as the atlas render devices sample (render::VoxelGIShading): each level's z
// slices side by side, as many a row as fit in a width of at most 4096 texels (8192 or 16384
// when the atlas would otherwise be taller than render::kMaxTextureSize), the levels one under
// another from level 0; texels between slices are 0.
struct VoxelGIAtlas {
    render::Image image;
    std::uint32_t levels = 0;
    // Each level's first slice's left and top texels and its slices a row.
    std::array<std::array<std::uint32_t, 3>, render::kMaxVoxelGILevels> placement{};
};
// The atlas of a texture of 1 to render::kMaxVoxelGILevels levels, or an error where it is too
// big for one.
[[nodiscard]] Result<VoxelGIAtlas> voxel_gi_atlas(const VoxelGITexture& texture);

// The VoxelGI a mesh traces (render::VoxelGIProbe), of the component voxel_gi placed at transform
// with data's light in atlas, as Godot's GI::setup_voxel_gi_instances sets one up: the world to its
// cells, the atlas's levels, its bias, normal bias and interior, and its light's scale, its
// dynamic range times its energy times exposure_normalization (the camera's exposure
// normalization over data's baked_exposure where the camera has attributes, else 1).
[[nodiscard]] render::VoxelGIProbe
voxel_gi_probe(const world::VoxelGI& voxel_gi, const VoxelGIData& data, const VoxelGIAtlas& atlas,
               const world::Affine3D& transform, float exposure_normalization = 1.0F);

// The light from a light in the world (render::Light3D, light_energy times
// light_indirect_energy) that a VoxelGI placed at transform injects, as VoxelGIInstance::update
// turns it into the cells' space; it casts shadows through the cells where light.shadow is set.
// Lights Godot leaves out (a directional light whose sky_mode is "sky_only") are the caller's to
// leave out.
[[nodiscard]] VoxelGILight voxel_gi_light(const render::Light3D& light, float indirect_energy,
                                          const VoxelGIData& data,
                                          const world::Affine3D& transform);

// What a VoxelGI bake makes: the data, the meshes it voxelized and what it left out.
struct VoxelGIBake {
    VoxelGIData data;
    std::size_t meshes = 0;
    std::vector<Problem> problems;
};

// Bakes the cells of a VoxelGI entity, as Godot's VoxelGI::bake: the static MeshInstance3Ds
// (gi_mode "static") visible under the entity's parent (everything for a root) that touch its box,
// with their materials read through sources (lightmap.hpp's), at its subdiv, its emission at the
// exposure_multiplier of camera_attributes' CameraAttributesPractical.
[[nodiscard]] Result<VoxelGIBake> bake_voxel_gi(const world::World& world, entt::entity entity,
                                                const LightmapSources& sources);

// Each VoxelGI's problems, for scene validate and exports: a subdiv that isn't "64", "128", "256"
// or "512", a size that isn't positive, camera_attributes naming no CameraAttributesPractical,
// and a data file that isn't a .voxelgi.json file, doesn't read or isn't valid.
[[nodiscard]] std::vector<Problem> check_voxel_gis(const world::World& world);

} // namespace slash::scene3d
