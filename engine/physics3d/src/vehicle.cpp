#include "space.hpp"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

// Godot's VehicleBody3D and VehicleWheel3D (scene/3d/physics/vehicle_body_3d.cpp), its port of
// Bullet's raycast vehicle: after each step every wheel of an awake vehicle casts a ray down its
// suspension, and its spring, damper and tire friction push the vehicle's body.
namespace slash::physics3d::detail {

namespace {

using world::Affine3D;
using world::Transform3D;
using world::VehicleBody3D;
using world::VehicleWheel3D;

constexpr float kTau = 2.0F * std::numbers::pi_v<float>;

// A rotation of angle radians about a unit axis, as Godot's Basis(axis, angle).
Affine3D rotation(Vec3 axis, float angle) {
    const JPH::Mat44 matrix = JPH::Mat44::sRotation(to_jolt(axis), angle);
    return {.x = from_jolt(matrix.GetAxisX()),
            .y = from_jolt(matrix.GetAxisY()),
            .z = from_jolt(matrix.GetAxisZ()),
            .origin = {}};
}

// A transform with each column's and its origin's components scaled: diag(scale) * transform.
Affine3D scale_rows(const Affine3D& transform, Vec3 scale) {
    const auto row_scaled = [&](Vec3 v) {
        return Vec3{v.x * scale.x, v.y * scale.y, v.z * scale.z};
    };
    return {.x = row_scaled(transform.x),
            .y = row_scaled(transform.y),
            .z = row_scaled(transform.z),
            .origin = row_scaled(transform.origin)};
}

Vec3 inverse_of(Vec3 scale) {
    const auto inverse = [](float s) { return s != 0.0F ? 1.0F / s : 0.0F; };
    return {inverse(scale.x), inverse(scale.y), inverse(scale.z)};
}

bool same_place(const Transform3D& a, const Transform3D& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale &&
           a.top_level == b.top_level;
}

// The wheels of a vehicle: the entities with a VehicleWheel3D placed right under it, in its
// children's order, as Godot keeps the wheels in the order they entered the tree.
std::vector<entt::entity> wheels_of(const entt::registry& registry, const world::Tree3D& tree,
                                    entt::entity vehicle) {
    std::vector<entt::entity> wheels;
    const auto* transform = registry.try_get<Transform3D>(vehicle);
    if (transform == nullptr) {
        return wheels;
    }
    for (const entt::entity child : transform->children) {
        if (registry.valid(child) && registry.all_of<VehicleWheel3D, Transform3D>(child) &&
            tree.transform_parent(child) == vehicle &&
            std::ranges::find(wheels, child) == wheels.end()) {
            wheels.push_back(child);
        }
    }
    return wheels;
}

// What a wheel finds in a step, as the members of Godot's VehicleWheel3D that the step works
// out: its m_raycastInfo, suspension and friction.
struct WheelStep {
    entt::entity entity = entt::null;
    VehicleWheel3D* wheel = nullptr;
    Wheel* mount = nullptr;
    Vec3 hard_point{}; // where it is mounted, in the world
    Vec3 direction{};  // down its suspension
    Vec3 axle{};
    Vec3 up{}; // up the wheel, turned by its steering
    bool in_contact = false;
    Vec3 contact_point{};
    Vec3 contact_normal{};
    entt::entity ground = entt::null;
    float suspension_length = 0.0F;
    float relative_velocity = 0.0F;
    float clipped_inverse = 0.0F; // Godot's m_clippedInvContactDotSuspension
    float suspension_force = 0.0F;
    Vec3 friction_axle{};
    Vec3 forward{};
    float side_impulse = 0.0F;
    float forward_impulse = 0.0F;
    float skid = 0.0F;
};

// The body under a wheel, as Godot's friction code reads it: where it is, how it moves and its
// inverse mass, 0 for anything but a rigid body.
struct Ground {
    Vec3 origin;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    float inverse_mass = 0.0F;
};

} // namespace

void Space::pre_step_vehicles(world::World& world, const world::Tree3D& tree, float seconds) {
    entt::registry& registry = world.ecs();
    wheels_.erase_if([&](entt::entity entity, const Wheel& /*wheel*/) {
        return !registry.valid(entity) || !registry.all_of<VehicleWheel3D, Transform3D>(entity);
    });
    vehicles_.erase_if([&](entt::entity entity, const VehicleInputs& /*inputs*/) {
        return !registry.valid(entity) || !registry.all_of<VehicleBody3D>(entity);
    });
    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Rigid || !registry.all_of<VehicleBody3D>(entity)) {
            continue;
        }
        const std::vector<entt::entity> wheels = wheels_of(registry, tree, entity);
        const Vec3 scale = unscale(tree.global(entity)).scale;
        for (const entt::entity wheel_entity : wheels) {
            const auto& transform = registry.get<Transform3D>(wheel_entity);
            Wheel* known = wheels_.find(wheel_entity);
            if (known != nullptr && same_place(known->written, transform)) {
                continue;
            }
            // A new or moved wheel: where it is mounted is where its Transform3D puts it, less its
            // steering, roll and suspension, as Godot takes the mount from where a wheel enters
            // the tree.
            const auto& wheel = registry.get<VehicleWheel3D>(wheel_entity);
            const Unscaled pose = unscale(scale_rows(world::affine(transform), scale));
            Affine3D mount = pose.transform * rotation({1.0F, 0.0F, 0.0F}, -wheel.rotation) *
                             rotation({0.0F, 1.0F, 0.0F}, -wheel.steering);
            mount.origin = pose.transform.origin + (mount.y * wheel.suspension_length);
            Wheel fresh{.mount = mount, .mount_scale = pose.scale, .written = transform};
            if (known != nullptr) {
                fresh.delta_rotation = known->delta_rotation;
                *known = fresh;
            } else {
                // Its last roll, from its rpm, as the step damped it.
                fresh.delta_rotation = wheel.rpm * kTau / 60.0F * seconds * 0.99F;
                wheels_.emplace(wheel_entity, fresh);
            }
        }
        // As Godot's set_engine_force, set_brake and set_steering, when they change. A change
        // also wakes the vehicle, which Godot leaves asleep.
        const auto& inputs = registry.get<VehicleBody3D>(entity);
        VehicleInputs* last = vehicles_.find(entity);
        if (last == nullptr) {
            last = &vehicles_.emplace(entity, VehicleInputs{});
        }
        bool changed = false;
        for (const entt::entity wheel_entity : wheels) {
            auto& wheel = registry.get<VehicleWheel3D>(wheel_entity);
            if (inputs.engine_force != last->engine_force && wheel.use_as_traction) {
                wheel.engine_force = inputs.engine_force;
            }
            if (inputs.brake != last->brake) {
                wheel.brake = inputs.brake;
            }
            if (inputs.steering != last->steering && wheel.use_as_steering) {
                wheel.steering = inputs.steering;
            }
        }
        if (inputs.engine_force != last->engine_force || inputs.brake != last->brake ||
            inputs.steering != last->steering) {
            changed = true;
            *last = {.engine_force = inputs.engine_force,
                     .brake = inputs.brake,
                     .steering = inputs.steering};
        }
        if (changed && !body->id.IsInvalid()) {
            const JPH::Body* jolt = system_->GetBodyLockInterfaceNoLock().TryGetBody(body->id);
            if (jolt != nullptr && jolt->IsDynamic() && jolt->IsInBroadPhase()) {
                system_->GetBodyInterfaceNoLock().ActivateBody(body->id);
            }
        }
    }
}

void Space::step_vehicles(world::World& world, const world::Tree3D& tree, float seconds) {
    if (vehicles_.size() == 0 || seconds <= 0.0F) {
        return;
    }
    entt::registry& registry = world.ecs();
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    const float step = seconds;

    const auto ground_of = [&](entt::entity entity) -> std::optional<Ground> {
        const Body* found = entity != entt::null ? body(entity) : nullptr;
        const JPH::Body* jolt =
            found != nullptr && !found->id.IsInvalid() ? locks.TryGetBody(found->id) : nullptr;
        if (jolt == nullptr) {
            return std::nullopt;
        }
        return Ground{.origin = from_jolt(JPH::Vec3(jolt->GetPosition())),
                      .linear_velocity =
                          from_jolt(jolt->GetLinearVelocity()) + found->constant_linear_velocity,
                      .angular_velocity =
                          from_jolt(jolt->GetAngularVelocity()) + found->constant_angular_velocity,
                      .inverse_mass = found->kind == Kind::Rigid && jolt->IsDynamic()
                                          ? jolt->GetMotionProperties()->GetInverseMass()
                                          : 0.0F};
    };

    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Rigid || body->id.IsInvalid() ||
            !registry.all_of<VehicleBody3D>(entity)) {
            continue;
        }
        JPH::Body* jolt = locks.TryGetBody(body->id);
        if (jolt == nullptr || !jolt->IsDynamic() || !jolt->IsActive()) {
            continue;
        }
        std::vector<WheelStep> wheels;
        for (const entt::entity wheel_entity : wheels_of(registry, tree, entity)) {
            if (Wheel* mount = wheels_.find(wheel_entity); mount != nullptr) {
                wheels.push_back({.entity = wheel_entity,
                                  .wheel = &registry.get<VehicleWheel3D>(wheel_entity),
                                  .mount = mount});
            }
        }
        if (wheels.empty()) {
            continue;
        }
        const Affine3D chassis = affine_of(jolt->GetPosition(), jolt->GetRotation());
        const Vec3 origin = chassis.origin;
        const float inverse_mass = jolt->GetMotionProperties()->GetInverseMass();
        const float mass = 1.0F / inverse_mass;
        const JPH::Mat44 inverse_inertia = jolt->GetInverseInertia();
        // The body's velocities as they stand: each impulse below changes them, as Godot's
        // direct body state reads them from Jolt.
        const auto velocity_at = [&, jolt](Vec3 relative) {
            return from_jolt(jolt->GetLinearVelocity()) +
                   cross(from_jolt(jolt->GetAngularVelocity()), relative);
        };
        const auto apply_impulse = [&, jolt](Vec3 impulse, Vec3 relative) {
            if (impulse != Vec3{}) {
                interface.AddImpulse(jolt->GetID(), to_jolt(impulse),
                                     jolt->GetPosition() + to_jolt(relative));
            }
        };
        const auto ground_velocity = [](const std::optional<Ground>& ground, Vec3 point) {
            return ground ? ground->linear_velocity +
                                cross(ground->angular_velocity, point - ground->origin)
                          : Vec3{};
        };

        // _update_wheel_transform and _ray_cast.
        for (WheelStep& step_wheel : wheels) {
            const VehicleWheel3D& wheel = *step_wheel.wheel;
            const Affine3D& mount = step_wheel.mount->mount;
            step_wheel.hard_point = chassis.apply(mount.origin);
            step_wheel.direction = normalized(chassis.basis(mount.y * -1.0F));
            step_wheel.axle = normalized(chassis.basis(mount.x));
            step_wheel.up = step_wheel.direction * -1.0F;

            const float ray_length = wheel.wheel_rest_length + wheel.wheel_radius;
            const Vec3 source = step_wheel.hard_point - (step_wheel.direction * wheel.wheel_radius);
            const Vec3 target = step_wheel.hard_point + (step_wheel.direction * ray_length);
            step_wheel.contact_point = target;
            QueryFilter filter;
            filter.collision_mask = body->mask;
            filter.exclude = {entity};
            const std::optional<RayHit> hit = cast_ray(source, target, filter, true);
            if (hit) {
                const float param = length(hit->point - source) / length(target - source);
                step_wheel.contact_normal = hit->normal;
                step_wheel.in_contact = true;
                step_wheel.ground = hit->entity;
                // Clamped to its travel, as Godot clamps it: the upper limit last.
                float suspension = (param * ray_length) - wheel.wheel_radius;
                suspension =
                    std::max(suspension, wheel.wheel_rest_length - wheel.suspension_travel);
                suspension =
                    std::min(suspension, wheel.wheel_rest_length + wheel.suspension_travel);
                step_wheel.suspension_length = suspension;
                step_wheel.contact_point = hit->point;
                const float denominator = dot(step_wheel.contact_normal, step_wheel.direction);
                const float projected =
                    dot(step_wheel.contact_normal, velocity_at(step_wheel.contact_point - origin));
                if (denominator >= -0.1F) {
                    step_wheel.relative_velocity = 0.0F;
                    step_wheel.clipped_inverse = 1.0F / 0.1F;
                } else {
                    const float inverse = -1.0F / denominator;
                    step_wheel.relative_velocity = projected * inverse;
                    step_wheel.clipped_inverse = inverse;
                }
            } else {
                step_wheel.suspension_length = wheel.wheel_rest_length;
                step_wheel.relative_velocity = 0.0F;
                step_wheel.contact_normal = step_wheel.direction * -1.0F;
                step_wheel.clipped_inverse = 1.0F;
            }
        }

        // _update_suspension, and the suspension's impulses.
        for (WheelStep& step_wheel : wheels) {
            const VehicleWheel3D& wheel = *step_wheel.wheel;
            if (!step_wheel.in_contact) {
                step_wheel.suspension_force = 0.0F;
                continue;
            }
            float force = wheel.suspension_stiffness *
                          (wheel.wheel_rest_length - step_wheel.suspension_length) *
                          step_wheel.clipped_inverse;
            const float damping = step_wheel.relative_velocity < 0.0F ? wheel.damping_compression
                                                                      : wheel.damping_relaxation;
            force -= damping * step_wheel.relative_velocity;
            step_wheel.suspension_force = std::max(force * mass, 0.0F);
        }
        for (const WheelStep& step_wheel : wheels) {
            const float force =
                std::min(step_wheel.suspension_force, step_wheel.wheel->suspension_max_force);
            apply_impulse(step_wheel.contact_normal * (force * step),
                          step_wheel.contact_point - origin);
        }

        // _update_friction: the sideways impulse that keeps each wheel from sliding along its
        // axle, then rolling friction, engine force or brake along it, both limited by grip.
        for (WheelStep& step_wheel : wheels) {
            if (!step_wheel.in_contact) {
                continue;
            }
            const VehicleWheel3D& wheel = *step_wheel.wheel;
            const Vec3 normal = step_wheel.contact_normal;
            Vec3 axle = rotation(step_wheel.up, wheel.steering).basis(step_wheel.axle);
            axle = normalized(axle - (normal * dot(axle, normal)));
            step_wheel.friction_axle = axle;
            step_wheel.forward = normalized(cross(normal, axle));
            // _resolve_single_bilateral, with only the linear mass.
            if (dot(axle, axle) > 1.1F) {
                step_wheel.side_impulse = 0.0F;
                continue;
            }
            const std::optional<Ground> ground = ground_of(step_wheel.ground);
            const Vec3 point = step_wheel.contact_point;
            const float relative =
                dot(axle, velocity_at(point - origin) - ground_velocity(ground, point));
            float damping = 0.2F;
            if (wheel.wheel_roll_influence > 0.0F) {
                damping = std::min(damping, step / wheel.wheel_roll_influence);
            }
            const float mass_term = 1.0F / (inverse_mass + (ground ? ground->inverse_mass : 0.0F));
            step_wheel.side_impulse = -damping * relative * mass_term;
        }
        bool sliding = false;
        for (WheelStep& step_wheel : wheels) {
            const VehicleWheel3D& wheel = *step_wheel.wheel;
            float rolling = 0.0F;
            if (step_wheel.in_contact) {
                if (wheel.engine_force != 0.0F) {
                    rolling = -wheel.engine_force * step;
                } else {
                    // btVehicleWheelContactPoint and _calc_rolling_friction.
                    const float max_impulse = wheel.brake != 0.0F ? wheel.brake : 0.0F;
                    const Vec3 point = step_wheel.contact_point;
                    const Vec3 forward = step_wheel.forward;
                    const Vec3 r0 = point - origin;
                    const Vec3 c0 = cross(r0, forward);
                    const Vec3 turned =
                        cross(from_jolt(inverse_inertia.Multiply3x3Transposed(to_jolt(c0))), r0);
                    const float jacobian = 1.0F / (inverse_mass + dot(forward, turned));
                    const std::optional<Ground> ground = ground_of(step_wheel.ground);
                    const float relative =
                        dot(forward, velocity_at(r0) - ground_velocity(ground, point));
                    rolling = std::clamp(-relative * jacobian, -max_impulse, max_impulse);
                }
            }
            step_wheel.forward_impulse = 0.0F;
            step_wheel.skid = 1.0F;
            if (step_wheel.in_contact) {
                const float max = step_wheel.suspension_force * step * wheel.wheel_friction_slip;
                step_wheel.forward_impulse = rolling;
                const float x = step_wheel.forward_impulse * 0.5F;
                const float y = step_wheel.side_impulse;
                const float squared = (x * x) + (y * y);
                if (squared > max * max) {
                    sliding = true;
                    step_wheel.skid *= max / std::sqrt(squared);
                }
            }
        }
        if (sliding) {
            for (WheelStep& step_wheel : wheels) {
                if (step_wheel.side_impulse != 0.0F && step_wheel.skid < 1.0F) {
                    step_wheel.forward_impulse *= step_wheel.skid;
                    step_wheel.side_impulse *= step_wheel.skid;
                }
            }
        }
        for (const WheelStep& step_wheel : wheels) {
            Vec3 relative = step_wheel.contact_point - origin;
            if (step_wheel.forward_impulse != 0.0F) {
                apply_impulse(step_wheel.forward * step_wheel.forward_impulse, relative);
            }
            if (step_wheel.side_impulse != 0.0F) {
                // Godot's ROLLING_INFLUENCE_FIX: the side impulse acts lower, by the roll
                // influence, so the vehicle rolls less.
                const Vec3 up = chassis.y;
                relative =
                    relative -
                    (up * (dot(up, relative) * (1.0F - step_wheel.wheel->wheel_roll_influence)));
                apply_impulse(step_wheel.friction_axle * step_wheel.side_impulse, relative);
            }
        }

        // The wheels roll with the ground under them, and keep turning, slower, in the air.
        const Vec3 scale = inverse_of(unscale(tree.global(entity)).scale);
        for (const WheelStep& step_wheel : wheels) {
            VehicleWheel3D& wheel = *step_wheel.wheel;
            Wheel& mount = *step_wheel.mount;
            if (step_wheel.in_contact) {
                Vec3 forward = chassis.z;
                forward =
                    forward - (step_wheel.contact_normal * dot(forward, step_wheel.contact_normal));
                const float projected = dot(forward, velocity_at(step_wheel.hard_point - origin));
                mount.delta_rotation = projected * step / wheel.wheel_radius;
            }
            wheel.rotation += mount.delta_rotation;
            wheel.rpm = mount.delta_rotation / step * 60.0F / kTau;
            mount.delta_rotation *= 0.99F;

            wheel.in_contact = step_wheel.in_contact;
            wheel.contact_point = step_wheel.contact_point;
            wheel.contact_normal = step_wheel.contact_normal;
            wheel.contact_body = step_wheel.ground;
            wheel.suspension_length = step_wheel.suspension_length;
            wheel.skid_info = step_wheel.skid;

            // Where it is now, in the vehicle's space: Godot's _update_wheel, from this step's
            // suspension and roll, and turned properly where Godot's basis is mirrored.
            Affine3D pose = mount.mount * rotation({0.0F, 1.0F, 0.0F}, wheel.steering) *
                            rotation({1.0F, 0.0F, 0.0F}, wheel.rotation);
            pose.x = pose.x * mount.mount_scale.x;
            pose.y = pose.y * mount.mount_scale.y;
            pose.z = pose.z * mount.mount_scale.z;
            pose.origin = mount.mount.origin - (mount.mount.y * step_wheel.suspension_length);
            const Unscaled local = unscale(scale_rows(pose, scale));
            auto& transform = registry.get<Transform3D>(step_wheel.entity);
            transform.position = local.transform.origin;
            transform.rotation = world::euler(local.transform);
            transform.scale = local.scale;
            mount.written = transform;
        }
    }
}

} // namespace slash::physics3d::detail
