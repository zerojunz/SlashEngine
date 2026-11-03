// Ground and a minimap made by gameplay code while the game runs, as Godot's SurfaceTool,
// ArrayMesh and ImageTexture.
//
// A game's own Ground component says which seed of noise to raise hills from. Its system, each
// time the seed changes, samples procedural::Noise over a grid, builds the hills with
// scene3d::SurfaceTool (vertices colored by height, smooth normals) and makes them a mesh at
// "made:ground", which the entity's MeshInstance3D draws, and paints the same heights into a
// texture at "made:minimap", which a Sprite2D shows in the corner. An agent then looks at both
// through the usual commands: asset.list, model.inspect, texture.generate and frame.capture.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/procedural/noise.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/surface_tool.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hills {

// Hills of noise: which seed, and what was last built.
struct Ground {
    int seed = 0;
    int built = -1;
};
SLASH_COMPONENT(Ground, seed, built)

} // namespace hills

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace procedural = slash::procedural;
namespace scene3d = slash::scene3d;
using slash::Vec4;
using slash::world::World;

constexpr int kCells = 16;      // cells along each side
constexpr float kSize = 8.0F;   // meters across
constexpr float kHeight = 1.5F; // meters from the lowest hill to the highest

// The height at each grid point, kCells + 1 a side, from 0 to 1.
std::vector<float> heights(int seed) {
    procedural::NoiseSettings settings;
    settings.seed = seed;
    settings.frequency = 0.12F;
    settings.fractal_octaves = 3;
    const procedural::Noise noise(settings);
    std::vector<float> out;
    for (int z = 0; z <= kCells; ++z) {
        for (int x = 0; x <= kCells; ++x) {
            const float value = noise.get_noise_2d(static_cast<float>(x), static_cast<float>(z));
            out.push_back(std::clamp((value + 1.0F) * 0.5F, 0.0F, 1.0F));
        }
    }
    return out;
}

// Green low down, brown up high.
Vec4 color_at(float height) {
    return {0.25F + (0.35F * height), 0.6F - (0.25F * height), 0.15F, 1.0F};
}

scene3d::Model build_hills(const std::vector<float>& height) {
    scene3d::SurfaceTool tool;
    const float step = kSize / static_cast<float>(kCells);
    const auto at = [&](int x, int z) {
        return height[(static_cast<std::size_t>(z) * (kCells + 1)) + static_cast<std::size_t>(x)];
    };
    for (int z = 0; z <= kCells; ++z) {
        for (int x = 0; x <= kCells; ++x) {
            tool.set_color(color_at(at(x, z)));
            tool.set_uv({static_cast<float>(x) / kCells, static_cast<float>(z) / kCells});
            tool.add_vertex({(static_cast<float>(x) * step) - (kSize / 2.0F), at(x, z) * kHeight,
                             (static_cast<float>(z) * step) - (kSize / 2.0F)});
        }
    }
    // Two triangles a cell, clockwise from above as Godot's front faces.
    for (int z = 0; z < kCells; ++z) {
        for (int x = 0; x < kCells; ++x) {
            const auto corner = [](int cx, int cz) {
                return static_cast<std::uint32_t>((cz * (kCells + 1)) + cx);
            };
            for (const std::uint32_t index :
                 {corner(x, z), corner(x + 1, z), corner(x, z + 1), corner(x + 1, z),
                  corner(x + 1, z + 1), corner(x, z + 1)}) {
                tool.add_index(index);
            }
        }
    }
    tool.generate_normals();
    tool.generate_tangents();
    slash::world::StandardMaterial3D material;
    material.vertex_color_use_as_albedo = true;
    material.roughness = 1.0F;
    tool.set_material(material);
    scene3d::Model model;
    (void)tool.commit(model);
    return model;
}

slash::render::Image paint_minimap(const std::vector<float>& height) {
    slash::render::Image image;
    image.width = kCells + 1;
    image.height = kCells + 1;
    for (const float h : height) {
        const Vec4 c = color_at(h);
        for (const float channel : {c.x, c.y, c.z}) {
            image.pixels.push_back(static_cast<std::uint8_t>(channel * 255.0F));
        }
        image.pixels.push_back(255);
    }
    return image;
}

// Gameplay code: builds the hills and the minimap again whenever a Ground's seed changes.
void grow_hills(World& world, double /*tick_seconds*/) {
    for (const auto [entity, ground] : world.ecs().view<hills::Ground>().each()) {
        if (ground.built == ground.seed) {
            continue;
        }
        const std::vector<float> height = heights(ground.seed);
        auto mesh = scene3d::make_mesh(world, "ground", build_hills(height));
        auto texture = procedural::make_texture(world, "minimap", paint_minimap(height));
        if (!mesh || !texture) {
            std::printf("couldn't make the hills: %s\n",
                        (!mesh ? mesh.error() : texture.error()).message.c_str());
        }
        ground.built = ground.seed;
    }
}

json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    command::Reply reply = session.call(name, json::parse(params).value());
    if (!reply.ok) {
        std::printf("%.*s failed: %s\n", static_cast<int>(name.size()), name.data(),
                    reply.message.c_str());
    }
    return std::move(reply.value);
}

void print(std::string_view what, const json::Value& value) {
    std::printf("%.*s: %s\n", static_cast<int>(what.size()), what.data(),
                json::write(value, json::Style::Compact).c_str());
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    components.add<hills::Ground>().set_doc("Hills of noise: the seed, and the seed last built.");
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    session.world().add_system("grow_hills", grow_hills,
                               {.doc = "Builds hills and a minimap from each Ground's seed."});

    call(session, "batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "camera", "components": {
            "Camera3D": {"current": true, "fov": 60},
            "Transform3D": {"position": [0, 6, 7], "rotation": [-0.7, 0, 0]}}}},
        {"command": "entity.create", "params": {"id": "sun", "components": {
            "DirectionalLight3D": {},
            "Transform3D": {"rotation": [-0.9, 0.6, 0]}}}},
        {"command": "entity.create", "params": {"id": "hills", "components": {
            "Ground": {"seed": 1},
            "MeshInstance3D": {"mesh": "made:ground"},
            "Transform3D": {}}}},
        {"command": "entity.create", "params": {"id": "minimap", "components": {
            "Sprite2D": {"centered": false, "texture": "made:minimap"},
            "Transform2D": {"position": [4, 4], "scale": [2, 2]}}}}]})");

    // Before the first tick nothing has made them, so nothing draws.
    print("before", call(session, "frame.capture", R"({"points": [[640, 400], [10, 10]]})"));

    call(session, "step", "{}");
    json::Value made = json::Array{};
    const json::Value listed = call(session, "asset.list", "{}");
    for (const json::Value& asset : listed.find("assets")->as_array()) {
        if (asset.find("path")->as_string().starts_with("made:")) {
            json::Value entry;
            entry["path"] = *asset.find("path");
            entry["type"] = *asset.find("type");
            entry["version"] = *asset.find("version");
            made.push_back(std::move(entry));
        }
    }
    print("made", made);
    print("ground", call(session, "model.inspect", R"({"path": "made:ground", "brief": true})"));
    print("minimap", call(session, "texture.generate", R"({"path": "made:minimap"})"));
    print("seed 1",
          call(session, "frame.capture", R"({"points": [[640, 400], [400, 500], [10, 10]]})"));

    // A new seed: the system builds both again in place, and every handle sees them.
    call(session, "component.set", R"({"id": "hills", "components": {"Ground": {"seed": 2}}})");
    call(session, "step", "{}");
    print("minimap", call(session, "texture.generate", R"({"path": "made:minimap"})"));
    print("seed 2",
          call(session, "frame.capture", R"({"points": [[640, 400], [400, 500], [10, 10]]})"));
    return 0;
}
