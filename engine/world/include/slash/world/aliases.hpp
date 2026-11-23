#pragma once

#include "slash/core/json.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// The names components, their fields and project settings had before SlashEngine gave them names
// of its own (November 2026). Scenes, prefabs, commands, project.json and other files may still
// use them: loading takes either name, and saving, formatting and describing use the current one.
namespace slash::world {

// A component's old name and its current one.
struct NameAlias {
    std::string_view old_name;
    std::string_view name;
};

// A field's old name and its current one, in a component named by its current name.
struct FieldAlias {
    std::string_view component;
    std::string_view old_name;
    std::string_view name;
};

// Every renamed component and field.
[[nodiscard]] std::span<const NameAlias> component_aliases();
[[nodiscard]] std::span<const FieldAlias> field_aliases();

// The current name of a component, given its current or an old name; any other name as given.
[[nodiscard]] std::string_view current_component_name(std::string_view name);

// The current name of a field of a component (by its current name), given the field's current or
// old name; any other name as given.
[[nodiscard]] std::string_view current_field_name(std::string_view component,
                                                  std::string_view field);

// An old name found and replaced: where (a JSON Pointer under what was upgraded), the old name and
// the current one.
struct Renamed {
    std::string path;
    std::string old_name;
    std::string name;
};

// Replaces old names in a map of components by name, as scenes, prefabs and commands hold them:
// each component's name, and the names of the fields in its object. Where a component or field
// is given under both names, the current one's value is kept. Returns what it replaced, with
// paths under `path`.
std::vector<Renamed> upgrade_components(json::Value& components, std::string_view path = "");

// Replaces old names in a scene or a prefab: each entity's components, or the prefab's.
std::vector<Renamed> upgrade_scene(json::Value& scene);

// Whether a scene or prefab names a component or field by an old name.
[[nodiscard]] bool has_old_names(const json::Value& scene);

// The scene or prefab with old names made current: `scene` itself when it has none, else `copy`,
// which this fills.
[[nodiscard]] const json::Value& current_names(const json::Value& scene, json::Value& copy);

// Replaces old names in a command's params, wherever they name components: the keys of a
// "components" object (and the fields in each), a "component" string, and the strings in a
// "components", "with" or "without" list, at any depth. Returns whether it replaced any.
bool upgrade_params(json::Value& params);

// Whether upgrade_params would replace anything.
[[nodiscard]] bool params_have_old_names(const json::Value& params);

// A component path ("Sprite2D/modulate/a", as animation tracks and synchronizers name fields)
// with its component's and first field's names made current.
[[nodiscard]] std::string current_field_path(std::string_view path);

// A project.json setting's old place and its current one, as names joined by '/'
// ("rendering/environment/ssao" is now "rendering/ssao").
struct SettingAlias {
    std::string_view old_path;
    std::string_view path;
};

// Every setting that moved or was renamed.
[[nodiscard]] std::span<const SettingAlias> setting_aliases();

// Moves settings a project.json document holds at old places to their current ones. Where a
// setting is given at both, the current one's value is kept. Returns what it moved, with paths
// as JSON Pointers into the document.
std::vector<Renamed> upgrade_settings(json::Value& project);

// Whether a project.json document holds a setting at an old place.
[[nodiscard]] bool has_old_settings(const json::Value& project);

// The project.json document with its settings at their current places: `project` itself when it
// has none at old ones, else `copy`, which this fills.
[[nodiscard]] const json::Value& current_settings(const json::Value& project, json::Value& copy);

// A file suffix's old spelling and its current one (".process_material.json" is now
// ".particles.json"). Files keep loading under either; new ones are written with the current one.
struct SuffixAlias {
    std::string_view old_suffix;
    std::string_view suffix;
};

// Every renamed file suffix.
[[nodiscard]] std::span<const SuffixAlias> suffix_aliases();

// Whether `path` ends with `suffix` or with an old suffix `suffix` replaced.
[[nodiscard]] bool has_suffix(std::string_view path, std::string_view suffix);

// The suffixes a file of the kind `suffix` names may end with: `suffix`, then its old ones.
[[nodiscard]] std::vector<std::string> suffixes_of(std::string_view suffix);

// Every renamed command, as old name and current one ("csg.get" is now "solid.get").
[[nodiscard]] std::span<const NameAlias> command_aliases();

// The current name of a command, given its current or an old name; any other name as given.
[[nodiscard]] std::string_view current_command_name(std::string_view name);

// A string param's old value and its current one, for one command ("schema.get"'s kind
// "process_material" is now "particles").
struct ParamValueAlias {
    std::string_view command;
    std::string_view param;
    std::string_view old_value;
    std::string_view value;
};

// Every renamed param value.
[[nodiscard]] std::span<const ParamValueAlias> param_value_aliases();

// The current value of a command's string param, given its current or an old value; any other
// value as given.
[[nodiscard]] std::string_view current_param_value(std::string_view command, std::string_view param,
                                                   std::string_view value);

// Replaces old values of a command's top-level string params. Returns whether it replaced any.
bool upgrade_param_values(std::string_view command, json::Value& params);

} // namespace slash::world
