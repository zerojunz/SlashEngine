// Importing an Unreal Engine game: project.import converts an Unreal project into this one.
//
// unreal/ holds "Crate Run", a small Unreal Engine 5 game: its .uproject file, config files with
// its settings and legacy input mappings, and a C++ class. Unreal's .umap and .uasset files are
// binary and only Unreal reads them, so the import runs the project's own Unreal Editor in the
// background (as Godot runs Blender for .blend files), with a Python script that exports each
// level's actors and components, the Enhanced Input assets, the static meshes as glTF and the
// sounds as WAV. This example runs a stand-in for the editor that copies unreal/FakeExport, which
// holds what the script writes for this project, so it runs where Unreal isn't installed.
//
// One call converts it. Its level becomes a .scene.json file beside the .umap, in Godot's axes
// and meters: meshes as MeshInstance3D entities over the exported glTF files (with the collision
// Unreal gives them), lights, the sky, fog, the camera on the player's spring arm, physics
// bodies, trigger and blocking volumes, and sounds; its settings and both input systems become
// project.json; and each Blueprint and C++ class becomes a storyboard task to port it to C++,
// since SlashEngine games are plain C++. The report lists what didn't convert, and why. The
// glTF files and the sound are made in code here, so the repository holds no binary files.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace audio = slash::audio;
namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

constexpr int kRate = 44100;

void write_file(const std::string& path, const std::string& contents) {
    if (!slash::platform::write_file(path, contents).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
}

// A box's surface, its faces wound clockwise from outside, as Godot's front faces are.
scene3d::ModelSurface box(std::array<float, 3> low, std::array<float, 3> high) {
    scene3d::ModelSurface surface;
    const auto corner = [&](std::size_t c) {
        return std::array<float, 3>{(c & 1U) != 0 ? high[0] : low[0],
                                    (c & 2U) != 0 ? high[1] : low[1],
                                    (c & 4U) != 0 ? high[2] : low[2]};
    };
    const std::array<std::pair<std::array<std::size_t, 4>, std::array<float, 3>>, 6> faces{{
        {{0, 2, 3, 1}, {0.0F, 0.0F, -1.0F}},
        {{4, 5, 7, 6}, {0.0F, 0.0F, 1.0F}},
        {{0, 4, 6, 2}, {-1.0F, 0.0F, 0.0F}},
        {{1, 3, 7, 5}, {1.0F, 0.0F, 0.0F}},
        {{0, 1, 5, 4}, {0.0F, -1.0F, 0.0F}},
        {{2, 6, 7, 3}, {0.0F, 1.0F, 0.0F}},
    }};
    for (const auto& [ring, normal] : faces) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (const std::size_t c : ring) {
            slash::render::Vertex3D vertex;
            const std::array<float, 3> at = corner(c);
            vertex.position = {at[0], at[1], at[2]};
            vertex.normal = {normal[0], normal[1], normal[2]};
            surface.vertices.push_back(vertex);
        }
        for (const std::uint32_t i : {0U, 2U, 1U, 0U, 3U, 2U}) {
            surface.indices.push_back(first + i);
        }
    }
    return surface;
}

// Writes a static mesh as Unreal's glTF exporter does: a box in glTF's axes (Unreal's x, z and
// y) and meters, from Unreal's bounds in centimeters.
void write_mesh(const std::string& path, std::array<float, 3> low, std::array<float, 3> high) {
    scene3d::Model model;
    scene3d::ModelMesh mesh;
    mesh.name = "Mesh";
    mesh.surfaces.push_back(box({low[0] / 100.0F, low[2] / 100.0F, low[1] / 100.0F},
                                {high[0] / 100.0F, high[2] / 100.0F, high[1] / 100.0F}));
    model.meshes.push_back(mesh);
    scene3d::ModelNode node;
    node.name = "Mesh";
    model.nodes.push_back(node);
    model.parts.push_back(
        {.node = "Mesh", .mesh = 0, .transform = {}, .node_index = 0, .skin = -1});
    const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no files");
    };
    write_file(path, scene3d::write_glb(model, path, none).value());
}

// Writes a second of wind: a quiet square wave whose pitch wanders, a whole number of cycles.
void write_wind(const std::string& path) {
    std::vector<audio::Frame> out(static_cast<std::size_t>(kRate));
    int phase = 0; // in cycles times kRate
    for (int i = 0; i < kRate; ++i) {
        const int hz = 100 + (i < kRate / 2 ? i : kRate - i) / 441;
        phase = (phase + (2 * hz)) % (2 * kRate);
        const float value = phase < kRate ? 0.1F : -0.1F;
        out[static_cast<std::size_t>(i)] = {.left = value, .right = value};
    }
    write_file(path, audio::encode_wav(out, kRate));
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
    const std::string exported = "unreal/FakeExport/";
    if (!slash::platform::create_directories(exported + "meshes/Game/Props").ok() ||
        !slash::platform::create_directories(exported + "sounds/Game/Audio").ok()) {
        return 1;
    }
    write_mesh(exported + "meshes/Game/Props/SM_Crate.glb", {-50.0F, -50.0F, 0.0F},
               {50.0F, 50.0F, 100.0F});
    write_mesh(exported + "meshes/Game/Props/SM_Floor.glb", {-1000.0F, -1000.0F, -10.0F},
               {1000.0F, 1000.0F, 0.0F});
    write_mesh(exported + "meshes/Game/Props/SM_Coin.glb", {-30.0F, -5.0F, -30.0F},
               {30.0F, 5.0F, 30.0F});
    write_wind(exported + "sounds/Game/Audio/Wind.wav");
    // The Blueprints the import's tasks link to, which only Unreal reads: stand-ins here.
    if (!slash::platform::create_directories("unreal/Content/Blueprints").ok() ||
        !slash::platform::create_directories("unreal/Content/UI").ok()) {
        return 1;
    }
    for (const char* blueprint :
         {"Blueprints/BP_Coin", "Blueprints/BP_CrateRunMode", "Blueprints/BP_Hero", "UI/WBP_Hud"}) {
        write_file(std::string("unreal/Content/") + blueprint + ".uasset", "stand-in");
    }

    const Agent agent;
    // A dry run first: what the import would write, writing nothing.
    agent.call("project.import", R"({"source": "unreal", "dry_run": true})");
    agent.call("project.import", R"({"source": "unreal"})");

    // The level, the report and the project's settings.
    print_file("unreal/Content/Maps/Main.scene.json");
    print_file("import/unreal.json");
    print_file("project.json");

    // The converted level opens and runs: the crate Unreal simulates falls onto the floor and
    // rests there.
    agent.call("scene.open", R"({"path": "unreal/Content/Maps/Main.scene.json"})");
    agent.call("step", R"({"ticks": 90})");
    agent.call("world.query", R"({"with": ["RigidBody3D"], "include": ["Transform3D"]})");
    // The floor collides as Unreal's complex collision: a ray down stops on its top.
    agent.call("physics.raycast", R"({"from": [-6, 5, -3], "to": [-6, -5, -3]})");
    // The goal is a trigger box, an area the player can walk into.
    agent.call("physics.overlap", R"({"shape": {"shape": "sphere", "radius": 0.1},
                                      "position": [8, 0.5, 1], "collide_with_areas": true})");
    agent.call("storyboard.query", R"({"kind": "task"})");
    agent.call("storyboard.check", "{}");
    return 0;
}
