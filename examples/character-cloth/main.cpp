// Character cloth, as Unreal's Chaos Cloth: a hero's cape on its skin in 3D and a cut-out kid's
// cape in 2D, each held at the top and free to swing up to its max distance below.
//
// The program writes the hero's model (hero.glb: a one-bone figure with a cape, made in code),
// opens main.scene.json, then works both capes as an agent would: it steps the world, moves the
// hero's spine and the kid with component.set, turns a breeze on and reads where the capes' points
// are with physics.soft_body.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3;

constexpr int kColumns = 5;
constexpr int kRows = 6;

// A figure with one bone, Spine, a meter and a half up: a small body and a cape behind it from
// its shoulders (y 1.5) down to y 0.5, both skinned to Spine. The cape's vertices go row by row
// from the top; its points are those vertices welded, in Jolt's order, in which 5 is the middle of
// its top and 27 the middle of its hem.
scene3d::Model caped_figure() {
    scene3d::Model model;
    for (const char* name : {"body", "cape"}) {
        scene3d::ModelMaterial material;
        material.name = name;
        model.materials.push_back(material);
    }
    const auto node = [&](const char* name, int parent, Vec3 position) {
        scene3d::ModelNode added;
        added.name = name;
        added.parent = parent;
        added.position = position;
        model.nodes.push_back(added);
    };
    node("Root", -1, {});
    node("Spine", 0, {0.0F, 1.5F, 0.0F});
    node("Figure", 0, {});
    world::Affine3D unbind;
    unbind.origin = {0.0F, -1.5F, 0.0F};
    model.skins.push_back({.name = "Skin", .joints = {1}, .inverse_binds = {unbind}});
    const auto weighted = [](scene3d::ModelSurface& surface) {
        scene3d::VertexWeights weights;
        weights.weights[0] = 1.0F;
        surface.weights.assign(surface.vertices.size(), weights);
    };
    scene3d::ModelSurface body;
    body.material = 0;
    for (const auto& [x, y] : {std::pair{-0.2F, 0.6F}, {0.2F, 0.6F}, {0.2F, 1.5F}, {-0.2F, 1.5F}}) {
        body.vertices.push_back({.position = {x, y, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}});
    }
    body.indices = {0, 1, 2, 0, 2, 3};
    weighted(body);
    scene3d::ModelSurface cape;
    cape.material = 1;
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            const float x = -0.3F + (0.6F * static_cast<float>(column) / (kColumns - 1));
            const float y = 1.5F - (1.0F * static_cast<float>(row) / (kRows - 1));
            cape.vertices.push_back({.position = {x, y, -0.25F},
                                     .normal = {0.0F, 0.0F, -1.0F},
                                     .uv = {static_cast<float>(column) / (kColumns - 1),
                                            static_cast<float>(row) / (kRows - 1)}});
        }
    }
    for (int row = 0; row + 1 < kRows; ++row) {
        for (int column = 0; column + 1 < kColumns; ++column) {
            const auto at = [](int r, int c) {
                return static_cast<std::uint32_t>((r * kColumns) + c);
            };
            cape.indices.insert(cape.indices.end(),
                                {at(row, column), at(row + 1, column), at(row + 1, column + 1),
                                 at(row, column), at(row + 1, column + 1), at(row, column + 1)});
        }
    }
    weighted(cape);
    scene3d::ModelMesh mesh;
    mesh.name = "Figure";
    mesh.surfaces = {body, cape};
    model.meshes.push_back(std::move(mesh));
    scene3d::ModelPart part;
    part.node = "Figure";
    part.node_index = 2;
    part.skin = 0;
    model.parts.push_back(part);
    return model;
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

void step(command::Session& session, int ticks) {
    std::printf("step %d ticks\n", ticks);
    session.call("step", json::parse(R"({"ticks": )" + std::to_string(ticks) + "}").value());
}

} // namespace

int main() {
    const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no files");
    };
    if (!slash::platform::write_file("hero.glb",
                                     scene3d::write_glb(caped_figure(), "hero.glb", none).value())
             .ok()) {
        return 1;
    }
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Half a second hanging still: the hero's cape hangs from its shoulders (top middle 5, hem
    // middle 27) and the kid's from the kid's back (top 1, hem 29), where their skins put them.
    step(session, 30);
    call(session, "physics.soft_body", R"({"id": "hero", "at": [5, 27]})");
    call(session, "physics.soft_body", R"({"id": "kid", "at": [1, 29]})");

    // The hero lunges half a meter forward and the kid runs 40 pixels right in one tick: the
    // capes' tops go with them at once, and the hems trail behind, no further than their max
    // distance from where the skin puts them.
    call(session, "component.set", R"({"id": "hero", "components": {
        "Skeleton3D": {"bone_positions": {"Spine": [0, 1.5, 0.5]}}}})");
    call(session, "component.set",
         R"({"id": "kid", "components": {"Transform2D": {"position": [140, 40]}}})");
    step(session, 1);
    call(session, "physics.soft_body", R"({"id": "hero", "at": [5, 27]})");
    call(session, "physics.soft_body", R"({"id": "kid", "at": [1, 29]})");

    // Four seconds on, both have swung back under their wearers and come to rest.
    step(session, 240);
    call(session, "physics.soft_body", R"({"id": "hero", "at": [5, 27]})");
    call(session, "physics.soft_body", R"({"id": "kid", "at": [1, 29]})");

    // A breeze from the left lifts the kid's cape out to the right, as far as its max distance
    // lets it go.
    call(session, "component.set",
         R"({"id": "breeze", "components": {"WindSource2D": {"enabled": true}}})");
    step(session, 120);
    call(session, "physics.soft_body", R"({"id": "kid", "at": [1, 29]})");
    return 0;
}
