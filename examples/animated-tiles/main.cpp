// Animated tiles and a tile larger than one cell, as Godot's TileSetAtlasSource.
//
// pond.tileset.json cuts pond.png into 16x16 cells. Its water tile plays three frames that follow
// it along the row, 0.4 seconds each. Its torch plays four frames that wrap after two columns,
// and starts each cell at a random point of its loop, so torches side by side don't flicker
// together. Its bridge is two cells wide (size_in_atlas [2, 1]); its texture_origin [-8, 0] moves
// it half a cell right, so it covers its own cell and the one after instead of half of each
// neighbor.
//
// This program draws the atlas from the tile set, fills a pond with water, lays the bridge and
// torches on a layer above it, and asks frame.capture for the colors at the start, after half a
// second of ticks and at one second. Games ship PNG files made in an image editor; this example
// draws its textures in code, so the repository holds no images.

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

// The water's three frames, the torch's four and the rest.
constexpr std::array<Rgba, 3> kWater{
    {{25, 118, 210, 255}, {66, 165, 245, 255}, {144, 202, 249, 255}}};
constexpr std::array<Rgba, 4> kFlame{
    {{255, 235, 59, 255}, {255, 152, 0, 255}, {244, 67, 54, 255}, {255, 87, 34, 255}}};
constexpr Rgba kPlank{161, 136, 127, 255};
constexpr Rgba kGap{93, 64, 55, 255};
constexpr Rgba kStone{120, 120, 120, 255};

void fill(render::Image& image, const std::array<int, 4>& region, Rgba color) {
    const auto [left, top, width, height] = region;
    for (int y = top; y < top + height; ++y) {
        for (int x = left; x < left + width; ++x) {
            const std::size_t at =
                ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
            std::copy(color.begin(), color.end(),
                      image.pixels.begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
}

// Each tile's frames, at the regions the tile set gives them: Godot lays an animated tile's
// frames out after it.
void write_atlas(const canvas::TileSet& set) {
    const canvas::AtlasSource& source = set.sources.at("pond");
    render::Image image{.width = 80, .height = 48, .pixels = {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    for (const auto& [atlas, tile] : source.tiles) {
        const auto frames = static_cast<int>(tile.animation_frames.size());
        for (int frame = 0; frame < frames; ++frame) {
            const std::array<int, 4> region = canvas::tile_region(source, atlas, frame);
            if (tile.name == "water") {
                fill(image, region, kWater.at(static_cast<std::size_t>(frame)));
            } else if (tile.name == "torch") {
                fill(image, region, kFlame.at(static_cast<std::size_t>(frame)));
            } else if (tile.name == "bridge") {
                // Planks across it, with a gap after every eighth pixel.
                fill(image, region, kPlank);
                for (int x = region[0] + 7; x < region[0] + region[2]; x += 8) {
                    fill(image, {x, region[1], 1, region[3]}, kGap);
                }
            } else {
                fill(image, region, kStone);
            }
        }
    }
    if (!slash::platform::write_file(source.texture, render::encode_png(image).value()).ok()) {
        std::fprintf(stderr, "can't write %s\n", source.texture.c_str());
        std::exit(1);
    }
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
};

// The water, the water left of the bridge, the bridge's two cells and the three torches.
constexpr std::string_view kPoints =
    R"("points": [[8, 8], [28, 24], [40, 24], [56, 24], [24, 56], [56, 56], [72, 56]])";

} // namespace

int main() {
    const Agent agent;
    write_atlas(*agent.session->assets().load<canvas::TileSet>("pond.tileset.json").value());
    (void)agent.call("scene.open", R"({"path": "main.scene.json"})");

    std::puts("-- a pond of water, and on the layer above it a bridge across two cells, torches "
              "and stones");
    (void)agent.call("tilemap.set", R"({"id": "pond", "rect": [0, 0, 8, 3], "tile": "water"})");
    (void)agent.call("tilemap.set", R"({"id": "deck", "cells": [[2, 1]], "tile": "bridge"})");
    (void)agent.call("tilemap.set",
                     R"({"id": "deck", "cells": [[1, 3], [3, 3], [4, 3]], "tile": "torch"})");
    (void)agent.call("tilemap.set",
                     R"({"id": "deck", "cells": [[0, 3], [7, 3]], "tile": "stone"})");
    std::puts("-- the bridge is one cell of the layer; it draws over the next one too");
    (void)agent.call("tilemap.get", R"({"id": "deck"})");

    std::puts("-- at the start: the water's first frame, also just left of the bridge, the "
              "bridge's planks over its cell and the next, and each torch at its own point of the "
              "loop");
    (void)agent.call("frame.capture", "{" + std::string(kPoints) + "}");
    std::puts("-- half a second of ticks later the water shows its second frame");
    (void)agent.call("step", R"({"ticks": 30})");
    (void)agent.call("frame.capture", "{" + std::string(kPoints) + "}");
    std::puts("-- frame.capture can draw any moment: at one second, the third frame");
    (void)agent.call("frame.capture", R"({"time": 1.0, )" + std::string(kPoints) + "}");
    return 0;
}
