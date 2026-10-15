// Watch conditions: run a game headless until something happens, instead of stepping and asking.
//
// To test gameplay, an agent wants to know when the ball reaches the floor, or whether it ever
// does. Stepping a tick at a time and querying after each costs a call per tick. run.until steps
// the world until an entity matches a condition, or until none does, and answers once with the
// tick and the matching entities.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace game {

struct Body {
    slash::Vec2 velocity;
};
SLASH_COMPONENT(Body, velocity)

} // namespace game

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

constexpr float kGravity = 1024.0F;  // pixels per second squared, y down
constexpr float kDespawnY = 1000.0F; // bodies below this are gone

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    components.add<game::Body>()
        .set_doc("A falling body.")
        .describe("velocity", {.doc = "Pixels per second, as [x, y]."});
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    world::World& w = session.world();
    w.add_system("gravity", [](world::World& game_world, double tick_seconds) {
        const auto seconds = static_cast<float>(tick_seconds);
        game_world.ecs().view<world::Transform2D, game::Body>().each(
            [seconds](world::Transform2D& transform, game::Body& body) {
                body.velocity.y += kGravity * seconds;
                transform.position = transform.position + body.velocity * seconds;
            });
    });
    w.add_system(
        "despawn",
        [](world::World& game_world, double /*tick_seconds*/) {
            std::vector<entt::entity> gone;
            game_world.ecs().view<const world::Transform2D, const game::Body>().each(
                [&](entt::entity entity, const world::Transform2D& transform, const game::Body&) {
                    if (transform.position.y > kDespawnY) {
                        gone.push_back(entity);
                    }
                });
            for (const entt::entity entity : gone) {
                game_world.destroy(entity);
            }
        },
        {.priority = 1});

    call(session, "batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "ball",
            "components": {"Transform2D": {"position": [40, 0]}, "Body": {}}}},
        {"command": "entity.create", "params": {"id": "floor",
            "components": {"Transform2D": {"position": [0, 600]}}}}]})");
    // When does the ball fall past the floor? Ticks here are 1/64 s; the default is 1/60.
    call(session, "run.until",
         R"({"with": ["Body"], "include": ["Transform2D", "Body"], "omit_defaults": true,
        "where": [{"component": "Transform2D", "field": "position.y", "op": ">", "value": 600}],
        "tick_seconds": 0.015625})");
    // A condition that never holds stops at the tick limit, and matched says so.
    call(session, "run.until", R"({"with": ["Body"], "ticks": 10, "tick_seconds": 0.015625,
        "where": [{"component": "Transform2D", "field": "position.x", "op": "!=", "value": 40}]})");
    // "until": "none" waits for the last match to go: here, for every body to despawn.
    call(session, "run.until", R"({"with": ["Body"], "until": "none", "tick_seconds": 0.015625})");
    // Each run is one undo step, like step: undo brings the ball back.
    call(session, "undo", "{}");
    call(session, "entity.get", R"({"id": "ball", "components": ["Transform2D", "Body"],
        "omit_defaults": true})");
    return 0;
}
