#pragma once

#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <functional>
#include <string>
#include <vector>

// Skeleton modifiers: bones of a model turned after its animations and Skeleton3D, as Godot 4.6's
// SkeletonModifier3D children of a Skeleton3D, by TwoBoneIK3D, FABRIK3D, CCDIK3D and JacobianIK3D
// components (Godot's IK modifiers; src/ik.cpp follows their code):
//
//   std::vector<scene3d::NodePose> pose = scene3d::pose_of(model, clips, skeleton, load);
//   scene3d::apply_modifiers(world, entity, model, place, global, pose);
//
// A two-bone solver and a deterministic iterating one solve from the pose each time. Another
// iterating one carries its chain from tick to tick, as Godot's does from frame to frame: the
// skeleton_modifiers system steps it once a tick, and apply_modifiers shows where it left it.
// Chains are kept beside the components, by entity ID, so saving, snapshots and undo leave them
// alone.
namespace slash::scene3d {

// An entity's transform in the world, as Tree3D::global gives it or part of the way between two
// ticks.
using Placer = std::function<world::Affine3D(entt::entity)>;

// Whether modifiers turn the bones of an entity's model: a TwoBoneIK3D, FABRIK3D, CCDIK3D or
// JacobianIK3D on it or on an entity in its Transform3D children.
[[nodiscard]] bool has_modifiers(const world::World& world, entt::entity entity);

// Runs the modifiers of an entity, placed at place, on a pose of the model it draws, in the order
// Godot runs a Skeleton3D's modifier children: those on the entity, then those on each entity in
// its Transform3D children, in list order; on each, TwoBoneIK3D, FABRIK3D, CCDIK3D, then
// JacobianIK3D. Targets and poles are where global puts them.
void apply_modifiers(const world::World& world, entt::entity entity, const Model& model,
                     const world::Affine3D& place, const Placer& global,
                     std::vector<NodePose>& pose);

// Adds the skeleton_modifiers system (priority 920, after the animation systems), which steps
// each iterating modifier that isn't deterministic once a tick, on a pose of the model load
// gives for its entity's MeshInstance3D.
void add_modifier_system(world::World& world, ModelLoader load);

// The problems with the modifiers of an entity that draws model (null when it draws none): bone
// names the model hasn't, with "did you mean", bones that aren't one under the other, names of
// directions and axes, and settings with no target or pole. Each is at a JSON Pointer in the
// modifier's component on entity, which is the drawing entity or one of its children.
struct ModifierProblem {
    entt::entity entity{entt::null};
    std::string path;
    std::string message;
};
[[nodiscard]] std::vector<ModifierProblem> check_modifiers(const world::World& world,
                                                           entt::entity entity, const Model* model);
// Modifiers on entities that neither draw a model nor are in the Transform3D children of one that
// does, so turn nothing.
[[nodiscard]] std::vector<ModifierProblem> check_stray_modifiers(const world::World& world);

} // namespace slash::scene3d
