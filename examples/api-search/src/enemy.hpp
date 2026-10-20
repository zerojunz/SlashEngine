#pragma once

#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/world.hpp"

namespace arena {

// An enemy that chases the player.
struct Enemy {
    float speed = 90.0F; // Pixels per second.
    float sight = 160.0F;
    bool chasing = false;
};
SLASH_COMPONENT(Enemy, speed, sight, chasing)

// What an enemy does next.
enum class State {
    Idle,  // Waits for the player to come near.
    Chase, // Flies at the player.
};

// What an enemy at from does about the player at to.
State next_state(const Enemy& enemy, slash::Vec2 from, slash::Vec2 to);

// Moves each enemy toward the player while it can see them.
void chase(slash::world::World& world, double tick_seconds);

} // namespace arena
