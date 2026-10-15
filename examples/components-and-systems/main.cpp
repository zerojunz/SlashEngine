// A game's own component and the system that moves it, run headless for one second.
//
// Gameplay code is plain C++: a component is a struct reflected with SLASH_COMPONENT, and a
// system is a function the world runs every tick. Registering the component is what lets scenes
// save and load it and lets engine.describe and agents see it.

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <cstdio>

namespace game {

struct Velocity {
    slash::Vec2 value;
};
SLASH_COMPONENT(Velocity, value)

} // namespace game

int main() {
    using slash::world::ComponentRegistry;
    using slash::world::Transform2D;
    using slash::world::World;

    ComponentRegistry components;
    slash::world::register_builtin_components(components);
    components.add<game::Velocity>()
        .set_doc("How fast an entity moves.")
        .describe("value", {.doc = "Pixels per second, as [x, y]."});

    // A scene is data. A game loads it from a .scene.json file; here it is inline.
    auto scene = slash::json::parse(R"({
        "entities": {
            "ball": {"components": {"Transform2D": {}, "Velocity": {"value": [32, -16]}}},
            "wall": {"components": {"Transform2D": {"position": [100, 0]}}}
        },
        "format": 1
    })");
    World world(components);
    if (!scene || !slash::world::load_scene(world, scene.value())) {
        return 1;
    }

    // Systems run once per tick, lowest priority first.
    world.add_system("movement",
                     [](World& w, double tick_seconds) {
                         const auto seconds = static_cast<float>(tick_seconds);
                         w.ecs().view<Transform2D, const game::Velocity>().each(
                             [seconds](Transform2D& transform, const game::Velocity& velocity) {
                                 transform.position = transform.position + velocity.value * seconds;
                             });
                     },
                     {.doc = "Moves entities by their velocity."});

    // One second at 64 ticks per second. Headless, a game runs as fast as it can.
    for (int tick = 0; tick < 64; ++tick) {
        world.step(1.0 / 64.0);
    }
    std::puts(slash::json::write(slash::world::save_scene(world)).c_str());
    return 0;
}
