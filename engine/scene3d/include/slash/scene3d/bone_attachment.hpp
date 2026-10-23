#pragma once

#include "slash/scene3d/ik.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <functional>
#include <vector>

// Bone attachments, as Godot's BoneAttachment3D: entities that the skeleton_modifiers system puts
// where a bone of a model is posed each tick (ik.hpp), after the model's animations and
// modifiers, so what hangs under them moves with the bone.
namespace slash::scene3d {

// The entity drawing the model a BoneAttachment3D on attachment follows: its external_skeleton,
// or else its Transform3D parent; null when neither is a valid entity.
[[nodiscard]] entt::entity attachment_skeleton(const world::World& world, const world::Tree3D& tree,
                                               entt::entity attachment);

// Puts each entity with a BoneAttachment3D where its bone is posed, as Godot's
// BoneAttachment3D::on_skeleton_update: its Transform3D becomes what puts it at the drawing
// entity's transform times the bone's in the model, the pose being pose_of's, then the modifiers'
// (apply_modifiers), on the models load gives. Parents come before their children, so one
// attachment can hang under another. Fields that don't change are left alone.
void place_attachments(world::World& world, const ModelLoader& load);

// The problems with the bone attachments of a world: bone names their model hasn't, with "did you
// mean", and attachments that follow no entity drawing a model file. model_of gives the model an
// entity draws, or null when it draws none or its model doesn't load, which leaves its
// attachments' bone names unchecked.
[[nodiscard]] std::vector<ModifierProblem>
check_attachments(const world::World& world,
                  const std::function<const Model*(entt::entity)>& model_of);

} // namespace slash::scene3d
