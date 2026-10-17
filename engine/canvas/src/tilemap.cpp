#include "slash/canvas/tilemap.hpp"

#include "slash/canvas/debug_draw.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
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
    tile["properties"]["peering"] =
        property("object", "The terrain each neighbor must have for the tile to fit, by "
                           "direction: n, ne, e, se, s, sw, w, nw. Left out, the tile's own "
                           "terrain; null, none.");
    tile["properties"]["peering"]["additionalProperties"] = false;
    for (const std::string_view name : kNeighborNames) {
        tile["properties"]["peering"]["properties"][name] = peering_bit;
    }
    tile["properties"]["probability"] =
        property("number", "How often terrain painting picks it over tiles that fit as well.");
    tile["properties"]["probability"]["minimum"] = 0;
    tile["properties"]["probability"]["default"] = 1;
    tile["properties"]["collision"] =
        property("array", "Polygons that make the tile solid, each in a physics layer.");
    tile["properties"]["collision"]["items"]["$ref"] = "#/$defs/polygon";
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
    polygon["properties"]["points"]["items"] = std::move(point);
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
    alternative["properties"]["collision"] =
        property("array", "Its own collision polygons, as the tile's; none if left out.");
    alternative["properties"]["collision"]["items"]["$ref"] = "#/$defs/polygon";

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
        property("string", "How tiles connect: by all eight neighbors, the four at their "
                           "corners or the four at their sides.");
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
    out["properties"]["physics_layers"] = property(
        "array", "Physics layers by number from 0: what tiles' collision polygons collide as.");
    out["properties"]["physics_layers"]["items"]["$ref"] = "#/$defs/physics_layer";
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

bool is_corner(std::size_t bit) {
    return bit % 2 == 1;
}

// Whether a terrain set's mode connects tiles by this neighbor.
bool connects(TerrainMode mode, std::size_t bit) {
    switch (mode) {
    case TerrainMode::Corners:
        return is_corner(bit);
    case TerrainMode::Sides:
        return !is_corner(bit);
    case TerrainMode::CornersAndSides:
        break;
    }
    return true;
}

std::string_view connected_bits(TerrainMode mode) {
    switch (mode) {
    case TerrainMode::Corners:
        return "ne, se, sw and nw";
    case TerrainMode::Sides:
        return "n, e, s and w";
    case TerrainMode::CornersAndSides:
        break;
    }
    return "all eight";
}

TerrainMode parse_mode(std::string_view name) {
    return static_cast<TerrainMode>(std::ranges::find(kModeNames, name) - kModeNames.begin());
}

std::string path_error() {
    return "expected a path inside the project folder, separated by '/', with no \".\" or "
           "\"..\" parts";
}

// Checks a document the schema accepted, collecting every error.
class Checker {
public:
    explicit Checker(const json::Value& doc) : doc_(doc) {}

    std::vector<json::ValidationError> run() {
        if (const json::Value* layers = doc_.find("physics_layers"); layers != nullptr) {
            physics_layers_ = layers->as_array().size();
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
        for (const auto& [key, tile] : tiles->as_object()) {
            const std::string tile_at = at + "/tiles/" + json::pointer_escape(key);
            if (!parse_atlas(key)) {
                fail(tile_at, R"(tiles are keyed by "column,row" in the texture's grid, as "2,0")");
                continue;
            }
            std::string where = name;
            where += ':';
            where += key;
            check_tile(where, tile, tile_at);
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

    void check_tile(const std::string& where, const json::Value& tile, const std::string& at) {
        if (const json::Value* collision = tile.find("collision"); collision != nullptr) {
            check_polygons(*collision, at + "/collision");
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
            const auto index = static_cast<std::size_t>(std::ranges::find(kNeighborNames, bit) -
                                                        kNeighborNames.begin());
            if (!connects(info->mode, index)) {
                fail(bit_at, "the terrain set " + quoted(info->set) + " is " +
                                 std::string(kModeNames.at(static_cast<std::size_t>(info->mode))) +
                                 ", which connects tiles by " +
                                 std::string(connected_bits(info->mode)) + " only");
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
    std::size_t physics_layers_ = 0;
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

// A cell's place in the hash that picks between tiles that fit equally well.
double position_hash(Vec2i cell) {
    std::uint64_t x = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.x)) << 32U) |
                      static_cast<std::uint32_t>(cell.y);
    // splitmix64's finalizer.
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    constexpr double kScale = 1.0 / 9007199254740992.0; // 2^-53
    return static_cast<double>(x >> 11U) * kScale;
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

std::string_view terrain_mode_name(TerrainMode mode) {
    return kModeNames.at(static_cast<std::size_t>(mode));
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
    TileSet set;
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
                        if (!connects(mode, bit)) {
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
                if (const json::Value* value = json_tile.find("collision"); value != nullptr) {
                    tile.collision = polygons_of(*value);
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
                        if (const json::Value* collision = json_alternative.find("collision");
                            collision != nullptr) {
                            alternative.collision = polygons_of(*collision);
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

TileLook tile_look(const TileSet& set, const TileRef& tile) {
    TileLook out{.flip_h = tile.flip_h,
                 .flip_v = tile.flip_v,
                 .transpose = tile.transpose,
                 .modulate = {1.0F, 1.0F, 1.0F, 1.0F}};
    if (const AlternativeTile* alternative = set.alternative(tile); alternative != nullptr) {
        out.flip_h = out.flip_h != alternative->flip_h;
        out.flip_v = out.flip_v != alternative->flip_v;
        out.transpose = out.transpose != alternative->transpose;
        out.modulate = alternative->modulate;
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

std::vector<physics2d::TileShape> tile_shapes(const TileSet& set, const Cells& cells) {
    std::vector<physics2d::TileShape> out;
    for (const auto& [cell, tile] : cells) {
        const Vec2 center{(static_cast<float>(cell.x) + 0.5F) * static_cast<float>(set.tile_size.x),
                          (static_cast<float>(cell.y) + 0.5F) *
                              static_cast<float>(set.tile_size.y)};
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

std::array<int, 4> tile_region(const AtlasSource& source, Vec2i atlas) {
    return {source.margins.x + (atlas.x * (source.region_size.x + source.separation.x)),
            source.margins.y + (atlas.y * (source.region_size.y + source.separation.y)),
            source.region_size.x, source.region_size.y};
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
    // The painted cells, and the terrain tiles around them that must fit them.
    std::set<Vec2i, CellOrder> affected = paint;
    for (const Vec2i cell : paint) {
        for (const Vec2i offset : kNeighborOffsets) {
            const Vec2i next = cell + offset;
            if (next.x >= kMinCell && next.x <= kMaxCell && next.y >= kMinCell &&
                next.y <= kMaxCell && terrain_at(next) >= 0) {
                affected.insert(next);
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
        std::array<int, 8> around{};
        for (std::size_t bit = 0; bit < around.size(); ++bit) {
            const int found = terrain_at(cell + kNeighborOffsets.at(bit));
            around.at(bit) =
                found >= 0 && set.terrains.at(static_cast<std::size_t>(found)).set == set_index
                    ? found
                    : -1;
        }
        // A side wants its neighbor's terrain. A corner wants its own terrain only where both
        // sides beside it and the cell across it have that terrain; else what breaks the run.
        std::array<int, 8> wanted = around;
        for (std::size_t bit = 1; bit < wanted.size(); bit += 2) {
            const int before = around.at(bit - 1);
            const int after = around.at((bit + 1) % around.size());
            if (before != own) {
                wanted.at(bit) = before;
            } else if (after != own) {
                wanted.at(bit) = after;
            }
        }
        // A wrong bit costs 2 where the tile joins its own terrain and the neighbor doesn't, or
        // the other way round, so it shows a seam; 1 where both differ from it, as water for
        // no terrain.
        std::size_t best = SIZE_MAX;
        std::vector<const Candidate*> fits;
        for (const Candidate& candidate : candidates.at(static_cast<std::size_t>(own))) {
            std::size_t cost = 0;
            for (std::size_t bit = 0; bit < wanted.size(); ++bit) {
                const int has = candidate.data->peering.at(bit);
                const int want = wanted.at(bit);
                if (connects(mode, bit) && has != want) {
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
