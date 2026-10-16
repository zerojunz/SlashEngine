#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

using slash::world::ComponentRegistry;
using slash::world::World;

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
