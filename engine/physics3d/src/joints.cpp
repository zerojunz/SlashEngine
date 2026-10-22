#include "space.hpp"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <iterator>
#include <numbers>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// 3D joints as Godot's Joint3D nodes make them and its Jolt module builds them
// (modules/jolt_physics/joints): each joint component becomes a Jolt constraint between the
// bodies it names, in frames taken from the entity's transform when it is made.
namespace slash::physics3d::detail {

namespace {

using world::ConeTwistJoint3D;
using world::Generic6DOFJoint3D;
using world::HingeJoint3D;
using world::Joint3DAngularAxis;
using world::Joint3DLinearAxis;
using world::PhysicalBone3D;
using world::PhysicalBone3DAxis;
using world::PinJoint3D;
using world::SliderJoint3D;
using world::Transform3D;

constexpr float kPi = std::numbers::pi_v<float>;

// Whether two of an entity's own Transform3Ds put it in the same place.
bool same_place(const Transform3D& a, const Transform3D& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale &&
           a.top_level == b.top_level;
}

entt::entity node_a_of(const JointKey& key) {
    return std::visit([](const auto& joint) { return joint.node_a; }, key);
}

entt::entity node_b_of(const JointKey& key) {
    return std::visit([](const auto& joint) { return joint.node_b; }, key);
}

bool excludes(const JointKey& key) {
    return std::visit([](const auto& joint) { return joint.exclude_nodes_from_collision; }, key);
}

// A joint's frames for Jolt, as Godot's JoltJoint3D::_shift_reference_frames gives them: each
// in its body's unscaled space from its center of mass (in the world's for no body), the first
// moved back along its axes by linear_shift and turned about its z axis by z_turn.
struct Frames {
    JPH::Mat44 a;
    JPH::Mat44 b;
};

Frames shift(const Joint& joint, Vec3 scale_a, Vec3 scale_b, Vec3 linear_shift, float z_turn) {
    const JPH::Mat44 local_a = matrix_of(joint.local_a);
    const JPH::Mat44 local_b = matrix_of(joint.local_b);
    JPH::Vec3 origin_a = local_a.GetTranslation();
    JPH::Vec3 origin_b = local_b.GetTranslation();
    if (joint.body_a != entt::null) {
        origin_a = (origin_a * to_jolt(scale_a)) - to_jolt(joint.com_a);
    }
    if (joint.body_b != entt::null) {
        origin_b = (origin_b * to_jolt(scale_b)) - to_jolt(joint.com_b);
    }
    const JPH::Mat44 basis_a = local_a.GetRotation();
    Frames frames{.a = basis_a * JPH::Mat44::sRotationZ(z_turn), .b = local_b.GetRotation()};
    frames.a.SetTranslation(origin_a - basis_a.Multiply3x3(to_jolt(linear_shift)));
    frames.b.SetTranslation(origin_b);
    return frames;
}

// Creates a constraint between Jolt bodies, the second the world when there is none.
template <typename Settings>
JPH::Ref<JPH::Constraint> create(const Settings& settings, JPH::Body& a, JPH::Body* b) {
    return settings.Create(a, b != nullptr ? *b : JPH::Body::sFixedToWorld);
}

// Godot's JoltHingeJoint3D::_build_fixed and the sliders': the two frames held together.
JPH::Ref<JPH::Constraint> build_fixed(const Frames& frames, JPH::Body& a, JPH::Body* b) {
    JPH::FixedConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mAutoDetectPoint = false;
    settings.mPoint1 = frames.a.GetTranslation();
    settings.mAxisX1 = frames.a.GetAxisX();
    settings.mAxisY1 = frames.a.GetAxisY();
    settings.mPoint2 = frames.b.GetTranslation();
    settings.mAxisX2 = frames.b.GetAxisX();
    settings.mAxisY2 = frames.b.GetAxisY();
    return create(settings, a, b);
}

// Godot's JoltPinJoint3D::rebuild.
JPH::Ref<JPH::Constraint> build_pin(const Frames& frames, JPH::Body& a, JPH::Body* b) {
    JPH::PointConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mPoint1 = frames.a.GetTranslation();
    settings.mPoint2 = frames.b.GetTranslation();
    return create(settings, a, b);
}

// Godot's JoltHingeJoint3D::rebuild: limits centered on their midpoint, by turning the first
// frame there, and a fixed joint for limits that meet.
JPH::Ref<JPH::Constraint> build_hinge(const Joint& joint, const HingeJoint3D& hinge, Vec3 scale_a,
                                      Vec3 scale_b, JPH::Body& a, JPH::Body* b) {
    float ref_shift = 0.0F;
    float limit = kPi;
    const bool limited =
        hinge.angular_limit_enabled && hinge.angular_limit_lower <= hinge.angular_limit_upper;
    if (limited) {
        const float midpoint = (hinge.angular_limit_lower + hinge.angular_limit_upper) / 2.0F;
        ref_shift = -midpoint;
        limit = std::min(hinge.angular_limit_upper - midpoint, kPi);
    }
    const Frames frames = shift(joint, scale_a, scale_b, {}, ref_shift);
    if (hinge.angular_limit_enabled && hinge.angular_limit_lower == hinge.angular_limit_upper) {
        return build_fixed(frames, a, b);
    }
    JPH::HingeConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mPoint1 = frames.a.GetTranslation();
    settings.mHingeAxis1 = frames.a.GetAxisZ();
    settings.mNormalAxis1 = frames.a.GetAxisX();
    settings.mPoint2 = frames.b.GetTranslation();
    settings.mHingeAxis2 = frames.b.GetAxisZ();
    settings.mNormalAxis2 = frames.b.GetAxisX();
    settings.mLimitsMin = -limit;
    settings.mLimitsMax = limit;
    JPH::Ref<JPH::Constraint> made = create(settings, a, b);
    auto* constraint = static_cast<JPH::HingeConstraint*>(made.GetPtr());
    constraint->SetMotorState(hinge.motor_enabled ? JPH::EMotorState::Velocity
                                                  : JPH::EMotorState::Off);
    // Jolt turns counterclockwise where Godot turns clockwise.
    constraint->SetTargetAngularVelocity(-hinge.motor_target_velocity);
    return made;
}

// Godot's JoltSliderJoint3D::rebuild: limits centered as a hinge's, by moving the first frame.
JPH::Ref<JPH::Constraint> build_slider(const Joint& joint, const SliderJoint3D& slider,
                                       Vec3 scale_a, Vec3 scale_b, JPH::Body& a, JPH::Body* b) {
    float ref_shift = 0.0F;
    float limit = FLT_MAX;
    if (slider.linear_limit_lower <= slider.linear_limit_upper) {
        const float midpoint = (slider.linear_limit_lower + slider.linear_limit_upper) / 2.0F;
        ref_shift = -midpoint;
        limit = slider.linear_limit_upper - midpoint;
    }
    const Frames frames = shift(joint, scale_a, scale_b, {ref_shift, 0.0F, 0.0F}, 0.0F);
    if (slider.linear_limit_lower == slider.linear_limit_upper) {
        return build_fixed(frames, a, b);
    }
    JPH::SliderConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mAutoDetectPoint = false;
    settings.mPoint1 = frames.a.GetTranslation();
    settings.mSliderAxis1 = frames.a.GetAxisX();
    settings.mNormalAxis1 = frames.a.GetAxisZ();
    settings.mPoint2 = frames.b.GetTranslation();
    settings.mSliderAxis2 = frames.b.GetAxisX();
    settings.mNormalAxis2 = frames.b.GetAxisZ();
    settings.mLimitsMin = -limit;
    settings.mLimitsMax = limit;
    return create(settings, a, b);
}

// Godot's JoltConeTwistJoint3D::_build_swing_twist: spans out of range leave the joint free.
JPH::Ref<JPH::Constraint> build_cone_twist(const Frames& frames, const ConeTwistJoint3D& cone,
                                           JPH::Body& a, JPH::Body* b) {
    JPH::SwingTwistConstraintSettings settings;
    const bool twist_valid = cone.twist_span >= 0.0F && cone.twist_span <= kPi;
    const bool swing_valid = cone.swing_span >= 0.0F && cone.swing_span <= kPi;
    settings.mTwistMinAngle = twist_valid ? -cone.twist_span : -kPi;
    settings.mTwistMaxAngle = twist_valid ? cone.twist_span : kPi;
    if (swing_valid) {
        settings.mNormalHalfConeAngle = cone.swing_span;
        settings.mPlaneHalfConeAngle = cone.swing_span;
    } else {
        settings.mNormalHalfConeAngle = kPi;
        settings.mPlaneHalfConeAngle = kPi;
        settings.mTwistMinAngle = -kPi;
        settings.mTwistMaxAngle = kPi;
    }
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mPosition1 = frames.a.GetTranslation();
    settings.mTwistAxis1 = frames.a.GetAxisX();
    settings.mPlaneAxis1 = frames.a.GetAxisZ();
    settings.mPosition2 = frames.b.GetTranslation();
    settings.mTwistAxis2 = frames.b.GetAxisX();
    settings.mPlaneAxis2 = frames.b.GetAxisZ();
    settings.mSwingType = JPH::ESwingType::Pyramid;
    return create(settings, a, b);
}

// Godot's JoltGeneric6DOFJoint3D::rebuild with its motors and springs.
JPH::Ref<JPH::Constraint> build_6dof(const Frames& frames, const Generic6DOFJoint3D& joint,
                                     JPH::Body& a, JPH::Body* b) {
    using Axis = JPH::SixDOFConstraintSettings::EAxis;
    const std::array<const Joint3DLinearAxis*, 3> linear{&joint.linear_x, &joint.linear_y,
                                                         &joint.linear_z};
    const std::array<const Joint3DAngularAxis*, 3> angular{&joint.angular_x, &joint.angular_y,
                                                           &joint.angular_z};
    JPH::SixDOFConstraintSettings settings;
    for (int i = 0; i < 3; ++i) {
        const auto axis = static_cast<Axis>(Axis::TranslationX + i);
        const Joint3DLinearAxis& along = *linear.at(static_cast<std::size_t>(i));
        if (!along.limit_enabled || along.limit_lower > along.limit_upper) {
            settings.MakeFreeAxis(axis);
        } else {
            settings.SetLimitedAxis(axis, along.limit_lower, along.limit_upper);
        }
    }
    for (int i = 0; i < 3; ++i) {
        const auto axis = static_cast<Axis>(Axis::RotationX + i);
        const Joint3DAngularAxis& about = *angular.at(static_cast<std::size_t>(i));
        // Jolt turns counterclockwise where Godot turns clockwise.
        const float lower = -about.limit_upper;
        const float upper = -about.limit_lower;
        if (!about.limit_enabled || lower > upper) {
            settings.MakeFreeAxis(axis);
        } else {
            settings.SetLimitedAxis(axis, std::clamp(lower, -kPi, kPi),
                                    std::clamp(upper, -kPi, kPi));
        }
    }
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mPosition1 = frames.a.GetTranslation();
    settings.mAxisX1 = frames.a.GetAxisX();
    settings.mAxisY1 = frames.a.GetAxisY();
    settings.mPosition2 = frames.b.GetTranslation();
    settings.mAxisX2 = frames.b.GetAxisX();
    settings.mAxisY2 = frames.b.GetAxisY();
    settings.mSwingType = JPH::ESwingType::Pyramid;
    JPH::Ref<JPH::Constraint> made = create(settings, a, b);
    auto* constraint = static_cast<JPH::SixDOFConstraint*>(made.GetPtr());

    // Motors drive an axis at a speed; springs, without a motor, pull it to its equilibrium.
    const auto drive = [&](Axis axis, bool motor, bool spring, float motor_limit, float stiffness,
                           float damping, bool linear_axis) {
        if (motor) {
            constraint->SetMotorState(axis, JPH::EMotorState::Velocity);
        } else if (spring) {
            constraint->SetMotorState(axis, JPH::EMotorState::Position);
        } else {
            constraint->SetMotorState(axis, JPH::EMotorState::Off);
        }
        JPH::MotorSettings& motor_settings = constraint->GetMotorSettings(axis);
        const float limit = motor ? motor_limit : FLT_MAX;
        if (linear_axis) {
            motor_settings.SetForceLimit(limit);
        } else {
            motor_settings.SetTorqueLimit(limit);
        }
        motor_settings.mSpringSettings.mMode = JPH::ESpringMode::StiffnessAndDamping;
        motor_settings.mSpringSettings.mStiffness = stiffness;
        motor_settings.mSpringSettings.mDamping = damping;
    };
    for (int i = 0; i < 3; ++i) {
        const Joint3DLinearAxis& along = *linear.at(static_cast<std::size_t>(i));
        drive(static_cast<Axis>(Axis::TranslationX + i), along.motor_enabled, along.spring_enabled,
              along.motor_force_limit, along.spring_stiffness, along.spring_damping, true);
        const Joint3DAngularAxis& about = *angular.at(static_cast<std::size_t>(i));
        drive(static_cast<Axis>(Axis::RotationX + i), about.motor_enabled, about.spring_enabled,
              about.motor_force_limit, about.spring_stiffness, about.spring_damping, false);
    }
    constraint->SetTargetVelocityCS(JPH::Vec3(joint.linear_x.motor_target_velocity,
                                              joint.linear_y.motor_target_velocity,
                                              joint.linear_z.motor_target_velocity));
    constraint->SetTargetAngularVelocityCS(JPH::Vec3(-joint.angular_x.motor_target_velocity,
                                                     -joint.angular_y.motor_target_velocity,
                                                     -joint.angular_z.motor_target_velocity));
    constraint->SetTargetPositionCS(JPH::Vec3(joint.linear_x.spring_equilibrium_point,
                                              joint.linear_y.spring_equilibrium_point,
                                              joint.linear_z.spring_equilibrium_point));
    // Godot's Basis::from_euler in ZYX order, of the negated angles.
    constraint->SetTargetOrientationCS(
        JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -joint.angular_z.spring_equilibrium_point) *
        JPH::Quat::sRotation(JPH::Vec3::sAxisY(), -joint.angular_y.spring_equilibrium_point) *
        JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -joint.angular_x.spring_equilibrium_point));
    return made;
}

// A joint's constraint between Jolt bodies, the second the world when there is none.
JPH::Ref<JPH::Constraint> build(const Joint& joint, Vec3 scale_a, Vec3 scale_b, JPH::Body& a,
                                JPH::Body* b) {
    if (const auto* hinge = std::get_if<HingeJoint3D>(&joint.key); hinge != nullptr) {
        return build_hinge(joint, *hinge, scale_a, scale_b, a, b);
    }
    if (const auto* slider = std::get_if<SliderJoint3D>(&joint.key); slider != nullptr) {
        return build_slider(joint, *slider, scale_a, scale_b, a, b);
    }
    const Frames frames = shift(joint, scale_a, scale_b, {}, 0.0F);
    if (const auto* cone = std::get_if<ConeTwistJoint3D>(&joint.key); cone != nullptr) {
        return build_cone_twist(frames, *cone, a, b);
    }
    if (const auto* six = std::get_if<Generic6DOFJoint3D>(&joint.key); six != nullptr) {
        return build_6dof(frames, *six, a, b);
    }
    return build_pin(frames, a, b);
}

// A physical bone's joint as the joint component that builds the same constraint, as Godot's
// PhysicalBone3D::_reload_joint passes its parameters to the physics server; nothing for "none".
std::optional<JointKey> joint_key_of(const PhysicalBone3D& bone) {
    if (bone.joint_type == "pin") {
        return PinJoint3D{.exclude_nodes_from_collision = false};
    }
    if (bone.joint_type == "cone") {
        return ConeTwistJoint3D{.exclude_nodes_from_collision = false,
                                .swing_span = bone.swing_span,
                                .twist_span = bone.twist_span};
    }
    if (bone.joint_type == "hinge") {
        return HingeJoint3D{.exclude_nodes_from_collision = false,
                            .angular_limit_enabled = bone.angular_limit_enabled,
                            .angular_limit_upper = bone.angular_limit_upper,
                            .angular_limit_lower = bone.angular_limit_lower,
                            .motor_enabled = false};
    }
    if (bone.joint_type == "slider") {
        return SliderJoint3D{.exclude_nodes_from_collision = false,
                             .linear_limit_upper = bone.linear_limit_upper,
                             .linear_limit_lower = bone.linear_limit_lower};
    }
    if (bone.joint_type == "6dof") {
        const auto linear = [](const PhysicalBone3DAxis& axis) {
            return Joint3DLinearAxis{.limit_enabled = axis.linear_limit_enabled,
                                     .limit_upper = axis.linear_limit_upper,
                                     .limit_lower = axis.linear_limit_lower,
                                     .motor_enabled = false,
                                     .motor_target_velocity = 0.0F,
                                     .motor_force_limit = 0.0F,
                                     .spring_enabled = axis.linear_spring_enabled,
                                     .spring_stiffness = axis.linear_spring_stiffness,
                                     .spring_damping = axis.linear_spring_damping,
                                     .spring_equilibrium_point = axis.linear_equilibrium_point};
        };
        const auto angular = [](const PhysicalBone3DAxis& axis) {
            return Joint3DAngularAxis{.limit_enabled = axis.angular_limit_enabled,
                                      .limit_upper = axis.angular_limit_upper,
                                      .limit_lower = axis.angular_limit_lower,
                                      .motor_enabled = false,
                                      .motor_target_velocity = 0.0F,
                                      .motor_force_limit = 0.0F,
                                      .spring_enabled = axis.angular_spring_enabled,
                                      .spring_stiffness = axis.angular_spring_stiffness,
                                      .spring_damping = axis.angular_spring_damping,
                                      .spring_equilibrium_point = axis.angular_equilibrium_point};
        };
        return Generic6DOFJoint3D{.exclude_nodes_from_collision = false,
                                  .linear_x = linear(bone.x),
                                  .linear_y = linear(bone.y),
                                  .linear_z = linear(bone.z),
                                  .angular_x = angular(bone.x),
                                  .angular_y = angular(bone.y),
                                  .angular_z = angular(bone.z)};
    }
    return std::nullopt;
}

// Whether two of a physical bone's components put its joint's frames in the same place.
bool same_frames(const PhysicalBone3D& a, const PhysicalBone3D& b) {
    return a.joint_type == b.joint_type && a.joint_rotation == b.joint_rotation &&
           a.body_offset_position == b.body_offset_position &&
           a.body_offset_rotation == b.body_offset_rotation;
}

} // namespace

void Space::remove_joint(Joint& joint) {
    if (joint.constraint != nullptr && system_ != nullptr) {
        system_->RemoveConstraint(joint.constraint);
        // What the joint held moves on its own now, as a body it held up falls; Jolt leaves
        // sleeping bodies asleep.
        const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
        for (const JPH::BodyID& id : {joint.id_a, joint.id_b}) {
            const JPH::Body* body = id.IsInvalid() ? nullptr : locks.TryGetBody(id);
            if (body != nullptr && body->IsDynamic() && body->IsInBroadPhase()) {
                system_->GetBodyInterfaceNoLock().ActivateBody(id);
            }
        }
    }
    joint.constraint = nullptr;
}

void Space::drop_joints(const JPH::BodyID& id) {
    for (auto& [entity, joint] : joints_) {
        if (joint.constraint != nullptr && (joint.id_a == id || joint.id_b == id)) {
            remove_joint(joint);
        }
    }
    for (auto& [entity, bone] : bone_joints_) {
        if (bone.joint.constraint != nullptr && (bone.joint.id_a == id || bone.joint.id_b == id)) {
            remove_joint(bone.joint);
        }
    }
}

void Space::sync_joints(world::World& world, const world::Tree3D& tree) {
    entt::registry& registry = world.ecs();
    // Joints whose entity is gone.
    joints_.erase_if([&](entt::entity entity, Joint& joint) {
        if (world.valid(entity) && registry.any_of<PinJoint3D, HingeJoint3D, SliderJoint3D,
                                                   ConeTwistJoint3D, Generic6DOFJoint3D>(entity)) {
            return false;
        }
        remove_joint(joint);
        return true;
    });

    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    // A body a joint can hold: any but an area, once Jolt has it.
    const auto body_for = [&](entt::entity node) -> const Body* {
        const Body* found = node != entt::null && world.valid(node) ? body(node) : nullptr;
        return found != nullptr && found->kind != Kind::Area && !found->id.IsInvalid() ? found
                                                                                       : nullptr;
    };
    const auto com_of = [&](const Body* found) {
        return found != nullptr ? from_jolt(found->shape->GetCenterOfMass()) : Vec3{};
    };

    const auto place = [&](entt::entity entity, const JointKey& key) {
        const auto* own_transform = registry.try_get<Transform3D>(entity);
        const Transform3D own = own_transform != nullptr ? *own_transform : Transform3D{};
        const entt::entity node_a = node_a_of(key);
        const entt::entity node_b = node_b_of(key);
        // Godot's Joint3D::_update_joint: both named nodes must be bodies, at least one named,
        // and not the same; with only node_b, it is the first body and the world the second.
        const Body* a = body_for(node_a);
        const Body* b = body_for(node_b);
        const bool usable = (node_a == entt::null || a != nullptr) &&
                            (node_b == entt::null || b != nullptr) &&
                            (a != nullptr || b != nullptr) && a != b;
        if (usable && a == nullptr) {
            std::swap(a, b);
        }

        Joint* existing = joints_.find(entity);
        if (existing != nullptr && existing->constraint != nullptr && usable &&
            existing->key == key && same_place(existing->own, own) && existing->node_a == node_a &&
            existing->node_b == node_b && existing->id_a == a->id &&
            existing->id_b == (b != nullptr ? b->id : JPH::BodyID()) &&
            existing->com_a == com_of(a) && existing->com_b == com_of(b)) {
            return;
        }
        if (existing != nullptr) {
            remove_joint(*existing);
        }
        if (!usable) {
            return; // the next sync tries again, once the bodies exist
        }
        if (existing == nullptr) {
            existing = &joints_.emplace(entity, Joint{});
        }
        Joint& joint = *existing;
        // Godot's Joint3D subclasses' _configure_joint: the joint's frame in each body's space
        // when it is made, or when it moves or names other bodies. Other changes rebuild it in
        // the frames it was made in, as Godot's rebuild.
        const bool remake =
            !same_place(joint.own, own) || joint.node_a != node_a || joint.node_b != node_b ||
            joint.body_a != a->entity || joint.body_b != (b != nullptr ? b->entity : entt::null) ||
            joint.id_a != a->id || joint.id_b != (b != nullptr ? b->id : JPH::BodyID()) ||
            joint.id_a.IsInvalid();
        if (remake) {
            const world::Affine3D global = tree.global(entity);
            const auto local_to = [&](const Body* found) {
                const world::Affine3D local =
                    found != nullptr ? tree.global(found->entity).inverse() * global : global;
                if (std::holds_alternative<PinJoint3D>(key)) {
                    // A pin keeps only its point, as PinJoint3D::_configure_joint.
                    return world::Affine3D{.origin = local.origin};
                }
                return unscale(local).transform;
            };
            joint.local_a = local_to(a);
            joint.local_b = local_to(b);
        }
        joint.key = key;
        joint.own = own;
        joint.node_a = node_a;
        joint.node_b = node_b;
        joint.body_a = a->entity;
        joint.body_b = b != nullptr ? b->entity : entt::null;
        joint.id_a = a->id;
        joint.id_b = b != nullptr ? b->id : JPH::BodyID();
        joint.com_a = com_of(a);
        joint.com_b = com_of(b);

        JPH::Body* jolt_a = locks.TryGetBody(a->id);
        JPH::Body* jolt_b = b != nullptr ? locks.TryGetBody(b->id) : nullptr;
        if (jolt_a == nullptr || (b != nullptr && jolt_b == nullptr)) {
            return;
        }
        joint.constraint =
            build(joint, a->shape_key.scale,
                  b != nullptr ? b->shape_key.scale : Vec3{1.0F, 1.0F, 1.0F}, *jolt_a, jolt_b);
        system_->AddConstraint(joint.constraint);
        // Godot's _wake_up_bodies.
        interface.ActivateBody(a->id);
        if (b != nullptr) {
            interface.ActivateBody(b->id);
        }
    };

    // In a fixed order of kinds, then each component's storage order; an entity with two joint
    // components keeps the first.
    for (const entt::entity entity : registry.view<PinJoint3D>()) {
        place(entity, registry.get<PinJoint3D>(entity));
    }
    for (const entt::entity entity : registry.view<HingeJoint3D>()) {
        if (!registry.all_of<PinJoint3D>(entity)) {
            place(entity, registry.get<HingeJoint3D>(entity));
        }
    }
    for (const entt::entity entity : registry.view<SliderJoint3D>()) {
        if (!registry.any_of<PinJoint3D, HingeJoint3D>(entity)) {
            place(entity, registry.get<SliderJoint3D>(entity));
        }
    }
    for (const entt::entity entity : registry.view<ConeTwistJoint3D>()) {
        if (!registry.any_of<PinJoint3D, HingeJoint3D, SliderJoint3D>(entity)) {
            place(entity, registry.get<ConeTwistJoint3D>(entity));
        }
    }
    for (const entt::entity entity : registry.view<Generic6DOFJoint3D>()) {
        if (!registry.any_of<PinJoint3D, HingeJoint3D, SliderJoint3D, ConeTwistJoint3D>(entity)) {
            place(entity, registry.get<Generic6DOFJoint3D>(entity));
        }
    }

    sync_bone_joints(world, tree);

    // Godot's joint_disable_collisions_between_bodies, as collision exceptions, and the bodies
    // physical bones' simulators keep them from. Bodies whose exceptions change forget their
    // cached contacts, so the change holds from the next step.
    std::set<std::pair<entt::entity, entt::entity>> exceptions;
    for (const auto& [entity, joint] : joints_) {
        if (joint.constraint != nullptr && joint.body_b != entt::null && excludes(joint.key)) {
            exceptions.insert(std::minmax(joint.body_a, joint.body_b));
        }
    }
    for (const auto& [entity, link] : bone_links_) {
        for (const entt::entity other : link.exceptions) {
            if (other != entity && world.valid(other)) {
                exceptions.insert(std::minmax(entity, other));
            }
        }
    }
    if (exceptions != exceptions_) {
        std::vector<std::pair<entt::entity, entt::entity>> changed;
        std::ranges::set_symmetric_difference(exceptions, exceptions_, std::back_inserter(changed));
        for (const auto& [first, second] : changed) {
            for (const entt::entity entity : {first, second}) {
                if (const Body* found = body(entity); found != nullptr && !found->id.IsInvalid()) {
                    interface.InvalidateContactCache(found->id);
                }
            }
        }
        exceptions_ = std::move(exceptions);
    }
}

void Space::sync_bone_joints(world::World& world, const world::Tree3D& tree) {
    entt::registry& registry = world.ecs();
    bone_joints_.erase_if([&](entt::entity entity, BoneJoint& bone) {
        if (world.valid(entity) && registry.all_of<PhysicalBone3D>(entity)) {
            return false;
        }
        remove_joint(bone.joint);
        return true;
    });
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    const auto bone_body = [&](entt::entity node) -> const Body* {
        const Body* found = node != entt::null && world.valid(node) ? body(node) : nullptr;
        return found != nullptr && found->kind == Kind::Bone && !found->id.IsInvalid() ? found
                                                                                       : nullptr;
    };
    const auto com_of = [](const Body* found) {
        return from_jolt(found->shape->GetCenterOfMass());
    };

    for (const entt::entity entity : registry.view<PhysicalBone3D>()) {
        const auto& component = registry.get<PhysicalBone3D>(entity);
        const auto link = bone_links_.find(entity);
        const entt::entity parent = link != bone_links_.end() ? link->second.parent : entt::null;
        const Body* a = bone_body(parent);
        const Body* b = bone_body(entity);
        const std::optional<JointKey> key = joint_key_of(component);
        const bool usable = a != nullptr && b != nullptr && a != b && key.has_value();

        BoneJoint* existing = bone_joints_.find(entity);
        if (existing != nullptr && existing->joint.constraint != nullptr && usable &&
            same_frames(existing->made, component) && existing->joint.key == *key &&
            existing->joint.body_a == a->entity && existing->joint.id_a == a->id &&
            existing->joint.id_b == b->id && existing->joint.com_a == com_of(a) &&
            existing->joint.com_b == com_of(b)) {
            continue;
        }
        if (existing != nullptr) {
            remove_joint(existing->joint);
        }
        if (!usable) {
            continue; // the next sync tries again, once there are bodies to join
        }
        if (existing == nullptr) {
            existing = &bone_joints_.emplace(entity, BoneJoint{});
        }
        Joint& joint = existing->joint;
        const bool remake = joint.id_a.IsInvalid() || !same_frames(existing->made, component) ||
                            joint.body_a != a->entity || joint.id_a != a->id || joint.id_b != b->id;
        if (remake) {
            // Godot's _reload_joint: the joint at the bone's origin in the body, turned by
            // joint_rotation (the joint offset, whose origin _fix_joint_offset puts there), and
            // the same place in the parent bone's body.
            world::Transform3D offset;
            offset.position = component.body_offset_position;
            offset.rotation = component.body_offset_rotation;
            world::Transform3D turn;
            turn.rotation = component.joint_rotation;
            world::Affine3D joint_offset = world::affine(turn);
            joint_offset.origin = world::affine(offset).inverse().origin;
            const world::Affine3D joint_global =
                unscale(tree.global(entity)).transform * joint_offset;
            const auto local_to = [&](entt::entity node) {
                const world::Affine3D local = tree.global(node).inverse() * joint_global;
                if (std::holds_alternative<PinJoint3D>(*key)) {
                    return world::Affine3D{.origin = local.origin};
                }
                return unscale(local).transform;
            };
            joint.local_a = local_to(a->entity);
            joint.local_b = local_to(entity);
        }
        existing->made = component;
        joint.key = *key;
        joint.body_a = a->entity;
        joint.body_b = entity;
        joint.id_a = a->id;
        joint.id_b = b->id;
        joint.com_a = com_of(a);
        joint.com_b = com_of(b);
        JPH::Body* jolt_a = locks.TryGetBody(a->id);
        JPH::Body* jolt_b = locks.TryGetBody(b->id);
        if (jolt_a == nullptr || jolt_b == nullptr) {
            continue;
        }
        joint.constraint = build(joint, a->shape_key.scale, b->shape_key.scale, *jolt_a, jolt_b);
        system_->AddConstraint(joint.constraint);
        interface.ActivateBody(a->id);
        interface.ActivateBody(b->id);
    }
}

void Space::pre_step_joints(float seconds) {
    // Godot gives a hinge motor's push as an impulse per tick; Jolt takes a torque, so it is
    // the impulse over the tick's length, as Godot's JoltHingeJoint3D estimates it.
    for (auto& [entity, joint] : joints_) {
        const auto* hinge = std::get_if<HingeJoint3D>(&joint.key);
        if (hinge == nullptr || joint.constraint == nullptr ||
            joint.constraint->GetSubType() != JPH::EConstraintSubType::Hinge) {
            continue;
        }
        const float torque = seconds > 0.0F ? hinge->motor_max_impulse / seconds : FLT_MAX;
        JPH::MotorSettings& motor =
            static_cast<JPH::HingeConstraint*>(joint.constraint.GetPtr())->GetMotorSettings();
        motor.mMinTorqueLimit = -torque;
        motor.mMaxTorqueLimit = torque;
    }
}

} // namespace slash::physics3d::detail
