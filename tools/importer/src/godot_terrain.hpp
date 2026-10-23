#pragma once

#include "slash/core/result.hpp"
#include "slash/importer/godot_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The data of the HTerrain plugin for Godot (zylann.hterrain): a folder holding data.hterrain,
// which lists the terrain's maps, and the maps beside it, as HTerrainData saves them
// (hterrain_data.gd), so an HTerrain node converts to SlashEngine's HTerrain component.
namespace slash::importer::godot {

// HTerrainData's map types, in the order data.hterrain lists them.
enum class TerrainMap : std::uint8_t {
    Height,
    Normal,
    Splat,
    Color,
    Detail,
    GlobalAlbedo,
    SplatIndex,
    SplatWeight,
};
inline constexpr std::size_t kTerrainMapTypes = 8;

// What data.hterrain says: its version and, for each map type, the ID of each map.
struct TerrainMetadata {
    std::string version;
    std::array<std::vector<int>, kTerrainMapTypes> maps;
};

[[nodiscard]] Result<TerrainMetadata> parse_terrain_metadata(std::string_view text);

// The file a map is saved in, in the data's folder, as HTerrainData::_get_map_filename names it
// with its extension: "height.res", "splat.png", "detail2.png".
[[nodiscard]] std::string terrain_map_file(TerrainMap type, int id);

// A terrain's heights in meters, resolution by resolution, row by row from -z.
struct TerrainHeights {
    int resolution = 0;
    std::vector<float> heights;
};

// The heights of a height.res file's Image, from its data property ({"width", "height",
// "format", "data", ...}): 32-bit floats ("RFloat").
[[nodiscard]] Result<TerrainHeights> terrain_heights_of_image(const Value& data);

// The heights of a height.png file of 8-bit red, green and blue, as the plugin's first Godot 4
// versions encoded them (decode_height_from_rgb8_unorm), from its decoded pixels.
[[nodiscard]] Result<TerrainHeights> terrain_heights_of_rgb8(int width, int height,
                                                             const std::vector<std::uint8_t>& rgba);

} // namespace slash::importer::godot
