#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Humanoids retargeted to one skeleton, as Godot 4.6's importer retargets them: its
// SkeletonProfileHumanoid (scene/resources/skeleton_profile.cpp), BoneMapper's auto mapping
// (editor/scene/3d/bone_map_editor_plugin.cpp) and the skeleton renamer, rest fixer and track
// organizer that run on import (editor/import/3d/post_import_plugin_skeleton_*.cpp), line for
// line where they can. A retargeted model's bones have the profile's names and its rests turned
// as the profile's are, so one model's animations play on every other:
//
//   scene3d::RetargetOptions options;
//   options.bone_map = scene3d::auto_map_bones(model);
//   auto report = scene3d::retarget_model(model, options);
//
// Angles go through slash::atan2 and scene3d builds with -ffp-contract=off, so a retargeted model
// is the same bits everywhere.
namespace slash::scene3d {

// Which way a profile bone points, as SkeletonProfile::TailDirection: to the average of its
// children's heads, to one child's (tail), or nowhere, at the end of a chain.
enum class TailDirection : std::uint8_t { AverageChildren, SpecificChild, End };

// A bone of the profile, as SkeletonProfile's bones: its name, its parent's ("" for the root),
// which way it points, its reference pose in its parent (a basis in Godot's row order, then an
// origin), its group in Godot's editor, and whether a humanoid must have it.
struct ProfileBone {
    std::string_view name;
    std::string_view parent;
    TailDirection tail_direction = TailDirection::AverageChildren;
    std::string_view tail;
    std::array<float, 9> basis{};
    Vec3 origin;
    std::string_view group;
    bool required = false;
};

// Godot's SkeletonProfileHumanoid: its 56 bones, from Root, Hips and Spine to each finger's
// distal bone, in its order.
[[nodiscard]] std::span<const ProfileBone> humanoid_profile();
// The profile bone with this name, or -1.
[[nodiscard]] int find_profile_bone(std::string_view name);
// The profile's root bone and the bone whose height scales position tracks (its
// scale_base_bone).
inline constexpr std::string_view kProfileRoot = "Root";
inline constexpr std::string_view kScaleBaseBone = "Hips";

// Which bone of a model each profile bone is, as Godot's BoneMap: by profile bone name, the name
// of a node of the model. Bones it leaves out are unmapped.
using BoneMap = std::map<std::string, std::string, std::less<>>;

// Guesses a bone map from a model's bone names and hierarchy, as Godot's BoneMapper auto
// mapping: Hips by name, the bones above it for Root, then feet, legs, toes, hands, fingers,
// arms, neck, head, eyes, jaw, chest and spine, each by words in its name ("thigh", "forearm"),
// "left" or "right" (or "l" or "r" alone) and where it is in the hierarchy. It works on the
// model's largest skeleton (model_skeletons); empty when it finds no hips.
[[nodiscard]] BoneMap auto_map_bones(const Model& model);

// What retarget_model does beyond Godot's defaults, as its import options name them.
struct RetargetOptions {
    BoneMap bone_map;
    // fix_silhouette: turns bones that point more than silhouette_threshold degrees away from
    // the profile's toward it, as for a model posed in an A-pose, and moves the hips up by
    // base_height_adjustment meters. Bones in silhouette_filter are left as they are.
    bool fix_silhouette = false;
    float silhouette_threshold = 15.0F;
    float base_height_adjustment = 0.0F;
    std::vector<std::string> silhouette_filter{}; // NOLINT(readability-redundant-member-init)
    // unimportant_positions: drops position tracks of mapped bones but Root and Hips, whose
    // positions differ between models.
    bool remove_unimportant_positions = true;
    // keep_global_rest_on_leftovers: unmapped bones with no mapped bone under them keep their
    // rest in the model's space.
    bool keep_global_rest_on_leftovers = true;
};

struct RetargetReport {
    // The profile's required bones the map leaves out, in the profile's order.
    std::vector<std::string> missing;
    // The skeleton's bones no profile bone maps, by their names after renaming.
    std::vector<std::string> unmapped;
    // Renamed nodes that weren't mapped but had a profile bone's name, by their new names.
    std::map<std::string, std::string> moved_aside{}; // NOLINT(readability-redundant-member-init)
    // The hips' height above the model's origin at rest: its motion scale.
    float motion_scale = 1.0F;
};

// Retargets a model's skeleton to the humanoid profile, as Godot's importer does with a bone map
// and its default options: folds the nodes above the skeleton into its roots
// (apply_node_transforms), renames the mapped bones to the profile's names (moving nodes that
// have a profile name aside, "Spine" becoming "Spine2"), turns each bone's rest so mapped bones
// lie as the profile's do and every animation, skin and node under a bone moves with them
// (Overwrite Axis), adds a rest rotation track where an animation has none for a mapped bone,
// and drops unimportant position tracks. The model draws the same at rest. A bone map naming
// a profile bone or model bone that doesn't exist, or one model bone twice, is an error, as is a
// model with no skin.
[[nodiscard]] Result<RetargetReport> retarget_model(Model& model, const RetargetOptions& options);

// A model's motion scale, as Godot's Skeleton3D::motion_scale after retargeting normalizes
// position tracks: how high its Hips node is above its origin at rest; 0 for a model without a
// skin or a node named Hips. Position tracks of one such model playing on another scale by the
// ratio of theirs (blend_animation), so a short character walks with shorter steps.
[[nodiscard]] float motion_scale(const Model& model);

// Rigs an unrigged figure standing on its lowest point and facing +z: a skeleton of the
// profile's body bones (no fingers, eyes or jaw) placed by the average proportions of a standing
// figure scaled to its height, the arms reaching for its widest point, and every vertex weighted
// to its two nearest bones on its side of the figure. Parts become skinned meshes placed in the
// model's space; animations are left out. The placeholder rig the library's test generator
// makes; services rig a figure better, from what they see of it. A model with a skin already, or
// no mesh, is an error.
[[nodiscard]] Result<Model> rig_humanoid(const Model& model);

} // namespace slash::scene3d
