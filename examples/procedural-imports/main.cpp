// Procedural content from other tools: Godot's noise and gradient textures, and terrains from
// World Machine, Gaea or World Creator.
//
// godot/ holds a Godot 4 scene whose sprites draw a NoiseTexture2D (a FastNoiseLite with a color
// ramp) and a GradientTexture2D. project.import writes each as a .texture.json file that makes the
// same pixels as it loads, so the water still tiles and can be tuned with texture.generate. Then a
// valley's heightmap and splat map come in as a terrain tool exports them, a 16-bit .r16 file and
// a PNG, through terrain.import. The terrain tool's files are made in code, with integer math, so
// the repository holds no binary files.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

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

void write(const std::string& path, const std::string& bytes) {
    if (!slash::platform::write_file(path, bytes).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
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
        std::puts(json::write(out, json::Style::Pretty).c_str());
    }
};

} // namespace

int main() {
    // A valley 129 cells square, as a terrain tool exports it: heights as 16-bit little-endian
    // values from the lowest to the highest, falling from the ridges on either side to a river
    // down the middle, and a splat map putting rock (green) on the ridges and grass (red) below.
    constexpr int kSize = 129;
    std::string heights;
    render::Image splat{.width = kSize, .height = kSize, .pixels = {}};
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const int across = x < 64 ? 64 - x : x - 64;
            const int value = (across * across * 12) + (y * 8);
            heights.push_back(static_cast<char>(value & 0xFF));
            heights.push_back(static_cast<char>(value >> 8));
            const bool rock = across > 40;
            splat.pixels.insert(splat.pixels.end(),
                                {static_cast<std::uint8_t>(rock ? 0 : 255),
                                 static_cast<std::uint8_t>(rock ? 255 : 0), 0, 0});
        }
    }
    if (!slash::platform::create_directories("terrain").ok()) {
        return 1;
    }
    write("terrain/valley.r16", heights);
    write("terrain/valley_splat.png", render::encode_png(splat).value());

    const Agent agent;
    agent.call("project.import", R"({"source": "godot"})");
    print_file("godot/pond_noise_texture2d_water.texture.json");
    print_file("godot/pond_gradient_texture2d_glow.texture.json");
    // The water's pixels, as the texture makes them: it tiles, since Godot's was seamless.
    agent.call("texture.generate", R"({"path": "godot/pond_noise_texture2d_water.texture.json"})");

    // A terrain whose maps the import writes, its height range set from the tool's export.
    agent.call("entity.create", R"({"id": "valley", "components": {"Transform3D": {},
        "HTerrain": {"heightmap": "terrain/valley.height.png", "splatmap":
                     "terrain/valley.splat.png", "resolution": 65}}})");
    agent.call("terrain.import", R"({"id": "valley", "heightmap": "terrain/valley.r16",
        "min_height": -5, "max_height": 120, "splatmap": "terrain/valley_splat.png"})");
    // The river, a slope and a ridge: their heights and whether grass or rock covers them.
    agent.call("terrain.get", R"({"id": "valley", "points": [[64, 0], [64, 128], [10, 64]]})");
    return 0;
}
