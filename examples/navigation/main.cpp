// Navigation, as Godot's NavigationServer: main.scene.json has a house of two rooms joined the
// long way round by a corridor and the short way by a hatch (a NavigationLink3D), with a guard
// (a NavigationAgent3D) sent across it, and a yard on the 2D map with a cat (a NavigationAgent2D).
//
// Agents move nothing themselves: a plain C++ system walks each one toward its next path
// position, as a game's would. Then the program asks the navigation commands about the maps, as
// an agent would.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace navigation = slash::navigation;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

constexpr float kGuardSpeed = 3.0F; // meters per second
constexpr float kCatSpeed = 90.0F;  // pixels per second

// Moves from toward to by at most step.
Vec3 toward(Vec3 from, Vec3 to, float step) {
    const Vec3 way = to - from;
    const float length = std::sqrt((way.x * way.x) + (way.y * way.y) + (way.z * way.z));
    return length <= step ? to : from + (way * (step / length));
}

// Walks each agent toward its next path position until its navigation finishes, as a Godot
// script calling get_next_path_position in _physics_process would.
void walk_agents(world::World& world, double seconds) {
    const auto step = static_cast<float>(seconds);
    for (const entt::entity guard : world.ecs().view<world::NavigationAgent3D>()) {
        if (!navigation::is_navigation_finished(world, guard)) {
            auto& transform = world.ecs().get<world::Transform3D>(guard);
            transform.position =
                toward(transform.position, navigation::next_path_position_3d(world, guard),
                       kGuardSpeed * step);
        }
    }
    for (const entt::entity cat : world.ecs().view<world::NavigationAgent2D>()) {
        if (!navigation::is_navigation_finished(world, cat)) {
            auto& transform = world.ecs().get<world::Transform2D>(cat);
            const Vec2 next = navigation::next_path_position_2d(world, cat);
            const Vec3 moved = toward({transform.position.x, transform.position.y, 0.0F},
                                      {next.x, next.y, 0.0F}, kCatSpeed * step);
            transform.position = {moved.x, moved.y};
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

void print_agents(const world::World& world) {
    const auto& guard = world.ecs().get<world::Transform3D>(world.find("guard")).position;
    const auto& cat = world.ecs().get<world::Transform2D>(world.find("cat")).position;
    std::printf("tick %llu: guard at (%.2f, %.2f, %.2f), cat at (%.1f, %.1f)\n",
                static_cast<unsigned long long>(world.tick()), guard.x, guard.y, guard.z, cat.x,
                cat.y);
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
    // Gameplay systems run before the navigation system, which steps at priority 960.
    session.world().add_system("walk_agents", walk_agents, {.doc = "Walks agents along paths."});
    call(session, "events.subscribe",
         R"({"kinds": ["link_reached", "target_reached", "navigation_finished"]})");

    // What the maps are made of: the house's seven polygons and the hatch, and the yard's two.
    call(session, "navigation.map", "{}");
    // The guard's path goes through the hatch: six meters at one and a half times the cost
    // beats walking round.
    call(session, "navigation.agent", R"({"id": "guard"})");

    // Three seconds, a second at a time: the guard crosses the hatch to the far room and the cat
    // walks round the corner of the yard's L.
    for (int second = 0; second < 3; ++second) {
        session.call("step", json::parse(R"({"ticks": 60})").value());
        print_agents(session.world());
    }
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "navigation.agent", R"({"id": "cat"})");

    // Paths and points, as an agent plans with them: with the hatch's owner and type ...
    call(session, "navigation.path", R"({"from": [1, 0, 1], "to": [10, 0, 2], "metadata": true})");
    // ... and with the hatch off layer 1, the long way round: pulled tight round the corners,
    // then through the middle of each edge.
    call(session, "component.set",
         R"({"id": "hatch", "components": {"NavigationLink3D": {"navigation_layers": 2}}})");
    call(session, "navigation.path", R"({"from": [1, 0, 1], "to": [10, 0, 2]})");
    call(session, "navigation.path",
         R"({"from": [1, 0, 1], "to": [10, 0, 2], "postprocessing": "edgecentered"})");
    // Where the map is closest to a point off it, and a random point of the yard.
    call(session, "navigation.closest", R"({"point": [6, 1, 2]})");
    call(session, "navigation.random", R"({"map": "2d", "uniformly": true, "seed": 7})");
    return 0;
}
