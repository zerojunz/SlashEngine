#pragma once

// A .character.json recipe (slash/world/character.hpp) built into a rigged model, after Unreal's
// MetaHuman Creator: a skeleton with Godot's humanoid bone names, so animations retarget onto it,
// the body, face and hair made of primitive meshes placed by the recipe's measures, clothing over
// them layer by layer, and props at its points. Models load .character.json files through it, so
// a MeshInstance3D draws one as any model.

#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/character.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace slash::scene3d {

// The model a recipe makes, its props' models read with read. Facing +z with y up and the feet
// at 0, in meters. Materials are named for what they draw: "skin", "eyes", "lips", "hair",
// "scalp" (where a groom grows), each garment's kind (a cloth garment's free part, which a
// Cloth3D's section names; the rest of it "<kind>_fitted") and the props' own. Attachment points
// are nodes under their bones: "right_hand", "left_hand", "back", "hip_left", "hip_right" and
// "head_top".
// A form picks what it draws instead of the person: "skeleton" the bones under the skin and
// "muscles" the muscles over them too (slash/world/flesh.hpp), on the same rig, as a path's
// "::skeleton" and "::muscles" name them.
[[nodiscard]] Result<Model> build_character(const world::CharacterRecipe& recipe,
                                            const ModelReader& read, std::string_view form = {});

// A .character.json file's model (load_model calls it), and its problems (check_model's).
[[nodiscard]] Result<Model> load_character_model(std::string_view bytes, const ModelReader& read,
                                                 std::string_view form = {});
[[nodiscard]] std::vector<json::ValidationError> check_character_model(std::string_view bytes,
                                                                       const ModelReader& read);

// A model's outline at rest, seen from the front (along -z) or the side (along -x): size by size
// cells, row 0 at the top, each 1 where a triangle covers it, over a square kSilhouetteMeters
// across with the feet at its bottom, so heights and builds compare in meters.
inline constexpr float kSilhouetteMeters = 2.5F;
struct Silhouette {
    int size = 0;
    std::vector<std::uint8_t> cells;
};
[[nodiscard]] Silhouette silhouette(const Model& model, bool side, int size = 64);
// How different two outlines are: 1 less the cells both cover over the cells either covers
// (0 the same, 1 nothing shared).
[[nodiscard]] float silhouette_difference(const Silhouette& a, const Silhouette& b);

} // namespace slash::scene3d
