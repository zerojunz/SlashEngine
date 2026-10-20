// The runner: moves, jumps, and cheers when it collects a coin.
#include "runner.hpp"

#include "slash/input/input.hpp"

namespace game {

void move_runner(slash::world::World& world, double /*tick_seconds*/) {
    if (slash::input::is_action_just_pressed(world, "jump")) {
        world.emit("jumped", "runner");
    }
    for (const slash::world::Event& event : world.events()) {
        if (event.kind == "coin_collected") {
            world.emit("cheered", "runner");
        }
    }
}

} // namespace game
