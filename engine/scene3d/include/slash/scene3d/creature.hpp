#pragma once

// A .creature.json recipe (slash/world/creature.hpp) built into a rigged, animated model, after
// Unreal's Mutable: a skeleton of creature_skeleton's bones, the body and its parts made of
// primitive meshes placed by the recipe's measures, and an animation for each of
// creature_animations'. Models load .creature.json files through it, so a MeshInstance3D draws
// one and an AnimationPlayer whose library is the same file plays its animations.

#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/creature.hpp"

#include <string_view>
#include <vector>

namespace slash::scene3d {

// The model a recipe makes, facing +z with y up and the feet at 0, in meters. Materials are named
// for what they draw: "coat", "belly", "markings", "eyes", "horn", "beak" and "wings" (an
// insect's, see-through). Its attachment points are "saddle", on its back, and "mouth".
[[nodiscard]] Model build_creature(const world::CreatureRecipe& recipe);

// A .creature.json file's model (load_model calls it), and its problems (check_model's).
[[nodiscard]] Result<Model> load_creature_model(std::string_view bytes, const ModelReader& read);
[[nodiscard]] std::vector<json::ValidationError> check_creature_model(std::string_view bytes,
                                                                      const ModelReader& read);

} // namespace slash::scene3d
