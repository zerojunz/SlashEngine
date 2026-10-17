// Custom data and occluders on tiles, as Godot's TileSet custom data and occlusion layers.
//
// dungeon.tileset.json has one occlusion layer and three custom data layers: damage (int), kind
// (string) and speed (float). Its wall tile has an occluder over the whole cell and its pillar a
// small octagon, so both cast shadows from lights with shadow_enabled. Its lava tile does 10
// damage and halves the speed; a tile that sets no value in a layer has the type's default.
//
// This program draws the atlas, lays a floor with lava in it, a wall and a pillar on a layer
// above it, and asks what an agent would ask: what is under a few cells, how lit points beside
// and behind the wall and the pillar are, and what color they draw. Then it turns the wall
// layer's occlusion off and asks again. Games ship PNG files made in an image editor; this
// example draws its texture in code, so the repository holds no images.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
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

// Four 16x16 tiles in a row: floor, wall, lava and pillar.
void write_atlas() {
    constexpr std::array<Rgba, 4> kColors{
        {{110, 110, 120, 255}, {120, 80, 50, 255}, {255, 120, 0, 255}, {170, 170, 170, 255}}};
    render::Image image{.width = 64, .height = 16, .pixels = {}};
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const Rgba& color = kColors.at(x / 16);
            image.pixels.insert(image.pixels.end(), color.begin(), color.end());
        }
    }
    if (!slash::platform::write_file("dungeon.png", render::encode_png(image).value()).ok()) {
        std::fprintf(stderr, "can't write dungeon.png\n");
        std::exit(1);
    }
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
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        const json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Compact).c_str());
    }
};

// Open floor near the torch, behind the wall, past the wall's end and behind the pillar.
constexpr std::string_view kPoints = R"({"points": [[40, 8], [100, 16], [100, 56], [56, 56]]})";

} // namespace

int main() {
    write_atlas();
    const Agent agent;
    agent.call("scene.open", R"({"path": "main.scene.json"})");

    std::puts("-- a stone floor with two cells of lava, and above it a wall two cells high and a "
              "pillar");
    agent.call("tilemap.set", R"({"id": "ground", "rect": [0, 0, 8, 4], "tile": "floor"})");
    agent.call("tilemap.set", R"({"id": "ground", "cells": [[2, 3], [3, 3]], "tile": "lava"})");
    agent.call("tilemap.set", R"({"id": "walls", "cells": [[4, 0], [4, 1]], "tile": "wall"})");
    agent.call("tilemap.set", R"({"id": "walls", "cells": [[2, 2]], "tile": "pillar"})");

    std::puts("-- what is under these cells: each layer's value, or its type's default");
    agent.call("tilemap.get", R"({"id": "ground", "cells": [[2, 3], [5, 3]]})");
    agent.call("tilemap.get", R"({"id": "walls", "cells": [[4, 0], [5, 0]]})");

    std::puts("-- the torch lights the open floor and the floor past the wall's end; the wall "
              "and the pillar shadow the floor behind them");
    agent.call("light.sample", kPoints);
    agent.call("frame.capture", kPoints);

    std::puts("-- with the wall layer's occlusion off, its tiles cast no shadows");
    agent.call("component.set",
               R"({"id": "walls", "components": {"TileMapLayer": {"occlusion_enabled": false}}})");
    agent.call("light.sample", R"({"points": [[100, 16], [56, 56]]})");
    return 0;
}
