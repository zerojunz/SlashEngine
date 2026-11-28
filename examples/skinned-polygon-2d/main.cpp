// Skinned polygon 2D: a tentacle whose one polygon bends with a chain of three bones.
//
// main.scene.json holds a Skeleton2D ("tentacle") with three Bone2Ds in a chain under it and a
// Polygon2D ("skin") beside them. skeleton.bind sets each bone's rest to where it is now and
// weights each of the skin's points to the bones nearest it, so the skin is bound in one call.
// The game system below then curls the bones a little more each tick, as an animation would, and
// the skin's points follow: each moves the way its bones moved from rest, blended by weight.
//
// This program plays it as an agent does over MCP or JSON-RPC: scene.open, skeleton.bind, step,
// skeleton.get, and component.set to show the rest pose; the tip of the skin is read with
// world::skin_2d, the points the canvas draws.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/skeleton2d.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdio>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

// Each bone curls by this many radians a second, the tip most.
constexpr std::array<std::string_view, 3> kBones{"root_bone", "mid_bone", "tip_bone"};
constexpr std::array<float, 3> kCurl{0.2F, 0.4F, 0.6F};

void curl(world::World& game_world, double tick_seconds) {
    for (std::size_t i = 0; i < kBones.size(); ++i) {
        const entt::entity bone = game_world.find(kBones[i]);
        if (bone != entt::null) {
            game_world.ecs().get<world::Transform2D>(bone).rotation +=
                kCurl[i] * static_cast<float>(tick_seconds);
        }
    }
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

// Where the skin's tip (its fifth point) draws, in the tentacle's space.
void show_tip(const world::World& game_world) {
    const entt::entity skin = game_world.find("skin");
    const auto& polygon = game_world.ecs().get<const world::Polygon2D>(skin);
    slash::Vec2 tip = polygon.polygon[4];
    if (const auto skinned =
            world::skin_2d(game_world, world::Tree2D(game_world), skin, polygon.polygon)) {
        tip = skinned->points[4];
    }
    std::printf("tip at [%.1f, %.1f]\n", static_cast<double>(tip.x), static_cast<double>(tip.y));
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    session.world().add_system("curl", curl);
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // Bind the skin: rest poses, then weights to the nearest bones.
    call(session, "skeleton.bind", R"({"id": "tentacle", "polygons": ["skin"]})");
    show_tip(session.world());

    // A second of curling: the tip swings down and back toward the root.
    call(session, "step", R"({"ticks": 10, "tick_seconds": 0.1})");
    call(session, "skeleton.get", R"({"id": "tentacle", "bones": ["tip_bone"]})");
    show_tip(session.world());

    // The rest pose, as the skin was bound, then back to the bones' pose.
    call(session, "component.set",
         R"({"id": "tentacle", "components": {"Skeleton2D": {"show_rest_only": true}}})");
    show_tip(session.world());
    call(session, "component.set",
         R"({"id": "tentacle", "components": {"Skeleton2D": {"show_rest_only": false}}})");
    show_tip(session.world());
    return 0;
}
