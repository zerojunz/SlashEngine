#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Tile maps, as Godot's TileSet and TileMapLayer. A .tileset.json file cuts atlas textures into
// tiles and names the terrains autotiling connects:
//
//   {
//     "format": 1,
//     "tile_size": [16, 16],
//     "terrain_sets": {
//       "ground": {"mode": "match_corners_and_sides", "terrains": ["grass", "water"]}
//     },
//     "sources": {
//       "ground": {
//         "texture": "tiles/ground.png",
//         "tiles": {
//           "0,0": {"name": "water", "terrain": "water"},
//           "1,0": {"name": "grass", "terrain": "grass"},
//           "2,0": {"name": "grass_n", "terrain": "grass",
//                   "peering": {"n": "water", "ne": "water", "nw": "water"}}
//         }
//       }
//     }
//   }
//
// A tile's "peering" says which terrain each neighbor must have for it to fit, as Godot's
// terrain peering bits; a neighbor left out must have the tile's own terrain, and null means no
// terrain. A TileMapLayer holds its cells as text, rows of legend keys (see decode_cells), and
// paint_terrain picks the tiles that connect, as Godot's terrain painting does.
//
// As in Godot, a tile set's "physics_layers" give tiles collision: each tile's "collision"
// polygons, in pixels from the tile's center, make a layer's cells solid through 2D physics. A
// tile's "alternatives" are its other versions, flipped, transposed or tinted, by number from 1;
// and any cell can flip or transpose its tile, as Godot's runtime tile transforms do.
namespace slash::canvas {

inline constexpr std::string_view kTileSetSuffix = ".tileset.json";

// How terrain tiles connect, as Godot's terrain modes: by all eight cells around them, by the
// four at their corners, or by the four at their sides.
enum class TerrainMode : std::uint8_t { CornersAndSides, Corners, Sides };

// A cell's neighbors, as Godot's CellNeighbor, by compass direction with north up (-y).
enum class Neighbor : std::uint8_t { N, NE, E, SE, S, SW, W, NW };
inline constexpr std::array<std::string_view, 8> kNeighborNames = {"n", "ne", "e", "se",
                                                                   "s", "sw", "w", "nw"};
inline constexpr std::array<Vec2i, 8> kNeighborOffsets = {
    {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}}};

// Cells in rows, top to bottom, each row left to right.
struct CellOrder {
    [[nodiscard]] bool operator()(Vec2i a, Vec2i b) const noexcept {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    }
};

// A tile: its atlas source, its column and row there and its alternative (0 for the tile
// itself), and how the cell turns it, as Godot's TRANSFORM_FLIP_H, TRANSFORM_FLIP_V and
// TRANSFORM_TRANSPOSE: transposed (x and y swapped) first, then flipped.
struct TileRef {
    std::string source;
    Vec2i atlas;
    int alternative = 0;
    bool flip_h = false;
    bool flip_v = false;
    bool transpose = false;

    friend bool operator==(const TileRef&, const TileRef&) = default;
};

// A polygon that makes a tile solid in one of its tile set's physics layers, as Godot's
// TileData collision polygons.
struct TilePolygon {
    int layer = 0;            // into TileSet::physics_layers
    std::vector<Vec2> points; // in pixels from the tile's center
    bool one_way = false;
    float one_way_margin = 1.0F;

    friend bool operator==(const TilePolygon&, const TilePolygon&) = default;
};

// Another version of a tile, as Godot's alternative tiles: its own look and its own collision
// polygons (none unless it gives some). Its flips change only how it draws, as in Godot; a
// cell's turns flip the polygons too.
struct AlternativeTile {
    bool flip_h = false;
    bool flip_v = false;
    bool transpose = false;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    std::vector<TilePolygon> collision;
};

struct TileData {
    std::string name;
    int terrain = -1; // into TileSet::terrains, or -1 for none
    // The terrain each neighbor must have, by Neighbor, or -1 for none.
    std::array<int, 8> peering{-1, -1, -1, -1, -1, -1, -1, -1};
    // How often autotiling picks this tile over others that fit as well.
    double probability = 1.0;
    std::vector<TilePolygon> collision;
    std::map<int, AlternativeTile> alternatives; // by number, from 1
};

// A tile set's physics layer, as Godot's: what its tiles' polygons collide as, and with.
struct PhysicsLayer {
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
};

struct AtlasSource {
    std::string texture;
    Vec2i region_size; // each tile's size in the texture, in pixels
    Vec2i margins;     // pixels before the first column and row
    Vec2i separation;  // pixels between columns and rows
    std::map<Vec2i, TileData, CellOrder> tiles;
};

struct Terrain {
    std::string name;
    int set = 0; // into TileSet::terrain_sets
};

struct TerrainSet {
    std::string name;
    TerrainMode mode = TerrainMode::CornersAndSides;
};

struct TileSet {
    Vec2i tile_size{16, 16}; // a cell's size, in pixels
    std::vector<PhysicsLayer> physics_layers;
    std::map<std::string, AtlasSource, std::less<>> sources;
    std::vector<TerrainSet> terrain_sets;
    std::vector<Terrain> terrains;
    std::map<std::string, TileRef, std::less<>> names; // tiles that have a name

    // A tile's data, or nullptr if the set has no such tile. An alternative's is its tile's.
    [[nodiscard]] const TileData* find(const TileRef& tile) const;
    // A tile's alternative, or nullptr for alternative 0 or one the tile doesn't have.
    [[nodiscard]] const AlternativeTile* alternative(const TileRef& tile) const;
    // A terrain's index, or -1.
    [[nodiscard]] int terrain(std::string_view name) const;
};

[[nodiscard]] std::string_view terrain_mode_name(TerrainMode mode);

// JSON Schema for .tileset.json files.
[[nodiscard]] json::Value tileset_schema();

// Every error in a .tileset.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_tileset(const json::Value& doc);

// The tile set in a .tileset.json document, or its first error.
[[nodiscard]] Result<TileSet> parse_tileset(const json::Value& doc);

// A tile by name or as "source:x,y", then optionally ":" and an alternative's number, then ":"
// and how the cell turns it, letters in this order: h (flipped horizontally), v (flipped
// vertically), t (transposed). "crate", "ground:2,0", "crate:1", "crate:hv" and
// "ground:2,0:1:t" are tiles. The error says what is wrong, with "did you mean".
[[nodiscard]] Result<TileRef> find_tile(const TileSet& set, std::string_view text);

// How legends write a tile, as find_tile reads it: its name, or "source:x,y" when it has none,
// then its alternative if not 0 and its turns if any.
[[nodiscard]] std::string tile_text(const TileSet& set, const TileRef& tile);

// How a tile draws in a cell: its alternative's flips with the cell's, and its alternative's
// modulate.
struct TileLook {
    bool flip_h = false;
    bool flip_v = false;
    bool transpose = false;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
};
[[nodiscard]] TileLook tile_look(const TileSet& set, const TileRef& tile);

// A tile's collision polygons as a cell places them, in pixels from the cell's center: the
// tile's or its alternative's, transposed then flipped as the cell turns it (Godot's
// TileData::get_transformed_vertices).
[[nodiscard]] std::vector<TilePolygon> tile_collision(const TileSet& set, const TileRef& tile);

// The pixels a tile takes in its atlas texture, as [x, y, width, height].
[[nodiscard]] std::array<int, 4> tile_region(const AtlasSource& source, Vec2i atlas);

using Cells = std::map<Vec2i, TileRef, CellOrder>;

// Where cells can be, as in Godot: columns and rows from -32768 to 32767.
inline constexpr int kMinCell = -32768;
inline constexpr int kMaxCell = 32767;

// A layer's cells. Legend keys are all the same length, one to three characters from
// kKeyCharacters; each row is a string of keys for the cells from the origin rightward, with "."
// (or "..", "...") for an empty one. The error message starts with the JSON Pointer of the bad
// value within the component, as "/rows/2: ...".
[[nodiscard]] Result<Cells> decode_cells(const world::TileMapLayer& layer, const TileSet& set);

// Every error decode_cells would find, each with its JSON Pointer within the component.
[[nodiscard]] std::vector<json::ValidationError> validate_cells(const world::TileMapLayer& layer,
                                                                const TileSet& set);

// The characters legend keys are made of: printable ASCII but space, '.', '"' and backslash.
inline constexpr std::string_view kKeyCharacters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$%&'()*+,-/:;<=>?@[]^_`{|}~!";

// Writes cells into a layer's origin, legend and rows in canonical form: the origin is the top
// left of the cells in use, rows end at their last tile, keys keep standing for the tiles they
// stood for, and new keys go to the most used tiles first, each the first letter of the tile's
// name where that is free. Keys are one character while a layer uses up to 91 tiles, then two,
// then three.
void encode_cells(const Cells& cells, const TileSet& set, world::TileMapLayer& layer);

// Paints a terrain on cells, then picks tiles for them and their neighbors so that edges
// connect, as Godot's terrain painting does. Each cell gets the tile of its terrain whose
// peering fits its neighbors best: first where its own terrain stops, then which terrain lies
// beyond, with empty cells and terrains of other sets counting as none. Among equal fits the
// cell's position picks, weighted by probability, so the same paint always gives the same
// tiles. -1 erases the cells. Returns how many cells changed.
[[nodiscard]] Result<std::size_t> paint_terrain(Cells& cells, const TileSet& set,
                                                std::span<const Vec2i> painted, int terrain);

// What 2D physics makes a layer's cells into: each cell's tile's collision polygons
// (tile_collision), moved to the cell's center, with their physics layers' settings, in the
// order of the cells and then of the polygons.
[[nodiscard]] std::vector<physics2d::TileShape> tile_shapes(const TileSet& set, const Cells& cells);

// Every error in the TileMapLayer components of a scene or prefab document the schema accepted:
// each layer's tile set must load through reader, and its legend and rows name tiles there.
// Paths point into the document. A layer that takes part of itself from a prefab is checked in
// the prefab.
[[nodiscard]] std::vector<json::ValidationError> check_tile_layers(const json::Value& doc,
                                                                   const world::FileReader& reader);

} // namespace slash::canvas
