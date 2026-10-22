#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"
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

struct Tuning {
    std::map<std::string, float> weights;
    slash::json::Value extra;
};
SLASH_COMPONENT(Tuning, weights, extra)

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
    game.ecs().emplace<Transform2D>(player).position = {1.5F, -2.0F};
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
                                                      "rotation": [0, 1.5707964, 0]}}},
            "pet": {"components": {
                "Follow": {"target": "leader", "distance": 2.5},
                "Loadout": {"items": ["bone", "ball"], "level": 4,
                            "stats": [{"name": "speed", "value": 7}, {"name": "luck"}]},
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
    // A struct in a component leaves out its fields equal to their defaults, as components do.
    CHECK(
        *saved.find("entities")->find("pet")->find("components")->find("Loadout")->find("stats") ==
        parse(R"([{"name": "speed", "value": 7}, {"name": "luck"}])"));
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

TEST_CASE("maps and JSON values save, load and validate") {
    world::ComponentRegistry registry;
    registry.add<demo::Tuning>();
    World game(registry);
    const entt::entity entity = game.create("knob").value();
    const world::ComponentType& tuning = *registry.find("Tuning");
    std::string path;
    REQUIRE(tuning
                .assign(game, entity,
                        parse(R"({"weights": {"a": 1, "b/c": 2.5}, "extra": [1, "x", null]})"),
                        path)
                .ok());
    const demo::Tuning& value = game.ecs().get<demo::Tuning>(entity);
    CHECK(value.weights == std::map<std::string, float>{{"a", 1.0F}, {"b/c", 2.5F}});
    CHECK(value.extra == parse(R"([1, "x", null])"));
    CHECK(*world::save_scene(game)
               .find("entities")
               ->find("knob")
               ->find("components")
               ->find("Tuning") ==
          parse(R"({"extra": [1, "x", null], "weights": {"a": 1.0, "b/c": 2.5}})"));

    // A map is replaced whole, and its errors point inside it.
    REQUIRE(tuning.assign(game, entity, parse(R"({"weights": {"c": 3}})"), path).ok());
    CHECK(game.ecs().get<demo::Tuning>(entity).weights ==
          std::map<std::string, float>{{"c", 3.0F}});
    path.clear();
    const auto bad = tuning.assign(game, entity, parse(R"({"weights": {"b/c": "x"}})"), path);
    REQUIRE_FALSE(bad.ok());
    CHECK(path == "/weights/b~1c");

    // The defaults, an empty map and null, are left out of saved scenes.
    game.ecs().get<demo::Tuning>(entity) = {};
    CHECK(*world::save_scene(game)
               .find("entities")
               ->find("knob")
               ->find("components")
               ->find("Tuning") == parse("{}"));

    world::SchemaBuilder builder;
    (void)tuning.schema(builder);
    const json::Value& definition = builder.definition("Tuning");
    CHECK(
        *definition.find("properties")->find("weights") ==
        parse(R"({"type": "object", "additionalProperties": {"type": "number"}, "default": {}})"));
    CHECK(json::validate(parse(R"({"weights": {"a": 1}, "extra": {"any": ["thing"]}})"), definition)
              .empty());
    CHECK_FALSE(json::validate(parse(R"({"weights": {"a": "x"}})"), definition).empty());
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
    CHECK(*rotation.find("maxItems") == json::Value(3));
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

TEST_CASE("restore_scene puts back fields that systems moved outside their range") {
    const auto registry = make_registry();
    World game(registry);
    const entt::entity hero = game.create("hero").value();
    game.ecs().emplace<demo::Health>(hero, demo::Health{.current = -20.0F});
    const json::Value saved = world::save_scene(game);

    // A scene file with that value is invalid, but a world saved that way restores exactly.
    World copy(registry);
    CHECK_FALSE(world::load_scene(copy, saved).ok());
    REQUIRE(world::restore_scene(copy, saved, game.prefab_links()).ok());
    CHECK(world::save_scene(copy) == saved);
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
          "/entities/e/extends: expected a .prefab.json or .scene.json file, got \"enemy.json\"");
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

TEST_CASE("reload_prefabs updates instances and keeps their overrides") {
    const auto registry = make_registry();
    std::map<std::string, std::string, std::less<>> files{
        {"base.prefab.json", R"({"format": 1, "components": {"Health": {"max": 200}}})"},
        {"boss.prefab.json", R"({"format": 1, "extends": "base.prefab.json",
                                 "components": {"Health": {"current": 150}, "Enemy": {}}})"},
    };
    World world(registry);
    world.set_file_reader([&files](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    });
    REQUIRE(world::load_scene(world, parse(R"({"format": 1, "entities": {
        "boss": {"extends": "boss.prefab.json", "components": {"Health": {"current": 99}}},
        "twin": {"extends": "boss.prefab.json"},
        "plain": {"components": {"Health": {}}}}})"))
                .ok());
    CHECK(world::reload_prefabs(world).changed.empty());

    // The base prefab changes: every instance down the chain picks it up.
    files["base.prefab.json"] =
        R"({"format": 1, "components": {"Health": {"max": 300}, "Velocity": {"linear": [1, 0]}}})";
    world::PrefabReload reload = world::reload_prefabs(world);
    CHECK(reload.errors.empty());
    CHECK(reload.changed == std::vector<std::string>{"boss", "twin"});
    const auto boss = world.find("boss");
    CHECK(world.ecs().get<demo::Health>(boss).current == 99.0F);
    CHECK(world.ecs().get<demo::Health>(boss).max == 300.0F);
    CHECK(world.ecs().get<demo::Velocity>(boss).linear.x == 1.0F);
    CHECK(world.ecs().get<demo::Health>(world.find("twin")).current == 150.0F);
    CHECK(world.prefab_of(boss)->base == world.prefab_of(world.find("twin"))->base);
    // Saving still writes only the overrides.
    CHECK(*world::save_scene(world).find("entities")->find("boss") ==
          parse(R"({"extends": "boss.prefab.json", "components": {"Health": {"current": 99.0}}})"));

    // Fields and components the prefab drops go back to their defaults or away.
    files["boss.prefab.json"] = R"({"format": 1, "extends": "base.prefab.json"})";
    files["base.prefab.json"] = R"({"format": 1, "components": {"Health": {}}})";
    reload = world::reload_prefabs(world);
    CHECK(reload.changed == std::vector<std::string>{"boss", "twin"});
    CHECK(world.ecs().get<demo::Health>(boss).max == 100.0F);
    CHECK(world.ecs().get<demo::Health>(boss).current == 99.0F);
    CHECK_FALSE(world.ecs().all_of<demo::Enemy>(boss));
    CHECK_FALSE(world.ecs().all_of<demo::Velocity>(boss));

    // A prefab that no longer loads leaves its instances alone.
    files["boss.prefab.json"] = "{";
    reload = world::reload_prefabs(world);
    CHECK(reload.changed.empty());
    REQUIRE(reload.errors.size() == 1);
    CHECK(reload.errors[0].path == "boss.prefab.json");
    CHECK(world.ecs().get<demo::Health>(boss).current == 99.0F);
}

namespace {

// A player scene: a root with a sprite and a gun in its space, and a sound outside it.
const std::string kPlayerScene = R"({"format": 1, "root": "player", "entities": {
    "player": {"components": {"Transform2D": {"children": ["sprite", "gun"]},
                              "Health": {"max": 50}}},
    "sprite": {"components": {"Transform2D": {"position": [0, -8]}}},
    "gun": {"components": {"Transform2D": {"position": [4, 0]}, "Follow": {"target": "player"}}},
    "sfx": {"components": {"Enemy": {}}}}})";

std::vector<std::string> entity_ids(const World& world) {
    std::vector<std::string> ids;
    for (const std::string_view id : world.ids()) {
        ids.emplace_back(id);
    }
    return ids;
}

} // namespace

TEST_CASE("an entity extends a scene and brings the scene's entities") {
    const auto registry = make_registry();
    World world(registry);
    world.set_file_reader(memory_files({{"player.scene.json", kPlayerScene}}));
    // hero moves, its gun follows from further away, it leaves out the sound, and the camera
    // follows its sprite.
    const json::Value scene = parse(R"({"format": 1, "entities": {
        "camera": {"components": {"Follow": {"target": "hero/sprite"}}},
        "hero": {"extends": "player.scene.json",
                 "components": {"Transform2D": {"position": [100.0, 50.0]}}},
        "hero/gun": {"components": {"Follow": {"distance": 3.0}}},
        "hero/sfx": null}})");
    REQUIRE(world::load_scene(world, scene).ok());
    CHECK(entity_ids(world) ==
          std::vector<std::string>{"camera", "hero", "hero/gun", "hero/sprite"});

    const auto hero = world.find("hero");
    const auto gun = world.find("hero/gun");
    const auto sprite = world.find("hero/sprite");
    CHECK(world.prefab_of(hero)->path == "player.scene.json");
    CHECK(world.prefab_of(gun) == nullptr);
    CHECK(world.ecs().get<demo::Health>(hero).max == 50.0F);
    // References inside the scene name the instance's entities.
    CHECK(world.ecs().get<Transform2D>(hero).children == std::vector<entt::entity>{sprite, gun});
    CHECK(world.ecs().get<demo::Follow>(gun).target == hero);
    CHECK(world.ecs().get<demo::Follow>(gun).distance == 3.0F);
    CHECK(world.ecs().get<demo::Follow>(world.find("camera")).target == sprite);
    CHECK(world::Tree2D(world).global(sprite).position == Vec2{100.0F, 42.0F});

    // Saving writes only what differs from the scene, and the parts left out.
    CHECK(world::save_scene(world) == scene);
    world.ecs().get<Transform2D>(sprite).position = {0.0F, -10.0F};
    world.ecs().get<Transform2D>(hero).children = {sprite};
    world.destroy(gun);
    CHECK(*world::save_scene(world).find("entities") == parse(R"({
        "camera": {"components": {"Follow": {"target": "hero/sprite"}}},
        "hero": {"extends": "player.scene.json",
                 "components": {"Transform2D": {"children": ["hero/sprite"],
                                                "position": [100.0, 50.0]}}},
        "hero/gun": null,
        "hero/sfx": null,
        "hero/sprite": {"components": {"Transform2D": {"position": [0.0, -10.0]}}}})"));

    // The instance's entities go with it.
    world.destroy(hero);
    CHECK(entity_ids(world) == std::vector<std::string>{"camera"});
}

TEST_CASE("instantiate_prefab makes an instance of a scene") {
    const auto registry = make_registry();
    World world(registry);
    world.set_file_reader(memory_files({{"player.scene.json", kPlayerScene}}));
    const auto hero = world.create("hero").value();
    REQUIRE(world::instantiate_prefab(world, hero, "player.scene.json").ok());
    CHECK(entity_ids(world) ==
          std::vector<std::string>{"hero", "hero/gun", "hero/sfx", "hero/sprite"});
    CHECK(world.ecs().get<demo::Follow>(world.find("hero/gun")).target == hero);
    CHECK(world::save_scene(world) ==
          parse(R"({"format": 1, "entities": {"hero": {"extends": "player.scene.json"}}})"));

    // A second instance needs its own IDs; a taken one fails and changes nothing.
    const auto twin = world.create("twin").value();
    REQUIRE(world.create_part("twin/gun").ok());
    const auto made = world::instantiate_prefab(world, twin, "player.scene.json");
    REQUIRE_FALSE(made.ok());
    CHECK(made.error().message == "entity id \"twin/gun\" is already in use");
    CHECK(entity_ids(world) == std::vector<std::string>{"hero", "hero/gun", "hero/sfx",
                                                        "hero/sprite", "twin", "twin/gun"});
    CHECK(world.prefab_of(twin) == nullptr);
}

TEST_CASE("scenes instance scenes") {
    const auto registry = make_registry();
    World world(registry);
    world.set_file_reader(memory_files({
        {"gun.scene.json", R"({"format": 1, "root": "gun", "entities": {
            "gun": {"components": {"Transform2D": {"children": ["muzzle"]}}},
            "muzzle": {"components": {"Transform2D": {"position": [8, 0]},
                                      "Follow": {"target": "gun"}}}}})"},
        {"player.scene.json", R"({"format": 1, "root": "player", "entities": {
            "player": {"components": {"Transform2D": {"children": ["arm"]}}},
            "arm": {"extends": "gun.scene.json", "components": {"Transform2D": {"rotation": 1}}},
            "arm/muzzle": {"components": {"Transform2D": {"position": [12, 0]}}}}})"},
    }));
    // The scene an instance extends holds its own instances' entities under their IDs.
    const auto resolved = world::resolve_prefab(world, "player.scene.json");
    REQUIRE(resolved.ok());
    CHECK(*resolved.value() == parse(R"({"root": "player", "entities": {
        "player": {"Transform2D": {"children": ["arm"]}},
        "arm": {"Transform2D": {"children": ["arm/muzzle"], "rotation": 1}},
        "arm/muzzle": {"Transform2D": {"position": [12, 0]}, "Follow": {"target": "arm"}}}})"));

    const json::Value scene = parse(R"({"format": 1, "entities": {
        "hero": {"extends": "player.scene.json"},
        "hero/arm/muzzle": {"components": {"Follow": {"distance": 2.0}}}}})");
    REQUIRE(world::load_scene(world, scene).ok());
    CHECK(entity_ids(world) == std::vector<std::string>{"hero", "hero/arm", "hero/arm/muzzle"});
    const auto arm = world.find("hero/arm");
    const auto muzzle = world.find("hero/arm/muzzle");
    CHECK(world.ecs().get<Transform2D>(world.find("hero")).children ==
          std::vector<entt::entity>{arm});
    CHECK(world.ecs().get<Transform2D>(arm).children == std::vector<entt::entity>{muzzle});
    CHECK(world.ecs().get<Transform2D>(arm).rotation == 1.0F);
    CHECK(world.ecs().get<Transform2D>(muzzle).position.x == 12.0F);
    CHECK(world.ecs().get<demo::Follow>(muzzle).target == arm);
    CHECK(world.ecs().get<demo::Follow>(muzzle).distance == 2.0F);
    CHECK(world::save_scene(world) == scene);
}

TEST_CASE("instanced scene errors name the value at fault") {
    const auto registry = make_registry();
    const auto reader = memory_files({
        {"player.scene.json", kPlayerScene},
        {"rootless.scene.json", R"({"format": 1, "entities": {"a": {}}})"},
        {"lost.scene.json", R"({"format": 1, "root": "b", "entities": {"a": {}}})"},
        {"a.scene.json", R"({"format": 1, "root": "a", "entities": {
            "a": {}, "b": {"extends": "b.scene.json"}}})"},
        {"b.scene.json", R"({"format": 1, "root": "b", "entities": {
            "b": {}, "a": {"extends": "a.scene.json"}}})"},
        {"wrapper.prefab.json", R"({"format": 1, "extends": "player.scene.json"})"},
        {"enemy.prefab.json", kEnemyPrefab},
    });
    const auto error = [&](const std::string& entities) {
        return load_error(registry, parse(R"({"format": 1, "entities": {)" + entities + "}}"),
                          reader);
    };
    CHECK(error(R"("e": {"extends": "player.scene.json"})").empty());
    CHECK(error(R"("e": {"extends": "rootless.scene.json"})") ==
          "/entities/e/extends: scene \"rootless.scene.json\" has no \"root\": name the entity an "
          "instance of it becomes (scene.save takes \"root\")");
    CHECK(error(R"("e": {"extends": "lost.scene.json"})") ==
          "/entities/e/extends: scene \"lost.scene.json\" is not valid: /root: no entity has the "
          "id \"b\"");
    CHECK(error(R"("e": {"extends": "a.scene.json"})") ==
          "/entities/e/extends: scene \"a.scene.json\" is not valid: /entities/b/extends: scene "
          "\"b.scene.json\" is not valid: /entities/a/extends: scenes instance each other in a "
          "loop: \"a.scene.json\" instances \"b.scene.json\" instances \"a.scene.json\"");
    CHECK(error(R"("e": {"extends": "wrapper.prefab.json"})") ==
          "/entities/e/extends: prefab \"wrapper.prefab.json\" is not valid: /extends: a prefab "
          "extends another .prefab.json file; an entity in a scene can extend a scene");
    CHECK(error(R"("e": {"extends": "player.scene.json"}, "e/hat": {})") ==
          "/entities/e~1hat: no instanced scene brings an entity \"e/hat\": "
          "\"player.scene.json\" has no entity \"hat\"");
    CHECK(error(R"("e": {"extends": "enemy.prefab.json"}, "e/gun": {})") ==
          "/entities/e~1gun: no instanced scene brings an entity \"e/gun\": \"e\" extends no "
          "scene");
    CHECK(error(R"("x/gun": {})") ==
          "/entities/x~1gun: no instanced scene brings an entity \"x/gun\": there is no entity "
          "\"x\"");
    CHECK(error(R"("e": null)") ==
          "/entities/e: null leaves out an entity an instanced scene brings, but \"e\" is not one");
    CHECK(error(R"("e": {"extends": "player.scene.json"},
                   "e/gun": {"extends": "enemy.prefab.json"})") ==
          "/entities/e~1gun/extends: \"e/gun\" comes from \"player.scene.json\", which decides "
          "what it extends");
    CHECK(error(R"("e": {"extends": "player.scene.json"},
                   "e/gun": {"components": {"Health": null}})") ==
          "/entities/e~1gun/components/Health: null removes a component the entity has in "
          "\"player.scene.json\", but it has no \"Health\"");
    CHECK(error(R"("e": {"extends": "player.scene.json"}, "e//gun": {})") ==
          "/entities/e~1~1gun: invalid entity id \"e//gun\": use 1 to 64 letters, digits, '_' or "
          "'-'");
    CHECK(load_error(registry, parse(R"({"format": 1, "root": "x", "entities": {"a": {}}})"),
                     reader) == "/root: no entity has the id \"x\"");
}

TEST_CASE("a scene's root saves and loads") {
    const auto registry = make_registry();
    World world(registry);
    REQUIRE(world::load_scene(world, parse(kPlayerScene)).ok());
    CHECK(world.scene_root() == "player");
    CHECK(world::save_scene(world) == parse(kPlayerScene));
    world.destroy(world.find("player"));
    CHECK(world::save_scene(world).find("root") == nullptr);
    world.clear();
    CHECK(world.scene_root().empty());
}

TEST_CASE("instanced scenes diff, restore and reload") {
    const auto registry = make_registry();
    std::map<std::string, std::string, std::less<>> files{{"player.scene.json", kPlayerScene}};
    World world(registry);
    world.set_file_reader([&files](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    });
    const json::Value before = parse(R"({"format": 1, "entities": {
        "hero": {"extends": "player.scene.json"},
        "hero/sfx": null}})");
    REQUIRE(world::load_scene(world, before).ok());
    const world::PrefabLinks links = world.prefab_links();
    CHECK(links.size() == 1);

    // A diff sees the entities an instance brings through the scene in the links.
    const json::Value after = parse(R"({"format": 1, "entities": {
        "hero": {"extends": "player.scene.json"},
        "hero/gun": {"components": {"Follow": {"distance": 3.0}}},
        "hero/sprite": null,
        "hero/sfx": null}})");
    CHECK(world::diff_scenes(registry, before, after, links, links) ==
          parse(R"({"removed": ["hero/sprite"],
                    "changed": {"hero/gun": {"Follow": {"distance": 3.0}}}})"));

    // A copy restores from the links after the file changes.
    world.ecs().get<demo::Follow>(world.find("hero/gun")).distance = 5.0F;
    const json::Value saved = world::save_scene(world);
    files["player.scene.json"] = "{";
    World copy(registry);
    REQUIRE(world::restore_scene(copy, saved, links).ok());
    CHECK(entity_ids(copy) == std::vector<std::string>{"hero", "hero/gun", "hero/sprite"});
    CHECK(copy.ecs().get<demo::Follow>(copy.find("hero/gun")).distance == 5.0F);
    CHECK(world::save_scene(copy) == saved);

    // The scene changes: the sprite moves, the gun goes, a shield comes. The instance keeps
    // leaving out the sound.
    files["player.scene.json"] = R"({"format": 1, "root": "player", "entities": {
        "player": {"components": {"Transform2D": {"children": ["sprite", "shield"]},
                                  "Health": {"max": 50}}},
        "sprite": {"components": {"Transform2D": {"position": [0, -12]}}},
        "shield": {"components": {"Follow": {"target": "sprite"}}},
        "sfx": {"components": {"Enemy": {}}}}})";
    world.ecs().get<Transform2D>(world.find("hero")).position = {7.0F, 0.0F};
    const world::PrefabReload reload = world::reload_prefabs(world);
    CHECK(reload.errors.empty());
    CHECK(reload.changed ==
          std::vector<std::string>{"hero", "hero/gun", "hero/shield", "hero/sprite"});
    CHECK(entity_ids(world) == std::vector<std::string>{"hero", "hero/shield", "hero/sprite"});
    const auto hero = world.find("hero");
    const auto sprite = world.find("hero/sprite");
    const auto shield = world.find("hero/shield");
    CHECK(world.ecs().get<Transform2D>(hero).position.x == 7.0F);
    CHECK(world.ecs().get<Transform2D>(hero).children == std::vector<entt::entity>{sprite, shield});
    CHECK(world.ecs().get<Transform2D>(sprite).position.y == -12.0F);
    CHECK(world.ecs().get<demo::Follow>(shield).target == sprite);
    CHECK(world::save_scene(world) == parse(R"({"format": 1, "entities": {
        "hero": {"extends": "player.scene.json",
                 "components": {"Transform2D": {"position": [7.0, 0.0]}}},
        "hero/sfx": null}})"));
}
