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
