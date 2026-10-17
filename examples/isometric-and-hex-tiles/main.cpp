// Isometric and hexagonal tile maps, as Godot's TileSet shapes and layouts.
//
// iso.tileset.json cuts iso.png into 32x16 diamonds in a diamond layout: columns run down to the
// right and rows down to the left, as in most isometric games. Its grass tiles' peering names
// the sides (ne, se, sw, nw) and corners (n, e, s, w) of a diamond that touch water.
// hex.tileset.json cuts hex.png into 32x28 hexagons in offset rows, the board of a strategy game.
//
// This program draws the atlases from the tile sets, paints an island with a pond on the
// diamonds, sets tiles on the hexagons, asks tilemap.convert where cells are and which cell a
// point is in, and asks frame.capture for the colors there. Games ship PNG files made in an image
// editor; this example draws its textures in code, so the repository holds no images.

#include "slash/canvas/tilemap.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

using Rgba = std::array<std::uint8_t, 4>;

constexpr Rgba kGrass{76, 175, 80, 255};
constexpr Rgba kWater{33, 150, 243, 255};
constexpr Rgba kRock{120, 120, 120, 255};
constexpr Rgba kForest{27, 94, 32, 255};
constexpr Rgba kHill{141, 110, 99, 255};
constexpr Rgba kClear{0, 0, 0, 0};

void write_png(const std::string& path, const render::Image& image) {
    if (!slash::platform::write_file(path, render::encode_png(image).value()).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
}

void put(render::Image& image, int x, int y, Rgba color) {
    const std::size_t at =
        ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
    std::copy(color.begin(), color.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(at));
}

// Each diamond split into a 3x3 grid along its sides: its middle in the tile's terrain, and each
// side and corner in the terrain its peering asks for there. Clear outside the diamond.
void write_diamonds(const canvas::TileSet& set) {
    const canvas::AtlasSource& source = set.sources.at("ground");
    render::Image image{.width = 128, .height = 64, .pixels = {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    // The direction (a canvas::Neighbor) for each part of the grid, by row along the sw side
    // and column along the se side; -1 is the middle.
    constexpr std::array<int, 9> kParts = {0, 2, 4, 14, -1, 6, 12, 10, 8};
    for (const auto& [atlas, tile] : source.tiles) {
        const auto [left, top, width, height] = canvas::tile_region(source, atlas);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float u = ((static_cast<float>(x) + 0.5F) / static_cast<float>(width)) - 0.5F;
                const float v =
                    ((static_cast<float>(y) + 0.5F) / static_cast<float>(height)) - 0.5F;
                Rgba color = kClear;
                if (std::abs(u) + std::abs(v) <= 0.5F) {
                    const int column = std::clamp(static_cast<int>((u + v + 0.5F) * 3.0F), 0, 2);
                    const int row = std::clamp(static_cast<int>((v - u + 0.5F) * 3.0F), 0, 2);
                    const int part = kParts.at((static_cast<std::size_t>(row) * 3) +
                                               static_cast<std::size_t>(column));
                    const int terrain =
                        part < 0 ? tile.terrain : tile.peering.at(static_cast<std::size_t>(part));
                    color = terrain == set.terrain("water") ? kWater : kGrass;
                    if (tile.terrain < 0) {
                        color = kRock;
                    }
                }
                put(image, left + x, top + y, color);
            }
        }
    }
    write_png(source.texture, image);
    std::printf("wrote %s: %ux%u, %zu tiles\n", source.texture.c_str(), image.width, image.height,
                source.tiles.size());
}

// Each hexagon pointing up and down, as Godot draws hexagons in offset rows, edged a little
// darker. Clear outside it.
void write_hexagons(const canvas::TileSet& set) {
    const canvas::AtlasSource& source = set.sources.at("board");
    render::Image image{.width = 128, .height = 28, .pixels = {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    constexpr std::array<Rgba, 4> kColors{kGrass, kForest, kWater, kHill};
    for (const auto& [atlas, tile] : source.tiles) {
        const auto [left, top, width, height] = canvas::tile_region(source, atlas);
        const Rgba fill = kColors.at(static_cast<std::size_t>(atlas.x));
        const Rgba edge{static_cast<std::uint8_t>(fill[0] * 7 / 10),
                        static_cast<std::uint8_t>(fill[1] * 7 / 10),
                        static_cast<std::uint8_t>(fill[2] * 7 / 10), 255};
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float u =
                    std::abs(((static_cast<float>(x) + 0.5F) / static_cast<float>(width)) - 0.5F);
                const float v =
                    std::abs(((static_cast<float>(y) + 0.5F) / static_cast<float>(height)) - 0.5F);
                // How far inside the slanted and the upright edges, in pixels.
                const float slanted = (0.5F - (0.5F * u) - v) * static_cast<float>(height);
                const float upright = (0.5F - u) * static_cast<float>(width);
                Rgba color = kClear;
                if (slanted >= 0.0F) {
                    color = std::min(slanted, upright) < 1.5F ? edge : fill;
                }
                put(image, left + x, top + y, color);
            }
        }
    }
    write_png(source.texture, image);
    std::printf("wrote %s: %ux%u, %zu tiles\n", source.texture.c_str(), image.width, image.height,
                source.tiles.size());
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Agent() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    // Prints the call as an agent would make it, then the reply.
    [[nodiscard]] json::Value call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Compact).c_str());
        return out;
    }

    // Calls tilemap.get and prints its rows, one per line.
    void print_rows(std::string_view id) const {
        const json::Value map = call("tilemap.get", R"({"id": ")" + std::string(id) + R"("})");
        for (const json::Value& row : map.find("rows")->as_array()) {
            std::printf("  %s\n", row.as_string().c_str());
        }
    }
};

} // namespace

int main() {
    const Agent agent;
    write_diamonds(*agent.session->assets().load<canvas::TileSet>("iso.tileset.json").value());
    write_hexagons(*agent.session->assets().load<canvas::TileSet>("hex.tileset.json").value());
    (void)agent.call("scene.open", R"({"path": "main.scene.json"})");

    std::puts("-- water, an island and a pond: each diamond's sides and corners match its "
              "neighbors'");
    (void)agent.call("tilemap.paint",
                     R"({"id": "island", "rect": [0, 0, 7, 7], "terrain": "water"})");
    (void)agent.call("tilemap.paint",
                     R"({"id": "island", "rect": [1, 1, 5, 5], "terrain": "grass"})");
    (void)agent.call("tilemap.paint", R"({"id": "island", "cells": [[3, 3]], "terrain": "water"})");
    (void)agent.call("tilemap.set", R"({"id": "island", "cells": [[2, 4]], "tile": "rock"})");
    agent.print_rows("island");

    std::puts("-- the pond's center and the island's top; the pond's box holds the corners of "
              "the diamonds around it");
    (void)agent.call("tilemap.convert", R"({"id": "island", "cells": [[3, 3], [1, 1]]})");
    (void)agent.call("tilemap.convert", R"({"id": "island", "points": [[160, 64], [147, 58]]})");

    std::puts("-- a board of hexagons in offset rows: odd rows sit half a hexagon over");
    (void)agent.call("tilemap.set", R"({"id": "board", "rect": [0, 0, 8, 3], "tile": "grass"})");
    (void)agent.call("tilemap.set",
                     R"({"id": "board", "cells": [[2, 0], [2, 1], [3, 1]], "tile": "forest"})");
    (void)agent.call("tilemap.set",
                     R"({"id": "board", "cells": [[5, 1], [6, 1], [5, 2]], "tile": "water"})");
    (void)agent.call("tilemap.set", R"({"id": "board", "cells": [[1, 2]], "tile": "hill"})");
    agent.print_rows("board");
    (void)agent.call("tilemap.convert", R"({"id": "board", "cells": [[3, 1], [1, 2]]})");
    std::puts("-- clicks either side of the slanted edge between two hexagons");
    (void)agent.call("tilemap.convert", R"({"id": "board", "points": [[136, 149], [136, 156]]})");

    std::puts("-- the frame at those places: pond, island, forest, hill, and grass and forest "
              "either side of the edge");
    (void)agent.call(
        "frame.capture",
        R"({"points": [[160, 64], [160, 32], [144, 163], [64, 184], [136, 149], [136, 156]]})");
    return 0;
}
