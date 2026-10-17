// Importing Unity's isometric and hexagonal tile maps: project.import keeps each cell where
// Unity draws it.
//
// unity/ holds "Hex Garden", a Unity 2D project as Unity saves it: a scene with an isometric
// tile map of blocks, a garden with a pond, and a hexagonal tile map, a board of grass and sand,
// with their sprites' import settings and tile assets. Isometric sprites stand on their bottom
// middle and Unity's isometric tile maps anchor them at each cell's bottom corner; hexagons are
// centered on their cells.
//
// Each Grid becomes a tile set of Godot's shape: Unity's isometric cells are an isometric tile
// set in the diamond_right layout and its hexagons a hexagon tile set in stacked_offset rows,
// Unity's cell (x, y) being (x, -y - 1). Each layer moves by a fixed offset so its cells' centers
// are where Unity's are, and each tile's texture_origin puts its sprite's pivot at the tile
// anchor. Games ship PNG files made in other tools; this example draws its textures in code, so
// the repository holds no images.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
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

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

using Rgba = std::array<std::uint8_t, 4>;

constexpr Rgba kGrass{76, 175, 80, 255};
constexpr Rgba kWater{33, 150, 243, 255};
constexpr Rgba kSand{230, 200, 120, 255};
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

Rgba shade(Rgba color, int tenths) {
    return {static_cast<std::uint8_t>(color[0] * tenths / 10),
            static_cast<std::uint8_t>(color[1] * tenths / 10),
            static_cast<std::uint8_t>(color[2] * tenths / 10), 255};
}

// Two 32x32 blocks side by side, grass and water: a diamond top 16 pixels high over its two
// sides, the left darker than the right, standing on the bottom middle of the sprite.
void write_blocks(const std::string& path) {
    render::Image image{.width = 64, .height = 32, .pixels = {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    for (int block = 0; block < 2; ++block) {
        const Rgba top = block == 0 ? kGrass : kWater;
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                const float u = std::abs(static_cast<float>(x) + 0.5F - 16.0F) / 16.0F;
                const float v = static_cast<float>(y) + 0.5F;
                Rgba color = kClear;
                if (u + (std::abs(v - 8.0F) / 8.0F) <= 1.0F) {
                    color = top;
                } else if (v >= 8.0F && v <= 32.0F - (8.0F * u)) {
                    color = shade(top, x < 16 ? 5 : 7);
                }
                put(image, (block * 32) + x, y, color);
            }
        }
    }
    write_png(path, image);
}

// Two hexagons 32 pixels wide and 36 high side by side, grass and sand, pointing up.
void write_hexes(const std::string& path) {
    render::Image image{.width = 64, .height = 36, .pixels = {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    for (int hex = 0; hex < 2; ++hex) {
        for (int y = 0; y < 36; ++y) {
            for (int x = 0; x < 32; ++x) {
                const float u = std::abs(static_cast<float>(x) + 0.5F - 16.0F) / 16.0F;
                const float v = std::abs(static_cast<float>(y) + 0.5F - 18.0F) / 18.0F;
                Rgba color = kClear;
                if (u <= 1.0F && v <= 1.0F - (0.5F * u)) {
                    color = hex == 0 ? kGrass : kSand;
                }
                put(image, (hex * 32) + x, y, color);
            }
        }
    }
    write_png(path, image);
}

void print_file(const std::string& path) {
    std::printf("--- %s\n%s", path.c_str(), slash::platform::read_file(path).value().c_str());
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Agent() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        slash::importer::register_import_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    // Prints the call as an agent would make it, then the reply.
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        const json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Compact).c_str());
    }
};

} // namespace

int main() {
    write_blocks("unity/Assets/Sprites/blocks.png");
    write_hexes("unity/Assets/Sprites/hexes.png");

    const Agent agent;
    agent.call("project.import", R"({"source": "unity"})");

    // The garden's tile set: isometric, its blocks drawn 8 pixels up so their bottom middles are
    // on the cells' bottom corners. The board's: hexagons in stacked_offset rows.
    print_file("unity/Assets/tiles.tileset.json");
    print_file("unity/Assets/tiles_2.tileset.json");

    // The layers: Unity's cell (x, y) is (x, -y - 1), the garden moved up half a cell and the
    // board left half a cell and down a quarter, which puts their cells where Unity has them.
    agent.call("scene.open", R"({"path": "unity/Assets/Scenes/Garden.scene.json"})");
    agent.call("entity.get", R"({"id": "garden", "components": ["Transform2D", "TileMapLayer"]})");
    agent.call("entity.get", R"({"id": "board", "components": ["Transform2D", "TileMapLayer"]})");

    // The pond, Unity's cell (2, 1), and the sand, Unity's (1, 0): their centers in the world.
    agent.call("tilemap.convert", R"({"id": "garden", "cells": [[2, -2]]})");
    agent.call("tilemap.convert", R"({"id": "board", "cells": [[1, -1]]})");

    // The camera, at the world's origin, puts it in the middle of the 320x180 window. The frame
    // there: the pond's top 16 pixels above its center, then the front block's grass top and its
    // two sides, the sand hexagon, and the clear color between the two maps.
    agent.call("frame.capture",
               R"({"points": [[96, 74], [80, 98], [72, 114], [88, 114], [256, 74], [160, 90]]})");
    return 0;
}
