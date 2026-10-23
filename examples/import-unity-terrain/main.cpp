// Importing a Unity terrain: project.import turns a Terrain and its TerrainCollider into
// SlashEngine's HTerrain component, writes its heights as a .height.png file and its alpha map
// as a splat map, and cuts its holes.
//
// unity/ holds "Island", a Unity 2022.3 project as its maker keeps it: a scene with a Terrain
// object, and the grass and rock terrain layers it paints with. Unity keeps a terrain's heights,
// holes and alpha map in its TerrainData asset, as hex numbers in the .asset file, and its layers'
// textures as PNG files. main.cpp first plays the part of Unity's terrain tools and writes those:
// a hill 8 meters high in the middle of 64 by 64 meters of flat ground, rock above 4 meters, a
// hole in the flat ground and the two textures; then it makes the calls an agent would make over
// MCP or JSON-RPC. The textures are made in code, so the repository holds no binary files.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

using Rgba = std::array<std::uint8_t, 4>;

// The terrain's heights along each side: 64 cells of one meter, up to 20 meters high.
constexpr int kResolution = 65;
constexpr double kHeight = 20.0;
// Unity's alpha maps are a power of two across: one texel a cell here.
constexpr int kAlphaSize = 64;

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(1);
}

void write(const std::string& path, const std::string& bytes) {
    if (!slash::platform::write_file(path, bytes).ok()) {
        fail("can't write " + path);
    }
}

// The hill, x and z in meters from the terrain's corner: 8 meters at the middle, falling away to
// the flat ground 22.6 meters out.
double height(double x, double z) {
    const double dx = x - 32.0;
    const double dz = z - 32.0;
    const double h = 8.0 - (((dx * dx) + (dz * dz)) / 64.0);
    return h > 0.0 ? h : 0.0;
}

// The cut cells: eight by eight of the flat ground, 23 to 31 meters along x and -6 to 2 along z
// from the middle.
bool cut(int x, int z) {
    return x >= 55 && x <= 62 && z >= 26 && z <= 33;
}

// Bytes as Unity writes arrays of numbers in text: two hex digits each.
std::string hex(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    for (const std::uint8_t byte : bytes) {
        out.push_back("0123456789abcdef"[byte >> 4U]);
        out.push_back("0123456789abcdef"[byte & 0xFU]);
    }
    return out;
}

// Island.asset as Unity saves it: the TerrainData, its heights as 16-bit numbers where 32766 is
// the full 20 meters, its holes a byte a cell (0 for cut), and its alpha map, a texture in the
// same file whose channels weigh the layers: red grass, green rock. Rows run along +z.
std::string terrain_data() {
    std::vector<std::uint8_t> heights;
    for (int z = 0; z < kResolution; ++z) {
        for (int x = 0; x < kResolution; ++x) {
            const auto value =
                static_cast<std::uint16_t>(std::lround(height(x, z) / kHeight * 32766.0));
            heights.push_back(static_cast<std::uint8_t>(value & 0xFFU));
            heights.push_back(static_cast<std::uint8_t>(value >> 8U));
        }
    }
    std::vector<std::uint8_t> holes;
    for (int z = 0; z < kResolution - 1; ++z) {
        for (int x = 0; x < kResolution - 1; ++x) {
            holes.push_back(cut(x, z) ? 0 : 1);
        }
    }
    std::vector<std::uint8_t> alpha;
    for (int z = 0; z < kAlphaSize; ++z) {
        for (int x = 0; x < kAlphaSize; ++x) {
            const bool rock = height(x + 0.5, z + 0.5) >= 4.0;
            const Rgba texel = rock ? Rgba{0, 255, 0, 0} : Rgba{255, 0, 0, 0};
            alpha.insert(alpha.end(), texel.begin(), texel.end());
        }
    }
    const std::string header = "  m_ObjectHideFlags: 0\n"
                               "  m_CorrespondingSourceObject: {fileID: 0}\n"
                               "  m_PrefabInstance: {fileID: 0}\n"
                               "  m_PrefabAsset: {fileID: 0}\n";
    std::string out = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!156 &15600000\n";
    out += "TerrainData:\n";
    out += header;
    out += "  m_Name: Island\n  serializedVersion: 6\n  m_SplatDatabase:\n";
    out += "    serializedVersion: 2\n    m_TerrainLayers:\n";
    out += "    - {fileID: 8574412962073106934, guid: b2000000000000000000000000000031, type: 2}\n";
    out += "    - {fileID: 8574412962073106934, guid: b2000000000000000000000000000032, type: 2}\n";
    out += "    m_AlphaTextures:\n    - {fileID: 2800002}\n";
    out += "    m_AlphamapResolution: " + std::to_string(kAlphaSize) + "\n";
    out += "    m_BaseMapResolution: 64\n    m_BaseMapDirty: 0\n";
    out += "  m_DetailDatabase:\n    serializedVersion: 3\n    m_Patches: []\n";
    out += "    m_DetailPrototypes: []\n    m_TreeInstances: []\n    m_TreePrototypes: []\n";
    out += "  m_Heightmap:\n    serializedVersion: 6\n    m_Holes: ";
    out += hex(holes);
    out += "\n    m_Heights: ";
    out += hex(heights);
    out += "\n    m_Resolution: 65\n    m_Levels: 4\n    m_Scale: {x: 1, y: 20, z: 1}\n";
    out += "--- !u!28 &2800002\nTexture2D:\n";
    out += header;
    out += "  m_Name: SplatAlpha 0\n  serializedVersion: 13\n";
    out += "  m_Width: " + std::to_string(kAlphaSize) + "\n";
    out += "  m_Height: " + std::to_string(kAlphaSize) + "\n";
    out += "  m_CompleteImageSize: " + std::to_string(alpha.size()) + "\n";
    out += "  m_TextureFormat: 4\n  m_MipCount: 1\n  m_IsReadable: 1\n";
    out += "  image data: " + std::to_string(alpha.size()) + "\n  _typelessdata: ";
    out += hex(alpha);
    out += "\n";
    return out;
}

// A layer's texture: one color.
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
    write("unity/Assets/Terrain/Island.asset", terrain_data());
    write("unity/Assets/Terrain/grass.png", texture_png({70, 140, 50, 255}));
    write("unity/Assets/Terrain/rock.png", texture_png({130, 125, 120, 255}));

    const Agent agent;
    agent.call("project.import", R"({"source": "unity"})");

    // The Island object and the terrain under it: Unity's terrain reaches toward +x from its
    // object, so toward -x mirrored, and its entity moves back the terrain's width. The heights'
    // file and range, the splat map made from the alpha map, the color map that cuts the holes,
    // the two layers with their tiles in cells, and the collider.
    agent.call("scene.open", R"({"path": "unity/Assets/Scenes/Island.scene.json"})");
    agent.call("entity.get", R"({"id": "island", "components": ["Transform3D"]})");
    agent.call("entity.get", R"({"id": "island_terrain", "omit_defaults": true})");

    // The ground at the summit, on the grassy slope toward the camera and in the hole.
    agent.call("terrain.get", R"({"id": "island_terrain", "global": true,
                                  "points": [[0, 0], [0, -18], [-27, -2]]})");

    // Rays straight down: onto the summit, 8 meters up, and through the hole.
    agent.call("physics.raycast", R"({"from": [0, 50, 0], "to": [0, -50, 0]})");
    agent.call("physics.raycast", R"({"from": [-27, 50, -2], "to": [-27, -50, -2]})");

    // The frame: the rock on the hilltop, the grassy slope, the flat ground, the sky's ground color
    // through the hole, and the sky.
    agent.call("frame.capture",
               R"({"points": [[80, 40], [80, 60], [20, 75], [130, 52], [80, 5]]})");
    return 0;
}
