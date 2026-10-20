#include "enemy.hpp"

#include "slash/world/components.hpp"

namespace arena {

State next_state(const Enemy& enemy, slash::Vec2 from, slash::Vec2 to) {
    const slash::Vec2 offset = to - from;
    const float distance_squared = (offset.x * offset.x) + (offset.y * offset.y);
    return distance_squared <= enemy.sight * enemy.sight ? State::Chase : State::Idle;
}

void chase(slash::world::World& world, double tick_seconds) {
    const entt::entity player = world.find("player");
    if (player == entt::null) {
        return;
    }
    const slash::Vec2 target = world.ecs().get<slash::world::Transform2D>(player).position;
    const auto step = static_cast<float>(tick_seconds);
    world.ecs().view<Enemy, slash::world::Transform2D>().each(
        [&](Enemy& enemy, slash::world::Transform2D& transform) {
            enemy.chasing = next_state(enemy, transform.position, target) == State::Chase;
            if (enemy.chasing) {
                const slash::Vec2 offset = target - transform.position;
                transform.position = transform.position + (offset * (step * 0.5F));
            }
        });
}

} // namespace arena
