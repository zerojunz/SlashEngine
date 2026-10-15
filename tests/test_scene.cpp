#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
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

TEST_CASE("diff_scenes lists what changed by entity ID") {
    const auto registry = make_registry();
    const json::Value before = parse(R"({"format": 1, "entities": {
        "player": {"components": {"Transform2D": {"position": [1, 2], "rotation": 0.5},
                                  "Health": {"current": 50}}},
        "grunt": {"components": {"Enemy": {}}},
        "door": {}
    }})");
    // The player's rotation goes back to its default, its health changes and it gains a velocity;
    // the grunt and the door are unchanged, and a crate appears.
    const json::Value after = parse(R"({"format": 1, "entities": {
        "player": {"components": {"Transform2D": {"position": [1, 2]}, "Velocity": {}}},
        "grunt": {"components": {"Enemy": {}}},
        "door": {},
        "crate": {"components": {"Health": {"max": 20}, "Enemy": {}}}
    }})");
    CHECK(world::diff_scenes(registry, before, after) == parse(R"({
        "added": ["crate"],
        "changed": {
            "crate": {"Enemy": {}, "Health": {"max": 20.0}},
            "player": {"Health": null, "Transform2D": {"rotation": 0.0}, "Velocity": {}}
        }
    })"));
    CHECK(world::diff_scenes(registry, after, before) == parse(R"({
        "removed": ["crate"],
        "changed": {
            "player": {"Health": {"current": 50.0}, "Transform2D": {"rotation": 0.5},
                       "Velocity": null}
        }
    })"));
    CHECK(world::diff_scenes(registry, before, before) == json::Value(json::Object{}));
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

namespace {

// Reads prefab files from memory.
world::FileReader memory_files(std::map<std::string, std::string, std::less<>> files) {
    return [files = std::move(files)](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
}

const std::string kEnemyPrefab = R"({"format": 1, "components": {
    "Health": {"current": 50, "max": 50}, "Enemy": {}, "Transform2D": {"rotation": 1}}})";

// The first error loading a scene, as "path: message", or "" if it loads.
std::string load_error(const world::ComponentRegistry& registry, const json::Value& scene,
                       const world::FileReader& reader) {
    const auto errors = world::validate_scene(registry, scene, reader);
    return errors.empty() ? std::string() : errors.front().path + ": " + errors.front().message;
}

} // namespace

TEST_CASE("an entity extends a prefab and overrides its fields") {
    const auto registry = make_registry();
    World world(registry);
    world.set_file_reader(memory_files({{"prefabs/enemy.prefab.json", kEnemyPrefab}}));
    // grunt overrides one field, sets another back to its default and drops a component.
    const json::Value scene = parse(R"({"format": 1, "entities": {
        "grunt": {"extends": "prefabs/enemy.prefab.json",
                  "components": {"Health": {"current": 20, "max": 100}, "Transform2D": null}},
        "plain": {"extends": "prefabs/enemy.prefab.json"}
    }})");
    REQUIRE(world::load_scene(world, scene).ok());

    const auto grunt = world.find("grunt");
    const auto plain = world.find("plain");
    CHECK(world.ecs().get<demo::Health>(grunt).current == 20.0F);
    CHECK(world.ecs().get<demo::Health>(grunt).max == 100.0F);
    CHECK(world.ecs().all_of<demo::Enemy>(grunt));
    CHECK_FALSE(world.ecs().all_of<Transform2D>(grunt));
    CHECK(world.ecs().get<demo::Health>(plain).current == 50.0F);
    CHECK(world.ecs().get<Transform2D>(plain).rotation == 1.0F);
    REQUIRE(world.prefab_of(grunt) != nullptr);
    CHECK(world.prefab_of(grunt)->path == "prefabs/enemy.prefab.json");
    // Every instance shares one copy of the prefab.
    CHECK(world.prefab_of(grunt)->base == world.prefab_of(plain)->base);

    // Saving writes only what differs from the prefab.
    CHECK(world::save_scene(world) == scene);

    world.ecs().get<demo::Health>(plain).current = 50.0F;
    world.ecs().get<Transform2D>(plain).rotation = 0.0F;
    CHECK(*world::save_scene(world).find("entities")->find("plain") == parse(R"({
        "extends": "prefabs/enemy.prefab.json",
        "components": {"Transform2D": {"rotation": 0.0}}})"));
    CHECK(world::save_prefab(world, plain) == parse(R"({"format": 1,
        "extends": "prefabs/enemy.prefab.json",
        "components": {"Transform2D": {"rotation": 0.0}}})"));
    CHECK(world::save_prefab(world, world.create("loose").value()) == parse(R"({"format": 1})"));
}

TEST_CASE("prefabs extend other prefabs") {
    const auto registry = make_registry();
    World world(registry);
    world.set_file_reader(memory_files({
        {"base.prefab.json", R"({"format": 1, "components": {"Health": {"max": 200}}})"},
        {"boss.prefab.json", R"({"format": 1, "extends": "base.prefab.json",
                                 "components": {"Health": {"current": 150}, "Enemy": {}}})"},
    }));
    const auto entity = world.create("boss").value();
    REQUIRE(world::instantiate_prefab(world, entity, "boss.prefab.json").ok());
    CHECK(world.ecs().get<demo::Health>(entity).current == 150.0F);
    CHECK(world.ecs().get<demo::Health>(entity).max == 200.0F);
    CHECK(world.ecs().all_of<demo::Enemy>(entity));
    CHECK(world::save_scene(world) ==
          parse(R"({"format": 1, "entities": {"boss": {"extends": "boss.prefab.json"}}})"));
    CHECK(world.prefab_links().size() == 1);
}

TEST_CASE("prefab errors name the value at fault") {
    const auto registry = make_registry();
    const auto reader = memory_files({
        {"enemy.prefab.json", kEnemyPrefab},
        {"a.prefab.json", R"({"format": 1, "extends": "b.prefab.json"})"},
        {"b.prefab.json", R"({"format": 1, "extends": "a.prefab.json"})"},
        {"bad.prefab.json", R"({"format": 1, "components": {"Health": {"current": "full"}}})"},
    });
    const auto error = [&](const std::string& entity) {
        return load_error(registry, parse(R"({"format": 1, "entities": {"e": )" + entity + "}}"),
                          reader);
    };
    CHECK(error(R"({"extends": "enemy.prefab.json"})").empty());
    CHECK(error(R"({"extends": "missing.prefab.json"})") ==
          "/entities/e/extends: prefab \"missing.prefab.json\" could not be read: no such file");
    CHECK(error(R"({"extends": "a.prefab.json"})") ==
          "/entities/e/extends: prefabs extend each other in a loop: \"a.prefab.json\" extends "
          "\"b.prefab.json\" extends \"a.prefab.json\"");
    CHECK(error(R"({"extends": "bad.prefab.json"})") ==
          "/entities/e/extends: prefab \"bad.prefab.json\" is not valid: "
          "/components/Health/current: expected number, got string");
    CHECK(error(R"({"extends": "enemy.json"})") ==
          "/entities/e/extends: expected a .prefab.json file, got \"enemy.json\"");
    CHECK(error(R"({"extends": "../enemy.prefab.json"})").find("inside the project folder") !=
          std::string::npos);
    CHECK(error(R"({"extends": "enemy.prefab.json", "components": {"Velocity": null}})") ==
          "/entities/e/components/Velocity: null removes a component the prefab has, but "
          "\"enemy.prefab.json\" has no \"Velocity\"");
    CHECK(error(R"({"components": {"Health": null}})") ==
          "/entities/e/components/Health: null removes a component the prefab has, but this "
          "entity extends no prefab");
    CHECK(load_error(registry,
                     parse(R"({"format": 1, "entities": {"e": {"extends": "enemy.prefab.json"}}})"),
                     {}) == "/entities/e/extends: prefab \"enemy.prefab.json\" cannot be read: "
                            "there is no project folder");
}

TEST_CASE("prefab files validate and format like scenes") {
    const auto registry = make_registry();
    const auto reader = memory_files({{"enemy.prefab.json", kEnemyPrefab}});
    CHECK(world::validate_prefab(registry, parse(kEnemyPrefab)).empty());
    const auto errors = world::validate_prefab(
        registry, parse(R"({"format": 1, "components": {"Health": {"current": -1}, "Nope": {}}})"));
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/components/Health/current");
    CHECK(errors[1].path == "/components/Nope");
    CHECK(world::validate_prefab(registry, parse(R"({"format": 1, "extends": "enemy.prefab.json",
                                                    "components": {"Velocity": null}})"),
                                 reader)
              .front()
              .path == "/components/Velocity");

    auto formatted = world::format_prefab(
        registry, R"({"components":{"Health":{"max":100,"current":7}},"format":1})");
    REQUIRE(formatted.ok());
    CHECK(formatted.value() == R"({
  "components": {
    "Health": {
      "current": 7.0
    }
  },
  "format": 1
}
)");
    formatted = world::format_prefab(
        registry,
        R"({"format":1,"extends":"enemy.prefab.json","components":{"Health":{"current":50}}})",
        reader);
    REQUIRE(formatted.ok());
    CHECK(parse(formatted.value()) == parse(R"({"extends": "enemy.prefab.json", "format": 1})"));
    CHECK(world::prefab_schema(registry).find("title")->as_string() == "SlashEngine prefab");
}

TEST_CASE("diff_scenes compares entities through their prefabs") {
    const auto registry = make_registry();
    World world(registry);
    world.set_file_reader(memory_files({{"enemy.prefab.json", kEnemyPrefab}}));
    const json::Value before =
        parse(R"({"format": 1, "entities": {"e": {"extends": "enemy.prefab.json"}}})");
    REQUIRE(world::load_scene(world, before).ok());
    const world::PrefabLinks prefabs = world.prefab_links();
    const json::Value after = parse(R"({"format": 1, "entities": {"e": {
        "extends": "enemy.prefab.json",
        "components": {"Health": {"current": 20}, "Transform2D": null}}}})");
    CHECK(world::diff_scenes(registry, before, after, prefabs, prefabs) ==
          parse(R"({"changed": {"e": {"Health": {"current": 20.0}, "Transform2D": null}}})"));
    CHECK(world::diff_scenes(registry, before, before, prefabs, prefabs) ==
          json::Value(json::Object{}));
    // The prefab's components count as changes for an entity that starts extending it.
    const json::Value plain = parse(R"({"format": 1, "entities": {"e": {}}})");
    CHECK(world::diff_scenes(registry, plain, before, {}, prefabs) ==
          parse(R"({"changed": {"e": {"Enemy": {}, "Health": {"current": 50.0, "max": 50.0},
                                      "Transform2D": {"rotation": 1.0}}}})"));
}
