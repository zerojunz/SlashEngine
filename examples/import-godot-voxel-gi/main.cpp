// Importing a Godot VoxelGI with its baked cells: project.import writes the
// cells of the VoxelGIData Godot baked as a .voxelgi.json file, so the room
// lights as it did in Godot with no new bake.
//
// godot/ holds "Gallery", a Godot 4 project as its maker keeps it: a room with
// a red wall and a crate, lit by the sun, and a VoxelGI over it whose data
// names main.gi_data.res, the VoxelGIData Godot's bake saves beside the scene
// in its binary resource format. main.cpp first plays the part of that bake: it
// voxelizes the same meshes with SlashEngine's port of Godot's Voxelizer and
// saves the cells as Godot's VoxelGIData keeps them. Then it makes the calls an
// agent would make over MCP or JSON-RPC. The data is made in code, so the
// repository holds no binary files.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/godot_pack.hpp"
#include "slash/importer/godot_text.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/voxel_gi.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <charconv>
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
namespace scene3d = slash::scene3d;
namespace world = slash::world;

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(1);
}

// A float as Godot writes it, in the fewest digits that read back the same.
std::string number(float value) {
    std::array<char, 32> text{};
    char* const end = std::to_chars(text.data(), text.data() + text.size(), value).ptr;
    return {text.data(), end};
}

// The cells Godot's bake makes of main.tscn's meshes in the VoxelGI's box.
scene3d::VoxelGIData bake_room() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World room{components};
    const auto create = [&](std::string_view id, std::string_view values) {
        const entt::entity entity = room.create(id).value();
        const json::Value parsed = json::parse(values).value();
        for (const auto& [name, fields] : parsed.as_object()) {
            std::string path;
            if (!components.find(name)->assign(room, entity, fields, path).ok()) {
                fail("can't make " + std::string(id));
            }
        }
        return entity;
    };
    const std::string white = R"("StandardMaterial3D": {"albedo_color": [0.8, 0.8, 0.8, 1]})";
    create("floor",
           R"({"Transform3D": {}, "MeshInstance3D": {}, "PlaneMesh": {"size": [6, 6]},
        )" + white +
               "}");
    create("red_wall",
           R"({"Transform3D": {"position": [-2.9, 1.25, 0]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [0.2, 2.5, 6]},
        "StandardMaterial3D": {"albedo_color": [0.9, 0.1, 0.05, 1]}})");
    create("back_wall",
           R"({"Transform3D": {"position": [0, 1.25, -2.9]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [6, 2.5, 0.2]}, )" +
               white + "}");
    create("crate",
           R"({"Transform3D": {"position": [1, 0.5, -0.4]}, "MeshInstance3D": {},
        "BoxMesh": {}, )" +
               white + "}");
    const entt::entity gi = create("gi", R"({"Transform3D": {"position": [0, 1.5, 0]},
        "VoxelGI": {"subdiv": "64", "size": [6.4, 3.2, 6.4]}})");
    auto bake = scene3d::bake_voxel_gi(room, gi, {});
    if (!bake) {
        fail(bake.error().message);
    }
    return bake.value().data;
}

// main.gi_data.res as Godot saves it: the VoxelGIData's settings, and in _data
// the bounds, octree size, level counts and cells VoxelGIData::_get_data
// writes, each cell four little-endian 32-bit values. Its octree and distance
// field, which follow from the cells and which the import doesn't read, are
// left out here.
std::string voxel_gi_data_res(const scene3d::VoxelGIData& data) {
    std::string cells;
    for (const scene3d::VoxelGICell& cell : data.cells) {
        for (const std::uint32_t word : cell) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                cells += static_cast<char>((word >> shift) & 0xFFU);
            }
        }
    }
    std::string levels;
    for (const std::uint32_t count : data.level_counts) {
        levels += (levels.empty() ? "" : ", ") + std::to_string(count);
    }
    const auto vector = [](const std::array<float, 3>& values) {
        return number(values[0]) + ", " + number(values[1]) + ", " + number(values[2]);
    };
    const std::string size = std::to_string(data.octree_size[0]) + ", " +
                             std::to_string(data.octree_size[1]) + ", " +
                             std::to_string(data.octree_size[2]);
    const std::string text = "[gd_resource type=\"VoxelGIData\" format=3]\n\n[resource]\n"
                             "_data = {\n\"bounds\": AABB(" +
                             vector(data.bounds_position) + ", " + vector(data.bounds_size) +
                             "),\n\"level_counts\": PackedInt32Array(" + levels +
                             "),\n\"octree_data\": PackedByteArray(\"" +
                             slash::base64_encode(cells) + "\"),\n\"octree_size\": Vector3(" +
                             size + ")\n}\nenergy = 1.2\n";
    auto sections = godot::parse_resource(text);
    if (!sections) {
        fail(sections.error().message);
    }
    auto binary =
        godot::write_binary_resource(sections.value(), {.path = "res://main.gi_data.res"});
    if (!binary) {
        fail(binary.error().message);
    }
    return binary.value();
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
    if (!slash::platform::write_file("godot/main.gi_data.res", voxel_gi_data_res(bake_room()))
             .ok()) {
        fail("can't write godot/main.gi_data.res");
    }

    const Agent agent;
    agent.call("project.import", R"({"source": "godot"})");

    // The VoxelGI: its box and subdiv, the data's energy, and the cells written
    // beside the scene.
    agent.call("scene.open", R"({"path": "godot/main.scene.json"})");
    agent.call("entity.get", R"({"id": "gi", "components": ["VoxelGI"]})");

    // The frame, lit by the imported cells with no bake: the crate's shadowed
    // side, the floor in its shadow, the floor by the red wall and the open
    // floor.
    agent.call("frame.capture", R"({"points": [[87, 41], [82, 45], [25, 70], [100, 60]]})");

    // Without the cells, only the sun and the ambient light are left.
    agent.call("component.set", R"({"id": "gi", "components": {"VoxelGI": {"data": ""}}})");
    agent.call("frame.capture", R"({"points": [[87, 41], [82, 45], [25, 70], [100, 60]]})");
    return 0;
}
