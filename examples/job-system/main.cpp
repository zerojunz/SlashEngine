// Systems on the job system's threads, as Godot's WorkerThreadPool and sub-thread process groups.
//
// A meadow: plants grow and drop seeds, grazers get hungry, wander and eat. grow and hunger are
// threaded systems whose reads and writes don't clash, so they run at the same time; wander
// writes the transforms grow reads, so it runs after them; graze is an ordinary system. grow
// shares its plants out over the pool with parallel_for and makes seedlings through
// World::defer. The same ticks run with no worker threads and with four, and come out the same.

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace meadow {

struct Plant {
    float growth = 0.0F;
};
SLASH_COMPONENT(Plant, growth)

struct Grazer {
    float hunger = 0.0F;
    float rate = 0.01F;
    slash::Vec2 velocity;
};
SLASH_COMPONENT(Grazer, hunger, rate, velocity)

} // namespace meadow

namespace {

using slash::Vec2;
using slash::world::Transform2D;
using slash::world::World;

constexpr float kSize = 400.0F;
constexpr std::size_t kMaxPlants = 300;

float wrap(float x) {
    if (x < 0.0F) {
        return x + kSize;
    }
    return x >= kSize ? x - kSize : x;
}

void add_systems(World& world) {
    world.add_system("grow",
                     [](World& w, double /*tick_seconds*/) {
                         auto view = w.ecs().view<meadow::Plant, Transform2D>();
                         const std::vector<entt::entity> plants(view.begin(), view.end());
                         // Each plant grows alone, so they share out over the pool's threads.
                         std::vector<char> ripe(plants.size(), 0);
                         w.pool().parallel_for(plants.size(), [&](std::size_t i) {
                             auto& plant = w.ecs().get<meadow::Plant>(plants[i]);
                             plant.growth += 0.004F;
                             if (plant.growth >= 1.0F) {
                                 plant.growth = 0.5F;
                                 ripe[i] = 1;
                             }
                         });
                         // A ripe plant drops a seed beside it, made once the stage is done.
                         std::size_t count = plants.size();
                         for (std::size_t i = 0; i < plants.size() && count < kMaxPlants; ++i) {
                             if (ripe[i] == 0) {
                                 continue;
                             }
                             ++count;
                             const auto step = static_cast<std::int64_t>(w.tick() + (i * 13));
                             const Vec2 offset{static_cast<float>((step * 7) % 41) - 20.0F,
                                               static_cast<float>((step * 11) % 41) - 20.0F};
                             const Vec2 from = w.ecs().get<Transform2D>(plants[i]).position;
                             const Vec2 at{wrap(from.x + offset.x), wrap(from.y + offset.y)};
                             w.defer([at](World& later) {
                                 const entt::entity seed = later.create();
                                 later.ecs().emplace<Transform2D>(seed).position = at;
                                 later.ecs().emplace<meadow::Plant>(seed);
                             });
                         }
                     },
                     {.doc = "Grows plants; ripe ones drop seeds.",
                      .priority = 10,
                      .threaded = true,
                      .reads = {"Transform2D"},
                      .writes = {"Plant"}});

    world.add_system(
        "hunger",
        [](World& w, double /*tick_seconds*/) {
            for (auto [entity, grazer] : w.ecs().view<meadow::Grazer>().each()) {
                const bool was_hungry = grazer.hunger >= 1.0F;
                grazer.hunger += grazer.rate;
                if (!was_hungry && grazer.hunger >= 1.0F) {
                    w.emit("hungry", w.id_of(entity));
                }
            }
        },
        {.doc = "Makes grazers hungry.", .priority = 20, .threaded = true, .writes = {"Grazer"}});

    world.add_system("wander",
                     [](World& w, double tick_seconds) {
                         const auto seconds = static_cast<float>(tick_seconds);
                         for (auto [entity, transform, grazer] :
                              w.ecs().view<Transform2D, const meadow::Grazer>().each()) {
                             const Vec2 to = transform.position + (grazer.velocity * seconds);
                             transform.position = {wrap(to.x), wrap(to.y)};
                         }
                     },
                     {.doc = "Moves grazers, wrapping round the meadow.",
                      .priority = 30,
                      .threaded = true,
                      .reads = {"Grazer"},
                      .writes = {"Transform2D"}});

    // An ordinary system: it runs alone on the thread stepping the world, and destroys plants
    // as it goes.
    world.add_system("graze",
                     [](World& w, double /*tick_seconds*/) {
                         auto& ecs = w.ecs();
                         for (auto [grazer_entity, grazer, at] :
                              ecs.view<meadow::Grazer, const Transform2D>().each()) {
                             if (grazer.hunger < 1.0F) {
                                 continue;
                             }
                             entt::entity nearest = entt::null;
                             float best = 30.0F * 30.0F;
                             for (auto [plant_entity, plant, where] :
                                  ecs.view<const meadow::Plant, const Transform2D>().each()) {
                                 const Vec2 d = where.position - at.position;
                                 const float distance = (d.x * d.x) + (d.y * d.y);
                                 if (plant.growth >= 0.5F && distance < best) {
                                     best = distance;
                                     nearest = plant_entity;
                                 }
                             }
                             if (nearest != entt::null) {
                                 w.emit(
                                     "ate", w.id_of(grazer_entity),
                                     slash::json::Object{{"plant", std::string(w.id_of(nearest))}});
                                 w.destroy(nearest);
                                 grazer.hunger = 0.0F;
                             }
                         }
                     },
                     {.doc = "Hungry grazers eat the nearest grown plant.", .priority = 40});
}

struct Meadow {
    std::string scene;
    std::size_t plants = 0;
    std::size_t grazers = 0;
    int meals = 0;
    int hungry = 0;
};

Meadow run(int threads) {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    components.add<meadow::Plant>().set_doc("Grows to 1, then drops a seed.");
    components.add<meadow::Grazer>().set_doc("Wanders, and eats when hungry.");
    // The pool outlives the world that runs on it.
    slash::jobs::Pool pool(threads);
    World world(components);
    world.set_pool(pool);
    Meadow out;
    world.set_event_listener([&out](const slash::world::Event& event) {
        out.meals += event.kind == "ate" ? 1 : 0;
        out.hungry += event.kind == "hungry" ? 1 : 0;
    });
    for (int i = 0; i < 16; ++i) {
        const entt::entity plant = world.create("plant" + std::to_string(i)).value();
        const int column = i % 4;
        const int row = i / 4;
        world.ecs().emplace<Transform2D>(plant).position = {static_cast<float>(column) * 100.0F,
                                                            static_cast<float>(row) * 100.0F};
        world.ecs().emplace<meadow::Plant>(plant).growth = static_cast<float>(i) / 16.0F;
    }
    for (int i = 0; i < 4; ++i) {
        const entt::entity grazer = world.create("grazer" + std::to_string(i)).value();
        world.ecs().emplace<Transform2D>(grazer).position = {
            50.0F + (static_cast<float>(i) * 90.0F), 200.0F};
        world.ecs().emplace<meadow::Grazer>(
            grazer, 0.0F, 0.004F + (static_cast<float>(i) * 0.002F),
            Vec2{20.0F + (static_cast<float>(i) * 7.0F), 15.0F - (static_cast<float>(i) * 9.0F)});
    }
    add_systems(world);
    for (int tick = 0; tick < 600; ++tick) {
        world.step(1.0 / 60.0);
    }
    out.scene = slash::json::write(slash::world::save_scene(world));
    out.plants = world.ecs().view<meadow::Plant>().size();
    out.grazers = world.ecs().view<meadow::Grazer>().size();
    return out;
}

void print(const char* label, const Meadow& meadow) {
    std::printf("%s: %zu plants, %zu grazers, %d times hungry, %d meals\n", label, meadow.plants,
                meadow.grazers, meadow.hungry, meadow.meals);
}

} // namespace

int main() {
    // Which systems run together: threaded neighbours that don't clash share a stage.
    {
        const slash::world::ComponentRegistry components;
        World world(components);
        add_systems(world);
        for (const slash::world::SystemInfo& system : world.systems()) {
            std::string line = "stage " + std::to_string(system.stage) + ": " +
                               std::string(system.name) + (system.threaded ? ", threaded" : "");
            for (const std::string& name : system.reads) {
                line += ", reads " + name;
            }
            for (const std::string& name : system.writes) {
                line += ", writes " + name;
            }
            std::puts(line.c_str());
        }
    }

    // Ten seconds of the meadow, on no worker threads and on four.
    const Meadow alone = run(0);
    const Meadow four = run(4);
    print("no worker threads", alone);
    print("4 worker threads", four);
    std::printf("the same world: %s\n", alone.scene == four.scene ? "yes" : "no");
    return 0;
}
