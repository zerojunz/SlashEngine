#include "slash/canvas/tilemap.hpp"

#include "slash/canvas/debug_draw.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace slash::canvas {
namespace {

constexpr std::array<std::string_view, 3> kModeNames = {"match_corners_and_sides", "match_corners",
                                                        "match_sides"};
// A texture is at most 16384 pixels on a side.
constexpr std::int64_t kMaxPixels = 16384;
constexpr std::size_t kMaxKeyLength = 3;
// Godot keeps alternative IDs below its transform flags, bit 12 up.
constexpr int kMaxAlternative = 4095;
constexpr std::int64_t kMaxFrames = 256;
// The most texture cells one tile and its frames can cover.
constexpr std::int64_t kMaxCovered = 65536;
constexpr std::array<std::string_view, 2> kAnimationModeNames = {"default", "random_start_times"};
constexpr std::size_t kKeyCount = kKeyCharacters.size();

json::Value property(std::string_view type, std::string_view description) {
    json::Value out;
    out["type"] = type;
    out["description"] = description;
    return out;
}

json::Value pair(std::string_view description, std::int64_t minimum) {
    json::Value item;
    item["type"] = "integer";
    item["minimum"] = minimum;
    item["maximum"] = kMaxPixels;
    json::Value out = property("array", description);
    out["items"] = std::move(item);
    out["minItems"] = 2;
    out["maxItems"] = 2;
    return out;
}

json::Value schema() {
    json::Value peering_bit;
    peering_bit["type"] = json::Array{"string", "null"};
    json::Value tile;
    tile["type"] = "object";
    tile["additionalProperties"] = false;
    tile["properties"]["name"] =
        property("string", "What legends and tilemap.set call it; unique in the file, no ':'.");
    tile["properties"]["name"]["minLength"] = 1;
    tile["properties"]["terrain"] = property("string", "The terrain it paints.");
    tile["properties"]["terrain"]["minLength"] = 1;
    tile["properties"]["peering"] = property(
        "object", "The terrain each neighbor must have for the tile to fit, by the compass "
                  "direction of the side or corner it touches: n, ne, e, se, s, sw, w, nw for "
                  "squares and diamonds; a hexagon's corners between two sides are ene, ese, "
                  "wsw, wnw (offset rows) or nne, sse, ssw, nnw (offset columns). Left out, the "
                  "tile's own terrain; null, none.");
    tile["properties"]["peering"]["additionalProperties"] = false;
    for (const std::string_view name : kNeighborNames) {
        tile["properties"]["peering"]["properties"][name] = peering_bit;
    }
    tile["properties"]["probability"] =
        property("number", "How often terrain painting picks it over tiles that fit as well.");
    tile["properties"]["probability"]["minimum"] = 0;
    tile["properties"]["probability"]["default"] = 1;
    tile["properties"]["texture_origin"] =
        pair("Where the cell's center is in the texture, in pixels from its middle: the texture "
             "draws that much up and to the left. Collision stays put.",
             -kMaxPixels);
    tile["properties"]["texture_origin"]["default"] = json::Array{0, 0};
    tile["properties"]["size_in_atlas"] =
        pair("How many of the texture's cells it covers from its own, across and down; it draws "
             "centered on one map cell.",
             1);
    tile["properties"]["size_in_atlas"]["default"] = json::Array{1, 1};
    tile["properties"]["animation_frames"] =
        property("array", "Each animation frame's duration in seconds; frames follow the tile "
                          "in the texture.");
    tile["properties"]["animation_frames"]["items"]["type"] = "number";
    tile["properties"]["animation_frames"]["items"]["exclusiveMinimum"] = 0;
    tile["properties"]["animation_frames"]["minItems"] = 1;
    tile["properties"]["animation_frames"]["maxItems"] = kMaxFrames;
    tile["properties"]["animation_frames"]["default"] = json::Array{1};
    tile["properties"]["animation_columns"] =
        property("integer", "Frames in a row before the next row; 0 puts them all in one row.");
    tile["properties"]["animation_columns"]["minimum"] = 0;
    tile["properties"]["animation_columns"]["maximum"] = kMaxFrames;
    tile["properties"]["animation_columns"]["default"] = 0;
    tile["properties"]["animation_separation"] =
        pair("Texture cells between one frame and the next, across and down.", 0);
    tile["properties"]["animation_separation"]["default"] = json::Array{0, 0};
    tile["properties"]["animation_speed"] =
        property("number", "How fast the animation plays: 2 is twice as fast.");
    tile["properties"]["animation_speed"]["exclusiveMinimum"] = 0;
    tile["properties"]["animation_speed"]["default"] = 1;
    tile["properties"]["animation_mode"] =
        property("string", "default: every cell shows the same frame; random_start_times: each "
                           "cell starts at its own point in the animation.");
    tile["properties"]["animation_mode"]["enum"] =
        json::Array{kAnimationModeNames[0], kAnimationModeNames[1]};
    tile["properties"]["animation_mode"]["default"] = kAnimationModeNames[0];
    tile["properties"]["collision"] =
        property("array", "Polygons that make the tile solid, each in a physics layer.");
    tile["properties"]["collision"]["items"]["$ref"] = "#/$defs/polygon";
    tile["properties"]["occlusion"] =
        property("array", "Polygons that cast shadows from 2D lights, each in an occlusion layer.");
    tile["properties"]["occlusion"]["items"]["$ref"] = "#/$defs/occluder";
    tile["properties"]["custom_data"] =
        property("object", "Values in the tile set's custom data layers, by layer name; the "
                           "others have their type's default.");
    tile["properties"]["alternatives"] =
        property("object", R"(Other versions of the tile by number from 1, as "1"; legends )"
                           R"(name them as "crate:1".)");
    tile["properties"]["alternatives"]["additionalProperties"]["$ref"] = "#/$defs/alternative";

    json::Value point;
    point["type"] = "array";
    point["items"]["type"] = "number";
    point["minItems"] = 2;
    point["maxItems"] = 2;
    json::Value polygon;
    polygon["type"] = "object";
    polygon["additionalProperties"] = false;
    polygon["required"] = json::Array{"points"};
    polygon["properties"]["points"] =
        property("array", "Corners in pixels from the tile's center, x right and y down.");
    polygon["properties"]["points"]["items"] = point;
    polygon["properties"]["points"]["minItems"] = 3;
    polygon["properties"]["layer"] = property("integer", "Its physics layer, from 0.");
    polygon["properties"]["layer"]["minimum"] = 0;
    polygon["properties"]["layer"]["default"] = 0;
    polygon["properties"]["one_way"] =
        property("boolean", "Only stops what comes from above (the tile's -y).");
    polygon["properties"]["one_way"]["default"] = false;
    polygon["properties"]["one_way_margin"] =
        property("number", "How deep a body can be in a one-way polygon and still land on it.");
    polygon["properties"]["one_way_margin"]["minimum"] = 0;
    polygon["properties"]["one_way_margin"]["default"] = 1;

    json::Value alternative;
    alternative["type"] = "object";
    alternative["additionalProperties"] = false;
    alternative["properties"]["flip_h"] = property("boolean", "Draws flipped horizontally.");
    alternative["properties"]["flip_h"]["default"] = false;
    alternative["properties"]["flip_v"] = property("boolean", "Draws flipped vertically.");
    alternative["properties"]["flip_v"]["default"] = false;
    alternative["properties"]["transpose"] =
        property("boolean", "Draws with x and y swapped, before flipping.");
    alternative["properties"]["transpose"]["default"] = false;
    alternative["properties"]["modulate"] = property("array", "Tint, as RGBA from 0 to 1.");
    alternative["properties"]["modulate"]["items"]["type"] = "number";
    alternative["properties"]["modulate"]["minItems"] = 4;
    alternative["properties"]["modulate"]["maxItems"] = 4;
    alternative["properties"]["modulate"]["default"] = json::Array{1, 1, 1, 1};
    alternative["properties"]["texture_origin"] =
        pair("Its own texture origin, as the tile's; [0, 0] if left out.", -kMaxPixels);
    alternative["properties"]["texture_origin"]["default"] = json::Array{0, 0};
    alternative["properties"]["collision"] =
        property("array", "Its own collision polygons, as the tile's; none if left out.");
    alternative["properties"]["collision"]["items"]["$ref"] = "#/$defs/polygon";
    alternative["properties"]["occlusion"] =
        property("array", "Its own occluders, as the tile's; none if left out.");
    alternative["properties"]["occlusion"]["items"]["$ref"] = "#/$defs/occluder";
    alternative["properties"]["custom_data"] =
        property("object", "Its own custom data, as the tile's; the layers' defaults if left out.");

    json::Value occluder;
    occluder["type"] = "object";
    occluder["additionalProperties"] = false;
    occluder["required"] = json::Array{"points"};
    occluder["properties"]["points"] =
        property("array", "Corners in pixels from the tile's center, x right and y down.");
    occluder["properties"]["points"]["items"] = point;
    occluder["properties"]["points"]["minItems"] = 2;
    occluder["properties"]["layer"] = property("integer", "Its occlusion layer, from 0.");
    occluder["properties"]["layer"]["minimum"] = 0;
    occluder["properties"]["layer"]["default"] = 0;
    occluder["properties"]["closed"] = property("boolean", "The last point joins the first.");
    occluder["properties"]["closed"]["default"] = true;
    occluder["properties"]["cull_mode"] =
        property("string", "Which way edges wind, seen from a light, to cast a shadow; disabled: "
                           "both.");
    occluder["properties"]["cull_mode"]["enum"] =
        json::Array{kTileCullNames[0], kTileCullNames[1], kTileCullNames[2]};
    occluder["properties"]["cull_mode"]["default"] = kTileCullNames[0];

    json::Value occlusion_layer;
    occlusion_layer["type"] = "object";
    occlusion_layer["additionalProperties"] = false;
    occlusion_layer["properties"]["light_mask"] =
        property("integer", "The lights its occluders shadow: those whose shadow_item_cull_mask "
                            "shares a bit.");
    occlusion_layer["properties"]["light_mask"]["minimum"] = 0;
    occlusion_layer["properties"]["light_mask"]["maximum"] = std::int64_t{UINT32_MAX};
    occlusion_layer["properties"]["light_mask"]["default"] = 1;

    json::Value custom_data_layer;
    custom_data_layer["type"] = "object";
    custom_data_layer["additionalProperties"] = false;
    custom_data_layer["required"] = json::Array{"name", "type"};
    custom_data_layer["properties"]["name"] =
        property("string", "What tiles' custom_data and games call it; unique in the file.");
    custom_data_layer["properties"]["name"]["minLength"] = 1;
    custom_data_layer["properties"]["type"] =
        property("string", "Its values' type: vector2 and vector2i are [x, y], color [r, g, b, "
                           "a].");
    json::Array types;
    for (const std::string_view type : kCustomDataTypeNames) {
        types.emplace_back(type);
    }
    custom_data_layer["properties"]["type"]["enum"] = std::move(types);

    json::Value physics_layer;
    physics_layer["type"] = "object";
    physics_layer["additionalProperties"] = false;
    physics_layer["properties"]["collision_layer"] =
        property("integer", "The layers its polygons are in, as bits.");
    physics_layer["properties"]["collision_layer"]["minimum"] = 0;
    physics_layer["properties"]["collision_layer"]["maximum"] = std::int64_t{UINT32_MAX};
    physics_layer["properties"]["collision_layer"]["default"] = 1;
    physics_layer["properties"]["collision_mask"] =
        property("integer", "The layers its polygons collide with, as bits.");
    physics_layer["properties"]["collision_mask"]["minimum"] = 0;
    physics_layer["properties"]["collision_mask"]["maximum"] = std::int64_t{UINT32_MAX};
    physics_layer["properties"]["collision_mask"]["default"] = 1;
    physics_layer["properties"]["friction"] =
        property("number", "Friction, as a physics material's.");
    physics_layer["properties"]["friction"]["minimum"] = 0;
    physics_layer["properties"]["friction"]["default"] = 1;
    physics_layer["properties"]["bounce"] = property("number", "Bounce, from 0 to 1.");
    physics_layer["properties"]["bounce"]["minimum"] = 0;
    physics_layer["properties"]["bounce"]["maximum"] = 1;
    physics_layer["properties"]["bounce"]["default"] = 0;

    json::Value source;
    source["type"] = "object";
    source["additionalProperties"] = false;
    source["required"] = json::Array{"texture"};
    source["properties"]["texture"] = property("string", "The atlas texture.");
    source["properties"]["texture"]["minLength"] = 1;
    source["properties"]["region_size"] =
        pair("Each tile's width and height in the texture, in pixels; tile_size if left out.", 1);
    source["properties"]["margins"] = pair("Pixels before the first column and row.", 0);
    source["properties"]["margins"]["default"] = json::Array{0, 0};
    source["properties"]["separation"] = pair("Pixels between columns and rows.", 0);
    source["properties"]["separation"]["default"] = json::Array{0, 0};
    source["properties"]["tiles"] =
        property("object", R"(Tiles by "column,row" in the texture's grid, as "2,0".)");
    source["properties"]["tiles"]["additionalProperties"]["$ref"] = "#/$defs/tile";
    source["properties"]["normal_texture"] =
        property("string", "A normal map laid out as the texture, OpenGL style (green up), for "
                           "2D lights; \"\" faces the viewer. Lights need a height to show it.");
    source["properties"]["normal_texture"]["default"] = "";
    source["properties"]["specular_texture"] =
        property("string", "A specular map laid out as the texture: highlight color and, in "
                           "alpha, shininess. Needs a normal_texture.");
    source["properties"]["specular_texture"]["default"] = "";
    source["properties"]["specular_color"] =
        property("array", "Multiplies the specular map's color, as RGBA from 0 to 1.");
    source["properties"]["specular_color"]["items"]["type"] = "number";
    source["properties"]["specular_color"]["items"]["minimum"] = 0;
    source["properties"]["specular_color"]["items"]["maximum"] = 1;
    source["properties"]["specular_color"]["minItems"] = 4;
    source["properties"]["specular_color"]["maxItems"] = 4;
    source["properties"]["specular_color"]["default"] = json::Array{1, 1, 1, 1};
    source["properties"]["specular_shininess"] =
        property("number", "Multiplies the specular map's shininess, from 0 to 1.");
    source["properties"]["specular_shininess"]["minimum"] = 0;
    source["properties"]["specular_shininess"]["maximum"] = 1;
    source["properties"]["specular_shininess"]["default"] = 1;

    json::Value terrain_set;
    terrain_set["type"] = "object";
    terrain_set["additionalProperties"] = false;
    terrain_set["required"] = json::Array{"terrains"};
    terrain_set["properties"]["mode"] =
        property("string", "How tiles connect: by their corners and sides, their corners only "
                           "or their sides only.");
    json::Array modes;
    for (const std::string_view mode : kModeNames) {
        modes.emplace_back(mode);
    }
    terrain_set["properties"]["mode"]["enum"] = std::move(modes);
    terrain_set["properties"]["mode"]["default"] = kModeNames.front();
    terrain_set["properties"]["terrains"] = property("array", "Terrain names, unique in the file.");
    terrain_set["properties"]["terrains"]["minItems"] = 1;
    terrain_set["properties"]["terrains"]["items"]["type"] = "string";
    terrain_set["properties"]["terrains"]["items"]["minLength"] = 1;

    json::Value out;
    out["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    out["title"] = "SlashEngine tile set";
    out["description"] = "Atlas textures cut into tiles, and the terrains that connect them, "
                         "like Godot's TileSet. TileMapLayer draws them.";
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = json::Array{"format", "sources"};
    out["properties"]["format"]["const"] = 1;
    out["properties"]["format"]["description"] = "Format version. Must be 1.";
    out["properties"]["tile_size"] = pair("A cell's width and height, in pixels.", 1);
    out["properties"]["tile_size"]["default"] = json::Array{16, 16};
    const auto names = [](std::span<const std::string_view> list) {
        json::Array out_names;
        for (const std::string_view name : list) {
            out_names.emplace_back(name);
        }
        return out_names;
    };
    out["properties"]["tile_shape"] =
        property("string", "The cells' shape, as Godot's: squares, isometric diamonds, squares in "
                           "rows offset by half a cell (connected as hexagons), or hexagons.");
    out["properties"]["tile_shape"]["enum"] = names(kTileShapeNames);
    out["properties"]["tile_shape"]["default"] = kTileShapeNames.front();
    out["properties"]["tile_layout"] =
        property("string", "How the columns and rows of cells that aren't square lie, as "
                           "Godot's TileLayout.");
    out["properties"]["tile_layout"]["enum"] = names(kTileLayoutNames);
    out["properties"]["tile_layout"]["default"] = kTileLayoutNames.front();
    out["properties"]["tile_offset_axis"] =
        property("string", "Whether cells that aren't square offset their rows (horizontal) or "
                           "their columns (vertical).");
    out["properties"]["tile_offset_axis"]["enum"] = names(kTileOffsetAxisNames);
    out["properties"]["tile_offset_axis"]["default"] = kTileOffsetAxisNames.front();
    out["properties"]["physics_layers"] = property(
        "array", "Physics layers by number from 0: what tiles' collision polygons collide as.");
    out["properties"]["physics_layers"]["items"]["$ref"] = "#/$defs/physics_layer";
    out["properties"]["occlusion_layers"] = property(
        "array", "Occlusion layers by number from 0: which lights tiles' occluders shadow.");
    out["properties"]["occlusion_layers"]["items"]["$ref"] = "#/$defs/occlusion_layer";
    out["properties"]["custom_data_layers"] = property(
        "array", "Values every tile has, each with a name and a type, as Godot's custom data.");
    out["properties"]["custom_data_layers"]["items"]["$ref"] = "#/$defs/custom_data_layer";
    out["properties"]["terrain_sets"] =
        property("object", "Terrain sets by name, each with the terrains whose tiles connect.");
    out["properties"]["terrain_sets"]["additionalProperties"]["$ref"] = "#/$defs/terrain_set";
    out["properties"]["sources"] = property("object", "Atlas sources by name.");
    out["properties"]["sources"]["minProperties"] = 1;
    out["properties"]["sources"]["additionalProperties"]["$ref"] = "#/$defs/source";
    out["$defs"]["terrain_set"] = std::move(terrain_set);
    out["$defs"]["source"] = std::move(source);
    out["$defs"]["tile"] = std::move(tile);
    out["$defs"]["polygon"] = std::move(polygon);
    out["$defs"]["alternative"] = std::move(alternative);
    out["$defs"]["physics_layer"] = std::move(physics_layer);
    out["$defs"]["occluder"] = std::move(occluder);
    out["$defs"]["occlusion_layer"] = std::move(occlusion_layer);
    out["$defs"]["custom_data_layer"] = std::move(custom_data_layer);
    return out;
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

template <typename Range>
void add_hint(std::string& message, std::string_view name, const Range& candidates) {
    if (const std::string_view hint = closest_match(name, candidates); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
}

// "column,row", as "2,0", within a texture's largest grid.
std::optional<Vec2i> parse_atlas(std::string_view text) {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return std::nullopt;
    }
    const auto number = [](std::string_view part) -> std::optional<int> {
        int value = 0;
        if (part.empty() || part.front() < '0' || part.front() > '9') {
            return std::nullopt;
        }
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (error != std::errc{} || end != part.data() + part.size() || value >= kMaxPixels) {
            return std::nullopt;
        }
        return value;
    };
    const std::optional<int> column = number(text.substr(0, comma));
    const std::optional<int> row = number(text.substr(comma + 1));
    if (!column || !row) {
        return std::nullopt;
    }
    return Vec2i{*column, *row};
}

// An alternative's number, "1" up to kMaxAlternative, with no leading zeros.
std::optional<int> parse_alternative(std::string_view text) {
    int value = 0;
    if (text.empty() || text.front() < '1' || text.front() > '9') {
        return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > kMaxAlternative) {
        return std::nullopt;
    }
    return value;
}

std::string atlas_text(Vec2i atlas) {
    return std::to_string(atlas.x) + "," + std::to_string(atlas.y);
}

Vec2i pair_of(const json::Value* value, Vec2i fallback) {
    if (value == nullptr) {
        return fallback;
    }
    const json::Array& items = value->as_array();
    return {static_cast<int>(items[0].as_int()), static_cast<int>(items[1].as_int())};
}

// The texture cell a tile's frame starts at, as Godot's get_tile_texture_region finds it.
Vec2i frame_cell(Vec2i atlas, Vec2i size, Vec2i separation, int columns, int frame) {
    const Vec2i at = columns > 0 ? Vec2i{frame % columns, frame / columns} : Vec2i{frame, 0};
    return {atlas.x + ((size.x + separation.x) * at.x), atlas.y + ((size.y + separation.y) * at.y)};
}

// What a direction is to a cell of a shape, as Godot's is_valid_terrain_peering_bit_for_mode
// with both corners and sides.
PeeringKind kind_of(TileShape shape, TileOffsetAxis axis, Neighbor direction) {
    const auto d = static_cast<std::size_t>(direction);
    const bool main = d % 2 == 0;  // one of the eight main points
    const bool along = d % 4 == 0; // n, e, s or w
    switch (shape) {
    case TileShape::Square:
    case TileShape::Isometric:
        // A square's sides face n, e, s and w; a diamond's corners do.
        if (!main) {
            return PeeringKind::None;
        }
        return along == (shape == TileShape::Square) ? PeeringKind::Side : PeeringKind::Corner;
    case TileShape::HalfOffsetSquare:
    case TileShape::Hexagon:
        break;
    }
    // Offset rows have sides e and w and the diagonals, and corners n, s and those between the
    // diagonals and e or w; offset columns, sides n and s and the diagonals, and corners e, w and
    // those between the diagonals and n or s.
    const bool rows = axis == TileOffsetAxis::Horizontal;
    if (!main) {
        const bool beside_east_or_west = direction == Neighbor::ENE || direction == Neighbor::ESE ||
                                         direction == Neighbor::WSW || direction == Neighbor::WNW;
        return beside_east_or_west == rows ? PeeringKind::Corner : PeeringKind::None;
    }
    if (!along) {
        return PeeringKind::Side;
    }
    const bool east_or_west = direction == Neighbor::E || direction == Neighbor::W;
    return east_or_west == rows ? PeeringKind::Side : PeeringKind::Corner;
}

// Whether a terrain set's mode connects a tile set's tiles by this direction.
bool connects(const TileSet& set, TerrainMode mode, Neighbor direction) {
    switch (peering_kind(set, direction)) {
    case PeeringKind::None:
        return false;
    case PeeringKind::Side:
        return mode != TerrainMode::Corners;
    case PeeringKind::Corner:
        break;
    }
    return mode != TerrainMode::Sides;
}

// The directions a mode connects a tile set's tiles by, as "n, e, s and w".
std::string connected_names(const TileSet& set, TerrainMode mode) {
    std::vector<std::string_view> names;
    for (std::size_t d = 0; d < kNeighborNames.size(); ++d) {
        if (connects(set, mode, static_cast<Neighbor>(d))) {
            names.push_back(kNeighborNames.at(d));
        }
    }
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out += i + 1 == names.size() ? " and " : ", ";
        }
        out += names[i];
    }
    return out;
}

// What a tile of a shape is called in messages, as "square".
std::string shape_words(TileShape shape) {
    std::string out(kTileShapeNames.at(static_cast<std::size_t>(shape)));
    std::ranges::replace(out, '_', ' ');
    return out;
}

TerrainMode parse_mode(std::string_view name) {
    return static_cast<TerrainMode>(std::ranges::find(kModeNames, name) - kModeNames.begin());
}

template <typename Enum, std::size_t N>
Enum parse_name(const json::Value* value, const std::array<std::string_view, N>& names) {
    if (value == nullptr) {
        return Enum{};
    }
    return static_cast<Enum>(std::ranges::find(names, value->as_string()) - names.begin());
}

// The shape, layout and offset axis of a .tileset.json document the schema accepted.
TileSet shape_of(const json::Value& doc) {
    TileSet set;
    set.shape = parse_name<TileShape>(doc.find("tile_shape"), kTileShapeNames);
    set.layout = parse_name<TileLayout>(doc.find("tile_layout"), kTileLayoutNames);
    set.offset_axis =
        parse_name<TileOffsetAxis>(doc.find("tile_offset_axis"), kTileOffsetAxisNames);
    return set;
}

int posmod2(int value) {
    return ((value % 2) + 2) % 2;
}

// One step from a cell of a shape other than squares toward one of the eight main points
// (0 n, 1 ne, ... 7 nw), as Godot's get_neighbor_cell for half-offset shapes.
Vec2i half_offset_step(const TileSet& set, Vec2i cell, std::size_t point) {
    const bool rows = set.offset_axis == TileOffsetAxis::Horizontal;
    switch (set.layout) {
    case TileLayout::Stacked:
    case TileLayout::StackedOffset: {
        // A stacked offset layout is a stacked one with the other rows (or columns) offset.
        const bool offset =
            (posmod2(rows ? cell.y : cell.x) == 1) != (set.layout == TileLayout::StackedOffset);
        // Toward the offset side, one more on the other axis in offset rows; away, one less in
        // the others.
        const auto shift = [&](bool toward) { return (offset ? 1 : 0) - (toward ? 0 : 1); };
        // Straight up or down offset rows, the cell across is two rows away, in the next row
        // offset the same way; along a row, it is the next cell. Offset columns the other way.
        constexpr std::array<Vec2i, 8> kAlongRows{
            {{0, -2}, {}, {1, 0}, {}, {0, 2}, {}, {-1, 0}, {}}};
        constexpr std::array<Vec2i, 8> kAlongColumns{
            {{0, -1}, {}, {2, 0}, {}, {0, 1}, {}, {-2, 0}, {}}};
        if (point % 2 == 0) {
            return rows ? kAlongRows.at(point) : kAlongColumns.at(point);
        }
        // ne 1, se 3, sw 5, nw 7.
        const bool east = point == 1 || point == 3;
        const bool south = point == 3 || point == 5;
        return rows ? Vec2i{shift(east), south ? 1 : -1} : Vec2i{east ? 1 : -1, shift(south)};
    }
    case TileLayout::StairsRight: {
        constexpr std::array<Vec2i, 8> kSteps{
            {{1, -2}, {1, -1}, {1, 0}, {0, 1}, {-1, 2}, {-1, 1}, {-1, 0}, {0, -1}}};
        return kSteps.at(point);
    }
    case TileLayout::StairsDown: {
        constexpr std::array<Vec2i, 8> kSteps{
            {{0, -1}, {1, -1}, {2, -1}, {1, 0}, {0, 1}, {-1, 1}, {-2, 1}, {-1, 0}}};
        return kSteps.at(point);
    }
    case TileLayout::DiamondRight: {
        constexpr std::array<Vec2i, 8> kSteps{
            {{1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}}};
        return kSteps.at(point);
    }
    case TileLayout::DiamondDown:
        break;
    }
    constexpr std::array<Vec2i, 8> kSteps{
        {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}}};
    return kSteps.at(point);
}

float cross(Vec2 a, Vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

// How much a shape's rows (or columns) overlap, as Godot's overlapping ratio: diamonds by half,
// hexagons by a quarter.
float overlap_of(TileShape shape) {
    switch (shape) {
    case TileShape::Isometric:
        return 0.5F;
    case TileShape::Hexagon:
        return 0.75F;
    case TileShape::Square:
    case TileShape::HalfOffsetSquare:
        break;
    }
    return 1.0F;
}

// A whole number of cells as an int, held in range for points far past any cell.
int whole(float value) {
    constexpr float kLimit = 1.0e9F;
    return static_cast<int>(std::clamp(value, -kLimit, kLimit));
}

std::string path_error() {
    return "expected a path inside the project folder, separated by '/', with no \".\" or "
           "\"..\" parts";
}

// Checks a document the schema accepted, collecting every error.
class Checker {
public:
    explicit Checker(const json::Value& doc) : doc_(doc), shape_(shape_of(doc)) {}

    std::vector<json::ValidationError> run() {
        if (const json::Value* layers = doc_.find("physics_layers"); layers != nullptr) {
            physics_layers_ = layers->as_array().size();
        }
        if (const json::Value* layers = doc_.find("occlusion_layers"); layers != nullptr) {
            occlusion_layers_ = layers->as_array().size();
        }
        if (const json::Value* layers = doc_.find("custom_data_layers"); layers != nullptr) {
            for (std::size_t i = 0; i < layers->as_array().size(); ++i) {
                const json::Value& layer = layers->as_array()[i];
                const std::string& name = layer.find("name")->as_string();
                const std::string& type = layer.find("type")->as_string();
                const auto kind = static_cast<CustomDataType>(
                    std::ranges::find(kCustomDataTypeNames, type) - kCustomDataTypeNames.begin());
                if (!custom_data_.try_emplace(name, kind).second) {
                    fail("/custom_data_layers/" + std::to_string(i) + "/name",
                         "another custom data layer is named " + quoted(name));
                }
            }
        }
        if (const json::Value* sets = doc_.find("terrain_sets"); sets != nullptr) {
            for (const auto& [name, set] : sets->as_object()) {
                check_terrain_set(name, set);
            }
        }
        for (const auto& [name, source] : doc_.find("sources")->as_object()) {
            check_source(name, source);
        }
        return std::move(errors_);
    }

private:
    struct TerrainInfo {
        std::string set;
        TerrainMode mode = TerrainMode::CornersAndSides;
    };

    void fail(std::string path, std::string message) {
        errors_.push_back({std::move(path), std::move(message)});
    }

    void check_terrain_set(const std::string& name, const json::Value& set) {
        const std::string at = "/terrain_sets/" + json::pointer_escape(name);
        if (name.empty()) {
            fail(at, "name the terrain set");
        }
        const json::Value* mode = set.find("mode");
        const TerrainMode parsed =
            mode != nullptr ? parse_mode(mode->as_string()) : TerrainMode::CornersAndSides;
        const json::Array& terrains = set.find("terrains")->as_array();
        for (std::size_t i = 0; i < terrains.size(); ++i) {
            const std::string& terrain = terrains[i].as_string();
            const auto [it, added] =
                terrains_.try_emplace(terrain, TerrainInfo{.set = name, .mode = parsed});
            if (!added) {
                fail(at + "/terrains/" + std::to_string(i),
                     quoted(terrain) + " is already a terrain, in the set " +
                         quoted(it->second.set) + "; terrain names are unique in the file");
            }
        }
    }

    // The terrain a name refers to, or nullptr after reporting it.
    const TerrainInfo* terrain(const std::string& name, const std::string& at) {
        const auto it = terrains_.find(name);
        if (it != terrains_.end()) {
            return &it->second;
        }
        std::vector<std::string_view> names;
        names.reserve(terrains_.size());
        for (const auto& [known, info] : terrains_) {
            names.emplace_back(known);
        }
        std::string message = "no terrain set has a terrain named " + quoted(name);
        add_hint(message, name, names);
        fail(at, std::move(message));
        return nullptr;
    }

    void check_source(const std::string& name, const json::Value& source) {
        const std::string at = "/sources/" + json::pointer_escape(name);
        if (name.empty()) {
            fail(at, "name the source");
        }
        for (const std::string_view field : {"texture", "normal_texture", "specular_texture"}) {
            const json::Value* path = source.find(field);
            if (path != nullptr && (field == "texture" || !path->as_string().empty()) &&
                !is_project_path(path->as_string())) {
                fail(at + "/" + std::string(field), path_error());
            }
        }
        const json::Value* tiles = source.find("tiles");
        if (tiles == nullptr) {
            return;
        }
        std::map<Vec2i, std::string, CellOrder> covered; // texture cell -> the tile covering it
        for (const auto& [key, tile] : tiles->as_object()) {
            const std::string tile_at = at + "/tiles/" + json::pointer_escape(key);
            const std::optional<Vec2i> atlas = parse_atlas(key);
            if (!atlas) {
                fail(tile_at, R"(tiles are keyed by "column,row" in the texture's grid, as "2,0")");
                continue;
            }
            std::string where = name;
            where += ':';
            where += key;
            check_tile(where, tile, tile_at);
            check_cover(key, *atlas, tile, tile_at, covered);
        }
    }

    // That a tile, with its size_in_atlas and its animation frames, covers no texture cell
    // another tile does, as Godot's has_room_for_tile.
    void check_cover(const std::string& key, Vec2i atlas, const json::Value& tile,
                     const std::string& at, std::map<Vec2i, std::string, CellOrder>& covered) {
        const Vec2i size = pair_of(tile.find("size_in_atlas"), {1, 1});
        const Vec2i separation = pair_of(tile.find("animation_separation"), {});
        const json::Value* columns = tile.find("animation_columns");
        const json::Value* frames = tile.find("animation_frames");
        const auto count = static_cast<int>(frames != nullptr ? frames->as_array().size() : 1);
        if (std::int64_t{size.x} * size.y * count > kMaxCovered) {
            fail(at + "/size_in_atlas", "a tile and its animation frames cover at most " +
                                            std::to_string(kMaxCovered) +
                                            " of the texture's cells");
            return;
        }
        for (int frame = 0; frame < count; ++frame) {
            const Vec2i first =
                frame_cell(atlas, size, separation,
                           columns != nullptr ? static_cast<int>(columns->as_int()) : 0, frame);
            for (int y = 0; y < size.y; ++y) {
                for (int x = 0; x < size.x; ++x) {
                    const Vec2i cell{first.x + x, first.y + y};
                    const auto [it, added] = covered.try_emplace(cell, key);
                    if (!added && it->second != key) {
                        fail(at, "it covers the texture's cell " + atlas_text(cell) +
                                     ", as the tile " + quoted(it->second) +
                                     " does; a tile, its size_in_atlas and its animation "
                                     "frames cover cells no other tile does");
                        return;
                    }
                }
            }
        }
    }

    void check_polygons(const json::Value& polygons, const std::string& at) {
        const json::Array& items = polygons.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string polygon_at = at + "/" + std::to_string(i);
            const json::Value* layer = items[i].find("layer");
            if (static_cast<std::uint64_t>(layer != nullptr ? layer->as_int() : 0) >=
                physics_layers_) {
                fail(polygon_at + "/layer",
                     physics_layers_ == 0
                         ? std::string("the tile set has no physics layers; add one to "
                                       "physics_layers")
                         : "the tile set has physics layers 0 to " +
                               std::to_string(physics_layers_ - 1));
            }
            std::vector<Vec2> points;
            for (const json::Value& point : items[i].find("points")->as_array()) {
                points.push_back({static_cast<float>(point.as_array()[0].as_double()),
                                  static_cast<float>(point.as_array()[1].as_double())});
            }
            if (triangulate(points).empty()) {
                fail(polygon_at + "/points", "the polygon has no area or crosses itself");
            }
        }
    }

    void check_occluders(const json::Value& occluders, const std::string& at) {
        const json::Array& items = occluders.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string occluder_at = at + "/" + std::to_string(i);
            const json::Value* layer = items[i].find("layer");
            if (static_cast<std::uint64_t>(layer != nullptr ? layer->as_int() : 0) >=
                occlusion_layers_) {
                fail(occluder_at + "/layer",
                     occlusion_layers_ == 0
                         ? std::string("the tile set has no occlusion layers; add one to "
                                       "occlusion_layers")
                         : "the tile set has occlusion layers 0 to " +
                               std::to_string(occlusion_layers_ - 1));
            }
            const json::Value* closed = items[i].find("closed");
            if ((closed == nullptr || closed->as_bool()) &&
                items[i].find("points")->as_array().size() < 3) {
                fail(occluder_at + "/points",
                     "a closed occluder has at least three points; an open one, two");
            }
        }
    }

    // Each value against its layer's type, as Godot's custom data layers hold one type each.
    void check_custom_data(const json::Value& data, const std::string& at) {
        for (const auto& [name, value] : data.as_object()) {
            const std::string value_at = at + "/" + json::pointer_escape(name);
            const auto layer = custom_data_.find(name);
            if (layer == custom_data_.end()) {
                std::string message =
                    custom_data_.empty()
                        ? std::string("the tile set has no custom data layers; add one to "
                                      "custom_data_layers")
                        : "the tile set has no custom data layer named " + quoted(name);
                std::vector<std::string_view> names;
                names.reserve(custom_data_.size());
                for (const auto& [other, type] : custom_data_) {
                    names.push_back(other);
                }
                add_hint(message, name, names);
                fail(value_at, std::move(message));
                continue;
            }
            const auto numbers = [](const json::Value& given, std::size_t count, bool whole) {
                if (!given.is_array() || given.as_array().size() != count) {
                    return false;
                }
                return std::ranges::all_of(given.as_array(), [&](const json::Value& item) {
                    return whole ? item.is_integral() : item.is_number();
                });
            };
            bool fits = false;
            switch (layer->second) {
            case CustomDataType::Bool:
                fits = value.is_bool();
                break;
            case CustomDataType::Int:
                fits = value.is_integral();
                break;
            case CustomDataType::Float:
                fits = value.is_number();
                break;
            case CustomDataType::String:
                fits = value.is_string();
                break;
            case CustomDataType::Vector2:
                fits = numbers(value, 2, false);
                break;
            case CustomDataType::Vector2i:
                fits = numbers(value, 2, true);
                break;
            case CustomDataType::Color:
                fits = numbers(value, 4, false);
                break;
            }
            if (!fits) {
                constexpr std::array<std::string_view, 7> kExpected = {"true or false",
                                                                       "a whole number",
                                                                       "a number",
                                                                       "a string",
                                                                       "[x, y] in numbers",
                                                                       "[x, y] in whole numbers",
                                                                       "[r, g, b, a] in numbers"};
                fail(value_at,
                     "the layer " + quoted(name) + " holds " +
                         std::string(
                             kCustomDataTypeNames.at(static_cast<std::size_t>(layer->second))) +
                         " values: " +
                         std::string(kExpected.at(static_cast<std::size_t>(layer->second))));
            }
        }
    }

    void check_tile(const std::string& where, const json::Value& tile, const std::string& at) {
        if (const json::Value* collision = tile.find("collision"); collision != nullptr) {
            check_polygons(*collision, at + "/collision");
        }
        if (const json::Value* occlusion = tile.find("occlusion"); occlusion != nullptr) {
            check_occluders(*occlusion, at + "/occlusion");
        }
        if (const json::Value* data = tile.find("custom_data"); data != nullptr) {
            check_custom_data(*data, at + "/custom_data");
        }
        if (const json::Value* alternatives = tile.find("alternatives"); alternatives != nullptr) {
            for (const auto& [key, alternative] : alternatives->as_object()) {
                const std::string alternative_at =
                    at + "/alternatives/" + json::pointer_escape(key);
                if (!parse_alternative(key)) {
                    fail(alternative_at, "alternatives are numbered from 1 to " +
                                             std::to_string(kMaxAlternative) + ", as \"1\"");
                }
                if (const json::Value* collision = alternative.find("collision");
                    collision != nullptr) {
                    check_polygons(*collision, alternative_at + "/collision");
                }
                if (const json::Value* occlusion = alternative.find("occlusion");
                    occlusion != nullptr) {
                    check_occluders(*occlusion, alternative_at + "/occlusion");
                }
                if (const json::Value* data = alternative.find("custom_data"); data != nullptr) {
                    check_custom_data(*data, alternative_at + "/custom_data");
                }
            }
        }
        if (const json::Value* name = tile.find("name"); name != nullptr) {
            const std::string& text = name->as_string();
            if (text.find(':') != std::string::npos) {
                fail(at + "/name", R"(a tile name has no ':', which "source:column,row" uses)");
            } else if (const auto [it, added] = names_.try_emplace(text, where); !added) {
                fail(at + "/name", "another tile is named " + quoted(text) + ": " + it->second);
            }
        }
        const json::Value* terrain_name = tile.find("terrain");
        const json::Value* peering = tile.find("peering");
        if (terrain_name == nullptr) {
            if (peering != nullptr) {
                fail(at + "/peering", R"(only a tile with a "terrain" has "peering")");
            }
            return;
        }
        const TerrainInfo* info = terrain(terrain_name->as_string(), at + "/terrain");
        if (info == nullptr || peering == nullptr) {
            return;
        }
        for (const auto& [bit, value] : peering->as_object()) {
            std::string bit_at = at;
            bit_at += "/peering/";
            bit_at += bit;
            const auto direction = static_cast<Neighbor>(std::ranges::find(kNeighborNames, bit) -
                                                         kNeighborNames.begin());
            if (peering_kind(shape_, direction) == PeeringKind::None) {
                fail(bit_at, quoted(bit) + " is no side or corner of a " +
                                 shape_words(shape_.shape) + " tile, which has " +
                                 connected_names(shape_, TerrainMode::CornersAndSides));
                continue;
            }
            if (!connects(shape_, info->mode, direction)) {
                fail(bit_at, "the terrain set " + quoted(info->set) + " is " +
                                 std::string(kModeNames.at(static_cast<std::size_t>(info->mode))) +
                                 ", which connects " + shape_words(shape_.shape) + " tiles by " +
                                 connected_names(shape_, info->mode) + " only");
                continue;
            }
            if (value.is_null()) {
                continue;
            }
            const TerrainInfo* other = terrain(value.as_string(), bit_at);
            if (other != nullptr && other->set != info->set) {
                fail(bit_at, quoted(value.as_string()) + " is in the terrain set " +
                                 quoted(other->set) +
                                 "; peering names terrains of the tile's "
                                 "own set, " +
                                 quoted(info->set));
            }
        }
    }

    const json::Value& doc_;
    TileSet shape_; // only its shape, layout and offset axis
    std::size_t physics_layers_ = 0;
    std::size_t occlusion_layers_ = 0;
    std::map<std::string, CustomDataType, std::less<>> custom_data_;
    std::map<std::string, TerrainInfo, std::less<>> terrains_;
    std::map<std::string, std::string, std::less<>> names_; // tile name -> "source:x,y"
    std::vector<json::ValidationError> errors_;
};

bool valid_key(std::string_view key) {
    return !key.empty() && key.size() <= kMaxKeyLength && std::ranges::all_of(key, [](char c) {
        return kKeyCharacters.find(c) != std::string_view::npos;
    });
}

bool empty_key(std::string_view key) {
    return std::ranges::all_of(key, [](char c) { return c == '.'; });
}

// Decodes a layer's cells into out, if given, collecting errors; stops at the first if asked.
void decode(const world::TileMapLayer& layer, const TileSet& set, Cells* out,
            std::vector<json::ValidationError>& errors, bool first_only) {
    const auto stop = [&] { return first_only && !errors.empty(); };
    std::string_view first_key;
    std::map<std::string, std::optional<TileRef>, std::less<>> keys;
    bool lengths_ok = true;
    for (const auto& [key, value] : layer.legend) {
        const std::string at = "/legend/" + json::pointer_escape(key);
        if (!valid_key(key)) {
            errors.push_back({at, "a key is one to three letters, digits or punctuation marks "
                                  "other than '.', '\"' and backslash"});
            lengths_ok = false;
        } else if (first_key.empty()) {
            first_key = key;
        } else if (key.size() != first_key.size()) {
            errors.push_back({at, "keys are all the same length: " + quoted(key) + " is " +
                                      std::to_string(key.size()) + " characters, " +
                                      quoted(first_key) + " " + std::to_string(first_key.size())});
            lengths_ok = false;
        }
        auto tile = find_tile(set, value);
        if (!tile) {
            errors.push_back({at, tile.error().message});
            keys.emplace(key, std::nullopt);
        } else {
            keys.emplace(key, std::move(tile).value());
        }
        if (stop()) {
            return;
        }
    }
    if (!lengths_ok) {
        return; // the rows can't be split into keys
    }
    const std::size_t length = std::max<std::size_t>(first_key.size(), 1);
    if (layer.origin.x < kMinCell || layer.origin.x > kMaxCell || layer.origin.y < kMinCell ||
        layer.origin.y > kMaxCell) {
        errors.push_back({"/origin", "cells are from -32768 to 32767 in each direction"});
        return;
    }
    for (std::size_t i = 0; i < layer.rows.size(); ++i) {
        const std::string& row = layer.rows[i];
        const std::string at = "/rows/" + std::to_string(i);
        if (row.size() % length != 0) {
            errors.push_back({at, "is " + std::to_string(row.size()) +
                                      " characters, not a whole number of " +
                                      std::to_string(length) + "-character keys"});
        } else if (static_cast<std::int64_t>(layer.origin.y) + static_cast<std::int64_t>(i) >
                       kMaxCell ||
                   static_cast<std::int64_t>(layer.origin.x) +
                           static_cast<std::int64_t>(row.size() / length) - 1 >
                       kMaxCell) {
            errors.push_back({at, "reaches past 32767, the last column or row cells can be in"});
        } else {
            for (std::size_t column = 0; column * length < row.size(); ++column) {
                const std::string_view key = std::string_view(row).substr(column * length, length);
                if (empty_key(key)) {
                    continue;
                }
                const auto it = keys.find(key);
                if (it == keys.end()) {
                    errors.push_back({at, "column " + std::to_string(column) + ": " + quoted(key) +
                                              " is not in the legend"});
                    break;
                }
                if (out != nullptr && it->second) {
                    out->insert_or_assign(layer.origin +
                                              Vec2i{static_cast<int>(column), static_cast<int>(i)},
                                          *it->second);
                }
            }
        }
        if (stop()) {
            return;
        }
    }
}

// The key a new tile gets: the start of its name if that is free, else the first free key.
std::string new_key(std::string_view name, std::size_t length,
                    const std::set<std::string, std::less<>>& taken) {
    if (name.size() >= length) {
        std::string start(name.substr(0, length));
        if (valid_key(start) && !taken.contains(start)) {
            return start;
        }
        if (length == 1 && start.front() >= 'a' && start.front() <= 'z') {
            start.front() = static_cast<char>(start.front() - 'a' + 'A');
            if (!taken.contains(start)) {
                return start;
            }
        }
    }
    // Every key of this length, in kKeyCharacters order.
    std::size_t combinations = 1;
    for (std::size_t i = 0; i < length; ++i) {
        combinations *= kKeyCount;
    }
    std::string key(length, ' ');
    for (std::size_t n = 0; n < combinations; ++n) {
        std::size_t rest = n;
        for (std::size_t i = length; i-- > 0;) {
            key[i] = kKeyCharacters[rest % kKeyCount];
            rest /= kKeyCount;
        }
        if (!taken.contains(key)) {
            return key;
        }
    }
    SLASH_ASSERT(false, "no legend key left");
    return key;
}

// A cell as 64 bits, for hashing.
std::uint64_t cell_bits(Vec2i cell) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.x)) << 32U) |
           static_cast<std::uint32_t>(cell.y);
}

// Bits scrambled into a number from 0 up to 1.
double unit_hash(std::uint64_t x) {
    // splitmix64's finalizer.
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    constexpr double kScale = 1.0 / 9007199254740992.0; // 2^-53
    return static_cast<double>(x >> 11U) * kScale;
}

// A cell's place in the hash that picks between tiles that fit equally well.
double position_hash(Vec2i cell) {
    return unit_hash(cell_bits(cell));
}

struct Candidate {
    TileRef ref;
    const TileData* data = nullptr;
};

std::vector<TilePolygon> polygons_of(const json::Value& polygons) {
    std::vector<TilePolygon> out;
    for (const json::Value& item : polygons.as_array()) {
        TilePolygon polygon;
        for (const json::Value& point : item.find("points")->as_array()) {
            polygon.points.push_back({static_cast<float>(point.as_array()[0].as_double()),
                                      static_cast<float>(point.as_array()[1].as_double())});
        }
        if (const json::Value* value = item.find("layer"); value != nullptr) {
            polygon.layer = static_cast<int>(value->as_int());
        }
        if (const json::Value* value = item.find("one_way"); value != nullptr) {
            polygon.one_way = value->as_bool();
        }
        if (const json::Value* value = item.find("one_way_margin"); value != nullptr) {
            polygon.one_way_margin = static_cast<float>(value->as_double());
        }
        out.push_back(std::move(polygon));
    }
    return out;
}

std::vector<TileOccluder> occluders_of(const json::Value& occluders) {
    std::vector<TileOccluder> out;
    for (const json::Value& item : occluders.as_array()) {
        TileOccluder occluder;
        for (const json::Value& point : item.find("points")->as_array()) {
            occluder.points.push_back({static_cast<float>(point.as_array()[0].as_double()),
                                       static_cast<float>(point.as_array()[1].as_double())});
        }
        if (const json::Value* value = item.find("layer"); value != nullptr) {
            occluder.layer = static_cast<int>(value->as_int());
        }
        if (const json::Value* value = item.find("closed"); value != nullptr) {
            occluder.closed = value->as_bool();
        }
        if (const json::Value* value = item.find("cull_mode"); value != nullptr) {
            occluder.cull = static_cast<TileCull>(
                std::ranges::find(kTileCullNames, value->as_string()) - kTileCullNames.begin());
        }
        out.push_back(std::move(occluder));
    }
    return out;
}

// Custom data by layer, each value as its layer's type writes it: 2.0 in an int layer is 2.
std::map<int, json::Value> custom_data_of(const TileSet& set, const json::Value& data) {
    // A whole number written as a float, kept in range.
    const auto whole = [](const json::Value& number) {
        constexpr double kLimit = 9.0e18;
        return static_cast<std::int64_t>(std::clamp(number.as_double(), -kLimit, kLimit));
    };
    std::map<int, json::Value> out;
    for (const auto& [name, value] : data.as_object()) {
        const int layer = set.custom_data_layer(name);
        if (layer < 0) {
            continue;
        }
        json::Value stored = value;
        switch (set.custom_data_layers[static_cast<std::size_t>(layer)].type) {
        case CustomDataType::Int:
            if (!value.is_int()) {
                stored = whole(value);
            }
            break;
        case CustomDataType::Float:
            stored = value.as_double();
            break;
        case CustomDataType::Vector2:
        case CustomDataType::Color:
            for (json::Value& item : stored.as_array()) {
                item = item.as_double();
            }
            break;
        case CustomDataType::Vector2i:
            for (json::Value& item : stored.as_array()) {
                if (!item.is_int()) {
                    item = whole(item);
                }
            }
            break;
        case CustomDataType::Bool:
        case CustomDataType::String:
            break;
        }
        out.emplace(layer, std::move(stored));
    }
    return out;
}

bool flag(const json::Value& object, std::string_view key) {
    const json::Value* value = object.find(key);
    return value != nullptr && value->as_bool();
}

world::TileMapLayer layer_from_json(const json::Value& component) {
    world::TileMapLayer layer;
    if (const json::Value* value = component.find("tile_set"); value != nullptr) {
        layer.tile_set = value->as_string();
    }
    if (const json::Value* value = component.find("origin"); value != nullptr) {
        layer.origin = pair_of(value, {});
    }
    if (const json::Value* value = component.find("legend"); value != nullptr) {
        for (const auto& [key, tile] : value->as_object()) {
            layer.legend.emplace(key, tile.as_string());
        }
    }
    if (const json::Value* value = component.find("rows"); value != nullptr) {
        for (const json::Value& row : value->as_array()) {
            layer.rows.push_back(row.as_string());
        }
    }
    return layer;
}

} // namespace

const TileData* TileSet::find(const TileRef& tile) const {
    const auto source = sources.find(tile.source);
    if (source == sources.end()) {
        return nullptr;
    }
    const auto found = source->second.tiles.find(tile.atlas);
    return found == source->second.tiles.end() ? nullptr : &found->second;
}

const AlternativeTile* TileSet::alternative(const TileRef& tile) const {
    const TileData* data = find(tile);
    if (data == nullptr || tile.alternative == 0) {
        return nullptr;
    }
    const auto found = data->alternatives.find(tile.alternative);
    return found == data->alternatives.end() ? nullptr : &found->second;
}

int TileSet::terrain(std::string_view name) const {
    for (std::size_t i = 0; i < terrains.size(); ++i) {
        if (terrains[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TileSet::custom_data_layer(std::string_view name) const {
    for (std::size_t i = 0; i < custom_data_layers.size(); ++i) {
        if (custom_data_layers[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<json::Value> TileSet::custom_data(const TileRef& tile, std::string_view layer) const {
    const TileData* data = find(tile);
    const int index = custom_data_layer(layer);
    if (data == nullptr || index < 0) {
        return std::nullopt;
    }
    const AlternativeTile* alternative = this->alternative(tile);
    if (tile.alternative != 0 && alternative == nullptr) {
        return std::nullopt;
    }
    const std::map<int, json::Value>& values =
        alternative != nullptr ? alternative->custom_data : data->custom_data;
    if (const auto found = values.find(index); found != values.end()) {
        return found->second;
    }
    return custom_data_default(custom_data_layers[static_cast<std::size_t>(index)].type);
}

json::Value custom_data_default(CustomDataType type) {
    switch (type) {
    case CustomDataType::Bool:
        return false;
    case CustomDataType::Int:
        return std::int64_t{0};
    case CustomDataType::Float:
        return 0.0;
    case CustomDataType::String:
        return "";
    case CustomDataType::Vector2:
        return json::Array{0.0, 0.0};
    case CustomDataType::Vector2i:
        return json::Array{std::int64_t{0}, std::int64_t{0}};
    case CustomDataType::Color:
        return json::Array{0.0, 0.0, 0.0, 1.0};
    }
    return nullptr;
}

std::string_view terrain_mode_name(TerrainMode mode) {
    return kModeNames.at(static_cast<std::size_t>(mode));
}

PeeringKind peering_kind(const TileSet& set, Neighbor direction) {
    return kind_of(set.shape, set.offset_axis, direction);
}

std::optional<Vec2i> neighbor_cell(const TileSet& set, Vec2i cell, Neighbor direction) {
    const auto d = static_cast<std::size_t>(direction);
    const PeeringKind kind = peering_kind(set, direction);
    // A hexagon's corners have no cell across them.
    if (kind == PeeringKind::None || d % 2 == 1 ||
        (kind == PeeringKind::Corner && set.shape != TileShape::Square &&
         set.shape != TileShape::Isometric)) {
        return std::nullopt;
    }
    if (set.shape == TileShape::Square) {
        constexpr std::array<Vec2i, 8> kSteps{
            {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}}};
        return cell + kSteps.at(d / 2);
    }
    return cell + half_offset_step(set, cell, d / 2);
}

std::optional<Neighbor> godot_neighbor(TileShape shape, TileOffsetAxis axis, int bit) {
    if (bit < 0 || bit > 15) {
        return std::nullopt;
    }
    // Godot's bits go round clockwise from the right, a side then a corner each way.
    constexpr std::array<Neighbor, 8> kFacing{Neighbor::E, Neighbor::SE, Neighbor::S, Neighbor::SW,
                                              Neighbor::W, Neighbor::NW, Neighbor::N, Neighbor::NE};
    const auto way = static_cast<std::size_t>(bit / 2);
    const bool corner = bit % 2 == 1;
    Neighbor direction = kFacing.at(way);
    if (corner && way % 2 == 1 &&
        (shape == TileShape::HalfOffsetSquare || shape == TileShape::Hexagon)) {
        // A hexagon's corner beside a diagonal side lies toward e or w in offset rows, toward n
        // or s in offset columns.
        constexpr std::array<Neighbor, 4> kRows{Neighbor::ESE, Neighbor::WSW, Neighbor::WNW,
                                                Neighbor::ENE};
        constexpr std::array<Neighbor, 4> kColumns{Neighbor::SSE, Neighbor::SSW, Neighbor::NNW,
                                                   Neighbor::NNE};
        direction = (axis == TileOffsetAxis::Horizontal ? kRows : kColumns).at(way / 2);
    }
    const PeeringKind want = corner ? PeeringKind::Corner : PeeringKind::Side;
    if (kind_of(shape, axis, direction) != want) {
        return std::nullopt;
    }
    return direction;
}

Vec2 map_to_local(const TileSet& set, Vec2i cell) {
    // Godot's TileSet::map_to_local, line for line.
    auto x = static_cast<float>(cell.x);
    auto y = static_cast<float>(cell.y);
    const bool rows = set.offset_axis == TileOffsetAxis::Horizontal;
    if (set.shape != TileShape::Square) {
        switch (set.layout) {
        case TileLayout::Stacked:
            if (rows) {
                x += posmod2(cell.y) == 0 ? 0.0F : 0.5F;
            } else {
                y += posmod2(cell.x) == 0 ? 0.0F : 0.5F;
            }
            break;
        case TileLayout::StackedOffset:
            if (rows) {
                x += posmod2(cell.y) == 1 ? 0.0F : 0.5F;
            } else {
                y += posmod2(cell.x) == 1 ? 0.0F : 0.5F;
            }
            break;
        case TileLayout::StairsRight:
            if (rows) {
                x += y / 2.0F;
            } else {
                x = (x * 2.0F) + y;
                y /= 2.0F;
            }
            break;
        case TileLayout::StairsDown:
            if (rows) {
                y = (y * 2.0F) + x;
                x /= 2.0F;
            } else {
                y += x / 2.0F;
            }
            break;
        case TileLayout::DiamondRight: {
            const float ox = x;
            if (rows) {
                x = (ox + y) / 2.0F;
                y -= ox;
            } else {
                x = ox + y;
                y = (y - ox) / 2.0F;
            }
            break;
        }
        case TileLayout::DiamondDown: {
            const float ox = x;
            if (rows) {
                x = (ox - y) / 2.0F;
                y += ox;
            } else {
                x = ox - y;
                y = (y + ox) / 2.0F;
            }
            break;
        }
        }
    }
    const float overlap = overlap_of(set.shape);
    if (rows) {
        y *= overlap;
    } else {
        x *= overlap;
    }
    return {(x + 0.5F) * static_cast<float>(set.tile_size.x),
            (y + 0.5F) * static_cast<float>(set.tile_size.y)};
}

Vec2i local_to_map(const TileSet& set, Vec2 point) {
    // Godot's TileSet::local_to_map, line for line.
    Vec2 at{point.x / static_cast<float>(set.tile_size.x),
            point.y / static_cast<float>(set.tile_size.y)};
    const bool rows = set.offset_axis == TileOffsetAxis::Horizontal;
    const float overlap = overlap_of(set.shape);
    if (rows) {
        at.y /= overlap;
    } else {
        at.x /= overlap;
    }
    const auto floored = [](Vec2 v) { return Vec2{std::floor(v.x), std::floor(v.y)}; };
    const auto odd = [](float value) { return posmod2(whole(std::floor(value))) == 1; };
    if (set.shape == TileShape::Square) {
        const Vec2 cell = floored({at.x + 0.00005F, at.y + 0.00005F});
        return {whole(cell.x), whole(cell.y)};
    }
    const bool stacked_offset = set.layout == TileLayout::StackedOffset;
    const float slope = (1.0F / overlap) - 1.0F;
    const Vec2 raw = at;
    Vec2 add;
    if (rows) {
        // Floor, half a cell over in offset rows; then whether the point is in a top corner,
        // which belongs to a cell of the row above.
        at = odd(at.y) != stacked_offset ? Vec2{std::floor(at.x + 0.5F) - 0.5F, std::floor(at.y)}
                                         : floored(at);
        const Vec2 in_tile = raw - at;
        const bool top_left = cross(in_tile - Vec2{0.5F, 0.0F}, {-0.5F, slope}) <= 0.0F;
        const bool top_right = cross(in_tile - Vec2{0.5F, 0.0F}, {0.5F, slope}) > 0.0F;
        Vec2 left;
        Vec2 right;
        switch (set.layout) {
        case TileLayout::Stacked:
            at = floored(at);
            left = {odd(at.y) ? 0.0F : -1.0F, -1.0F};
            right = {odd(at.y) ? 1.0F : 0.0F, -1.0F};
            break;
        case TileLayout::StackedOffset:
            at = floored(at);
            left = {odd(at.y) ? -1.0F : 0.0F, -1.0F};
            right = {odd(at.y) ? 0.0F : 1.0F, -1.0F};
            break;
        case TileLayout::StairsRight:
            at = floored({at.x - (at.y / 2.0F), at.y});
            left = {0.0F, -1.0F};
            right = {1.0F, -1.0F};
            break;
        case TileLayout::StairsDown:
            at = floored({at.x * 2.0F, (at.y / 2.0F) - at.x});
            left = {-1.0F, 0.0F};
            right = {1.0F, -1.0F};
            break;
        case TileLayout::DiamondRight:
            at = floored({at.x - (at.y / 2.0F), (at.y / 2.0F) + at.x});
            left = {0.0F, -1.0F};
            right = {1.0F, 0.0F};
            break;
        case TileLayout::DiamondDown:
            at = floored({at.x + (at.y / 2.0F), (at.y / 2.0F) - at.x});
            left = {-1.0F, 0.0F};
            right = {0.0F, -1.0F};
            break;
        }
        if (top_left) {
            add = left;
        } else if (top_right) {
            add = right;
        }
    } else {
        at = odd(at.x) != stacked_offset ? Vec2{std::floor(at.x), std::floor(at.y + 0.5F) - 0.5F}
                                         : floored(at);
        const Vec2 in_tile = raw - at;
        const bool top_left = cross(in_tile - Vec2{0.0F, 0.5F}, {slope, -0.5F}) > 0.0F;
        const bool bottom_left = cross(in_tile - Vec2{0.0F, 0.5F}, {slope, 0.5F}) <= 0.0F;
        Vec2 top;
        Vec2 bottom;
        switch (set.layout) {
        case TileLayout::Stacked:
            at = floored(at);
            top = {-1.0F, odd(at.x) ? 0.0F : -1.0F};
            bottom = {-1.0F, odd(at.x) ? 1.0F : 0.0F};
            break;
        case TileLayout::StackedOffset:
            at = floored(at);
            top = {-1.0F, odd(at.x) ? -1.0F : 0.0F};
            bottom = {-1.0F, odd(at.x) ? 0.0F : 1.0F};
            break;
        case TileLayout::StairsRight:
            at = floored({(at.x / 2.0F) - at.y, at.y * 2.0F});
            top = {0.0F, -1.0F};
            bottom = {-1.0F, 1.0F};
            break;
        case TileLayout::StairsDown:
            at = floored({at.x, at.y - (at.x / 2.0F)});
            top = {-1.0F, 0.0F};
            bottom = {-1.0F, 1.0F};
            break;
        case TileLayout::DiamondRight:
            at = floored({(at.x / 2.0F) - at.y, at.y + (at.x / 2.0F)});
            top = {0.0F, -1.0F};
            bottom = {-1.0F, 0.0F};
            break;
        case TileLayout::DiamondDown:
            at = floored({(at.x / 2.0F) + at.y, at.y - (at.x / 2.0F)});
            top = {-1.0F, 0.0F};
            bottom = {0.0F, 1.0F};
            break;
        }
        if (top_left) {
            add = top;
        } else if (bottom_left) {
            add = bottom;
        }
    }
    return {whole(at.x + add.x), whole(at.y + add.y)};
}

std::vector<const Cells::value_type*> draw_order(const TileSet& set, const Cells& cells) {
    std::vector<const Cells::value_type*> out;
    out.reserve(cells.size());
    // Square cells' centers go as the cells do, in rows.
    if (set.shape == TileShape::Square) {
        for (const auto& cell : cells) {
            out.push_back(&cell);
        }
        return out;
    }
    std::vector<std::pair<Vec2, const Cells::value_type*>> centers;
    centers.reserve(cells.size());
    for (const auto& cell : cells) {
        centers.emplace_back(map_to_local(set, cell.first), &cell);
    }
    std::ranges::stable_sort(centers, [](const auto& a, const auto& b) {
        return a.first.y != b.first.y ? a.first.y < b.first.y : a.first.x < b.first.x;
    });
    for (const auto& entry : centers) {
        out.push_back(entry.second);
    }
    return out;
}

json::Value tileset_schema() {
    return schema();
}

std::vector<json::ValidationError> validate_tileset(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, schema());
    if (!errors.empty()) {
        return errors;
    }
    return Checker(doc).run();
}

Result<TileSet> parse_tileset(const json::Value& doc) {
    if (const auto errors = validate_tileset(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    TileSet set = shape_of(doc);
    set.tile_size = pair_of(doc.find("tile_size"), set.tile_size);
    if (const json::Value* layers = doc.find("physics_layers"); layers != nullptr) {
        for (const json::Value& json_layer : layers->as_array()) {
            PhysicsLayer layer;
            if (const json::Value* value = json_layer.find("collision_layer"); value != nullptr) {
                layer.collision_layer = static_cast<std::uint32_t>(value->as_int());
            }
            if (const json::Value* value = json_layer.find("collision_mask"); value != nullptr) {
                layer.collision_mask = static_cast<std::uint32_t>(value->as_int());
            }
            if (const json::Value* value = json_layer.find("friction"); value != nullptr) {
                layer.friction = static_cast<float>(value->as_double());
            }
            if (const json::Value* value = json_layer.find("bounce"); value != nullptr) {
                layer.bounce = static_cast<float>(value->as_double());
            }
            set.physics_layers.push_back(layer);
        }
    }
    if (const json::Value* layers = doc.find("occlusion_layers"); layers != nullptr) {
        for (const json::Value& json_layer : layers->as_array()) {
            OcclusionLayer layer;
            if (const json::Value* value = json_layer.find("light_mask"); value != nullptr) {
                layer.light_mask = static_cast<std::uint32_t>(value->as_int());
            }
            set.occlusion_layers.push_back(layer);
        }
    }
    if (const json::Value* layers = doc.find("custom_data_layers"); layers != nullptr) {
        for (const json::Value& json_layer : layers->as_array()) {
            const std::string& type = json_layer.find("type")->as_string();
            set.custom_data_layers.push_back(
                {.name = json_layer.find("name")->as_string(),
                 .type = static_cast<CustomDataType>(std::ranges::find(kCustomDataTypeNames, type) -
                                                     kCustomDataTypeNames.begin())});
        }
    }
    if (const json::Value* sets = doc.find("terrain_sets"); sets != nullptr) {
        for (const auto& [name, json_set] : sets->as_object()) {
            const json::Value* mode = json_set.find("mode");
            set.terrain_sets.push_back({.name = name,
                                        .mode = mode != nullptr ? parse_mode(mode->as_string())
                                                                : TerrainMode::CornersAndSides});
            for (const json::Value& terrain : json_set.find("terrains")->as_array()) {
                set.terrains.push_back({.name = terrain.as_string(),
                                        .set = static_cast<int>(set.terrain_sets.size() - 1)});
            }
        }
    }
    for (const auto& [name, json_source] : doc.find("sources")->as_object()) {
        AtlasSource source;
        source.texture = json_source.find("texture")->as_string();
        source.region_size = pair_of(json_source.find("region_size"), set.tile_size);
        source.margins = pair_of(json_source.find("margins"), {});
        source.separation = pair_of(json_source.find("separation"), {});
        if (const json::Value* path = json_source.find("normal_texture"); path != nullptr) {
            source.normal_texture = path->as_string();
        }
        if (const json::Value* path = json_source.find("specular_texture"); path != nullptr) {
            source.specular_texture = path->as_string();
        }
        if (const json::Value* color = json_source.find("specular_color"); color != nullptr) {
            const json::Array& rgba = color->as_array();
            source.specular_color = {
                static_cast<float>(rgba[0].as_double()), static_cast<float>(rgba[1].as_double()),
                static_cast<float>(rgba[2].as_double()), static_cast<float>(rgba[3].as_double())};
        }
        if (const json::Value* shininess = json_source.find("specular_shininess");
            shininess != nullptr) {
            source.specular_shininess = static_cast<float>(shininess->as_double());
        }
        if (const json::Value* tiles = json_source.find("tiles"); tiles != nullptr) {
            for (const auto& [key, json_tile] : tiles->as_object()) {
                const Vec2i atlas = parse_atlas(key).value_or(Vec2i{});
                TileData tile;
                if (const json::Value* value = json_tile.find("name"); value != nullptr) {
                    tile.name = value->as_string();
                    set.names.emplace(tile.name, TileRef{.source = name, .atlas = atlas});
                }
                if (const json::Value* value = json_tile.find("terrain"); value != nullptr) {
                    tile.terrain = set.terrain(value->as_string());
                    const TerrainMode mode =
                        set.terrain_sets
                            .at(static_cast<std::size_t>(
                                set.terrains.at(static_cast<std::size_t>(tile.terrain)).set))
                            .mode;
                    const json::Value* peering = json_tile.find("peering");
                    for (std::size_t bit = 0; bit < kNeighborNames.size(); ++bit) {
                        if (!connects(set, mode, static_cast<Neighbor>(bit))) {
                            continue;
                        }
                        const json::Value* value_bit =
                            peering != nullptr ? peering->find(kNeighborNames.at(bit)) : nullptr;
                        if (value_bit == nullptr) {
                            tile.peering.at(bit) = tile.terrain;
                        } else if (!value_bit->is_null()) {
                            tile.peering.at(bit) = set.terrain(value_bit->as_string());
                        }
                    }
                }
                if (const json::Value* value = json_tile.find("probability"); value != nullptr) {
                    tile.probability = value->as_double();
                }
                tile.texture_origin = pair_of(json_tile.find("texture_origin"), {});
                tile.size_in_atlas = pair_of(json_tile.find("size_in_atlas"), {1, 1});
                if (const json::Value* frames = json_tile.find("animation_frames");
                    frames != nullptr) {
                    tile.animation_frames.clear();
                    for (const json::Value& duration : frames->as_array()) {
                        tile.animation_frames.push_back(duration.as_double());
                    }
                }
                if (const json::Value* columns = json_tile.find("animation_columns");
                    columns != nullptr) {
                    tile.animation_columns = static_cast<int>(columns->as_int());
                }
                tile.animation_separation = pair_of(json_tile.find("animation_separation"), {});
                if (const json::Value* speed = json_tile.find("animation_speed");
                    speed != nullptr) {
                    tile.animation_speed = speed->as_double();
                }
                if (const json::Value* mode = json_tile.find("animation_mode");
                    mode != nullptr && mode->as_string() == kAnimationModeNames[1]) {
                    tile.animation_mode = TileAnimationMode::RandomStartTimes;
                }
                if (const json::Value* value = json_tile.find("collision"); value != nullptr) {
                    tile.collision = polygons_of(*value);
                }
                if (const json::Value* value = json_tile.find("occlusion"); value != nullptr) {
                    tile.occlusion = occluders_of(*value);
                }
                if (const json::Value* value = json_tile.find("custom_data"); value != nullptr) {
                    tile.custom_data = custom_data_of(set, *value);
                }
                if (const json::Value* value = json_tile.find("alternatives"); value != nullptr) {
                    for (const auto& [number, json_alternative] : value->as_object()) {
                        AlternativeTile alternative;
                        alternative.flip_h = flag(json_alternative, "flip_h");
                        alternative.flip_v = flag(json_alternative, "flip_v");
                        alternative.transpose = flag(json_alternative, "transpose");
                        if (const json::Value* modulate = json_alternative.find("modulate");
                            modulate != nullptr) {
                            const json::Array& rgba = modulate->as_array();
                            alternative.modulate = {static_cast<float>(rgba[0].as_double()),
                                                    static_cast<float>(rgba[1].as_double()),
                                                    static_cast<float>(rgba[2].as_double()),
                                                    static_cast<float>(rgba[3].as_double())};
                        }
                        alternative.texture_origin =
                            pair_of(json_alternative.find("texture_origin"), {});
                        if (const json::Value* collision = json_alternative.find("collision");
                            collision != nullptr) {
                            alternative.collision = polygons_of(*collision);
                        }
                        if (const json::Value* occlusion = json_alternative.find("occlusion");
                            occlusion != nullptr) {
                            alternative.occlusion = occluders_of(*occlusion);
                        }
                        if (const json::Value* data = json_alternative.find("custom_data");
                            data != nullptr) {
                            alternative.custom_data = custom_data_of(set, *data);
                        }
                        tile.alternatives.emplace(parse_alternative(number).value_or(1),
                                                  std::move(alternative));
                    }
                }
                source.tiles.emplace(atlas, std::move(tile));
            }
        }
        set.sources.emplace(name, std::move(source));
    }
    return set;
}

Result<TileRef> find_tile(const TileSet& set, std::string_view text) {
    // The turns and the alternative come last; what is left names the tile.
    std::string_view base = text;
    bool flip_h = false;
    bool flip_v = false;
    bool transpose = false;
    int alternative = 0;
    const auto last_part = [&] {
        const std::size_t colon = base.rfind(':');
        return colon == std::string_view::npos ? std::string_view() : base.substr(colon + 1);
    };
    if (const std::string_view turns = last_part();
        !turns.empty() && turns.find_first_not_of("hvt") == std::string_view::npos) {
        constexpr std::array<std::string_view, 7> kTurns = {"h", "v", "t", "hv", "ht", "vt", "hvt"};
        if (std::ranges::find(kTurns, turns) == kTurns.end()) {
            return make_error(ErrorCode::InvalidArgument,
                              quoted(text) + ": a cell's turns are h, v and t, each at most once "
                                             "and in that order, as \"crate:hv\"");
        }
        flip_h = turns.find('h') != std::string_view::npos;
        flip_v = turns.find('v') != std::string_view::npos;
        transpose = turns.find('t') != std::string_view::npos;
        base = base.substr(0, base.size() - turns.size() - 1);
    }
    if (const std::string_view number = last_part();
        !number.empty() && number.find_first_not_of("0123456789") == std::string_view::npos) {
        const std::optional<int> parsed = parse_alternative(number);
        if (!parsed && number != "0") {
            return make_error(ErrorCode::InvalidArgument,
                              quoted(text) + ": alternatives are numbered from 1 to " +
                                  std::to_string(kMaxAlternative));
        }
        alternative = parsed.value_or(0);
        base = base.substr(0, base.size() - number.size() - 1);
    }

    TileRef out;
    const std::size_t colon = base.find(':');
    if (colon == std::string_view::npos) {
        const auto it = set.names.find(base);
        if (it == set.names.end() && set.sources.contains(base)) {
            return make_error(ErrorCode::InvalidArgument,
                              quoted(text) +
                                  " is neither a tile name nor \"source:column,row\", as "
                                  "\"ground:2,0\"");
        }
        if (it == set.names.end()) {
            std::vector<std::string_view> names;
            names.reserve(set.names.size());
            for (const auto& [name, tile] : set.names) {
                names.emplace_back(name);
            }
            std::string message = "the tile set has no tile named " + quoted(base);
            add_hint(message, base, names);
            return make_error(ErrorCode::NotFound, std::move(message));
        }
        out = it->second;
    } else {
        const std::string_view source_name = base.substr(0, colon);
        const auto source = set.sources.find(source_name);
        if (source == set.sources.end()) {
            std::vector<std::string_view> names;
            names.reserve(set.sources.size());
            for (const auto& [name, unused] : set.sources) {
                names.emplace_back(name);
            }
            std::string message = "the tile set has no source " + quoted(source_name);
            add_hint(message, source_name, names);
            return make_error(ErrorCode::NotFound, std::move(message));
        }
        const std::optional<Vec2i> atlas = parse_atlas(base.substr(colon + 1));
        if (!atlas) {
            return make_error(ErrorCode::InvalidArgument,
                              quoted(text) +
                                  " is neither a tile name nor \"source:column,row\", as "
                                  "\"ground:2,0\", with an alternative and turns after it, as "
                                  "\"ground:2,0:1:hv\"");
        }
        if (!source->second.tiles.contains(*atlas)) {
            return make_error(ErrorCode::NotFound, "the source " + quoted(source_name) +
                                                       " has no tile at " + atlas_text(*atlas));
        }
        out.source = std::string(source_name);
        out.atlas = *atlas;
    }
    if (alternative != 0 && !set.find(out)->alternatives.contains(alternative)) {
        std::string message =
            "the tile " + quoted(base) + " has no alternative " + std::to_string(alternative);
        const auto& alternatives = set.find(out)->alternatives;
        if (alternatives.empty()) {
            message += "; it has none";
        } else {
            message += "; it has " + std::to_string(alternatives.begin()->first) + " to " +
                       std::to_string(alternatives.rbegin()->first);
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    out.alternative = alternative;
    out.flip_h = flip_h;
    out.flip_v = flip_v;
    out.transpose = transpose;
    return out;
}

std::string tile_text(const TileSet& set, const TileRef& tile) {
    std::string out;
    if (const TileData* data = set.find(tile); data != nullptr && !data->name.empty()) {
        out = data->name;
    } else {
        out = tile.source + ":" + atlas_text(tile.atlas);
    }
    if (tile.alternative != 0) {
        out += ":" + std::to_string(tile.alternative);
    }
    if (tile.flip_h || tile.flip_v || tile.transpose) {
        out += ':';
        if (tile.flip_h) {
            out += 'h';
        }
        if (tile.flip_v) {
            out += 'v';
        }
        if (tile.transpose) {
            out += 't';
        }
    }
    return out;
}

// Godot's TileMapLayer::compute_transformed_tile_dest_rect: the tile centered on the cell, turned
// by its alternative, moved by its texture origin, then turned by the cell.
TileLook tile_look(const TileSet& set, const TileRef& tile) {
    TileLook out;
    bool tile_flip_h = false;
    bool tile_flip_v = false;
    bool tile_transpose = false;
    Vec2i origin;
    if (const AlternativeTile* alternative = set.alternative(tile); alternative != nullptr) {
        tile_flip_h = alternative->flip_h;
        tile_flip_v = alternative->flip_v;
        tile_transpose = alternative->transpose;
        out.modulate = alternative->modulate;
        origin = alternative->texture_origin;
    } else if (const TileData* data = set.find(tile); data != nullptr) {
        origin = data->texture_origin;
    }
    out.transpose = tile_transpose != tile.transpose;
    out.flip_h = tile.flip_h != (tile.transpose ? tile_flip_v : tile_flip_h);
    out.flip_v = tile.flip_v != (tile.transpose ? tile_flip_h : tile_flip_v);
    Vec2i region;
    if (const auto source = set.sources.find(tile.source); source != set.sources.end()) {
        const auto [x, y, width, height] = tile_region(source->second, tile.atlas);
        region = {width, height};
    }
    out.size = out.transpose ? Vec2{static_cast<float>(region.y), static_cast<float>(region.x)}
                             : Vec2{static_cast<float>(region.x), static_cast<float>(region.y)};
    const Vec2 shift = tile.transpose
                           ? Vec2{static_cast<float>(origin.y), static_cast<float>(origin.x)}
                           : Vec2{static_cast<float>(origin.x), static_cast<float>(origin.y)};
    out.position = {(-0.5F * out.size.x) - shift.x, (-0.5F * out.size.y) - shift.y};
    if (tile.flip_h) {
        out.position.x = -(out.position.x + out.size.x);
    }
    if (tile.flip_v) {
        out.position.y = -(out.position.y + out.size.y);
    }
    return out;
}

std::vector<TilePolygon> tile_collision(const TileSet& set, const TileRef& tile) {
    const TileData* data = set.find(tile);
    if (data == nullptr) {
        return {};
    }
    const AlternativeTile* alternative = set.alternative(tile);
    std::vector<TilePolygon> out =
        alternative != nullptr ? alternative->collision : data->collision;
    for (TilePolygon& polygon : out) {
        for (Vec2& point : polygon.points) {
            if (tile.transpose) {
                point = {point.y, point.x};
            }
            if (tile.flip_h) {
                point.x = -point.x;
            }
            if (tile.flip_v) {
                point.y = -point.y;
            }
        }
    }
    return out;
}

std::vector<TileOccluder> tile_occluders(const TileSet& set, const TileRef& tile) {
    const TileData* data = set.find(tile);
    if (data == nullptr) {
        return {};
    }
    const AlternativeTile* alternative = set.alternative(tile);
    std::vector<TileOccluder> out =
        alternative != nullptr ? alternative->occlusion : data->occlusion;
    // An odd number of flips and transposes mirrors the points.
    const bool mirrored = (static_cast<int>(tile.flip_h) + static_cast<int>(tile.flip_v) +
                           static_cast<int>(tile.transpose)) %
                              2 !=
                          0;
    for (TileOccluder& occluder : out) {
        for (Vec2& point : occluder.points) {
            if (tile.transpose) {
                point = {point.y, point.x};
            }
            if (tile.flip_h) {
                point.x = -point.x;
            }
            if (tile.flip_v) {
                point.y = -point.y;
            }
        }
        if (mirrored && occluder.cull != TileCull::Disabled) {
            occluder.cull = occluder.cull == TileCull::Clockwise ? TileCull::CounterClockwise
                                                                 : TileCull::Clockwise;
        }
    }
    return out;
}

std::vector<physics2d::TileShape> tile_shapes(const TileSet& set, const Cells& cells) {
    std::vector<physics2d::TileShape> out;
    for (const auto& [cell, tile] : cells) {
        const Vec2 center = map_to_local(set, cell);
        for (TilePolygon& polygon : tile_collision(set, tile)) {
            if (polygon.layer < 0 ||
                static_cast<std::size_t>(polygon.layer) >= set.physics_layers.size()) {
                continue;
            }
            const PhysicsLayer& layer = set.physics_layers[static_cast<std::size_t>(polygon.layer)];
            for (Vec2& point : polygon.points) {
                point = point + center;
            }
            out.push_back({.cell = cell,
                           .points = std::move(polygon.points),
                           .collision_layer = layer.collision_layer,
                           .collision_mask = layer.collision_mask,
                           .friction = layer.friction,
                           .bounce = layer.bounce,
                           .one_way = polygon.one_way,
                           .one_way_margin = polygon.one_way_margin});
        }
    }
    return out;
}

std::array<int, 4> tile_region(const AtlasSource& source, Vec2i atlas, int frame) {
    Vec2i size{1, 1};
    Vec2i start = atlas;
    if (const auto tile = source.tiles.find(atlas); tile != source.tiles.end()) {
        const TileData& data = tile->second;
        size = data.size_in_atlas;
        start = frame_cell(atlas, size, data.animation_separation, data.animation_columns, frame);
    }
    return {source.margins.x + (start.x * (source.region_size.x + source.separation.x)),
            source.margins.y + (start.y * (source.region_size.y + source.separation.y)),
            (source.region_size.x * size.x) + (source.separation.x * (size.x - 1)),
            (source.region_size.y * size.y) + (source.separation.y * (size.y - 1))};
}

int tile_frame(const TileData& tile, double time, double offset) {
    const auto count = static_cast<int>(tile.animation_frames.size());
    if (count <= 1) {
        return 0;
    }
    // As Godot's draw_tile and the canvas renderer's animation slices: the frames' durations are
    // summed before they are divided by the speed, and a frame shows from its start up to, not
    // including, its end.
    const double speed = tile.animation_speed;
    double total = 0.0;
    for (const double duration : tile.animation_frames) {
        total += duration;
    }
    const double length = total / speed;
    double local = std::fmod(time - (offset * length), length);
    if (local < 0.0) {
        local += length;
    }
    double unscaled = 0.0;
    for (int frame = 0; frame < count; ++frame) {
        const double duration = tile.animation_frames[static_cast<std::size_t>(frame)];
        if (local >= unscaled / speed && local < (unscaled + duration) / speed) {
            return frame;
        }
        unscaled += duration;
    }
    return count - 1;
}

double tile_animation_offset(std::string_view layer, Vec2i cell) {
    // FNV-1a over the layer's ID, then the cell mixed in.
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : layer) {
        h = (h ^ static_cast<unsigned char>(c)) * 0x100000001B3ULL;
    }
    return unit_hash(h ^ cell_bits(cell));
}

Result<Cells> decode_cells(const world::TileMapLayer& layer, const TileSet& set) {
    Cells cells;
    std::vector<json::ValidationError> errors;
    decode(layer, set, &cells, errors, true);
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          errors.front().path + ": " + errors.front().message);
    }
    return cells;
}

std::vector<json::ValidationError> validate_cells(const world::TileMapLayer& layer,
                                                  const TileSet& set) {
    std::vector<json::ValidationError> errors;
    decode(layer, set, nullptr, errors, false);
    return errors;
}

void encode_cells(const Cells& cells, const TileSet& set, world::TileMapLayer& layer) {
    if (cells.empty()) {
        layer.origin = {};
        layer.legend.clear();
        layer.rows.clear();
        return;
    }
    // The tiles in use, the most used first, each with its key.
    std::vector<std::string> order;
    std::map<std::string, std::string, std::less<>> key_of;
    std::map<std::string, std::size_t, std::less<>> uses;
    std::map<std::string, std::string_view, std::less<>> name_of; // "" for a tile with none
    int min_x = INT_MAX;
    for (const auto& [cell, tile] : cells) {
        min_x = std::min(min_x, cell.x);
        std::string text = tile_text(set, tile);
        ++uses[text];
        if (key_of.try_emplace(text).second) {
            const TileData* data = set.find(tile);
            name_of.emplace(text, data != nullptr ? std::string_view(data->name) : "");
            order.push_back(std::move(text));
        }
    }
    std::ranges::stable_sort(order, [&](const std::string& a, const std::string& b) {
        return uses.find(a)->second > uses.find(b)->second;
    });
    std::size_t length = 1;
    for (std::size_t capacity = kKeyCount; capacity < order.size(); capacity *= kKeyCount) {
        ++length;
    }
    std::set<std::string, std::less<>> taken;
    for (const auto& [key, value] : layer.legend) {
        if (key.size() != length || !valid_key(key)) {
            continue;
        }
        auto tile = find_tile(set, value);
        if (!tile) {
            continue;
        }
        const auto it = key_of.find(tile_text(set, tile.value()));
        if (it != key_of.end() && it->second.empty()) {
            it->second = key;
            taken.insert(key);
        }
    }
    for (const std::string& text : order) {
        std::string& key = key_of.find(text)->second;
        if (key.empty()) {
            key = new_key(name_of.find(text)->second, length, taken);
            taken.insert(key);
        }
    }

    const int min_y = cells.begin()->first.y;
    const int max_y = cells.rbegin()->first.y;
    layer.origin = {min_x, min_y};
    layer.legend.clear();
    for (const auto& [text, key] : key_of) {
        layer.legend.emplace(key, text);
    }
    layer.rows.assign(static_cast<std::size_t>(max_y - min_y) + 1, std::string());
    for (const auto& [cell, tile] : cells) {
        std::string& row = layer.rows[static_cast<std::size_t>(cell.y - min_y)];
        const std::size_t at = static_cast<std::size_t>(cell.x - min_x) * length;
        row.append(at - row.size(), '.');
        row += key_of.find(tile_text(set, tile))->second;
    }
}

Result<std::size_t> paint_terrain(Cells& cells, const TileSet& set, std::span<const Vec2i> painted,
                                  int terrain) {
    if (terrain < -1 || terrain >= static_cast<int>(set.terrains.size())) {
        return make_error(ErrorCode::InvalidArgument,
                          "the tile set has no terrain " + std::to_string(terrain));
    }
    // The tiles of each terrain, in source and then grid order.
    std::vector<std::vector<Candidate>> candidates(set.terrains.size());
    for (const auto& [name, source] : set.sources) {
        for (const auto& [atlas, tile] : source.tiles) {
            if (tile.terrain >= 0) {
                candidates.at(static_cast<std::size_t>(tile.terrain))
                    .push_back({.ref = {.source = name, .atlas = atlas}, .data = &tile});
            }
        }
    }
    if (terrain >= 0 && candidates.at(static_cast<std::size_t>(terrain)).empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "no tile has the terrain " +
                              quoted(set.terrains.at(static_cast<std::size_t>(terrain)).name));
    }
    const std::set<Vec2i, CellOrder> paint(painted.begin(), painted.end());
    const auto terrain_at = [&](Vec2i cell) {
        if (paint.contains(cell)) {
            return terrain;
        }
        const auto it = cells.find(cell);
        if (it == cells.end()) {
            return -1;
        }
        const TileData* data = set.find(it->second);
        return data != nullptr ? data->terrain : -1;
    };
    // The shape's sides and corners in order round a cell, and the cells beside a cell: across
    // its sides, and across its corners where a cell is.
    std::vector<Neighbor> ring;
    for (std::size_t d = 0; d < kNeighborNames.size(); ++d) {
        if (peering_kind(set, static_cast<Neighbor>(d)) != PeeringKind::None) {
            ring.push_back(static_cast<Neighbor>(d));
        }
    }
    const auto in_range = [](Vec2i cell) {
        return cell.x >= kMinCell && cell.x <= kMaxCell && cell.y >= kMinCell && cell.y <= kMaxCell;
    };
    const auto beside = [&](Vec2i cell, Neighbor direction) -> std::optional<Vec2i> {
        const std::optional<Vec2i> next = neighbor_cell(set, cell, direction);
        return next && in_range(*next) ? next : std::nullopt;
    };
    // The painted cells, and the terrain tiles around them that must fit them.
    std::set<Vec2i, CellOrder> affected = paint;
    for (const Vec2i cell : paint) {
        for (const Neighbor direction : ring) {
            if (const auto next = beside(cell, direction); next && terrain_at(*next) >= 0) {
                affected.insert(*next);
            }
        }
    }

    Cells picked;
    std::set<Vec2i, CellOrder> erased;
    for (const Vec2i cell : affected) {
        const int own = terrain_at(cell);
        if (own < 0) {
            erased.insert(cell);
            continue;
        }
        const int set_index = set.terrains.at(static_cast<std::size_t>(own)).set;
        const TerrainMode mode = set.terrain_sets.at(static_cast<std::size_t>(set_index)).mode;
        // The terrain across each side, and across each corner where a cell is; a hexagon's
        // corners have none, so the cell's own.
        std::array<int, 16> around{};
        around.fill(-1);
        for (const Neighbor direction : ring) {
            const auto next = neighbor_cell(set, cell, direction);
            int found = own;
            if (next) {
                found = in_range(*next) ? terrain_at(*next) : -1;
            }
            around.at(static_cast<std::size_t>(direction)) =
                found >= 0 && set.terrains.at(static_cast<std::size_t>(found)).set == set_index
                    ? found
                    : -1;
        }
        // A side wants its neighbor's terrain. A corner wants its own terrain only where the
        // sides beside it and the cell across it have that terrain; else what breaks the run.
        std::array<int, 16> wanted = around;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            if (peering_kind(set, ring[i]) != PeeringKind::Corner) {
                continue;
            }
            const int before =
                around.at(static_cast<std::size_t>(ring[(i + ring.size() - 1) % ring.size()]));
            const int after = around.at(static_cast<std::size_t>(ring[(i + 1) % ring.size()]));
            int& corner = wanted.at(static_cast<std::size_t>(ring[i]));
            if (before != own) {
                corner = before;
            } else if (after != own) {
                corner = after;
            }
        }
        // A wrong bit costs 2 where the tile joins its own terrain and the neighbor doesn't, or
        // the other way round, so it shows a seam; 1 where both differ from it, as water for
        // no terrain.
        std::size_t best = SIZE_MAX;
        std::vector<const Candidate*> fits;
        for (const Candidate& candidate : candidates.at(static_cast<std::size_t>(own))) {
            std::size_t cost = 0;
            for (const Neighbor direction : ring) {
                const auto bit = static_cast<std::size_t>(direction);
                const int has = candidate.data->peering.at(bit);
                const int want = wanted.at(bit);
                if (connects(set, mode, direction) && has != want) {
                    cost += (has == own) != (want == own) ? 2 : 1;
                }
            }
            if (cost < best) {
                best = cost;
                fits.clear();
            }
            if (cost == best) {
                fits.push_back(&candidate);
            }
        }
        double total = 0.0;
        for (const Candidate* fit : fits) {
            total += fit->data->probability;
        }
        const Candidate* chosen = fits.front();
        if (total > 0.0) {
            const double roll = position_hash(cell) * total;
            double sum = 0.0;
            for (const Candidate* fit : fits) {
                sum += fit->data->probability;
                if (fit->data->probability > 0.0 && roll < sum) {
                    chosen = fit;
                    break;
                }
            }
        }
        picked.emplace(cell, chosen->ref);
    }

    std::size_t changed = 0;
    for (const Vec2i cell : erased) {
        changed += cells.erase(cell);
    }
    for (auto& [cell, tile] : picked) {
        const auto it = cells.find(cell);
        if (it == cells.end() || !(it->second == tile)) {
            ++changed;
            cells.insert_or_assign(cell, std::move(tile));
        }
    }
    return changed;
}

std::vector<json::ValidationError> check_tile_layers(const json::Value& doc,
                                                     const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    if (!reader) {
        return errors;
    }
    std::map<std::string, Result<TileSet>, std::less<>> loaded;
    const auto load = [&](const std::string& path) -> const Result<TileSet>& {
        if (const auto it = loaded.find(path); it != loaded.end()) {
            return it->second;
        }
        const auto parse = [&]() -> Result<TileSet> {
            auto text = reader(path);
            if (!text) {
                return text.error();
            }
            auto parsed = json::parse(text.value());
            if (!parsed) {
                return parsed.error();
            }
            return parse_tileset(parsed.value());
        };
        return loaded.emplace(path, parse()).first->second;
    };
    const auto check = [&](const json::Value& components, const std::string& at, bool extends) {
        const json::Value* component = components.find("TileMapLayer");
        if (component == nullptr || !component->is_object()) {
            return;
        }
        // Parts from a prefab are checked there.
        if (extends &&
            (component->find("tile_set") == nullptr || component->find("legend") == nullptr ||
             component->find("rows") == nullptr)) {
            return;
        }
        const world::TileMapLayer layer = layer_from_json(*component);
        if (layer.tile_set.empty()) {
            if (!layer.legend.empty() || !layer.rows.empty()) {
                errors.push_back({at + "/tile_set", "a layer with cells needs a tile set"});
            }
            return;
        }
        if (!layer.tile_set.ends_with(kTileSetSuffix)) {
            errors.push_back(
                {at + "/tile_set",
                 "tile sets are .tileset.json files, as \"tiles/ground.tileset.json\""});
            return;
        }
        const Result<TileSet>& set = load(layer.tile_set);
        if (!set) {
            errors.push_back({at + "/tile_set", layer.tile_set + ": " + set.error().message});
            return;
        }
        for (json::ValidationError& error : validate_cells(layer, set.value())) {
            errors.push_back({at + error.path, std::move(error.message)});
        }
    };
    if (const json::Value* entities = doc.find("entities"); entities != nullptr) {
        for (const auto& [id, entity] : entities->as_object()) {
            // An entity an instanced scene brings ("hero/map") takes the rest from the scene.
            if (const json::Value* components = entity.find("components"); components != nullptr) {
                check(*components,
                      "/entities/" + json::pointer_escape(id) + "/components/TileMapLayer",
                      entity.find("extends") != nullptr || id.find('/') != std::string::npos);
            }
        }
    } else if (const json::Value* components = doc.find("components"); components != nullptr) {
        check(*components, "/components/TileMapLayer", doc.find("extends") != nullptr);
    }
    return errors;
}

} // namespace slash::canvas
