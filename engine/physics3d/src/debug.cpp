// The visual debugger's record of a 3D step, as Chaos's Visual Debugger keeps one: the contacts
// the step solved, how hard each joint pulled and how far it stretched, and how many bodies were
// awake. The fields' share is recorded where they act (fields.cpp).

#include "space.hpp"

#include "slash/world/world.hpp"

#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <variant>

namespace slash::physics3d::detail {

namespace {

Vec3 from_jolt_vector(JPH::Vec3 v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

// The impulse a constraint's position part took in its last collision step, as Jolt keeps it.
float position_lambda(const JPH::Constraint& constraint) {
    switch (constraint.GetSubType()) {
    case JPH::EConstraintSubType::Fixed:
        return static_cast<const JPH::FixedConstraint&>(constraint)
            .GetTotalLambdaPosition()
            .Length();
    case JPH::EConstraintSubType::Point:
        return static_cast<const JPH::PointConstraint&>(constraint)
            .GetTotalLambdaPosition()
            .Length();
    case JPH::EConstraintSubType::Hinge:
        return static_cast<const JPH::HingeConstraint&>(constraint)
            .GetTotalLambdaPosition()
            .Length();
    case JPH::EConstraintSubType::Slider: {
        const JPH::Vector<2> lambda =
            static_cast<const JPH::SliderConstraint&>(constraint).GetTotalLambdaPosition();
        return std::sqrt((lambda[0] * lambda[0]) + (lambda[1] * lambda[1]));
    }
    case JPH::EConstraintSubType::SwingTwist:
        return static_cast<const JPH::SwingTwistConstraint&>(constraint)
            .GetTotalLambdaPosition()
            .Length();
    case JPH::EConstraintSubType::SixDOF:
        return static_cast<const JPH::SixDOFConstraint&>(constraint)
            .GetTotalLambdaPosition()
            .Length();
    default:
        return 0.0F;
    }
}

std::string joint_kind(const JointKey& key) {
    return std::visit(
        [](const auto& joint) {
            using Joint = std::decay_t<decltype(joint)>;
            if constexpr (std::is_same_v<Joint, world::PinJoint3D>) {
                return std::string("PinJoint3D");
            } else if constexpr (std::is_same_v<Joint, world::HingeJoint3D>) {
                return std::string("HingeJoint3D");
            } else if constexpr (std::is_same_v<Joint, world::SliderJoint3D>) {
                return std::string("SliderJoint3D");
            } else if constexpr (std::is_same_v<Joint, world::ConeTwistJoint3D>) {
                return std::string("ConeTwistJoint3D");
            } else {
                return std::string("Generic6DOFJoint3D");
            }
        },
        key);
}

} // namespace

void Space::note_debug_contact(const JPH::Body& a, const JPH::Body& b,
                               const JPH::ContactManifold& manifold) {
    if (debug_ == nullptr || a.IsSensor() || b.IsSensor() ||
        manifold.mRelativeContactPointsOn1.empty()) {
        return;
    }
    const Body* first = Space::body_of(a);
    const Body* second = Space::body_of(b);
    if (first == nullptr || second == nullptr) {
        return;
    }
    // The normal points from the first body toward the second; the record names them in order.
    const bool swap = second->name < first->name;
    JPH::RVec3 middle = JPH::RVec3::sZero();
    const std::size_t count = manifold.mRelativeContactPointsOn1.size();
    for (std::size_t i = 0; i < count; ++i) {
        middle += manifold.GetWorldSpaceContactPointOn1(static_cast<JPH::uint>(i));
        middle += manifold.GetWorldSpaceContactPointOn2(static_cast<JPH::uint>(i));
    }
    middle /= static_cast<JPH::Real>(2 * count);
    const float speed =
        (a.GetPointVelocity(middle) - b.GetPointVelocity(middle)).Dot(manifold.mWorldSpaceNormal);
    const Vec3 normal = from_jolt_vector(manifold.mWorldSpaceNormal);
    world::DebugContact contact{
        .a = swap ? second->name : first->name,
        .b = swap ? first->name : second->name,
        .point = from_jolt_place(middle),
        .normal = swap ? normal * -1.0F : normal,
        .depth = std::max(manifold.mPenetrationDepth, 0.0F),
        .speed = std::max(speed, 0.0F),
    };
    std::pair<std::string, std::string> key{contact.a, contact.b};
    const auto found = debug_contacts_.find(key);
    if (found == debug_contacts_.end()) {
        debug_contacts_.emplace(std::move(key), std::move(contact));
    } else if (found->second.depth < contact.depth) {
        found->second = std::move(contact);
    }
}

void Space::finish_debug(const world::World& world, float seconds) {
    if (debug_ == nullptr) {
        return;
    }
    world::PhysicsDebugStep& step = *debug_;
    debug_ = nullptr;
    for (auto& [pair, contact] : debug_contacts_) {
        step.contacts.push_back(std::move(contact));
    }
    debug_contacts_.clear();
    for (const auto& [entity, body] : bodies_) {
        if (body->kind == Kind::Rigid || body->kind == Kind::Bone) {
            ++(body->sleeping ? step.sleeping : step.awake);
        }
    }
    const float substep = seconds / static_cast<float>(std::max(settings_.substeps, 1));
    // Where a joint's frame is in the world, by one of its bodies' places.
    const auto anchor = [&](entt::entity on, const world::Affine3D& local) {
        const Body* made = on != entt::null ? body(on) : nullptr;
        return made != nullptr ? (made->placed * local).origin : local.origin;
    };
    const auto add = [&](entt::entity entity, std::string kind, const Joint& joint) {
        if (joint.constraint == nullptr) {
            return;
        }
        const Vec3 apart =
            anchor(joint.body_a, joint.local_a) - anchor(joint.body_b, joint.local_b);
        const float stretch = std::sqrt(dot(apart, apart));
        step.joints.push_back({
            .id = std::string(world.id_of(entity)),
            .kind = std::move(kind),
            .a = joint.body_a != entt::null ? std::string(world.id_of(joint.body_a)) : "",
            .b = joint.body_b != entt::null ? std::string(world.id_of(joint.body_b)) : "",
            .force = substep > 0.0F ? position_lambda(*joint.constraint) / substep : 0.0F,
            .stretch = stretch,
        });
    };
    for (const auto& [entity, joint] : joints_) {
        add(entity, joint_kind(joint.key), joint);
    }
    for (const auto& [entity, bone] : bone_joints_) {
        add(entity, "PhysicalBone3D", bone.joint);
    }
    std::ranges::sort(step.joints, [](const world::DebugJoint& x, const world::DebugJoint& y) {
        return x.id < y.id;
    });
}

} // namespace slash::physics3d::detail
