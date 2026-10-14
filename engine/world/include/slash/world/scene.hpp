#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstddef>
#include <cstdint>
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
namespace slash::world {

class ComponentRegistry;
class World;

inline constexpr std::int64_t kSceneFormat = 1;

// The JSON Schema for scene files with these components.
[[nodiscard]] json::Value scene_schema(const ComponentRegistry& components);

// Checks a parsed scene against the schema, then against the rules a schema cannot express,
// such as references to entities that exist. Returns every schema error, or the first load error.
[[nodiscard]] std::vector<json::ValidationError> validate_scene(const ComponentRegistry& components,
                                                                const json::Value& scene);

// Adds the scene's entities to the world and returns how many were added. The scene is validated
// first; on any error the world is left unchanged and the error names the offending path.
[[nodiscard]] Result<std::size_t> load_scene(World& world, const json::Value& scene);

// Every entity that has an ID, with the registered components it carries.
[[nodiscard]] json::Value save_scene(const World& world);

// Rewrites scene text in canonical form: parsed, loaded into a fresh world and saved again, so
// defaults are dropped and numbers normalized. The text ends with a newline.
[[nodiscard]] Result<std::string> format_scene(const ComponentRegistry& components,
                                               std::string_view text);

} // namespace slash::world
