// Tile maps, as Godot's TileSet and TileMapLayer: ground.tileset.json cuts ground.png into 16x16
// tiles and names the terrains they connect, and the TileMapLayer in main.scene.json holds its
// cells as rows of legend keys.
//
// This program draws the atlas from the tile set's own peering, paints an island with a pond as
// an agent would, reads the cells back, then draws the layer into a texture.

#include "slash/canvas/canvas.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr Rgba kFlower{255, 235, 59, 255};

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// The atlas: each tile split into a 3x3 grid, its middle in the tile's terrain and each side and
// corner in the terrain its peering asks for there. Games ship PNG files made in an image editor
// or by a generator; this one is made here so the example has no binary file.
void write_atlas(const canvas::TileSet& set) {
    const canvas::AtlasSource& source = set.sources.at("ground");
    render::Image image{.width = 64, .height = 64, .pixels = {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    // The Neighbor for each part of the 3x3 grid; -1 is the middle.
    constexpr std::array<int, 9> kParts = {7, 0, 1, 6, -1, 2, 5, 4, 3};
    for (const auto& [atlas, tile] : source.tiles) {
        const auto [left, top, width, height] = canvas::tile_region(source, atlas);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int grid = ((y * 3 / height) * 3) + (x * 3 / width);
                const int part = kParts.at(static_cast<std::size_t>(grid));
                const int terrain =
                    part < 0 ? tile.terrain : tile.peering.at(static_cast<std::size_t>(part));
                Rgba color = terrain == set.terrain("water") ? kWater : kGrass;
                if (tile.terrain < 0) {
                    color = kRock;
                } else if (tile.name == "flowers" && x % 6 == 2 && y % 6 == 2) {
                    color = kFlower;
                }
                const int pixel = ((top + y) * static_cast<int>(image.width)) + left + x;
                const std::size_t at = static_cast<std::size_t>(pixel) * 4;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    image.pixels[at + channel] = color.at(channel);
                }
            }
        }
    }
    check(slash::platform::write_file(source.texture, render::encode_png(image).value()));
    std::printf("wrote %s: %ux%u, %zu tiles\n", source.texture.c_str(), image.width, image.height,
                source.tiles.size());
}

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.value;
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    write_atlas(*session.assets().load<canvas::TileSet>("ground.tileset.json").value());
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Water everywhere, then an island of grass: tilemap.paint picks each grass tile whose
    // edges match its neighbors, so the shore draws itself.
    call(session, "tilemap.paint",
         R"({"id": "ground", "rect": [0, 0, 12, 8], "terrain": "water"})");
    call(session, "tilemap.paint", R"({"id": "ground", "rect": [2, 1, 8, 6], "terrain": "grass"})");
    // A pond: its neighbors turn to face it, inner corners at the diagonals.
    call(session, "tilemap.paint", R"({"id": "ground", "cells": [[4, 3]], "terrain": "water"})");
    // tilemap.set places one tile by name, or as "source:column,row".
    call(session, "tilemap.set", R"({"id": "ground", "cells": [[8, 2]], "tile": "rock"})");

    // The layer's cells: rows of legend keys from the origin, "." for an empty cell.
    const json::Value map = call(session, "tilemap.get", R"({"id": "ground"})");
    for (const json::Value& row : map.find("rows")->as_array()) {
        std::printf("  %s\n", row.as_string().c_str());
    }
    // A window around the pond, in the same keys.
    call(session, "tilemap.get", R"({"id": "ground", "rect": [3, 2, 3, 3]})");

    // Misspelled names are refused with the closest match.
    call(session, "tilemap.paint", R"({"id": "ground", "cells": [[0, 0]], "terrain": "watr"})");
    call(session, "tilemap.set", R"({"id": "ground", "cells": [[0, 0]], "tile": "grass_nn"})");

    // Draw into a 192x128 texture: one GPU draw, since every tile is in one atlas.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 192,
                                                           .height = 128,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(session.world(), {.target = target,
                                          .clear = render::kDefaultClearColor,
                                          .width = 0.0F,
                                          .height = 0.0F,
                                          .interpolation = 1.0,
                                          .texture_filter = render::Filter::Linear,
                                          .time = 0.0}));
    const render::Stats stats = device->stats();
    std::printf("tiles drawn: %zu, GPU draws: %llu, triangles: %llu\n", renderer.tiles_drawn(),
                static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.triangles));
    return 0;
}
