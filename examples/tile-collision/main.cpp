// Tile collision, as Godot's TileSet physics layers: level.tileset.json gives its tiles collision
// polygons, and each TileMapLayer whose tiles have them is a static body in 2D physics.
//
// The level in main.scene.json has blocks, a slope, a one-way ledge, a springy tile on a second
// physics layer and a see-through alternative of the block with no collision. Four balls show
// each: one rolls down the slope, one bounces on the spring, one falls through the see-through
// block and one rises up through the ledge and lands on it.

#include "slash/canvas/tilemap.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

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

constexpr Rgba kStone{120, 120, 130, 255};
constexpr Rgba kMortar{80, 80, 90, 255};
constexpr Rgba kEarth{150, 100, 60, 255};
constexpr Rgba kWood{190, 140, 80, 255};
constexpr Rgba kSpring{220, 60, 50, 255};
constexpr Rgba kCoil{250, 210, 60, 255};
constexpr Rgba kClear{0, 0, 0, 0};

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// The atlas, four 16x16 tiles drawn to match their collision polygons: a stone block, a slope
// rising to the right, a ledge four pixels thick and a spring filling the lower half. Games ship
// PNG files; this one is made here so the example has no binary file.
void write_atlas(const canvas::TileSet& set) {
    const canvas::AtlasSource& source = set.sources.at("blocks");
    render::Image image{.width = 64, .height = 16, .pixels = {}};
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int tile = x / 16;
            const int u = x % 16;
            Rgba color = kClear;
            if (tile == 0) {
                color = (u == 0 || y == 0 || (y == 8 && u > 0)) ? kMortar : kStone;
            } else if (tile == 1) {
                color = u + y >= 15 ? kEarth : kClear;
            } else if (tile == 2) {
                color = y < 4 ? kWood : kClear;
            } else if (y >= 8) {
                color = y % 3 == 0 ? kCoil : kSpring;
            }
            const std::size_t at =
                ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                image.pixels[at + channel] = color.at(channel);
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

void print_balls(const world::World& world) {
    std::printf("tick %llu:\n", static_cast<unsigned long long>(world.tick()));
    for (const std::string_view id : {"roller", "jumper", "ghost_test", "riser"}) {
        const entt::entity ball = world.find(id);
        const slash::Vec2 at = world.ecs().get<world::Transform2D>(ball).position;
        const slash::Vec2 velocity = world.ecs().get<world::RigidBody2D>(ball).linear_velocity;
        std::printf("  %-10.*s at (%.1f, %.1f), moving (%.1f, %.1f)\n", static_cast<int>(id.size()),
                    id.data(), at.x, at.y, velocity.x + 0.0F,
                    velocity.y + 0.0F); // + 0 prints -0 as 0
    }
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

    write_atlas(*session.assets().load<canvas::TileSet>("level.tileset.json").value());
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // A slope turned to fall the other way, beside the wall: "h" flips the tile and its
    // collision polygon left to right. The layer gets a new legend key for it.
    call(session, "tilemap.set", R"({"id": "level", "cells": [[1, 6]], "tile": "slope:h"})");
    const json::Value map = call(session, "tilemap.get", R"({"id": "level"})");
    for (const json::Value& row : map.find("rows")->as_array()) {
        std::printf("  %s\n", row.as_string().c_str());
    }

    // Two seconds, half a second at a time: the layer's tiles are solid where their polygons
    // are, and each physics layer has its own friction and bounce.
    for (int step = 0; step < 4; ++step) {
        session.call("step", json::parse(R"({"ticks": 30})").value());
        print_balls(session.world());
    }

    // A ray that hits a tile reports the layer and the cell it hit.
    call(session, "physics.raycast", R"({"from": [72, 0], "to": [72, 128]})");

    // Tiles and turns are checked, with what there is.
    call(session, "tilemap.set", R"({"id": "level", "cells": [[0, 0]], "tile": "block:2"})");
    call(session, "tilemap.set", R"({"id": "level", "cells": [[0, 0]], "tile": "slope:hh"})");
    return 0;
}
