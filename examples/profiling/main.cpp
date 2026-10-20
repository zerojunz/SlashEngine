// Finding the slow system with perf.stats, then making it fast.
//
// A flock of 600 boids steers by its neighbours. The first flock system compares every boid with
// every other; perf.stats, which every session has, names it as the slowest system. Swapping in
// one that sorts the boids into a grid first, so each looks only at the cells around it, makes it
// faster, and perf.stats shows by how much. Times vary by machine, so this prints what they say,
// not the times themselves.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flock {

struct Boid {
    slash::Vec2 velocity;
};
SLASH_COMPONENT(Boid, velocity)

} // namespace flock

namespace {

namespace command = slash::command;
namespace json = slash::json;
using slash::Vec2;
using slash::world::Transform2D;
using slash::world::World;

constexpr int kBoids = 600;
constexpr float kSize = 800.0F;
constexpr float kRadius = 24.0F; // how far a boid sees
constexpr float kMaxSpeed = 60.0F;

struct Neighbours {
    Vec2 center;
    Vec2 away;
    int count = 0;
};

// What one boid makes of another.
void see(Neighbours& seen, Vec2 self, Vec2 other) {
    const Vec2 d = other - self;
    const float distance = (d.x * d.x) + (d.y * d.y);
    if (distance > 0.0F && distance < kRadius * kRadius) {
        seen.center = seen.center + other;
        seen.away = seen.away - (d * (1.0F / distance));
        ++seen.count;
    }
}

// Turns each boid towards its neighbours' center and away from the closest, then moves it.
void steer(World& world, const std::vector<entt::entity>& boids,
           const std::vector<Neighbours>& seen, float seconds) {
    for (std::size_t i = 0; i < boids.size(); ++i) {
        auto& transform = world.ecs().get<Transform2D>(boids[i]);
        auto& boid = world.ecs().get<flock::Boid>(boids[i]);
        if (seen[i].count > 0) {
            const Vec2 center = seen[i].center * (1.0F / static_cast<float>(seen[i].count));
            boid.velocity =
                boid.velocity + ((center - transform.position) * 0.02F) + (seen[i].away * 40.0F);
        }
        const float speed =
            std::sqrt((boid.velocity.x * boid.velocity.x) + (boid.velocity.y * boid.velocity.y));
        if (speed > kMaxSpeed) {
            boid.velocity = boid.velocity * (kMaxSpeed / speed);
        }
        const Vec2 to = transform.position + (boid.velocity * seconds);
        transform.position = {std::fmod(to.x + kSize, kSize), std::fmod(to.y + kSize, kSize)};
    }
}

std::vector<entt::entity> all_boids(World& world) {
    auto view = world.ecs().view<flock::Boid, Transform2D>();
    return {view.begin(), view.end()};
}

// Every boid looks at every other: 600 x 600 checks a tick.
void flock_every_pair(World& world, double tick_seconds) {
    const std::vector<entt::entity> boids = all_boids(world);
    std::vector<Neighbours> seen(boids.size());
    for (std::size_t i = 0; i < boids.size(); ++i) {
        const Vec2 self = world.ecs().get<Transform2D>(boids[i]).position;
        for (const entt::entity other : boids) {
            see(seen[i], self, world.ecs().get<Transform2D>(other).position);
        }
    }
    steer(world, boids, seen, static_cast<float>(tick_seconds));
}

// Boids go into cells as wide as they see, so each looks only at the nine cells around it.
void flock_by_grid(World& world, double tick_seconds) {
    const std::vector<entt::entity> boids = all_boids(world);
    const auto cell_of = [](Vec2 at) {
        return std::pair{static_cast<std::int64_t>(at.x / kRadius),
                         static_cast<std::int64_t>(at.y / kRadius)};
    };
    const auto key = [](std::int64_t x, std::int64_t y) { return (x << 32) ^ (y & 0xffffffff); };
    std::unordered_map<std::int64_t, std::vector<Vec2>> cells;
    std::vector<Vec2> positions;
    positions.reserve(boids.size());
    for (const entt::entity boid : boids) {
        positions.push_back(world.ecs().get<Transform2D>(boid).position);
        const auto [x, y] = cell_of(positions.back());
        cells[key(x, y)].push_back(positions.back());
    }
    std::vector<Neighbours> seen(boids.size());
    for (std::size_t i = 0; i < boids.size(); ++i) {
        const auto [cx, cy] = cell_of(positions[i]);
        for (std::int64_t y = cy - 1; y <= cy + 1; ++y) {
            for (std::int64_t x = cx - 1; x <= cx + 1; ++x) {
                if (const auto found = cells.find(key(x, y)); found != cells.end()) {
                    for (const Vec2 other : found->second) {
                        see(seen[i], positions[i], other);
                    }
                }
            }
        }
    }
    steer(world, boids, seen, static_cast<float>(tick_seconds));
}

json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    command::Reply reply = session.call(name, json::parse(params).value());
    if (!reply.ok) {
        std::printf("%.*s failed: %s\n", static_cast<int>(name.size()), name.data(),
                    reply.message.c_str());
    }
    return std::move(reply.value);
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    components.add<flock::Boid>().set_doc("Flies with the flock.");
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    World& world = session.world();

    std::uint32_t seed = 7;
    const auto random = [&seed] {
        seed = (seed * 1664525U) + 1013904223U;
        return static_cast<float>(seed >> 8) / static_cast<float>(1U << 24);
    };
    for (int i = 0; i < kBoids; ++i) {
        const entt::entity boid = world.create("boid" + std::to_string(i)).value();
        world.ecs().emplace<Transform2D>(boid).position = {random() * kSize, random() * kSize};
        world.ecs().emplace<flock::Boid>(boid).velocity = {(random() * 60.0F) - 30.0F,
                                                           (random() * 60.0F) - 30.0F};
    }

    // Two seconds of the flock, then what took the time.
    world.add_system("flock", flock_every_pair, {.doc = "Steers each boid by its neighbours."});
    call(session, "step", R"({"ticks": 120})");
    const json::Value before = call(session, "perf.stats", R"({"top": 3})");
    const json::Array& systems = before.find("systems")->as_array();
    const json::Value& slowest = systems.front();
    std::printf("perf.stats {\"top\": 3} lists %zu systems, the slowest first\n", systems.size());
    std::printf("  slowest: %s, in stage %lld\n", slowest.find("name")->as_string().c_str(),
                static_cast<long long>(slowest.find("stage")->as_int()));
    const double every_pair_ms = slowest.find("avg")->as_double();
    std::printf("  its max is at least its average: %s\n",
                slowest.find("max")->as_double() >= every_pair_ms ? "yes" : "no");
    std::printf("  a tick takes at least as long as it: %s\n",
                before.find("tick_ms")->find("avg")->as_double() >= every_pair_ms ? "yes" : "no");

    // The same flock, by grid. Systems are swapped between ticks, as a game's code reloads.
    world.remove_system("flock");
    world.add_system("flock", flock_by_grid, {.doc = "Steers each boid by its neighbours."});
    call(session, "step", R"({"ticks": 120})");
    const json::Value after = call(session, "perf.stats", R"({"top": 20})");
    for (const json::Value& system : after.find("systems")->as_array()) {
        if (system.find("name")->as_string() == "flock") {
            const double grid_ms = system.find("avg")->as_double();
            std::printf("by grid, flock takes less than a third of the time: %s\n",
                        grid_ms * 3.0 < every_pair_ms ? "yes" : "no");
        }
    }
    return 0;
}
