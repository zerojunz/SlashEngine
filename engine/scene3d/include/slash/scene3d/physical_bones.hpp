#pragma once

#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/components.hpp"
#include "slash/world/physical_bones.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <utility>
#include <vector>

// Ragdolls, as Godot 4.6's BoneSimulation3D and SimulatedBone3D: a simulator is a skeleton
// modifier (ik.hpp runs it in its place among them) whose SimulatedBone3D children are bodies for
// the model's bones. engine/physics3d builds the bodies and their joints from the links
// physical_bone_links works out; the skeleton_modifiers system moves the bodies that follow the
// pose, and the bones that simulate follow their bodies wherever poses are made.
namespace slash::scene3d {

// A SimulatedBone3D of a simulator, bound to the bone of the model it moves, as Godot's
// BoneSimulation3D binds them.
struct PhysicalBone {
    entt::entity entity{entt::null};
    // The model's node, or -1 when it has no such bone or an earlier physical bone has it.
    int bone = -1;
    // The physical bone of the nearest bone above its own that has one.
    entt::entity parent{entt::null};
    // The simulator simulates it, so physics moves it.
    bool simulating = false;
};

// The physical bones of the BoneSimulation3D on entity: the entities in its Transform3D
// children with a SimulatedBone3D, in list order, bound to bones of model (null when there is
// none, which binds none).
[[nodiscard]] std::vector<PhysicalBone> physical_bones(const world::World& world,
                                                       entt::entity entity, const Model* model);

// Each entity with a BoneSimulation3D, with the entity that draws the model it turns: its
// own, or the one whose Transform3D children list it; null when neither has a Model3D.
[[nodiscard]] std::vector<std::pair<entt::entity, entt::entity>>
simulators(const world::World& world);

// A body's transform in its bone's space, from SimulatedBone3D's body offset.
[[nodiscard]] world::Affine3D body_offset(const world::SimulatedBone3D& bone);

// How each physical bone under a simulator takes part in physics, on the models load gives:
// simulating, following its bone while the simulator is active, or off.
[[nodiscard]] world::PhysicalBoneLinks physical_bone_links(const world::World& world,
                                                           const ModelLoader& load);

// Puts an entity where global says, as Godot's set_global_transform: its own Transform3D is what
// puts it there in its parent's space. Fields that don't change are left alone.
void set_global_transform(world::World& world, const world::Tree3D& tree, entt::entity entity,
                          const world::Affine3D& global);
// Makes an entity top_level or not, keeping it where it is, as Godot's set_as_top_level.
void set_top_level(world::World& world, const world::Tree3D& tree, entt::entity entity,
                   bool top_level);

} // namespace slash::scene3d
