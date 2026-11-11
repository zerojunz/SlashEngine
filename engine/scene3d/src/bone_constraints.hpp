#pragma once

#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <functional>
#include <optional>
#include <vector>

// Godot's LookAtModifier3D (scene/3d/look_at_modifier_3d.cpp) and CopyTransformModifier3D
// (scene/3d/copy_transform_modifier_3d.cpp, over bone_constraint_3d.cpp), ported line for line
// onto a model's pose in its own space. ik.cpp runs them among the other skeleton modifiers.
namespace slash::scene3d::detail {

// What a LookAtModifier3D carries from one process to the next, as Godot's node does: the way it
// last looked, the turn it eases from and how much of the turn is left.
struct LookAtState {
    Vec3 forward_vector;
    Vec3 forward_vector_nrm;
    Quat from_q;
    Quat prev_q;
    float remaining = 0.0F;
};

// Where a LookAtModifier3D looks: its target and its external origin, each in the model's space,
// or none.
struct LookAtPlaces {
    std::optional<Vec3> target;
    std::optional<Vec3> origin_external;
};

// LookAtModifier3D::_process_modification: turns modifier.bone of pose, advancing state by delta
// seconds. influence is the modifier's, which decides whether turns ease through the rest.
void run_look_at(const world::LookAtModifier3D& modifier, const Model& model,
                 std::vector<NodePose>& pose, const LookAtPlaces& places, LookAtState& state,
                 double delta);

// BoneConstraint3D::_process_modification over CopyTransformModifier3D's settings. node_place
// gives a reference_node entity's transform in the model's space, or none.
using NodePlace = std::function<std::optional<world::Affine3D>(entt::entity)>;
void run_copy_transform(const world::CopyTransformModifier3D& modifier, const Model& model,
                        std::vector<NodePose>& pose, const NodePlace& node_place);

} // namespace slash::scene3d::detail
