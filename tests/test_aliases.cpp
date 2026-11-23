#include "slash/core/json.hpp"
#include "slash/world/aliases.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>

using namespace slash;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

} // namespace

TEST_CASE("every old component name stands for a component that exists") {
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    const auto aliases = world::component_aliases();
    CHECK(std::ranges::is_sorted(aliases, {}, &world::NameAlias::old_name));
    std::set<std::string_view> olds;
    for (const world::NameAlias& alias : aliases) {
        CAPTURE(alias.old_name);
        CHECK(olds.insert(alias.old_name).second);
        // The new name is registered, the old one isn't, and finding either finds it.
        const world::ComponentType* type = registry.find(alias.name);
        REQUIRE(type != nullptr);
        CHECK(registry.find(alias.old_name) == type);
        CHECK(std::ranges::none_of(registry.types(), [&](const auto& registered) {
            return registered->name() == alias.old_name;
        }));
        CHECK(world::current_component_name(alias.old_name) == alias.name);
    }
    CHECK(world::current_component_name("Transform2D") == "Transform2D");
    CHECK(world::current_component_name("Nonsense") == "Nonsense");
    CHECK(world::current_field_path("MeshInstance3D/mesh") == "Model3D/mesh");
    CHECK(world::current_field_path("Sprite2D/modulate/a") == "Sprite2D/modulate/a");
}

TEST_CASE("scenes written with old names load and save with the new ones") {
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    const json::Value scene = parse(R"({
      "format": 1,
      "entities": {
        "crate": {"components": {"Transform3D": {}, "MeshInstance3D": {"mesh": "crate.glb"}}},
        "hud": {"components": {"Control": {"offset_right": 40}}},
        "both": {"components": {"Area2D": {"priority": 1}, "Zone2D": {"priority": 2}}}
      }
    })");
    CHECK(world::has_old_names(scene));
    CHECK(world::validate_scene(registry, scene, {}).empty());

    world::World loaded(registry);
    REQUIRE(world::load_scene(loaded, scene).ok());
    const json::Value saved = world::save_scene(loaded);
    CHECK_FALSE(world::has_old_names(saved));
    const json::Value* crate = saved.find("entities")->find("crate")->find("components");
    REQUIRE(crate->find("Model3D") != nullptr);
    CHECK(crate->find("Model3D")->find("mesh")->as_string() == "crate.glb");
    CHECK(saved.find("entities")->find("hud")->find("components")->find("Widget") != nullptr);
    // Given under both names, the current name's value wins.
    const json::Value* both = saved.find("entities")->find("both")->find("components");
    CHECK(both->find("Zone2D")->find("priority")->as_int() == 2);

    json::Value upgraded = scene;
    const auto renamed = world::upgrade_scene(upgraded);
    CHECK(renamed.size() == 3);
    CHECK(std::ranges::any_of(renamed, [](const world::Renamed& one) {
        return one.path == "/entities/crate/components/MeshInstance3D" && one.name == "Model3D";
    }));
}

TEST_CASE("command params name components by their old names too") {
    json::Value params = parse(R"({
      "id": "a",
      "components": {"CharacterBody2D": {}},
      "with": ["Area3D", "Transform3D"],
      "commands": [{"command": "component.set", "params": {"component": "OmniLight3D"}}]
    })");
    REQUIRE(world::params_have_old_names(params));
    CHECK(world::upgrade_params(params));
    CHECK_FALSE(world::params_have_old_names(params));
    CHECK(params.find("components")->find("CharacterMover2D") != nullptr);
    CHECK(params.find("with")->as_array().front().as_string() == "Zone3D");
    CHECK(params.find("commands")
              ->as_array()
              .front()
              .find("params")
              ->find("component")
              ->as_string() == "PointLight3D");
}
