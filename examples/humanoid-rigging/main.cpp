// An unrigged figure rigged through the asset library: a boxy knight standing with its arms at
// its sides goes in with asset.add, and asset.refine's rig has a generator give it a skeleton
// and skin, retargeted to Godot's humanoid skeleton before the library keeps it. A dancer rigged
// another way, in a T-pose with a wave, then plays its wave on the knight.
//
// The test generator rigs here, placing the skeleton by a standing figure's proportions; with
// Meshy's key in the environment, a "meshy-rig" generator in providers.json rigs it instead.
// The models are made in code and written as .glb files, so none is committed.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
using slash::Vec3;

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.ok ? reply.value : json::Value();
}

// A box of a surface, its faces wound clockwise from outside, as Godot's front faces are.
void add_box(scene3d::ModelSurface& surface, Vec3 low, Vec3 high) {
    const std::array<Vec3, 8> corner{{{low.x, low.y, low.z},
                                      {high.x, low.y, low.z},
                                      {high.x, high.y, low.z},
                                      {low.x, high.y, low.z},
                                      {low.x, low.y, high.z},
                                      {high.x, low.y, high.z},
                                      {high.x, high.y, high.z},
                                      {low.x, high.y, high.z}}};
    const std::array<std::pair<std::array<std::size_t, 4>, Vec3>, 6> faces{{
        {{0, 3, 2, 1}, {0.0F, 0.0F, -1.0F}},
        {{4, 5, 6, 7}, {0.0F, 0.0F, 1.0F}},
        {{0, 4, 7, 3}, {-1.0F, 0.0F, 0.0F}},
        {{1, 2, 6, 5}, {1.0F, 0.0F, 0.0F}},
        {{0, 1, 5, 4}, {0.0F, -1.0F, 0.0F}},
        {{3, 7, 6, 2}, {0.0F, 1.0F, 0.0F}},
    }};
    for (const auto& [ring, normal] : faces) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (const std::size_t c : ring) {
            slash::render::Vertex3D vertex;
            vertex.position = {corner.at(c).x, corner.at(c).y, corner.at(c).z};
            vertex.normal = {normal.x, normal.y, normal.z};
            surface.vertices.push_back(vertex);
        }
        for (const std::uint32_t i : {0U, 2U, 1U, 0U, 3U, 2U}) {
            surface.indices.push_back(first + i);
        }
    }
}

// A figure of boxes facing +z, scaled to a height, its arms out level or at its sides.
scene3d::Model figure(float height, bool t_pose) {
    const float k = height / 1.8F;
    scene3d::Model model;
    scene3d::ModelMesh body;
    body.name = "Body";
    scene3d::ModelSurface& surface = body.surfaces.emplace_back();
    const auto box = [&](float x0, float y0, float x1, float y1, float depth) {
        add_box(surface, Vec3{x0, y0, -depth} * k, Vec3{x1, y1, depth} * k);
        add_box(surface, Vec3{-x1, y0, -depth} * k, Vec3{-x0, y1, depth} * k);
    };
    box(0.03F, 0.0F, 0.15F, 0.9F, 0.07F); // legs
    if (t_pose) {
        box(0.18F, 1.38F, 0.8F, 1.48F, 0.05F);
    } else {
        box(0.19F, 0.8F, 0.27F, 1.46F, 0.05F);
    }
    add_box(surface, Vec3{-0.18F, 0.9F, -0.1F} * k, Vec3{0.18F, 1.5F, 0.1F} * k);
    add_box(surface, Vec3{-0.1F, 1.55F, -0.1F} * k, Vec3{0.1F, 1.8F, 0.1F} * k);
    model.meshes.push_back(body);
    scene3d::ModelNode node;
    node.name = "Body";
    model.nodes.push_back(node);
    model.parts.push_back(
        {.node = "Body", .mesh = 0, .transform = {}, .node_index = 0, .skin = -1});
    return model;
}

} // namespace

int main() {
    const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no files");
    };
    (void)slash::platform::create_directories("game/source");
    (void)slash::platform::write_file(
        "game/source/knight.glb",
        scene3d::write_glb(figure(1.8F, false), "source/knight.glb", none).value());

    // The dancer, 1.5 meters tall in a T-pose, rigged with the profile's bone names and a wave:
    // the left arm raised 60 degrees and the hips bobbing up 10 centimeters.
    scene3d::Model dancer = scene3d::rig_humanoid(figure(1.5F, true)).value();
    scene3d::ModelAnimation wave;
    wave.name = "wave";
    wave.length = 1.0;
    wave.loop = true;
    const auto track = [&](const char* bone, scene3d::NodeProperty property,
                           std::vector<std::array<float, 4>> values) {
        scene3d::NodeTrack out;
        out.node = static_cast<std::size_t>(scene3d::find_node(dancer, bone));
        out.property = property;
        out.times = {0.0F, 0.5F, 1.0F};
        out.values = std::move(values);
        wave.tracks.push_back(out);
    };
    track("LeftUpperArm", scene3d::NodeProperty::Rotation,
          {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.5F, 0.866F}, {0.0F, 0.0F, 0.0F, 1.0F}});
    const Vec3 hips =
        dancer.nodes.at(static_cast<std::size_t>(scene3d::find_node(dancer, "Hips"))).position;
    track("Hips", scene3d::NodeProperty::Position,
          {{hips.x, hips.y, hips.z, 0.0F},
           {hips.x, hips.y + 0.1F, hips.z, 0.0F},
           {hips.x, hips.y, hips.z, 0.0F}});
    dancer.animations.push_back(wave);
    (void)slash::platform::write_file(
        "game/source/dancer.glb", scene3d::write_glb(dancer, "source/dancer.glb", none).value());

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::library::register_commands(commands);
    // The library would be ~/.slash/library; the example keeps its own.
    command::SessionOptions options;
    options.capture_logs = false;
    options.library_dir = "library";
    options.project_root = "game";
    command::Session game(components, commands, options);

    // The knight goes into the library as it is, then is rigged: the reply says how many of the
    // profile's bones it has, that none the profile needs is missing, and how high its hips are.
    const std::string knight =
        call(game, "asset.add",
             R"({"path": "source/knight.glb", "description": "A boxy knight, arms at its sides.",
                 "tags": ["character"]})")["id"]
            .as_string();
    const std::string rigged =
        call(game, "asset.refine",
             R"({"id": ")" + knight +
                 R"(", "rig": "humanoid", "generator": "test", "path": "characters/knight.glb"})")
            ["id"]
                .as_string();
    call(game, "asset.info", R"({"id": ")" + rigged + R"("})");

    // The dancer's own rig retargeted to the same skeleton.
    call(game, "model.retarget",
         R"({"path": "source/dancer.glb", "out": "characters/dancer.glb"})");

    // The dancer's wave half way through, on the knight: its left arm rises as the dancer's does,
    // and its hips bob in proportion to its height.
    call(game, "entity.create",
         R"({"id": "knight", "components": {"Transform3D": {},
             "MeshInstance3D": {"mesh": "characters/knight.glb"},
             "AnimationPlayer": {"library": "characters/dancer.glb", "animation": "wave-loop",
                                 "time": 0.5, "playing": false}}})");
    call(game, "skeleton.get",
         R"({"id": "knight", "bones": ["Hips", "LeftUpperArm", "LeftHand"]})");

    // A model rigged already isn't rigged again.
    call(game, "asset.refine",
         R"({"id": ")" + rigged + R"(", "rig": "humanoid", "generator": "test"})");
    return 0;
}
