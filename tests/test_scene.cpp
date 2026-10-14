#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace demo {

struct Health {
    float current = 100.0F;
    float max = 100.0F;
};
SLASH_COMPONENT(Health, current, max)

struct Velocity {
    slash::Vec2 linear;
};
SLASH_COMPONENT(Velocity, linear)

struct Follow {
    entt::entity target = entt::null;
    float distance = 1.0F;
};
SLASH_COMPONENT(Follow, target, distance)

struct Stat {
    std::string name;
    int value = 0;
};
SLASH_REFLECT(Stat, name, value)

struct Loadout {
    std::vector<std::string> items;
    std::vector<Stat> stats;
    std::uint8_t level = 1;
};
SLASH_COMPONENT(Loadout, items, stats, level)

struct Enemy {};
SLASH_COMPONENT(Enemy)

} // namespace demo

namespace json = slash::json;
namespace world = slash::world;
using slash::Vec2;
using world::Transform2D;
using world::World;

namespace {

world::ComponentRegistry make_registry() {
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    registry.add<demo::Health>()
        .set_doc("Hit points.")
        .describe("current", {.doc = "Hit points left.", .min = 0.0});
    registry.add<demo::Velocity>();
    registry.add<demo::Follow>();
    registry.add<demo::Loadout>();
    registry.add<demo::Enemy>();
    return registry;
}

json::Value parse(const std::string& text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return parsed.value();
}

} // namespace

TEST_CASE("saving writes only fields that differ from their defaults") {
    const auto registry = make_registry();
    World game(registry);
    const entt::entity player = game.create("player").value();
    game.ecs().emplace<Transform2D>(player, Transform2D{.position = {1.5F, -2.0F}});
    game.ecs().emplace<demo::Health>(player, demo::Health{.current = 50.0F});
    const entt::entity grunt = game.create("grunt").value();
    game.ecs().emplace<demo::Enemy>(grunt);
    game.ecs().emplace<Transform2D>(grunt);

    CHECK(json::write(world::save_scene(game)) == R"({
  "entities": {
    "grunt": {
      "components": {
        "Enemy": {},
        "Transform2D": {}
      }
    },
    "player": {
      "components": {
        "Health": {
          "current": 50.0
        },
        "Transform2D": {
          "position": [1.5, -2.0]
        }
      }
    }
  },
  "format": 1
})");
}

TEST_CASE("a saved scene loads back into an identical world") {
    const auto registry = make_registry();
    const json::Value scene = parse(R"({
        "format": 1,
        "entities": {
            "leader": {"components": {"Transform3D": {"position": [1, 2, 3],
                                                      "rotation": [0, 0.7071068, 0, 0.7071068]}}},
            "pet": {"components": {
                "Follow": {"target": "leader", "distance": 2.5},
                "Loadout": {"items": ["bone", "ball"], "level": 4,
                            "stats": [{"name": "speed", "value": 7}]},
                "Enemy": {}
            }},
            "empty": {}
        }
    })");
    World first(registry);
    auto loaded = world::load_scene(first, scene);
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? "" : loaded.error().message));
    CHECK(loaded.value() == 3);

    const entt::entity pet = first.find("pet");
    CHECK(first.ecs().get<demo::Follow>(pet).target == first.find("leader"));
    const auto& loadout = first.ecs().get<demo::Loadout>(pet);
    CHECK(loadout.items == std::vector<std::string>{"bone", "ball"});
    CHECK(loadout.stats.at(0).name == "speed");
    CHECK(loadout.level == 4);
    CHECK(first.ecs().all_of<demo::Enemy>(pet));

    const json::Value saved = world::save_scene(first);
    World second(registry);
    REQUIRE(world::load_scene(second, saved).ok());
    CHECK(world::save_scene(second) == saved);
}

TEST_CASE("assign changes only the fields it is given") {
    const auto registry = make_registry();
    World game(registry);
    const entt::entity player = game.create("player").value();
    game.ecs().emplace<demo::Health>(player, demo::Health{.current = 10.0F, .max = 20.0F});
    std::string path;
    REQUIRE(registry.find("Health")->assign(game, player, parse(R"({"max": 30})"), path).ok());
    CHECK(game.ecs().get<demo::Health>(player).current == 10.0F);
    CHECK(game.ecs().get<demo::Health>(player).max == 30.0F);

    auto failed = registry.find("Health")->assign(game, player, parse(R"({"max": "x"})"), path);
    REQUIRE_FALSE(failed.ok());
    CHECK(path == "/max");
    CHECK(game.ecs().get<demo::Health>(player).max == 30.0F); // unchanged
}

TEST_CASE("the scene schema carries docs, defaults and ranges") {
    const json::Value schema = world::scene_schema(make_registry());
    const json::Value& definitions = *schema.find("$defs");

    const json::Value& health = *definitions.find("Health");
    CHECK(health.find("description")->as_string() == "Hit points.");
    const json::Value& current = *health.find("properties")->find("current");
    CHECK(current.find("type")->as_string() == "number");
    CHECK(current.find("description")->as_string() == "Hit points left.");
    CHECK(*current.find("default") == json::Value(100.0));
    CHECK(*current.find("minimum") == json::Value(0.0));

    const json::Value& level = *definitions.find("Loadout")->find("properties")->find("level");
    CHECK(*level.find("maximum") == json::Value(255));
    CHECK(definitions.find("Stat") != nullptr);
    const json::Value& stats = *definitions.find("Loadout")->find("properties")->find("stats");
    CHECK(stats.find("items")->find("$ref")->as_string() == "#/$defs/Stat");

    const json::Value& rotation =
        *definitions.find("Transform3D")->find("properties")->find("rotation");
    CHECK(*rotation.find("items")->find("minimum") == json::Value(-1.0));
}

TEST_CASE("validation lists every schema error with its path") {
    const auto registry = make_registry();
    const auto errors = world::validate_scene(registry, parse(R"({
        "format": 1,
        "entities": {
            "hero": {"components": {
                "Healht": {},
                "Health": {"current": -5},
                "Loadout": {"level": 300, "items": [1]}
            }}
        }
    })"));
    REQUIRE(errors.size() == 4);
    CHECK(errors[0].path == "/entities/hero/components/Healht");
    CHECK(errors[0].message == R"(unknown property "Healht"; did you mean "Health"?)");
    CHECK(errors[1].path == "/entities/hero/components/Health/current");
    CHECK(errors[1].message == "must be at least 0.0, got -5");
    CHECK(errors[2].path == "/entities/hero/components/Loadout/items/0");
    CHECK(errors[3].path == "/entities/hero/components/Loadout/level");
    CHECK(errors[3].message == "must be at most 255, got 300");
}

TEST_CASE("validation reports references to entities that do not exist") {
    const auto errors = world::validate_scene(make_registry(), parse(R"({
        "format": 1,
        "entities": {"pet": {"components": {"Follow": {"target": "ghost"}}}}
    })"));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/entities/pet/components/Follow/target");
    CHECK(errors[0].message == R"(no entity has the id "ghost")");
}

TEST_CASE("a failed load leaves the world unchanged") {
    const auto registry = make_registry();
    World game(registry);
    REQUIRE(game.create("existing").ok());

    auto bad_reference = world::load_scene(game, parse(R"({
        "format": 1,
        "entities": {"a": {}, "b": {"components": {"Follow": {"target": "ghost"}}}}
    })"));
    REQUIRE_FALSE(bad_reference.ok());
    CHECK(bad_reference.error().message ==
          R"(/entities/b/components/Follow/target: no entity has the id "ghost")");
    CHECK(game.size() == 1);

    auto clash = world::load_scene(game, parse(R"({"format": 1, "entities": {"existing": {}}})"));
    REQUIRE_FALSE(clash.ok());
    CHECK(clash.error().message == R"(/entities/existing: entity id "existing" is already in use)");

    auto invalid = world::load_scene(game, parse(R"({"format": 1, "entities": {"bad id": {}}})"));
    REQUIRE_FALSE(invalid.ok());
    CHECK(game.size() == 1);
}

TEST_CASE("format_scene rewrites any valid scene in canonical form") {
    auto formatted = world::format_scene(
        make_registry(),
        R"({"format":1,"entities":{"b":{"components":{"Health":{"max":100,"current":7}}},"a":{}}})");
    REQUIRE(formatted.ok());
    CHECK(formatted.value() == R"({
  "entities": {
    "a": {},
    "b": {
      "components": {
        "Health": {
          "current": 7.0
        }
      }
    }
  },
  "format": 1
}
)");
    CHECK_FALSE(world::format_scene(make_registry(), "{").ok());
}

TEST_CASE("headless runs of the same scene are deterministic") {
    const auto registry = make_registry();
    const json::Value scene = parse(R"({
        "format": 1,
        "entities": {
            "ball": {"components": {"Transform2D": {}, "Velocity": {"linear": [60, -30]}}},
            "wall": {"components": {"Transform2D": {"position": [300, 0]}}}
        }
    })");
    const auto simulate = [&] {
        World game(registry);
        REQUIRE(world::load_scene(game, scene).ok());
        game.add_system("movement", [](World& w, double dt) {
            const auto view = w.ecs().view<Transform2D, demo::Velocity>();
            for (const auto [entity, transform, velocity] : view.each()) {
                transform.position = transform.position + velocity.linear * static_cast<float>(dt);
            }
        });
        for (int i = 0; i < 60; ++i) {
            game.step(1.0 / 60.0);
        }
        return world::save_scene(game);
    };
    const json::Value first = simulate();
    CHECK(first == simulate());
    const auto& position = first.find("entities")
                               ->find("ball")
                               ->find("components")
                               ->find("Transform2D")
                               ->find("position")
                               ->as_array();
    CHECK(position[0].as_double() == doctest::Approx(60.0).epsilon(0.001));
    CHECK(position[1].as_double() == doctest::Approx(-30.0).epsilon(0.001));
}
