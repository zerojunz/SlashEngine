// The muscles of physical bones, SlashEngine's own after Unreal's physical animation drives: each
// simulating PhysicalBone3D with a drive turns toward where the animated pose puts it (relative to
// its parent bone's body, or in the world) and, in the world, moves toward that place. Each is a
// spring and damper taken as acceleration and stepped implicitly, so stiff muscles stay stable at
// any tick rate and a body's mass doesn't change how it moves.

#include "space.hpp"

#include "slash/core/math.hpp"
#include "slash/world/physical_bones.hpp"
#include "slash/world/world.hpp"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <cmath>

namespace slash::physics3d::detail {

namespace {

// The rotation that turns from to to, as a vector along its axis as long as its angle in
// radians: the shorter way round.
JPH::Vec3 turn_between(JPH::QuatArg from, JPH::QuatArg to) {
    JPH::Quat delta = (to * from.Conjugated()).Normalized();
    if (delta.GetW() < 0.0F) {
        delta = -delta;
    }
    const JPH::Vec3 axis = delta.GetXYZ();
    const float sine = axis.Length();
    if (sine < 1.0e-6F) {
        return axis * 2.0F;
    }
    return axis * (2.0F * slash::atan2(sine, delta.GetW()) / sine);
}

// A velocity relative to the target's after a tick of a spring of stiffness and a damper of
// damping on it, stepped implicitly: (v + dt k x) / (1 + dt c + dt^2 k).
JPH::Vec3 sprung(JPH::Vec3Arg velocity, JPH::Vec3Arg offset, float stiffness, float damping,
                 float seconds) {
    const float denominator = 1.0F + (seconds * damping) + (seconds * seconds * stiffness);
    return (velocity + (offset * (seconds * stiffness))) / denominator;
}

// A change of velocity held to at most limit times the tick (0: no limit).
JPH::Vec3 limited(JPH::Vec3Arg change, float limit, float seconds) {
    if (limit <= 0.0F) {
        return change;
    }
    const float most = limit * seconds;
    const float size = change.Length();
    return size > most ? change * (most / size) : change;
}

} // namespace

void Space::apply_drives(const world::World& world, float seconds) {
    const world::PhysicalBoneTargets* targets = world::physical_bone_targets(world);
    if (targets == nullptr || targets->empty() || seconds <= 0.0F) {
        return;
    }
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    const entt::registry& registry = world.ecs();
    // In the order components were made, the same in every session.
    for (const entt::entity entity : registry.view<const world::PhysicalBone3D>()) {
        const auto& bone = registry.get<const world::PhysicalBone3D>(entity);
        const world::PhysicalBoneDrive& drive = bone.drive;
        if (drive.space == "off" || drive.space.empty()) {
            continue;
        }
        const Body* found = body(entity);
        if (found == nullptr || found->kind != Kind::Bone || found->id.IsInvalid() ||
            found->bone_mode != world::PhysicalBoneMode::Simulate) {
            continue;
        }
        const auto target = targets->find(entity);
        JPH::Body* jolt = locks.TryGetBody(found->id);
        if (target == targets->end() || jolt == nullptr || !jolt->IsDynamic()) {
            continue;
        }
        interface.ActivateBody(found->id);
        const JPH::Quat aim = rotation_of(target->second);

        // Local drives turn the body toward its pose in its parent bone's body, pushing the
        // parent back as a muscle does, so a chain of them can hold weight; a bone whose parent
        // body doesn't simulate turns in the world.
        JPH::Quat desired = aim;
        JPH::Body* parent_body = nullptr;
        if (drive.space == "local") {
            const auto link = bone_links_.find(entity);
            const entt::entity parent =
                link != bone_links_.end() ? link->second.parent : entt::entity{entt::null};
            const Body* up = parent != entt::null ? body(parent) : nullptr;
            const auto parent_target = targets->find(parent);
            if (up != nullptr && !up->id.IsInvalid() && parent_target != targets->end()) {
                JPH::Body* found_parent = locks.TryGetBody(up->id);
                if (found_parent != nullptr) {
                    desired = (found_parent->GetRotation() *
                               rotation_of(parent_target->second).Conjugated() * aim)
                                  .Normalized();
                    if (found_parent->IsDynamic()) {
                        parent_body = found_parent;
                    }
                }
            }
        }
        if (drive.orientation_strength > 0.0F || drive.angular_velocity_strength > 0.0F) {
            const JPH::Vec3 base =
                parent_body != nullptr ? parent_body->GetAngularVelocity() : JPH::Vec3::sZero();
            const JPH::Vec3 relative = jolt->GetAngularVelocity() - base;
            const JPH::Vec3 change = limited(
                sprung(relative, turn_between(jolt->GetRotation(), desired),
                       drive.orientation_strength, drive.angular_velocity_strength, seconds) -
                    relative,
                drive.max_angular_acceleration, seconds);
            if (parent_body == nullptr) {
                jolt->SetAngularVelocity(jolt->GetAngularVelocity() + change);
            } else {
                // The angular impulse that changes the two bodies' relative turning by change,
                // given to one and taken from the other.
                const JPH::Mat44 both =
                    jolt->GetInverseInertia() + parent_body->GetInverseInertia();
                if (std::abs(both.GetDeterminant3x3()) > 1.0e-12F) {
                    const JPH::Vec3 impulse = both.Inversed3x3().Multiply3x3(change);
                    jolt->AddAngularImpulse(impulse);
                    parent_body->AddAngularImpulse(-impulse);
                    interface.ActivateBody(parent_body->GetID());
                }
            }
        }
        // Pinning toward the pose's place in the world, so each body holds its own weight.
        if (drive.position_strength > 0.0F || drive.velocity_strength > 0.0F) {
            const JPH::Vec3 linear = jolt->GetLinearVelocity();
            const JPH::Vec3 offset =
                JPH::Vec3(to_jolt_place(target->second.origin) - jolt->GetPosition());
            const JPH::Vec3 change = limited(
                sprung(linear, offset, drive.position_strength, drive.velocity_strength, seconds) -
                    linear,
                drive.max_linear_acceleration, seconds);
            jolt->SetLinearVelocity(linear + change);
        }
    }
}

} // namespace slash::physics3d::detail
