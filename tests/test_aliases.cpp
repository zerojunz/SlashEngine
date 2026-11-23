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
    CHECK(world::current_field_path("Sprite2D/modulate/a") == "Sprite2D/tint/a");
    CHECK(world::current_field_path("Sprite2D/tint/a") == "Sprite2D/tint/a");
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

TEST_CASE("every old field name stands for a field of its component") {
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    const auto aliases = world::field_aliases();
    CHECK(std::ranges::is_sorted(aliases, {}, [](const world::FieldAlias& alias) {
        return std::pair{alias.component, alias.old_name};
    }));
    for (const world::FieldAlias& alias : aliases) {
        CAPTURE(alias.component);
        CAPTURE(alias.old_name);
        const world::ComponentType* type = registry.find(alias.component);
        REQUIRE(type != nullptr);
        CHECK(type->name() == alias.component);
        const auto fields = type->fields();
        CHECK(std::ranges::find(fields, alias.name) != fields.end());
        CHECK(std::ranges::find(fields, alias.old_name) == fields.end());
        CHECK(world::current_field_name(alias.component, alias.old_name) == alias.name);
    }
    CHECK(world::current_field_name("Sprite2D", "texture") == "texture");
    CHECK(world::current_field_name("Nonsense", "modulate") == "modulate");
}

TEST_CASE("scenes written with old field names load and save with the new ones") {
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    const json::Value scene = parse(R"({
      "format": 1,
      "entities": {
        "hero": {"components": {"Transform2D": {},
                                "Sprite2D": {"modulate": [1, 0, 0, 1], "z_index": 3}}},
        "door": {"components": {"Area2D": {"monitoring": false, "detectable": false}}}
      }
    })");
    CHECK(world::has_old_names(scene));
    CHECK(world::validate_scene(registry, scene, {}).empty());

    world::World loaded(registry);
    REQUIRE(world::load_scene(loaded, scene).ok());
    const json::Value saved = world::save_scene(loaded);
    CHECK_FALSE(world::has_old_names(saved));
    const json::Value* sprite =
        saved.find("entities")->find("hero")->find("components")->find("Sprite2D");
    REQUIRE(sprite != nullptr);
    CHECK(sprite->find("modulate") == nullptr);
    CHECK(sprite->find("tint") != nullptr);
    CHECK(sprite->find("sort_order")->as_int() == 3);
    const json::Value* zone =
        saved.find("entities")->find("door")->find("components")->find("Zone2D");
    REQUIRE(zone != nullptr);
    CHECK_FALSE(zone->find("detects")->as_bool());
    CHECK_FALSE(zone->find("detectable")->as_bool());
}

TEST_CASE("project.json settings at their old places move to their new ones") {
    const auto aliases = world::setting_aliases();
    REQUIRE_FALSE(aliases.empty());
    // Deepest first, so a group moves after the settings inside it.
    for (std::size_t i = 1; i < aliases.size(); ++i) {
        CHECK(std::ranges::count(aliases[i - 1].old_path, '/') >=
              std::ranges::count(aliases[i].old_path, '/'));
    }
    json::Value project = parse(R"({
      "format": 1,
      "window": {"width": 320, "vsync_mode": "disabled"},
      "gui": {"theme": "ui/game.theme.json", "tooltip_delay_sec": 1},
      "rendering": {
        "lights_and_shadows": {"directional_shadow": {"size": 2048},
                               "use_physical_light_units": true},
        "environment": {"ssao": {"quality": "high"}},
        "anti_aliasing": {"quality": {"use_taa": true}},
        "renderer": "gl_compatibility", "rendering_method": "forward_plus"
      },
      "internationalization": {"locale": {"translations": ["menu.csv"], "fallback": "de"},
                               "pseudolocalization": {"use_pseudolocalization": true}}
    })");
    CHECK(world::has_old_settings(project));
    json::Value copy;
    const json::Value& current = world::current_settings(project, copy);
    CHECK(&current == &copy);
    const auto moved = world::upgrade_settings(project);
    CHECK_FALSE(moved.empty());
    CHECK_FALSE(world::has_old_settings(project));
    CHECK(json::write(project) == json::write(current));
    CHECK(&world::current_settings(project, copy) == &project);
    // The value at a JSON Pointer of plain names and indices, or null.
    const auto at = [&](std::string_view pointer) -> const json::Value* {
        const json::Value* value = &project;
        while (value != nullptr && !pointer.empty()) {
            pointer.remove_prefix(1);
            const std::string_view part = pointer.substr(0, pointer.find('/'));
            pointer.remove_prefix(part.size());
            if (value->is_array()) {
                const auto index = static_cast<std::size_t>(std::stoul(std::string(part)));
                value = index < value->as_array().size() ? &value->as_array()[index] : nullptr;
            } else {
                value = value->is_object() ? value->find(part) : nullptr;
            }
        }
        return value;
    };
    CHECK(at("/window/vsync")->as_string() == "disabled");
    CHECK(at("/window/width")->as_int() == 320);
    CHECK(at("/ui/theme")->as_string() == "ui/game.theme.json");
    CHECK(at("/ui/tooltip_delay_seconds")->as_int() == 1);
    CHECK(at("/rendering/shadows/directional_size")->as_int() == 2048);
    CHECK(at("/rendering/physical_light_units")->as_bool());
    CHECK(at("/rendering/ssao/quality")->as_string() == "high");
    CHECK(at("/rendering/anti_aliasing/taa")->as_bool());
    CHECK(at("/localization/translations/0")->as_string() == "menu.csv");
    CHECK(at("/localization/fallback")->as_string() == "de");
    CHECK(at("/localization/pseudo/enabled")->as_bool());
    // Given at both places, the new one's value is kept.
    CHECK(at("/rendering/renderer")->as_string() == "gl_compatibility");
    // Emptied groups go.
    CHECK(project.find("gui") == nullptr);
    CHECK(project.find("internationalization") == nullptr);
    CHECK(at("/rendering/lights_and_shadows") == nullptr);
    CHECK(at("/rendering/environment") == nullptr);
    CHECK(std::ranges::any_of(moved, [](const world::Renamed& one) {
        return one.old_name == "window/vsync_mode" && one.name == "window/vsync";
    }));
}
