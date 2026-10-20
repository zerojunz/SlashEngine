#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using slash::world::ComponentRegistry;
using slash::world::World;

namespace stage_test {

struct Heat {
    float value = 0.0F;
};
SLASH_COMPONENT(Heat, value)

struct Charge {
    float value = 0.0F;
};
SLASH_COMPONENT(Charge, value)

} // namespace stage_test

TEST_CASE("entities are created and found by stable ID") {
    const ComponentRegistry registry;
    World world(registry);
    auto player = world.create("player");
    REQUIRE(player.ok());
    CHECK(world.find("player") == player.value());
    CHECK(world.id_of(player.value()) == "player");
    CHECK(world.size() == 1);
    CHECK(world.find("nobody") == entt::entity{entt::null});
}

TEST_CASE("IDs must be well formed and unique") {
    const ComponentRegistry registry;
    World world(registry);
    REQUIRE(world.create("enemy_03").ok());
    auto duplicate = world.create("enemy_03");
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error().message == R"(entity id "enemy_03" is already in use)");
    CHECK_FALSE(world.create("").ok());
    CHECK_FALSE(world.create("has space").ok());
    CHECK_FALSE(world.create(std::string(65, 'a')).ok());
    CHECK(World::is_valid_id("Boss-2_final"));
}

TEST_CASE("an instance's entities have IDs under its own and go with it") {
    const ComponentRegistry registry;
    World world(registry);
    CHECK(World::is_valid_part_id("hero/sprite"));
    CHECK(World::is_valid_part_id("hero/gun/muzzle"));
    CHECK_FALSE(World::is_valid_part_id("hero"));
    CHECK_FALSE(World::is_valid_part_id("hero/"));
    CHECK_FALSE(World::is_valid_part_id("/sprite"));
    CHECK_FALSE(World::is_valid_part_id("hero//sprite"));
    CHECK_FALSE(World::is_valid_part_id("hero/" + std::string(251, 'a')));
    CHECK_FALSE(world.create("hero/sprite").ok());
    CHECK_FALSE(world.create_part("hero").ok());

    const entt::entity hero = world.create("hero").value();
    REQUIRE(world.create_part("hero/sprite").ok());
    REQUIRE(world.create_part("hero/gun/muzzle").ok());
    REQUIRE(world.create("hero-2").ok());
    REQUIRE(world.create("heroine").ok());
    world.destroy(hero);
    CHECK(world.ids() == std::vector<std::string_view>{"hero-2", "heroine"});
}

TEST_CASE("generated IDs skip ones already taken") {
    const ComponentRegistry registry;
    World world(registry);
    REQUIRE(world.create("entity_1").ok());
    const entt::entity generated = world.create();
    CHECK(world.id_of(generated) == "entity_2");
}

TEST_CASE("destroying an entity frees its ID, even through the raw registry") {
    const ComponentRegistry registry;
    World world(registry);
    const entt::entity a = world.create("a").value();
    const entt::entity b = world.create("b").value();
    world.destroy(a);
    world.ecs().destroy(b);
    CHECK(world.size() == 0);
    CHECK(world.find("a") == entt::entity{entt::null});
    CHECK(world.create("b").ok());
    CHECK(world.id_of(a).empty());
}

TEST_CASE("systems run in the order they were added, once per step") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> calls;
    world.add_system("input", [&](World& w, double dt) {
        calls.push_back("input " + std::to_string(w.tick()) + " " + std::to_string(dt));
    });
    world.add_system("physics",
                     [&](World& /*w*/, double /*dt*/) { calls.emplace_back("physics"); });
    world.step(0.5);
    world.step(0.5);
    CHECK(world.tick() == 2);
    CHECK(calls ==
          std::vector<std::string>{"input 0 0.500000", "physics", "input 1 0.500000", "physics"});
    CHECK(world.system_names() == std::vector<std::string_view>{"input", "physics"});
}

TEST_CASE("systems run in order of priority, then in the order they were added") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> calls;
    const auto record = [&](std::string name) {
        return [&calls, name](World& /*w*/, double /*dt*/) { calls.push_back(name); };
    };
    world.add_system("render", record("render"), {.doc = "Draws.", .priority = 100});
    world.add_system("movement", record("movement"));
    world.add_system("input", record("input"), {.priority = -10});
    world.add_system("collisions", record("collisions"));
    world.step(0.5);
    CHECK(calls == std::vector<std::string>{"input", "movement", "collisions", "render"});

    const std::vector<slash::world::SystemInfo> systems = world.systems();
    REQUIRE(systems.size() == 4);
    CHECK(systems.back().name == "render");
    CHECK(systems.back().doc == "Draws.");
    CHECK(systems.back().priority == 100);
    CHECK(systems.front().priority == -10);
}

TEST_CASE("systems see each event once, at the tick after it came, whatever their order") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> early;
    std::vector<std::string> late;
    const auto record = [](std::vector<std::string>& seen) {
        return [&seen](World& w, double /*dt*/) {
            for (const slash::world::Event& event : w.events()) {
                seen.push_back(std::to_string(w.tick()) + " " + event.kind + " " + event.id + " " +
                               std::to_string(event.tick));
            }
        };
    };
    world.add_system("early", record(early), {.priority = -10});
    world.add_system("emitter", [](World& w, double /*dt*/) {
        if (w.tick() == 1) {
            w.emit("hit", "coin", slash::json::Value(slash::json::Object{{"damage", 2}}));
        }
    });
    world.add_system("late", record(late), {.priority = 10});
    // Events between ticks, such as a loaded scene's entities, come at the next tick.
    REQUIRE(world.create("coin").ok());
    world.step(0.5);
    world.step(0.5);
    CHECK(world.events().empty());
    world.step(0.5);
    REQUIRE(world.events().size() == 1);
    CHECK(world.events().front().data == slash::json::Value(slash::json::Object{{"damage", 2}}));
    world.step(0.5);
    const std::vector<std::string> expected{"0 entity.created coin 0", "2 hit coin 1"};
    CHECK(early == expected);
    CHECK(late == expected);

    // A rolled back command's events don't come; reset_state forgets them all.
    World::EventQueues before = world.event_queues();
    world.destroy(world.find("coin"));
    world.set_event_queues(std::move(before));
    world.step(0.5);
    CHECK(world.events().empty());
    world.emit("hit", "coin");
    world.reset_state();
    world.step(0.5);
    CHECK(world.events().empty());
}

TEST_CASE("reset_state forgets what systems keep, and the step listener hears each tick first") {
    const ComponentRegistry registry;
    World world(registry);
    int counted = 0;
    world.add_system("count", [&](World& w, double /*dt*/) { counted += w.stepping() ? 1 : 100; });
    world.add_reset([&](World& /*w*/) { counted = 0; });
    std::vector<std::string> heard;
    world.set_step_listener([&](World& w, double dt) {
        heard.push_back(std::to_string(w.tick()) + " " + std::to_string(dt) +
                        (w.stepping() ? " stepping" : ""));
    });
    REQUIRE(world.create("player").ok());
    world.step(0.5);
    world.step(0.25);
    CHECK(counted == 2);
    CHECK(heard == std::vector<std::string>{"0 0.500000", "1 0.250000"});

    // The entities and the tick stay.
    world.reset_state();
    CHECK(counted == 0);
    CHECK(world.size() == 1);
    CHECK(world.tick() == 2);

    world.set_step_listener({});
    world.step(0.5);
    CHECK(heard.size() == 2);
}

TEST_CASE("deferred calls run after the system that made them, and at once outside systems") {
    const ComponentRegistry registry;
    World world(registry);
    std::vector<std::string> calls;
    world.add_system("first", [&](World& w, double /*dt*/) {
        w.defer([&](World& later) {
            calls.push_back("deferred " + std::to_string(later.size()));
            later.defer([&](World& /*again*/) { calls.emplace_back("deferred again"); });
            REQUIRE(later.create("made").ok());
        });
        calls.emplace_back("first");
    });
    world.add_system("second", [&](World& w, double /*dt*/) {
        calls.push_back("second " + std::to_string(w.size()));
    });
    world.step(0.5);
    CHECK(calls == std::vector<std::string>{"first", "deferred 0", "deferred again", "second 1"});
    world.defer([&](World& /*w*/) { calls.emplace_back("now"); });
    CHECK(calls.back() == "now");
}

TEST_CASE("threaded systems next to each other share a stage unless they clash") {
    const ComponentRegistry registry;
    World world(registry);
    const auto none = [](World& /*w*/, double /*dt*/) {};
    world.add_system("a", none,
                     {.priority = 1, .threaded = true, .reads = {"Heat"}, .writes = {"Charge"}});
    // Reading what another reads is no clash.
    world.add_system("b", none, {.priority = 2, .threaded = true, .reads = {"Heat"}});
    // a writes what c reads.
    world.add_system("c", none, {.priority = 3, .threaded = true, .reads = {"Charge"}});
    world.add_system("d", none, {.priority = 4});
    // A system that isn't threaded runs alone, so e starts a stage; f writes other state.
    world.add_system("e", none, {.priority = 5, .threaded = true, .writes = {"Heat"}});
    world.add_system("f", none, {.priority = 6, .threaded = true, .writes = {"assets"}});
    std::vector<std::string> stages;
    for (const slash::world::SystemInfo& system : world.systems()) {
        stages.push_back(std::string(system.name) + " " + std::to_string(system.stage) +
                         (system.threaded ? " threaded" : ""));
    }
    CHECK(stages == std::vector<std::string>{"a 0 threaded", "b 0 threaded", "c 1 threaded", "d 2",
                                             "e 3 threaded", "f 3 threaded"});
    REQUIRE(world.remove_system("c"));
    CHECK(world.systems()[2].name == "d");
    CHECK(world.systems()[2].stage == 1);
}

namespace {

// Ticks of threaded work that writes components, emits events and makes an entity, as one
// line per event and the components after.
std::vector<std::string> run_threaded(int threads) {
    ComponentRegistry registry;
    registry.add<stage_test::Heat>();
    registry.add<stage_test::Charge>();
    slash::jobs::Pool pool(threads);
    World world(registry);
    world.set_pool(pool);
    std::vector<std::string> lines;
    world.set_event_listener([&](const slash::world::Event& event) {
        lines.push_back(std::to_string(event.tick) + " " + event.kind +
                        (event.id.empty() ? "" : " " + event.id));
    });
    for (int i = 0; i < 20; ++i) {
        const entt::entity entity = world.create("e" + std::to_string(i)).value();
        world.ecs().emplace<stage_test::Heat>(entity, static_cast<float>(i));
        world.ecs().emplace<stage_test::Charge>(entity);
    }
    world.add_system("warm",
                     [](World& w, double /*dt*/) {
                         for (auto [entity, heat] : w.ecs().view<stage_test::Heat>().each()) {
                             heat.value += 1.5F;
                             if (heat.value > 25.0F && heat.value <= 26.5F) {
                                 w.emit("hot", w.id_of(entity));
                             }
                         }
                         if (w.tick() == 4) {
                             w.defer([](World& later) { REQUIRE(later.create("spark").ok()); });
                         }
                     },
                     {.threaded = true, .writes = {"Heat"}});
    world.add_system("charge",
                     [](World& w, double /*dt*/) {
                         auto view = w.ecs().view<stage_test::Charge>();
                         const std::vector<entt::entity> entities(view.begin(), view.end());
                         w.pool().parallel_for(entities.size(), [&](std::size_t i) {
                             w.ecs().get<stage_test::Charge>(entities[i]).value +=
                                 static_cast<float>(i);
                         });
                         w.emit("charged");
                     },
                     {.threaded = true, .writes = {"Charge"}});
    world.add_system("count", [&lines](World& w, double /*dt*/) {
        lines.push_back(std::to_string(w.tick()) + " count " + std::to_string(w.size()));
    });
    for (int i = 0; i < 5; ++i) {
        world.step(0.5);
    }
    for (auto [entity, heat, charge] :
         world.ecs().view<stage_test::Heat, stage_test::Charge>().each()) {
        lines.push_back(std::string(world.id_of(entity)) + " " + std::to_string(heat.value) + " " +
                        std::to_string(charge.value));
    }
    return lines;
}

} // namespace

TEST_CASE("threaded systems run the same ticks with any number of threads") {
    const std::vector<std::string> alone = run_threaded(0);
    CHECK(run_threaded(1) == alone);
    CHECK(run_threaded(3) == alone);
    // A stage's events and deferred calls go out in system order once it is done: warm's, then
    // the entity it made, then charge's.
    const auto spark = std::ranges::find(alone, "4 entity.created spark");
    REQUIRE(spark - alone.begin() >= 2);
    REQUIRE(alone.end() - spark >= 3);
    const std::vector<std::string> tick_four(spark - 2, spark + 3);
    CHECK(tick_four == std::vector<std::string>{"4 hot e19", "4 hot e18", "4 entity.created spark",
                                                "4 charged", "4 count 21"});
}
