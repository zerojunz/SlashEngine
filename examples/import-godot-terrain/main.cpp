// Importing a Godot terrain made with the HTerrain plugin (zylann.hterrain): project.import turns
// an HTerrain node into SlashEngine's HTerrain component and writes its heights as a .height.png
// file.
//
// godot/ holds "Valley", a Godot 4 project as its maker keeps it: a scene with an HTerrain node,
// the data.hterrain file the plugin saved beside the terrain's maps, and the texture set that
// names its grass and rock. The plugin saves its heights as height.res, a Godot Image of 32-bit
// floats in Godot's binary resource format, and its splat and color maps as PNG files. main.cpp
// first plays the part of the plugin and writes those maps, a hill 8 meters high with rock on its
// top, and the two textures; then it makes the calls an agent would make over MCP or JSON-RPC.
// The maps are made in code, so the repository holds no binary files.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/godot_pack.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace godot = slash::importer::godot;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

using Rgba = std::array<std::uint8_t, 4>;

// The terrain's heights along each side: 64 cells of one meter.
constexpr int kResolution = 65;

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(1);
}

void write(const std::string& path, const std::string& bytes) {
    if (!slash::platform::write_file(path, bytes).ok()) {
        fail("can't write " + path);
    }
}

// The hill: 8 meters at the middle, falling away to the flat ground 22.6 meters out.
float height(int x, int z) {
    const int dx = x - 32;
    const int dz = z - 32;
    const float h = 8.0F - (static_cast<float>((dx * dx) + (dz * dz)) / 64.0F);
    return h > 0.0F ? h : 0.0F;
}

// height.res as the plugin saves it: an Image of 32-bit floats, row by row from -z.
std::string height_res() {
    std::string bytes;
    for (int z = 0; z < kResolution; ++z) {
        for (int x = 0; x < kResolution; ++x) {
            const auto bits = std::bit_cast<std::uint32_t>(height(x, z));
            for (unsigned shift = 0; shift < 32; shift += 8) {
                bytes += static_cast<char>((bits >> shift) & 0xFFU);
            }
        }
    }
    const std::string text = R"([gd_resource type="Image" format=3]

[resource]
data = {"data": PackedByteArray(")" +
                             slash::base64_encode(bytes) +
                             R"("), "format": "RFloat", "height": 65, "mipmaps": false, "width": 65}
)";
    auto sections = godot::parse_resource(text);
    if (!sections) {
        fail(sections.error().message);
    }
    auto binary =
        godot::write_binary_resource(sections.value(), {.path = "res://terrain/height.res"});
    if (!binary) {
        fail(binary.error().message);
    }
    return binary.value();
}

// A map of the terrain's size, each pixel from its height.
template <typename Pixel>
std::string map_png(Pixel pixel) {
    render::Image image{.width = kResolution, .height = kResolution, .pixels = {}};
    for (int z = 0; z < kResolution; ++z) {
        for (int x = 0; x < kResolution; ++x) {
            const Rgba color = pixel(height(x, z));
            image.pixels.insert(image.pixels.end(), color.begin(), color.end());
        }
    }
    return render::encode_png(image).value();
}

// A ground texture: its color, with its bump height in alpha.
std::string texture_png(Rgba color) {
    render::Image image{.width = 8, .height = 8, .pixels = {}};
    for (int i = 0; i < 64; ++i) {
        image.pixels.insert(image.pixels.end(), color.begin(), color.end());
    }
    return render::encode_png(image).value();
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
    // The plugin's maps: the heights, flat normals, grass below 5 meters and rock above, white
    // color and a few flowers in the detail map.
    write("godot/terrain/height.res", height_res());
    write("godot/terrain/normal.png", map_png([](float) { return Rgba{128, 128, 255, 255}; }));
    write("godot/terrain/splat.png",
          map_png([](float h) { return h < 5.0F ? Rgba{255, 0, 0, 0} : Rgba{0, 255, 0, 0}; }));
    write("godot/terrain/color.png", map_png([](float) { return Rgba{255, 255, 255, 255}; }));
    write("godot/terrain/detail.png", map_png([](float h) {
              const auto density = static_cast<std::uint8_t>(h > 0.0F && h < 2.0F ? 255 : 0);
              return Rgba{density, density, density, 255};
          }));
    write("godot/terrain/grass_albedo_bump.png", texture_png({70, 140, 50, 96}));
    write("godot/terrain/rock_albedo_bump.png", texture_png({130, 125, 120, 200}));

    const Agent agent;
    agent.call("project.import", R"({"source": "godot"})");

    // The terrain: the heights' file and their range, the first splat map and the color map, the
    // texture set's two textures with the Classic4Lite shader's one UV scale, and the collider's
    // friction from its physics material.
    agent.call("scene.open", R"({"path": "godot/main.scene.json"})");
    agent.call("entity.get", R"({"id": "hill", "components": ["HTerrain"]})");

    // The ground at the summit, on the slope and on the flat: its height, normal, textures and
    // color, as the plugin's maps gave them.
    agent.call("terrain.get", R"({"id": "hill", "points": [[32, 32], [32, 46], [4, 60]]})");

    // A ray straight down onto the summit hits the terrain's height field at 8 meters.
    agent.call("physics.raycast", R"({"from": [32, 50, 32], "to": [32, -50, 32]})");

    // The frame: the rock on top of the hill, its grassy slope, the flat ground at the terrain's
    // edge, the sky's ground color past the terrain, and the sky.
    agent.call("frame.capture",
               R"({"points": [[160, 92], [160, 150], [20, 150], [20, 90], [160, 20]]})");
    return 0;
}
