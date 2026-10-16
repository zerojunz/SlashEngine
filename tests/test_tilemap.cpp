#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/debug_draw.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec2i;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// Grass on water, as most 2D games start: water, and grass tiles for a block's middle, its four
// edges, its four outer corners and the four inner corners where water cuts in diagonally. Each
// tile's peering names the sides and corners that touch water; the rest touch grass.
constexpr std::string_view kGround = R"({
  "format": 1,
  "tile_size": [8, 8],
  "terrain_sets": {"ground": {"terrains": ["grass", "water"]}},
  "sources": {
    "ground": {
      "texture": "ground.png",
      "tiles": {
        "0,0": {"name": "grass_nw", "terrain": "grass",
                "peering": {"n": "water", "nw": "water", "w": "water", "ne": "water", "sw": "water"}},
        "1,0": {"name": "grass_n", "terrain": "grass",
                "peering": {"n": "water", "ne": "water", "nw": "water"}},
        "2,0": {"name": "grass_ne", "terrain": "grass",
                "peering": {"n": "water", "ne": "water", "e": "water", "nw": "water", "se": "water"}},
        "3,0": {"name": "water", "terrain": "water"},
        "0,1": {"name": "grass_w", "terrain": "grass",
                "peering": {"w": "water", "nw": "water", "sw": "water"}},
        "1,1": {"name": "grass", "terrain": "grass"},
        "2,1": {"name": "grass_e", "terrain": "grass",
                "peering": {"e": "water", "ne": "water", "se": "water"}},
        "3,1": {"name": "grass_in_nw", "terrain": "grass", "peering": {"nw": "water"}},
        "0,2": {"name": "grass_sw", "terrain": "grass",
                "peering": {"s": "water", "sw": "water", "w": "water", "se": "water", "nw": "water"}},
        "1,2": {"name": "grass_s", "terrain": "grass",
                "peering": {"s": "water", "se": "water", "sw": "water"}},
        "2,2": {"name": "grass_se", "terrain": "grass",
                "peering": {"s": "water", "se": "water", "e": "water", "sw": "water", "ne": "water"}},
        "3,2": {"name": "grass_in_ne", "terrain": "grass", "peering": {"ne": "water"}},
        "0,3": {"name": "grass_in_sw", "terrain": "grass", "peering": {"sw": "water"}},
        "1,3": {"name": "grass_in_se", "terrain": "grass", "peering": {"se": "water"}},
        "2,3": {"name": "rock"}
      }
    }
  }
})";

canvas::TileSet ground() {
    auto set = canvas::parse_tileset(parse(kGround));
    REQUIRE(set.ok());
    return std::move(set).value();
}

std::string first_error(std::string_view text) {
    const auto errors = canvas::validate_tileset(parse(text));
    return errors.empty() ? "(valid)" : errors.front().path + ": " + errors.front().message;
}

// The tile names in a rectangle of cells, row by row, "." for an empty cell.
std::vector<std::string> names(const canvas::Cells& cells, const canvas::TileSet& set, int x, int y,
                               int width, int height) {
    std::vector<std::string> rows;
    for (int row = y; row < y + height; ++row) {
        std::string line;
        for (int column = x; column < x + width; ++column) {
            const auto it = cells.find(Vec2i{column, row});
            line += (column == x ? "" : " ") +
                    (it == cells.end() ? std::string(".") : canvas::tile_text(set, it->second));
        }
        rows.push_back(line);
    }
    return rows;
}

std::vector<Vec2i> rect(int x, int y, int width, int height) {
    std::vector<Vec2i> out;
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            out.push_back({column, row});
        }
    }
    return out;
}

world::TileMapLayer layer(std::map<std::string, std::string> legend, std::vector<std::string> rows,
                          Vec2i origin = {}) {
    world::TileMapLayer out;
    out.tile_set = "ground.tileset.json";
    out.origin = origin;
    out.legend = std::move(legend);
    out.rows = std::move(rows);
    return out;
}

std::string decode_error(const world::TileMapLayer& layer) {
    auto cells = canvas::decode_cells(layer, ground());
    return cells.ok() ? "(valid)" : cells.error().message;
}

} // namespace

TEST_CASE("tile set files are checked, with every mistake located") {
    CHECK(first_error(kGround) == "(valid)");
    CHECK(first_error(R"({"format": 1, "sources": {}})") ==
          "/sources: expected at least 1 properties, got 0");
    CHECK(first_error(R"({"format": 1, "sources": {"a": {"texture": "a.png",
          "tiles": {"0;0": {}}}}})") ==
          "/sources/a/tiles/0;0: tiles are keyed by \"column,row\" in the texture's grid, as "
          "\"2,0\"");
    CHECK(first_error(R"({"format": 1, "sources": {"a": {"texture": "../a.png"}}})")
              .starts_with("/sources/a/texture: expected a path inside the project"));
    CHECK(first_error(R"({"format": 1, "terrain_sets": {"t": {"terrains": ["grass"]}},
          "sources": {"a": {"texture": "a.png", "tiles": {"0,0": {"terrain": "gras"}}}}})") ==
          "/sources/a/tiles/0,0/terrain: no terrain set has a terrain named \"gras\"; did you "
          "mean \"grass\"?");
    CHECK(first_error(R"({"format": 1, "terrain_sets": {"t": {"terrains": ["grass"]},
          "u": {"terrains": ["grass"]}}, "sources": {"a": {"texture": "a.png"}}})") ==
          "/terrain_sets/u/terrains/0: \"grass\" is already a terrain, in the set \"t\"; "
          "terrain names are unique in the file");
    CHECK(first_error(R"({"format": 1, "terrain_sets": {"t": {"mode": "match_sides",
          "terrains": ["grass", "water"]}}, "sources": {"a": {"texture": "a.png",
          "tiles": {"0,0": {"terrain": "grass", "peering": {"ne": "water"}}}}}})") ==
          "/sources/a/tiles/0,0/peering/ne: the terrain set \"t\" is match_sides, which connects "
          "tiles by n, e, s and w only");
    CHECK(first_error(R"({"format": 1, "terrain_sets": {"t": {"terrains": ["grass"]},
          "u": {"terrains": ["lava"]}}, "sources": {"a": {"texture": "a.png",
          "tiles": {"0,0": {"terrain": "grass", "peering": {"n": "lava"}}}}}})") ==
          "/sources/a/tiles/0,0/peering/n: \"lava\" is in the terrain set \"u\"; peering names "
          "terrains of the tile's own set, \"t\"");
    CHECK(first_error(R"({"format": 1, "sources": {"a": {"texture": "a.png",
          "tiles": {"0,0": {"peering": {"n": null}}}}}})") ==
          "/sources/a/tiles/0,0/peering: only a tile with a \"terrain\" has \"peering\"");
    CHECK(first_error(R"({"format": 1, "sources": {"a": {"texture": "a.png",
          "tiles": {"0,0": {"name": "x"}}}, "b": {"texture": "b.png",
          "tiles": {"1,0": {"name": "x"}}}}})") ==
          "/sources/b/tiles/1,0/name: another tile is named \"x\": a:0,0");
    CHECK(first_error(R"({"format": 1, "sources": {"a": {"texture": "a.png",
          "tiles": {"0,0": {"name": "a:b"}}}}})") ==
          "/sources/a/tiles/0,0/name: a tile name has no ':', which \"source:column,row\" uses");
    CHECK(first_error(R"({"format": 1, "sources": {"a": {"texture": "a.png",
          "tiles": {"0,0": {"terrain": "x", "peering": {"north": null}}}}}})")
              .find("did you mean \"n") == std::string::npos); // checked by the schema first

    const canvas::TileSet set = ground();
    CHECK(set.tile_size == Vec2i{8, 8});
    REQUIRE(set.terrain_sets.size() == 1);
    CHECK(set.terrain_sets[0].mode == canvas::TerrainMode::CornersAndSides);
    CHECK(set.terrain("water") == 1);
    CHECK(set.terrain("lava") == -1);
    const canvas::AtlasSource& source = set.sources.at("ground");
    CHECK(source.region_size == Vec2i{8, 8}); // the tile size, when left out
    const canvas::TileData& north = source.tiles.at({1, 0});
    CHECK(north.name == "grass_n");
    CHECK(north.peering == std::array<int, 8>{1, 1, 0, 0, 0, 0, 0, 1});
    CHECK(source.tiles.at({2, 3}).terrain == -1);
    CHECK(source.tiles.at({2, 3}).peering == std::array<int, 8>{-1, -1, -1, -1, -1, -1, -1, -1});
    CHECK(set.names.size() == 15);

    // A side-only set leaves the corners out.
    auto sides = canvas::parse_tileset(parse(R"({"format": 1, "terrain_sets": {"t": {
        "mode": "match_sides", "terrains": ["a", "b"]}}, "sources": {"s": {"texture": "s.png",
        "region_size": [16, 24], "margins": [1, 2], "separation": [3, 4],
        "tiles": {"2,1": {"terrain": "a", "peering": {"e": "b", "w": null},
                          "probability": 0.5}}}}})"));
    REQUIRE(sides.ok());
    const canvas::AtlasSource& s = sides.value().sources.at("s");
    CHECK(s.tiles.at({2, 1}).peering == std::array<int, 8>{0, -1, 1, -1, 0, -1, -1, -1});
    CHECK(s.tiles.at({2, 1}).probability == doctest::Approx(0.5));
    CHECK(canvas::tile_region(s, {2, 1}) == std::array<int, 4>{1 + (2 * 19), 2 + 28, 16, 24});
    CHECK(canvas::tile_text(sides.value(), {.source = "s", .atlas = {2, 1}}) == "s:2,1");
}

TEST_CASE("tiles are found by name or by source and grid cell, with hints") {
    const canvas::TileSet set = ground();
    auto tile = canvas::find_tile(set, "grass_ne");
    REQUIRE(tile.ok());
    CHECK(tile.value() == canvas::TileRef{.source = "ground", .atlas = {2, 0}});
    tile = canvas::find_tile(set, "ground:2,3");
    REQUIRE(tile.ok());
    CHECK(canvas::tile_text(set, tile.value()) == "rock");
    CHECK(canvas::find_tile(set, "gras").error().message ==
          "the tile set has no tile named \"gras\"; did you mean \"grass\"?");
    CHECK(canvas::find_tile(set, "grond:1,1").error().message ==
          "the tile set has no source \"grond\"; did you mean \"ground\"?");
    CHECK(canvas::find_tile(set, "ground:9,9").error().message ==
          "the source \"ground\" has no tile at 9,9");
    CHECK(canvas::find_tile(set, "ground:1").error().message ==
          "\"ground:1\" is neither a tile name nor \"source:column,row\", as \"ground:2,0\"");
}

TEST_CASE("a layer's cells are rows of legend keys, and encode in canonical form") {
    const canvas::TileSet set = ground();
    auto cells = canvas::decode_cells(
        layer({{"g", "grass"}, {"w", "water"}, {"r", "ground:2,3"}}, {"gw", "", "..r"}, {-2, 5}),
        set);
    REQUIRE(cells.ok());
    REQUIRE(cells.value().size() == 3);
    CHECK(canvas::tile_text(set, cells.value().at({-2, 5})) == "grass");
    CHECK(canvas::tile_text(set, cells.value().at({-1, 5})) == "water");
    CHECK(canvas::tile_text(set, cells.value().at({0, 7})) == "rock");

    CHECK(decode_error(layer({{"g", "grass"}}, {"gx"})) ==
          "/rows/0: column 1: \"x\" is not in the legend");
    CHECK(decode_error(layer({{"gg", "grass"}}, {"gg", "ggg"})) ==
          "/rows/1: is 3 characters, not a whole number of 2-character keys");
    CHECK(decode_error(layer({{"g", "grass"}, {"ww", "water"}}, {})) ==
          "/legend/ww: keys are all the same length: \"ww\" is 2 characters, \"g\" 1");
    CHECK(decode_error(layer({{" ", "grass"}}, {})) ==
          "/legend/ : a key is one to three letters, digits or punctuation marks other than '.', "
          "'\"' and backslash");
    CHECK(decode_error(layer({{"g", "gras"}}, {"g"})) ==
          "/legend/g: the tile set has no tile named \"gras\"; did you mean \"grass\"?");
    CHECK(decode_error(layer({{"g", "grass"}}, {"g"}, {40000, 0})) ==
          "/origin: cells are from -32768 to 32767 in each direction");
    CHECK(decode_error(layer({{"g", "grass"}}, {"gg"}, {32767, 0})) ==
          "/rows/0: reaches past 32767, the last column or row cells can be in");
    // validate_cells lists every error, not just the first.
    const auto errors =
        canvas::validate_cells(layer({{"g", "gras"}, {"w", "water"}}, {"wx", "wq", "w."}), set);
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].path == "/legend/g");
    CHECK(errors[1].path == "/rows/0");
    CHECK(errors[2].path == "/rows/1");

    // Encoding: the origin is the top left of the cells, rows stop at their last tile, keys keep
    // standing for their tiles, and new keys go to the most used tiles first.
    world::TileMapLayer out = layer({{"W", "water"}, {"z", "grass"}}, {});
    canvas::Cells edit;
    for (const Vec2i cell : rect(3, -1, 3, 2)) {
        edit.emplace(cell, canvas::TileRef{.source = "ground", .atlas = {3, 0}}); // water
    }
    edit.insert_or_assign(Vec2i{5, 0}, canvas::TileRef{.source = "ground", .atlas = {2, 3}});
    edit.emplace(Vec2i{4, 2}, canvas::TileRef{.source = "ground", .atlas = {1, 0}});
    edit.emplace(Vec2i{5, 2}, canvas::TileRef{.source = "ground", .atlas = {1, 0}});
    canvas::encode_cells(edit, set, out);
    CHECK(out.origin == Vec2i{3, -1});
    CHECK(out.legend ==
          std::map<std::string, std::string>{{"W", "water"}, {"g", "grass_n"}, {"r", "rock"}});
    CHECK(out.rows == std::vector<std::string>{"WWW", "WWr", "", ".gg"});
    auto again = canvas::decode_cells(out, set);
    REQUIRE(again.ok());
    CHECK(again.value() == edit);
    canvas::encode_cells({}, set, out);
    CHECK(out.rows.empty());
    CHECK(out.legend.empty());
    CHECK(out.origin == Vec2i{});

    // A layer with more tiles than one-character keys can name uses two.
    std::string tiles;
    for (int i = 0; i < 100; ++i) {
        tiles += (i == 0 ? "" : ", ") + std::string("\"") + std::to_string(i) + ",0\": {}";
    }
    auto many = canvas::parse_tileset(parse(
        R"({"format": 1, "sources": {"s": {"texture": "s.png", "tiles": {)" + tiles + "}}}}"));
    REQUIRE(many.ok());
    canvas::Cells wide;
    for (int i = 0; i < 100; ++i) {
        wide.emplace(Vec2i{i, 0}, canvas::TileRef{.source = "s", .atlas = {i, 0}});
    }
    world::TileMapLayer big;
    canvas::encode_cells(wide, many.value(), big);
    CHECK(big.legend.size() == 100);
    CHECK(big.legend.at("aa") == "s:0,0");
    CHECK(big.legend.at("bi") == "s:99,0");
    CHECK(big.rows.at(0).size() == 200);
    auto back = canvas::decode_cells(big, many.value());
    REQUIRE(back.ok());
    CHECK(back.value() == wide);
}

TEST_CASE("painting terrain picks the tiles whose edges connect, as Godot does") {
    const canvas::TileSet set = ground();
    const int grass = set.terrain("grass");
    const int water = set.terrain("water");
    canvas::Cells cells;
    auto changed = canvas::paint_terrain(cells, set, rect(0, 0, 3, 3), grass);
    REQUIRE(changed.ok());
    CHECK(changed.value() == 9);
    CHECK(names(cells, set, 0, 0, 3, 3) == std::vector<std::string>{
                                               "grass_nw grass_n grass_ne",
                                               "grass_w grass grass_e",
                                               "grass_sw grass_s grass_se",
                                           });
    // The same paint again changes nothing.
    CHECK(canvas::paint_terrain(cells, set, rect(0, 0, 3, 3), grass).value() == 0);

    // Water around the block: the grass edges now face water, the same tiles.
    changed = canvas::paint_terrain(cells, set, rect(-1, -1, 5, 1), water);
    REQUIRE(changed.ok());
    CHECK(changed.value() == 5);
    CHECK(
        names(cells, set, -1, -1, 5, 2) ==
        std::vector<std::string>{"water water water water water", ". grass_nw grass_n grass_ne ."});

    // Growing the block to 4x4 turns the old east edge into middle tiles.
    canvas::Cells big;
    REQUIRE(canvas::paint_terrain(big, set, rect(0, 0, 4, 4), grass).ok());
    CHECK(names(big, set, 0, 0, 4, 4) == std::vector<std::string>{
                                             "grass_nw grass_n grass_n grass_ne",
                                             "grass_w grass grass grass_e",
                                             "grass_w grass grass grass_e",
                                             "grass_sw grass_s grass_s grass_se",
                                         });
    // Water in one corner cuts in diagonally: the cell across from it gets an inner corner.
    REQUIRE(canvas::paint_terrain(big, set, std::vector<Vec2i>{{0, 0}}, water).ok());
    CHECK(canvas::tile_text(set, big.at({0, 0})) == "water");
    CHECK(canvas::tile_text(set, big.at({1, 1})) == "grass_in_nw");
    // Erasing a column makes its neighbors edges, and leaves plain tiles alone.
    big.emplace(Vec2i{5, 5}, canvas::TileRef{.source = "ground", .atlas = {2, 3}}); // rock
    changed = canvas::paint_terrain(big, set, rect(3, 0, 1, 4), -1);
    REQUIRE(changed.ok());
    CHECK_FALSE(big.contains({3, 1}));
    CHECK(canvas::tile_text(set, big.at({2, 1})) == "grass_e");
    CHECK(canvas::tile_text(set, big.at({2, 2})) == "grass_e");
    CHECK(canvas::tile_text(set, big.at({5, 5})) == "rock");

    CHECK(canvas::paint_terrain(cells, set, rect(0, 0, 1, 1), 7).error().message ==
          "the tile set has no terrain 7");
}

TEST_CASE("equally good tiles are picked by probability, the same way every time") {
    std::string text(kGround);
    const std::string anchor = R"("2,3": {"name": "rock"})";
    text.replace(text.find(anchor), anchor.size(),
                 R"("2,3": {"name": "flowers", "terrain": "grass", "probability": 0.5},
                    "3,3": {"name": "stones", "terrain": "grass", "probability": 0})");
    auto parsed = canvas::parse_tileset(parse(text));
    REQUIRE(parsed.ok());
    const canvas::TileSet& set = parsed.value();
    canvas::Cells cells;
    REQUIRE(canvas::paint_terrain(cells, set, rect(0, 0, 12, 12), set.terrain("grass")).ok());
    std::map<std::string, int> counts;
    for (const auto& [cell, tile] : cells) {
        ++counts[canvas::tile_text(set, tile)];
    }
    // 100 middle cells: about two thirds plain grass, a third flowers, no stones.
    CHECK(counts["grass"] + counts["flowers"] == 100);
    CHECK(counts["grass"] > 50);
    CHECK(counts["flowers"] > 20);
    CHECK(counts["stones"] == 0);
    canvas::Cells again;
    REQUIRE(canvas::paint_terrain(again, set, rect(0, 0, 12, 12), set.terrain("grass")).ok());
    CHECK(again == cells);
}

TEST_CASE("TileMapLayer saves its cells as text and checks them against its tile set") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World world(components);
    const world::ComponentType* type = components.find("TileMapLayer");
    REQUIRE(type != nullptr);
    const entt::entity entity = world.create("map").value();
    std::string path;
    REQUIRE(type->assign(world, entity,
                         parse(R"({"tile_set": "ground.tileset.json", "origin": [-3, 2],
                                   "legend": {"g": "grass"}, "rows": ["gg"]})"),
                         path)
                .ok());
    CHECK(json::write(type->save(world, entity), json::Style::Compact) ==
          R"({"legend":{"g":"grass"},"origin":[-3,2],"rows":["gg"],)"
          R"("tile_set":"ground.tileset.json"})");
    // Vec2i fields take whole numbers only.
    CHECK_FALSE(type->assign(world, entity, parse(R"({"origin": [1.5, 2]})"), path).ok());

    const auto reader = [](std::string_view file) -> slash::Result<std::string> {
        if (file == "ground.tileset.json") {
            return std::string(kGround);
        }
        return slash::make_error(slash::ErrorCode::NotFound, "no such file");
    };
    const json::Value scene = parse(R"({"format": 1, "entities": {
        "ok": {"components": {"TileMapLayer": {"tile_set": "ground.tileset.json",
               "legend": {"g": "grass"}, "rows": ["g.g"]}}},
        "typo": {"components": {"TileMapLayer": {"tile_set": "ground.tileset.json",
                 "legend": {"g": "gras"}, "rows": ["gx"]}}},
        "gone": {"components": {"TileMapLayer": {"tile_set": "gone.tileset.json"}}},
        "no_set": {"components": {"TileMapLayer": {"rows": ["g"]}}},
        "prefab": {"extends": "map.prefab.json",
                   "components": {"TileMapLayer": {"rows": ["zz"]}}}}})");
    const auto errors = canvas::check_tile_layers(scene, reader);
    REQUIRE(errors.size() == 4);
    CHECK(errors[0].path == "/entities/gone/components/TileMapLayer/tile_set");
    CHECK(errors[0].message.find("gone.tileset.json") != std::string::npos);
    CHECK(errors[1].path == "/entities/no_set/components/TileMapLayer/tile_set");
    CHECK(errors[1].message == "a layer with cells needs a tile set");
    CHECK(errors[2].path == "/entities/typo/components/TileMapLayer/legend/g");
    CHECK(errors[3].path == "/entities/typo/components/TileMapLayer/rows/0");
    CHECK(errors[3].message == "column 1: \"x\" is not in the legend");
}

namespace {

using Rgba = std::array<std::uint8_t, 4>;

// A texture of size x size squares, one color each, in rows of columns.
std::string grid_png(const std::vector<std::vector<Rgba>>& rows, std::uint32_t size) {
    render::Image image;
    image.width = size * static_cast<std::uint32_t>(rows.front().size());
    image.height = size * static_cast<std::uint32_t>(rows.size());
    for (const std::vector<Rgba>& row : rows) {
        for (std::uint32_t y = 0; y < size; ++y) {
            for (const Rgba& color : row) {
                for (std::uint32_t x = 0; x < size; ++x) {
                    image.pixels.insert(image.pixels.end(), color.begin(), color.end());
                }
            }
        }
    }
    auto png = render::encode_png(image);
    REQUIRE(png.ok());
    return png.value();
}

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> edits;

    void write(const std::string& path, std::string text) {
        files[path] = std::move(text);
        ++edits[path];
    }

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = edits.find(path)->second,
                                        .size = it->second.size()};
            },
        };
    }
};

struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        // Two 8x8 tiles side by side: red, then blue; and a 4x4 green one in another texture.
        files.write("tiles.png", grid_png({{{255, 0, 0, 255}, {0, 0, 255, 255}}}, 8));
        files.write("small.png", grid_png({{{0, 255, 0, 255}}}, 4));
        files.write("map.tileset.json", R"({"format": 1, "tile_size": [8, 8], "sources": {
            "main": {"texture": "tiles.png", "tiles": {"0,0": {"name": "red"},
                                                       "1,0": {"name": "blue"},
                                                       "2,0": {"name": "past_the_edge"}}},
            "small": {"texture": "small.png", "region_size": [4, 4],
                      "tiles": {"0,0": {"name": "green"}}}}})");
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            REQUIRE(components.find(name)->assign(world, entity, fields, path).ok());
        }
        return entity;
    }
};

struct Warnings {
    std::vector<std::string> lines;
    Warnings() {
        slash::log::set_sink([this](slash::log::Level level, std::string_view line) {
            if (level == slash::log::Level::Warn) {
                lines.emplace_back(line);
            }
        });
    }
    ~Warnings() { slash::log::reset_sink(); }
    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;
    Warnings(Warnings&&) = delete;
    Warnings& operator=(Warnings&&) = delete;
};

} // namespace

TEST_CASE("the canvas draws tile map layers, one draw per run of a texture") {
    Scene scene;
    const Warnings warnings;
    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    scene.create("map", R"({"TileMapLayer": {"tile_set": "map.tileset.json",
        "legend": {"r": "red", "b": "blue", "g": "green", "x": "past_the_edge"},
        "rows": ["rbg", "xrr"]}})");
    scene.create("hidden", R"({"TileMapLayer": {"tile_set": "map.tileset.json",
        "legend": {"r": "red"}, "rows": ["r"], "visible": false}})");
    scene.create("broken", R"({"TileMapLayer": {"tile_set": "map.tileset.json",
        "legend": {"r": "red"}, "rows": ["q"]}})");
    scene.create("no_set", R"({"TileMapLayer": {"tile_set": "gone.tileset.json",
        "legend": {"r": "red"}, "rows": ["r"]}})");

    REQUIRE(renderer.draw(scene.world, {}).ok());
    CHECK(renderer.tiles_drawn() == 5);
    CHECK(renderer.sprites_drawn() == 0);
    auto target = device->create_texture(
        {.width = 32, .height = 32, .filter = render::Filter::Linear, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasOptions options;
    options.target = target.value();
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.tiles_drawn() == 5);
    // tiles.png, small.png, then tiles.png again: three runs, three draws.
    CHECK(device->stats().draws == 3);
    CHECK(device->stats().triangles == 10);
    const auto logged = [&](std::string_view text) {
        return std::ranges::any_of(warnings.lines, [&](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    };
    CHECK(logged(R"("error":"the tile main:2,0 at [16, 0, 8, 8] goes outside the 16x8 )"));
    CHECK(logged(R"("id":"broken","error":"TileMapLayer/rows/0: column 0: \"q\" is not )"));
    CHECK(logged(R"("tile_set":"gone.tileset.json")"));

    // Painting a cell is seen on the next frame.
    auto& map = scene.world.ecs().get<world::TileMapLayer>(scene.world.find("map"));
    map.rows = {"rrr"};
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.tiles_drawn() == 3);
}

namespace {

std::unique_ptr<render::RenderDevice> gpu_device() {
    auto device = render::create_device({.window = nullptr, .driver = "auto", .debug = false});
    if (!device) {
        if (!slash::platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no GPU device works: " << device.error().message);
        }
        MESSAGE("no GPU here, so tile pixels are not checked: " << device.error().message);
        return nullptr;
    }
    return std::move(device).value();
}

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("tiles draw centered on their cells, moved by the layer's transform, on a GPU") {
    auto device = gpu_device();
    if (device == nullptr) {
        return;
    }
    Scene scene;
    scene.create("map", R"({"Transform2D": {"position": [8, 0]},
        "TileMapLayer": {"tile_set": "map.tileset.json", "texture_filter": "nearest",
                         "origin": [-1, 1], "legend": {"r": "red", "b": "blue", "g": "green"},
                         "rows": ["rb", ".g"]}})");
    // A sprite above the layer covers one tile.
    scene.files.write("white.png", grid_png({{{255, 255, 255, 255}}}, 8));
    scene.create("marker", R"({"Transform2D": {"position": [12, 12]},
        "Sprite2D": {"texture": "white.png", "texture_filter": "nearest", "z_index": 1}})");
    auto target = device->create_texture(
        {.width = 32, .height = 32, .filter = render::Filter::Nearest, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasRenderer renderer(*device, scene.assets);
    canvas::CanvasOptions options;
    options.target = target.value();
    options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.tiles_drawn() == 3);
    auto image = device->read_texture(target.value());
    REQUIRE(image.ok());
    // Cell (-1, 1) spans x 0..8 after the transform, y 8..16: red, under the sprite.
    CHECK(pixel(image.value(), 3, 9) == Rgba{255, 0, 0, 255});
    CHECK(pixel(image.value(), 9, 9) == Rgba{255, 255, 255, 255}); // the sprite, over blue
    CHECK(pixel(image.value(), 17, 9) == Rgba{0, 0, 0, 255});      // past the blue tile
    // The 4x4 green tile sits centered in cell (0, 2): x 10..14, y 18..22.
    CHECK(pixel(image.value(), 12, 20) == Rgba{0, 255, 0, 255});
    CHECK(pixel(image.value(), 9, 20) == Rgba{0, 0, 0, 255});
    CHECK(pixel(image.value(), 12, 17) == Rgba{0, 0, 0, 255});
}

namespace {

// Two tiles that are solid in physics layer 0: a block, and a ledge 4 pixels thick that only
// stops what lands on it. The block's alternative 1 is a slope with its own polygon.
constexpr std::string_view kSolid = R"({
  "format": 1,
  "tile_size": [16, 16],
  "physics_layers": [{"collision_layer": 2, "collision_mask": 1, "friction": 0.5}],
  "sources": {
    "main": {"texture": "tiles.png", "region_size": [8, 8], "tiles": {
      "0,0": {"name": "block", "collision": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}],
              "alternatives": {"1": {"flip_h": true, "modulate": [0, 1, 1, 1],
                                     "collision": [{"points": [[-8, 8], [8, -8], [8, 8]]}]}}},
      "1,0": {"name": "ledge", "collision": [{"points": [[-8, -8], [8, -8], [8, -4], [-8, -4]],
                                              "one_way": true}]}}}}})";

canvas::TileSet solid() {
    auto set = canvas::parse_tileset(parse(kSolid));
    REQUIRE(set.ok());
    return std::move(set).value();
}

std::string points_text(const std::vector<Vec2>& points) {
    std::string out;
    for (const Vec2 point : points) {
        out += (out.empty() ? "" : " ") + std::to_string(static_cast<int>(point.x)) + "," +
               std::to_string(static_cast<int>(point.y));
    }
    return out;
}

} // namespace

TEST_CASE("tile sets give tiles collision polygons and alternatives, all checked") {
    CHECK(first_error(kSolid) == "(valid)");
    const auto set = solid();
    REQUIRE(set.physics_layers.size() == 1);
    CHECK(set.physics_layers[0].collision_layer == 2);
    CHECK(set.physics_layers[0].friction == doctest::Approx(0.5));
    CHECK(set.physics_layers[0].bounce == 0.0F);
    const canvas::TileData* block = set.find({.source = "main", .atlas = {0, 0}});
    REQUIRE(block != nullptr);
    REQUIRE(block->collision.size() == 1);
    CHECK(points_text(block->collision[0].points) == "-8,-8 8,-8 8,8 -8,8");
    REQUIRE(block->alternatives.contains(1));
    CHECK(block->alternatives.at(1).flip_h);
    CHECK(block->alternatives.at(1).collision.size() == 1);
    CHECK(set.find({.source = "main", .atlas = {1, 0}})->collision[0].one_way);

    const auto with = [](std::string_view from, std::string_view to) {
        std::string text(kSolid);
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    };
    CHECK(
        first_error(with(
            R"("physics_layers": [{"collision_layer": 2, "collision_mask": 1, "friction": 0.5}],)",
            "")) ==
        "/sources/main/tiles/0,0/collision/0/layer: the tile set has no physics layers; add one "
        "to physics_layers");
    CHECK(first_error(with(R"({"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]})",
                           R"({"layer": 1, "points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]})")) ==
          "/sources/main/tiles/0,0/collision/0/layer: the tile set has physics layers 0 to 0");
    CHECK(first_error(with("[[-8, -8], [8, -8], [8, 8], [-8, 8]]",
                           "[[-8, -8], [8, 8], [8, -8], [-8, 8]]")) ==
          "/sources/main/tiles/0,0/collision/0/points: the polygon has no area or crosses itself");
    CHECK(
        first_error(with(R"("alternatives": {"1":)", R"("alternatives": {"0":)")) ==
        R"(/sources/main/tiles/0,0/alternatives/0: alternatives are numbered from 1 to 4095, as "1")");
    CHECK(first_error(with(R"("flip_h": true)", R"("flip_x": true)"))
              .starts_with("/sources/main/tiles/0,0/alternatives/1"));
}

TEST_CASE("cells name a tile's alternative and turns after the tile") {
    const auto set = solid();
    const auto ref = [&](std::string_view text) {
        auto found = canvas::find_tile(set, text);
        REQUIRE_MESSAGE(found.ok(), found.error().message);
        return std::move(found).value();
    };
    const auto error = [&](std::string_view text) {
        auto found = canvas::find_tile(set, text);
        return found.ok() ? std::string("(found)") : found.error().message;
    };
    const canvas::TileRef flipped = ref("block:hv");
    CHECK(flipped.atlas == Vec2i{0, 0});
    CHECK(flipped.alternative == 0);
    CHECK(flipped.flip_h);
    CHECK(flipped.flip_v);
    CHECK_FALSE(flipped.transpose);
    const canvas::TileRef turned = ref("main:0,0:1:t");
    CHECK(turned.alternative == 1);
    CHECK(turned.transpose);
    CHECK_FALSE(turned.flip_h);
    CHECK(ref("block:0") == ref("block"));
    CHECK(canvas::tile_text(set, turned) == "block:1:t");
    CHECK(canvas::tile_text(set, ref("ledge:hvt")) == "ledge:hvt");
    CHECK(canvas::tile_text(set, ref("ledge")) == "ledge");

    CHECK(
        error("block:vh") ==
        R"("block:vh": a cell's turns are h, v and t, each at most once and in that order, as "crate:hv")");
    CHECK(error("block:2") == R"(the tile "block" has no alternative 2; it has 1 to 1)");
    CHECK(error("ledge:1") == R"(the tile "ledge" has no alternative 1; it has none)");
    CHECK(error("blok:h") == R"(the tile set has no tile named "blok"; did you mean "block"?)");
    CHECK(error("main:9,9:h") == R"(the source "main" has no tile at 9,9)");

    // Turned tiles are their own legend entries, keyed by the tile's name where it is free.
    world::TileMapLayer layer;
    layer.tile_set = "solid.tileset.json";
    canvas::Cells cells;
    cells[{0, 0}] = ref("block");
    cells[{1, 0}] = ref("block");
    cells[{2, 0}] = ref("block:h");
    cells[{3, 0}] = ref("block:1");
    canvas::encode_cells(cells, set, layer);
    CHECK(layer.legend ==
          std::map<std::string, std::string>{{"b", "block"}, {"B", "block:h"}, {"a", "block:1"}});
    CHECK(layer.rows == std::vector<std::string>{"bbBa"});
    auto decoded = canvas::decode_cells(layer, set);
    REQUIRE(decoded.ok());
    CHECK(decoded.value() == cells);
}

TEST_CASE("a tile's collision turns with its cell, as Godot's tile physics turns it") {
    const auto set = solid();
    const auto polygon = [&](std::string_view text) {
        auto found = canvas::find_tile(set, text);
        REQUIRE(found.ok());
        const auto polygons = canvas::tile_collision(set, found.value());
        REQUIRE(polygons.size() == 1);
        return points_text(polygons[0].points);
    };
    CHECK(polygon("ledge") == "-8,-8 8,-8 8,-4 -8,-4");
    CHECK(polygon("ledge:v") == "-8,8 8,8 8,4 -8,4");
    CHECK(polygon("ledge:h") == "8,-8 -8,-8 -8,-4 8,-4");
    // Transposed, then flipped: the ledge stands along the tile's left side, then its right.
    CHECK(polygon("ledge:t") == "-8,-8 -8,8 -4,8 -4,-8");
    CHECK(polygon("ledge:ht") == "8,-8 8,8 4,8 4,-8");
    // The alternative has its own polygon, which its own flip_h doesn't flip, as in Godot.
    CHECK(polygon("block:1") == "-8,8 8,-8 8,8");
    CHECK(polygon("block:1:h") == "8,8 -8,-8 -8,8");

    canvas::Cells cells;
    cells[{2, 1}] = canvas::find_tile(set, "block").value();
    cells[{3, 1}] = canvas::find_tile(set, "ledge").value();
    const auto shapes = canvas::tile_shapes(set, cells);
    REQUIRE(shapes.size() == 2);
    CHECK(shapes[0].cell == Vec2i{2, 1});
    CHECK(points_text(shapes[0].points) == "32,16 48,16 48,32 32,32");
    CHECK(shapes[0].collision_layer == 2);
    CHECK(shapes[0].collision_mask == 1);
    CHECK(shapes[0].friction == doctest::Approx(0.5));
    CHECK_FALSE(shapes[0].one_way);
    CHECK(shapes[1].one_way);
    CHECK(points_text(shapes[1].points) == "48,16 64,16 64,20 48,20");
}

TEST_CASE("turned and alternative tiles draw as Godot's draw_tile draws them") {
    // An 8x8 tile in four colors: red top left, green top right, blue bottom left, white
    // bottom right. Each cell below is 8x8 too, so each quarter is a 4x4 square.
    const Rgba red{255, 0, 0, 255};
    const Rgba green{0, 255, 0, 255};
    const Rgba blue{0, 0, 255, 255};
    const Rgba white{255, 255, 255, 255};
    const Rgba black{0, 0, 0, 255};
    std::vector<std::unique_ptr<render::RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    for (const auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        Scene scene;
        scene.files.write("quarters.png", grid_png({{red, green}, {blue, white}}, 4));
        scene.files.write("quarters.tileset.json", R"({"format": 1, "tile_size": [8, 8],
            "sources": {"q": {"texture": "quarters.png", "tiles": {"0,0": {"name": "q",
                "alternatives": {"1": {"flip_h": true, "modulate": [0, 1, 1, 1]}}}}}}})");
        scene.create("map", R"({"TileMapLayer": {"tile_set": "quarters.tileset.json",
            "texture_filter": "nearest",
            "legend": {"a": "q", "b": "q:h", "c": "q:v", "d": "q:t", "e": "q:ht", "f": "q:1",
                       "g": "q:1:h"},
            "rows": ["abcd", "efg"]}})");
        auto target = device->create_texture(
            {.width = 32, .height = 16, .filter = render::Filter::Nearest, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasRenderer renderer(*device, scene.assets);
        canvas::CanvasOptions options;
        options.target = target.value();
        options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.tiles_drawn() == 7);
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        // The four quarters of the cell at column, row: top left, top right, bottom left,
        // bottom right.
        const auto quarters = [&](std::uint32_t column, std::uint32_t row) {
            const std::uint32_t x = column * 8;
            const std::uint32_t y = row * 8;
            return std::array<Rgba, 4>{
                pixel(image.value(), x + 2, y + 2), pixel(image.value(), x + 6, y + 2),
                pixel(image.value(), x + 2, y + 6), pixel(image.value(), x + 6, y + 6)};
        };
        using Quarters = std::array<Rgba, 4>;
        CHECK(quarters(0, 0) == Quarters{red, green, blue, white});
        CHECK(quarters(1, 0) == Quarters{green, red, white, blue}); // flipped horizontally
        CHECK(quarters(2, 0) == Quarters{blue, white, red, green}); // flipped vertically
        CHECK(quarters(3, 0) == Quarters{red, blue, green, white}); // transposed
        CHECK(quarters(0, 1) == Quarters{blue, red, white, green}); // turned right
        // The alternative flips and tints out red; the cell's flip undoes its flip.
        const Rgba cyan{0, 255, 255, 255};
        CHECK(quarters(1, 1) == Quarters{green, black, cyan, blue});
        CHECK(quarters(2, 1) == Quarters{black, green, blue, cyan});
    }
}

TEST_CASE("tile map layers are solid where their tiles have collision polygons") {
    Scene scene;
    slash::physics2d::install(scene.world);
    scene.files.write("solid.tileset.json", std::string(kSolid));
    // Five blocks from x 0 to 80, y 32 to 48, and a ledge row above them on the right.
    const entt::entity ground = scene.create("ground", R"({"TileMapLayer": {
        "tile_set": "solid.tileset.json", "origin": [0, 2], "legend": {"b": "block"},
        "rows": ["bbbbb"]}})");
    scene.create("ledges", R"({"TileMapLayer": {"tile_set": "solid.tileset.json",
        "origin": [6, 1], "legend": {"l": "ledge"}, "rows": ["ll"]}})");
    scene.create("ball", R"({"Transform2D": {"position": [40, 0]}, "RigidBody2D": {},
        "CollisionShape2D": {"shape": "circle", "radius": 4}})");
    scene.create("riser", R"({"Transform2D": {"position": [112, 40]},
        "RigidBody2D": {"gravity_scale": 0, "linear_velocity": [0, -300]},
        "CollisionShape2D": {"shape": "circle", "radius": 4}})");
    scene.create("lander", R"({"Transform2D": {"position": [104, 0]}, "RigidBody2D": {},
        "CollisionShape2D": {"shape": "circle", "radius": 4}})");
    const auto y_of = [&](std::string_view id) {
        return scene.world.ecs().get<world::Transform2D>(scene.world.find(id)).position.y;
    };
    for (int tick = 0; tick < 120; ++tick) {
        scene.world.step(1.0 / 60.0);
    }
    // The ball rests on the blocks, the lander on a ledge (16 to 20); the riser came up
    // through the one-way ledge.
    CHECK(y_of("ball") == doctest::Approx(28.0).epsilon(0.02));
    CHECK(y_of("lander") == doctest::Approx(12.0).epsilon(0.05));
    CHECK(y_of("riser") < 0.0F);

    // A ray finds the layer and the cell it hit.
    auto hit = slash::physics2d::raycast(scene.world, {8, 0}, {8, 100});
    REQUIRE(hit.has_value());
    CHECK(scene.world.id_of(hit->entity) == "ground");
    CHECK(hit->point.y == doctest::Approx(32.0));
    REQUIRE(hit->cell.has_value());
    CHECK(*hit->cell == Vec2i{0, 2});

    // Debug drawing fills each tile's polygon.
    CHECK(canvas::collision_shapes(scene.world).tiles == 7);
    CHECK(canvas::collision_shapes(scene.world).shapes == 3);

    // Moving the layer moves its body; painting cells rebuilds it.
    auto& map = scene.world.ecs().get<world::TileMapLayer>(ground);
    scene.world.ecs().emplace<world::Transform2D>(ground).position = {0, 16};
    hit = slash::physics2d::raycast(scene.world, {8, 0}, {8, 100});
    REQUIRE(hit.has_value());
    CHECK(hit->point.y == doctest::Approx(48.0));
    map.rows = {"b...b"};
    hit = slash::physics2d::raycast(scene.world, {24, 0}, {24, 100});
    CHECK_FALSE(hit.has_value());
    CHECK(canvas::collision_shapes(scene.world).tiles == 4);

    // A layer with collision off isn't solid, and the ball falls.
    map.rows = {"bbbbb"};
    map.collision_enabled = false;
    for (int tick = 0; tick < 30; ++tick) {
        scene.world.step(1.0 / 60.0);
    }
    CHECK(y_of("ball") > 60.0F);
    CHECK(canvas::collision_shapes(scene.world).tiles == 2);

    // Editing the tile set rebuilds the shapes: the ledge becomes solid from below too.
    std::string edited(kSolid);
    edited.replace(edited.find(R"("one_way": true)"), 15, R"("one_way": false)");
    scene.files.write("solid.tileset.json", edited);
    CHECK(scene.assets.poll().size() == 1);
    auto shapes = slash::physics2d::tile_shapes(scene.world, scene.world.find("ledges"));
    REQUIRE(shapes != nullptr);
    CHECK_FALSE(shapes->front().one_way);
}
