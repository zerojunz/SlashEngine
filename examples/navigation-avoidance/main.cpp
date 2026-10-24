// Avoidance, as Godot's NavigationAgent2D avoidance: main.scene.json has a plaza with a fountain
// and walkers (NavigationAgent2D with avoidance_enabled): two from the west and two from the east
// meeting head on, and a courier crossing them from the north, round the fountain. The courier
// has the highest avoidance_priority, so it makes way for no one and the others make way for it.
// A cart (a NavigationObstacle2D with a velocity) rolls across the plaza on its own.
//
// As in a Godot game, gameplay code asks each agent to go toward its next path position by
// setting its velocity, and moves it at the safe velocity avoidance gives back, which steers it
// round the others, the fountain and the cart. Then the program asks the commands about it, as
// an agent would.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace navigation = slash::navigation;
namespace world = slash::world;
using slash::Vec2;

constexpr float kWalkSpeed = 80.0F; // pixels per second

float length(Vec2 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

// Before the navigation system: each walker asks to head for its next path position, as a
// Godot script setting NavigationAgent2D.velocity in _physics_process would; a walker that has
// arrived asks to stand still.
void steer(world::World& world, double /*seconds*/) {
    for (const auto [entity, agent, transform] :
         world.ecs().view<world::NavigationAgent2D, world::Transform2D>().each()) {
        if (navigation::is_navigation_finished(world, entity)) {
            agent.velocity = {};
            continue;
        }
        const Vec2 way = navigation::next_path_position_2d(world, entity) - transform.position;
        const float distance = length(way);
        agent.velocity = distance > 0.0F ? way * (kWalkSpeed / distance) : Vec2{};
    }
}

// After it: each walker moves at its safe velocity, as a script connected to velocity_computed
// would, and the cart rolls on at its own velocity.
void move(world::World& world, double seconds) {
    const auto step = static_cast<float>(seconds);
    for (const auto [entity, agent, transform] :
         world.ecs().view<world::NavigationAgent2D, world::Transform2D>().each()) {
        transform.position =
            transform.position + (navigation::safe_velocity_2d(world, entity) * step);
    }
    for (const auto [entity, obstacle, transform] :
         world.ecs().view<world::NavigationObstacle2D, world::Transform2D>().each()) {
        transform.position = transform.position + (obstacle.velocity * step);
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

Vec2 position_of(const world::World& world, std::string_view id) {
    return world.ecs().get<world::Transform2D>(world.find(id)).position;
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    // The fountain has affect_navigation_mesh, so baking the plaza cuts it out, grown by the
    // agents' radius, and paths go round it.
    call(session, "navigation.bake", R"({"id": "plaza"})");
    // The navigation system steps at priority 960: steer before it, move after.
    session.world().add_system("steer", steer, {.doc = "Walkers ask for their velocities."});
    session.world().add_system("move", move,
                               {.doc = "Walkers move at their safe velocities.", .priority = 970});
    call(session, "events.subscribe", R"({"kinds": ["target_reached"]})");

    // Six seconds, a second at a time, with how close any two walkers, or a walker and the cart,
    // came: never much nearer than their radii, 10 each and the cart's 15.
    const std::vector<std::string_view> walkers{"courier", "west_1", "west_2", "east_1", "east_2"};
    float closest = std::numeric_limits<float>::max();
    float cart = std::numeric_limits<float>::max();
    for (int second = 1; second <= 6; ++second) {
        for (int tick = 0; tick < 60; ++tick) {
            session.call("step", json::parse(R"({"ticks": 1})").value());
            const world::World& w = session.world();
            for (std::size_t i = 0; i < walkers.size(); ++i) {
                const Vec2 a = position_of(w, walkers[i]);
                cart = std::min(cart, length(a - position_of(w, "cart")));
                for (std::size_t j = i + 1; j < walkers.size(); ++j) {
                    closest = std::min(closest, length(a - position_of(w, walkers[j])));
                }
            }
        }
        std::printf("%d s:", second);
        for (const std::string_view id : walkers) {
            const Vec2 p = position_of(session.world(), id);
            std::printf(" %.*s (%.1f, %.1f)", static_cast<int>(id.size()), id.data(),
                        static_cast<double>(p.x), static_cast<double>(p.y));
        }
        const Vec2 c = position_of(session.world(), "cart");
        std::printf(" cart (%.1f, %.1f)\n", static_cast<double>(c.x), static_cast<double>(c.y));
        if (second == 2) {
            // Passing the walkers from the east, west_1 is given a safe velocity off its path.
            call(session, "navigation.agent", R"({"id": "west_1"})");
        }
    }
    std::printf("closest two walkers came: %.1f; closest a walker came to the cart: %.1f\n",
                static_cast<double>(closest), static_cast<double>(cart));
    call(session, "events.read", R"({"subscription": "events_1"})");
    return 0;
}
