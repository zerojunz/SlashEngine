#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {
class World;
}

// Heightmap terrains, as the HTerrain plugin for Godot (zylann.hterrain, MIT): an HTerrain's
// heights a cell apart, read from a 16-bit gray .height.png file, drawn in chunks whose detail
// falls off with distance as its quad tree (util/quad_tree_lod.gd) settles, stitched to coarser
// neighbors as its mesher (hterrain_mesher.gd) stitches them, shaded with its four ground
// textures (render::TerrainShading, its simple4 shader), colliding as a height field and walked
// on by navigation bakes. Its cell space has x and z in cells from the first height and y in
// meters before map_scale.
namespace slash::scene3d {

inline constexpr std::string_view kHeightmapSuffix = ".height.png";

// Registers .height.png files with assets as render::Gray16. Called by add_asset_types.
void add_heightmap_type(asset::Assets& assets);

// The sides of a chunk drawn next to a coarser one, as HTerrainMesher's SEAM_ bits: the chunk's
// edge vertices there skip every other one, so no crack shows.
inline constexpr int kSeamLeft = 1;   // toward -x
inline constexpr int kSeamRight = 2;  // toward +x
inline constexpr int kSeamBottom = 4; // toward -z
inline constexpr int kSeamTop = 8;    // toward +z

// The range map's blocks, as HTerrainDataRangeMap's CHUNK_SIZE: each knows the lowest and
// highest of its 17 x 17 heights, sharing its edges with its neighbors.
inline constexpr int kTerrainRangeBlock = 16;

// What an HTerrain's map files hold.
struct TerrainData {
    int resolution = 0;
    // resolution * resolution heights in meters before map_scale.y, by row (z) then column (x);
    // empty when the terrain can't be made, and problems say why.
    std::vector<float> heights;
    // The maps as images of resolution * resolution pixels, row z at the top; null for one whose
    // file doesn't exist yet: the first texture everywhere, and white with no holes.
    std::shared_ptr<const render::Image> splatmap;
    std::shared_ptr<const render::Image> colormap;
    // The range map: [lowest, highest] per block, range_size blocks each way.
    std::vector<std::array<float, 2>> ranges;
    int range_size = 0;
    float lowest = 0.0F;
    float highest = 0.0F;
    // A number no other data made in this process has, for keeping what is made from it.
    std::uint64_t version = 0;
    // Why the heights can't be made: a resolution or chunk_size HTerrain refuses, a map that
    // doesn't load or isn't resolution pixels square.
    std::vector<Problem> problems;

    [[nodiscard]] bool empty() const noexcept { return heights.empty(); }
    // The height at a cell, cells outside clamped to the edge, as HTerrainData.get_height_at.
    [[nodiscard]] float height(int x, int z) const;
    // The height at a point between cells, bilinear over the four around it, as
    // get_interpolated_height_at.
    [[nodiscard]] float interpolated_height(float x, float z) const;
    // Whether a cell is a hole: its color map pixel's alpha is under half.
    [[nodiscard]] bool hole(int x, int z) const;
    // The [lowest, highest] heights of the block a cell is in, clamped to the map, as
    // HTerrainDataRangeMap.get_point_aabb.
    [[nodiscard]] std::array<float, 2> point_range(int x, int z) const;
};

// The range map of heights, as HTerrainDataRangeMap.update.
void update_ranges(TerrainData& data);

// The heights a .height.png's values stand for: 0 is low and 65535 high.
[[nodiscard]] std::vector<float> decode_heights(const render::Gray16& values, Vec2 range);
// The values that stand for heights, rounded to the nearest and clamped to the range.
[[nodiscard]] render::Gray16 encode_heights(const std::vector<float>& heights, int resolution,
                                            Vec2 range);

// What an HTerrain's maps hold. With install_terrain it is kept until the HTerrain or a map
// changes; without, it is made on each call, the maps read with the world's file reader. A
// terrain that can't be made has no heights and says why in problems.
[[nodiscard]] std::shared_ptr<const TerrainData> terrain_data(const world::World& world,
                                                              entt::entity entity);

// Where an HTerrain's cells are, as HTerrain's get_internal_transform: its entity's place times
// map_scale, moved back half its size when centered.
[[nodiscard]] world::Affine3D terrain_transform(const world::HTerrain& terrain, int resolution,
                                                const world::Affine3D& global);

// The levels of detail a terrain has past the finest, as HT_QuadTreeLod.compute_lod_count: the
// halvings from its resolution to its chunk size.
[[nodiscard]] int terrain_lod_count(int resolution, int chunk_size);

// A chunk drawn at one level of detail: chunk_size quads each way, each 2^lod cells across,
// from cell (x, z) * (chunk_size << lod), with seams toward coarser neighbors.
struct TerrainChunk {
    int x = 0;
    int z = 0;
    int lod = 0;
    int seams = 0;

    friend bool operator==(const TerrainChunk&, const TerrainChunk&) = default;
};

// The chunks drawn for a viewer at viewer in the terrain's cell space, as HTerrain's quad tree
// settles there: a quad splits while the viewer is nearer its middle (at the height of its
// middle's range map block) than chunk_size << lod times lod_scale (2 to 5). In quad tree order.
[[nodiscard]] std::vector<TerrainChunk> terrain_chunks(const TerrainData& data, int chunk_size,
                                                       float lod_scale, Vec3 viewer);

// The triangles of a chunk of quads_x by quads_z quads with seams, as HTerrainMesher's
// make_indices, clockwise from above as Godot's front faces are.
[[nodiscard]] std::vector<std::uint32_t> terrain_indices(int quads_x, int quads_z, int seams);

// A chunk's corners in the terrain's cell space, raised to their heights, each with its texture
// coordinate at its cell's middle in the maps, as HTerrain's vertex shader places them.
[[nodiscard]] std::vector<render::Vertex3D>
terrain_chunk_vertices(const TerrainData& data, int chunk_size, const TerrainChunk& chunk);

// The terrain's normals as an image the size of its maps, in its scaled space, packed as
// n * 0.5 + 0.5: HTerrain's bump2normal, (left - right, 2, back - front) normalized, turned by
// the inverse transpose of map_scale as its u_terrain_normal_basis turns it.
[[nodiscard]] render::Image terrain_normals(const TerrainData& data, Vec3 map_scale);

// The normal at a point between cells in the terrain's scaled space, the normal map sampled
// there.
[[nodiscard]] Vec3 terrain_normal(const TerrainData& data, Vec3 map_scale, float x, float z);

// Every triangle of the terrain at full detail in its cell space, three points each, as Godot's
// HeightMapShape3D parses into navigation source geometry; triangles touching a hole are left
// out.
[[nodiscard]] std::vector<Vec3> terrain_faces(const TerrainData& data);

// Reads heightmaps by project path; map images are read with an ImageLoader
// (gpu_particles.hpp).
using HeightmapLoader =
    std::function<Result<std::shared_ptr<const render::Gray16>>(const std::string& path)>;

// Keeps each HTerrain's data in the world until it changes, reading heightmaps with heightmaps
// and the splat and color maps with images. Sessions and games call it through canvas::install.
void install_terrain(world::World& world, HeightmapLoader heightmaps, ImageLoader images);

// An HTerrain that collides: its layer, mask, physics material and data, placed by map_scale
// and centered in its entity's space.
struct TerrainBody {
    entt::entity entity = entt::null;
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    std::shared_ptr<const TerrainData> data;
    Vec3 map_scale{1.0F, 1.0F, 1.0F};
    bool centered = false;
};
[[nodiscard]] std::vector<TerrainBody> terrain_bodies(const world::World& world);

// Every HTerrain that can't be drawn, in the order entities were made: a resolution that isn't a
// power of two plus one, a chunk_size that isn't 16, 32 or 64, more than four textures, an
// HTerrain on a physics body's entity, and terrain_data's problems.
[[nodiscard]] std::vector<Problem> check_terrains(const world::World& world);

// ---- Making and painting heights, as HTerrain's generator and brushes.

// HTerrain's generator dialog: fractal Perlin noise, curved, shaped into an island or canyon and
// eroded, as its perlin_noise and erode shaders.
struct TerrainNoise {
    int seed = 0;
    Vec2 offset;
    float base_height = -50.0F;
    float height_range = 150.0F;
    float scale = 100.0F; // cells a noise period spans, about
    float roughness = 0.4F;
    float curve = 1.0F;
    int octaves = 6;
    int erosion_steps = 0;
    float erosion_weight = 0.5F;
    float erosion_slope_factor = 0.0F;
    Vec2 erosion_slope_direction;
    bool erosion_slope_invert = false;
    float dilation = 0.0F;
    float island_weight = 0.0F;
    float island_sharpness = 0.0F;
    float island_height_ratio = -1.0F;
    float island_shape = 0.0F;
};
// Heights of a terrain of resolution cells each way made by noise, added to additive's when it
// is given (its additive_heightmap), the same on every machine.
[[nodiscard]] std::vector<float> generate_heights(const TerrainNoise& noise, int resolution,
                                                  const std::vector<float>* additive = nullptr);

// What a brush dab does, as HTerrain's painter modes.
enum class BrushMode : std::uint8_t {
    Raise,   // heights up by amount at the middle
    Lower,   // heights down by amount
    Smooth,  // toward the mean of each height and its four neighbors
    Flatten, // toward height
    Level,   // toward the mean of 5 x 5 heights across the brush
    Splat,   // the splat map toward the texture
    Color,   // the color map's color toward color
    Hole,    // the color map's alpha to 0, cutting a hole
    Unhole,  // and back to 1
};

// One dab of a round brush at a cell, radius cells across from its middle to its edge: each cell
// within takes the brush's value there (1 inside 1 - falloff of the radius, easing to 0 at its
// edge) times opacity, as HTerrain's shaders take their brush texture's red times opacity.
// Splat and color dabs are limited by slope (the normal's y from slope_min to slope_max) as its
// splat4 and color shaders limit them, the normal the ground's once map_scale stretches it.
struct BrushDab {
    BrushMode mode = BrushMode::Raise;
    Vec2 center;
    float radius = 8.0F;
    float falloff = 0.5F;
    float opacity = 1.0F;
    float amount = 1.0F;                // raise and lower, in meters at full value
    float height = 0.0F;                // flatten
    int texture = 0;                    // splat: 0 to 3
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F}; // color, 0 to 1
    float slope_min = 0.0F;
    float slope_max = 1.0F;
    Vec3 map_scale{1.0F, 1.0F, 1.0F}; // the terrain's
};

// A terrain's maps as a brush paints them: heights in meters, and the splat and color maps as
// RGBA pixels, resolution * resolution each.
struct TerrainMaps {
    int resolution = 0;
    std::vector<float> heights;
    std::vector<std::uint8_t> splat;
    std::vector<std::uint8_t> color;
};

// The rectangle of cells a dab changes: [x0, z0, x1, z1), empty when it misses the map.
[[nodiscard]] std::array<int, 4> brush_rect(const BrushDab& dab, int resolution);

// Paints one dab into the maps it touches; returns how many cells changed. The same on every
// machine.
std::size_t paint(TerrainMaps& maps, const BrushDab& dab);

} // namespace slash::scene3d
