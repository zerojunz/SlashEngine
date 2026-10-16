#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <entt/entity/fwd.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Scenes are JSON files (.scene.json) holding entities keyed by stable ID:
//
//   {
//     "entities": {
//       "player": {
//         "components": {
//           "Transform2D": {
//             "position": [64.0, 32.0]
//           }
//         }
//       }
//     },
//     "format": 1
//   }
//
// Fields equal to their defaults are left out, as Godot does. The schema every scene must satisfy
// is generated from the registered components, so it always matches the engine.
//
// An entity can extend a prefab, a .prefab.json file holding one entity's components (and
// optionally extending another prefab):
//
//   "enemy_01": {"extends": "prefabs/enemy.prefab.json",
//                "components": {"Transform2D": {"position": [200.0, 40.0]}, "Sprite": null}}
//
// It starts with the prefab's components, and its own fields override the prefab's. A null
// component removes one the prefab has. Saving writes only what differs from the prefab, as Godot
// does for instanced scenes. Prefab paths are relative to the project folder and are read with
// the world's file reader.
//
// An entity can also extend a scene that names its "root", as Godot instances a PackedScene. It
// becomes the scene's root entity, and the scene's other entities come with it, with IDs under
// the instance's and the references between them renamed to match:
//
//   "hero": {"extends": "scenes/player.scene.json"},
//   "hero/sprite": {"components": {"Sprite2D": {"flip_h": true}}},
//   "hero/gun": null
//
// An entry keyed with '/' holds the overrides of an entity an instance brings, or null to leave
// it out. Scenes can instance scenes, so IDs nest ("hero/gun/muzzle").
namespace slash::world {

inline constexpr std::int64_t kSceneFormat = 1;
inline constexpr std::string_view kSceneSuffix = ".scene.json";
inline constexpr std::string_view kPrefabSuffix = ".prefab.json";
// The asset type name prefab files are read under (asset::Assets::read).
inline constexpr std::string_view kPrefabType = "prefab";

// The JSON Schema for scene files with these components.
[[nodiscard]] json::Value scene_schema(const ComponentRegistry& components);

// The JSON Schema for prefab files: {"components": {...}, "extends": "...", "format": 1}.
[[nodiscard]] json::Value prefab_schema(const ComponentRegistry& components);

// Checks a parsed scene against the schema, then against the rules a schema cannot express,
// such as references to entities that exist and prefabs that load. Returns every schema error,
// or the first load error. Prefabs are read with reader; without one, extending fails.
[[nodiscard]] std::vector<json::ValidationError> validate_scene(const ComponentRegistry& components,
                                                                const json::Value& scene,
                                                                const FileReader& reader = {});

// The same for a parsed prefab file.
[[nodiscard]] std::vector<json::ValidationError>
validate_prefab(const ComponentRegistry& components, const json::Value& prefab,
                const FileReader& reader = {});

// Adds the scene's entities to the world and returns how many were added. The scene is validated
// first; on any error the world is left unchanged and the error names the offending path.
// Entities in links take their prefab from there rather than from the file, as when restoring a
// world's saved entities (World::prefab_links) exactly as they were.
[[nodiscard]] Result<std::size_t> load_scene(World& world, const json::Value& scene,
                                             const PrefabLinks& links = {});

// Puts back entities that save_scene saved from a world with the same components, exactly as they
// were. Unlike load_scene it skips the schema check: systems may have moved a field outside its
// documented range, such as hit points below a minimum of 0, and the world must still restore.
[[nodiscard]] Result<std::size_t> restore_scene(World& world, const json::Value& saved,
                                                const PrefabLinks& links);

// Every entity that has an ID, with the registered components it carries. An entity that extends
// a prefab keeps "extends" and only the fields that differ from the prefab.
[[nodiscard]] json::Value save_scene(const World& world);

// A prefab's components, resolved through every "extends", read with the world's file reader and
// cached in the world so every instance shares them.
[[nodiscard]] Result<std::shared_ptr<const json::Value>> resolve_prefab(World& world,
                                                                        std::string_view path);

// Gives an entity a prefab's components and links it to the prefab.
[[nodiscard]] Result<void> instantiate_prefab(World& world, entt::entity entity,
                                              std::string_view path);

// Reads every prefab the world's entities extend again and updates the instances of those that
// changed, keeping each instance's own overrides, as Godot does when an instanced scene changes.
// A prefab that no longer loads leaves its instances as they are.
struct PrefabReload {
    std::vector<std::string> changed;          // IDs of entities whose components changed, sorted
    std::vector<json::ValidationError> errors; // {prefab path, why it did not load}
};
[[nodiscard]] PrefabReload reload_prefabs(World& world);

// One entity in prefab-file form: its components, or for an entity that extends a prefab, that
// "extends" and only what differs from it.
[[nodiscard]] json::Value save_prefab(const World& world, entt::entity entity);

// What changed between two saved scenes, by entity ID:
//
//   {"added": ["bullet_1"], "removed": ["enemy_2"],
//    "changed": {"player": {"Transform2D": {"position": [64.0, 40.0]}, "Health": null}}}
//
// "changed" holds every added or changed entity with the new value of each field that differs:
// from before, or from its default for a component the entity did not have, which is {} when every
// field is at its default. A removed component is null. Empty sections are left out, so identical
// scenes give {}.
//
// Entities that extend a prefab are compared with the prefab's components under their own, taken
// from the links of the world each scene was saved from (World::prefab_links).
[[nodiscard]] json::Value diff_scenes(const ComponentRegistry& components,
                                      const json::Value& before, const json::Value& after,
                                      const PrefabLinks& before_links = {},
                                      const PrefabLinks& after_links = {});

// Rewrites scene text in canonical form: parsed, loaded into a fresh world and saved again, so
// defaults are dropped and numbers normalized. The text ends with a newline.
[[nodiscard]] Result<std::string> format_scene(const ComponentRegistry& components,
                                               std::string_view text,
                                               const FileReader& reader = {});

// The same for prefab files.
[[nodiscard]] Result<std::string> format_prefab(const ComponentRegistry& components,
                                                std::string_view text,
                                                const FileReader& reader = {});

} // namespace slash::world
