#include "slash/physics3d/vehicle_tool.hpp"

#include "slash/core/log.hpp"
#include "slash/scene3d/ragdoll.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::physics3d {

namespace {

using world::Affine3D;
using world::VehicleRecipe;

constexpr float kAir = 1.225F;       // kilograms a cubic meter
constexpr float kGravity = 9.8F;     // meters a second squared
constexpr float kPersonMass = 80.0F; // what a character or creature weighs when it says nothing
constexpr float kHitRestitution = 0.5F;
constexpr float kHitCooldown = 0.5F; // seconds before the same vehicle hits the same one again
constexpr float kReach = 0.4F;       // meters around the body a hit reaches

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const real_t l = length(v);
    return l > 0.0F ? v * (1.0F / l) : v;
}

// What the system keeps of each vehicle: its recipe as tuned, and when it last hit each one.
struct Kept {
    std::shared_ptr<const VehicleRecipe> base;
    std::string overrides;
    std::shared_ptr<const VehicleRecipe> tuned;
    std::map<entt::entity, float> hits;
};

struct VehicleState {
    std::map<entt::entity, Kept> vehicles;
};
using VehicleStateHolder = std::shared_ptr<VehicleState>;

VehicleState& state_of(world::World& world) {
    auto& ctx = world.ecs().ctx();
    if (ctx.find<VehicleStateHolder>() == nullptr) {
        ctx.emplace<VehicleStateHolder>(std::make_shared<VehicleState>());
    }
    return *ctx.get<VehicleStateHolder>();
}

Result<std::shared_ptr<const VehicleRecipe>> tuned_of(const world::World& world,
                                                      const world::Vehicle3D& vehicle, Kept* kept) {
    auto base = world::load_vehicle(world, vehicle.recipe);
    if (!base) {
        return base.error();
    }
    const bool plain = vehicle.overrides.is_null() ||
                       (vehicle.overrides.is_object() && vehicle.overrides.as_object().empty());
    if (plain) {
        return base.value();
    }
    std::string overrides = json::write(vehicle.overrides, json::Style::Compact);
    if (kept != nullptr && kept->base == base.value() && kept->overrides == overrides &&
        kept->tuned != nullptr) {
        return kept->tuned;
    }
    auto resolved = world::load_vehicle_json(world, vehicle.recipe);
    if (!resolved) {
        return resolved.error();
    }
    auto tuned = world::tuned_vehicle(resolved.value(), vehicle.overrides);
    if (!tuned) {
        return tuned.error();
    }
    auto made = std::make_shared<const VehicleRecipe>(std::move(tuned.value()));
    if (kept != nullptr) {
        kept->base = base.value();
        kept->overrides = std::move(overrides);
        kept->tuned = made;
    }
    return made;
}

// An entity named "<base>" if it is free, else made without an ID.
entt::entity create_named(world::World& world, const std::string& id) {
    if (!id.empty() && world.find(id) == entt::null) {
        if (auto made = world.create(id); made) {
            return made.value();
        }
    }
    return world.create();
}

template <typename T>
void set_if_changed(T& field, const T& value, bool& changed) {
    if (field != value) {
        field = value;
        changed = true;
    }
}

// The rigid body as the recipe weighs it.
void keep_body(entt::registry& ecs, entt::entity entity, const VehicleRecipe& recipe) {
    if (!ecs.all_of<world::RigidBody3D>(entity)) {
        ecs.emplace<world::RigidBody3D>(entity);
    }
    world::RigidBody3D body = ecs.get<world::RigidBody3D>(entity);
    bool changed = false;
    set_if_changed(body.mass, recipe.physics.mass, changed);
    set_if_changed(body.center_of_mass_mode, std::string("custom"), changed);
    set_if_changed(body.center_of_mass, recipe.physics.center_of_mass, changed);
    set_if_changed(body.can_sleep, false, changed);
    // Its own drag slows it, not the project's damping on top.
    set_if_changed(body.linear_damp_mode, std::string("replace"), changed);
    set_if_changed(body.linear_damp, 0.0F, changed);
    float angular_damp = 0.0F;
    if (recipe.kind == "boat") {
        angular_damp = 1.5F;
    } else if (recipe.kind == "aircraft") {
        angular_damp = 1.0F;
    }
    set_if_changed(body.angular_damp, angular_damp, changed);
    if (changed) {
        ecs.replace<world::RigidBody3D>(entity, body);
    }
    if (!ecs.all_of<world::MeshInstance3D>(entity)) {
        world::MeshInstance3D instance;
        instance.mesh = ecs.get<world::Vehicle3D>(entity).recipe;
        ecs.emplace<world::MeshInstance3D>(entity, instance);
    }
}

world::VehicleWheel3D wheel_settings(world::VehicleWheel3D wheel, const VehicleRecipe& recipe,
                                     const world::VehicleWheel& made) {
    const world::VehiclePhysics& p = recipe.physics;
    const auto count = static_cast<float>(std::max<std::size_t>(recipe.wheels.size(), 1));
    wheel.use_as_traction = made.drive;
    wheel.use_as_steering = made.steer;
    wheel.wheel_radius = made.radius;
    wheel.wheel_rest_length = p.suspension_rest;
    wheel.suspension_travel = p.suspension_travel;
    wheel.suspension_stiffness = p.suspension_stiffness;
    wheel.damping_compression = p.suspension_damping;
    wheel.damping_relaxation = p.suspension_relaxation;
    wheel.wheel_friction_slip = p.tire_friction;
    wheel.wheel_roll_influence = p.roll_influence;
    wheel.suspension_max_force =
        p.suspension_max_force > 0.0F ? p.suspension_max_force : 4.0F * p.mass * kGravity / count;
    return wheel;
}

bool same_settings(const world::VehicleWheel3D& a, const world::VehicleWheel3D& b) {
    return a.use_as_traction == b.use_as_traction && a.use_as_steering == b.use_as_steering &&
           a.wheel_radius == b.wheel_radius && a.wheel_rest_length == b.wheel_rest_length &&
           a.suspension_travel == b.suspension_travel &&
           a.suspension_stiffness == b.suspension_stiffness &&
           a.damping_compression == b.damping_compression &&
           a.damping_relaxation == b.damping_relaxation &&
           a.wheel_friction_slip == b.wheel_friction_slip &&
           a.wheel_roll_influence == b.wheel_roll_influence &&
           a.suspension_max_force == b.suspension_max_force;
}

// The wheels' entities: one per recipe wheel under the vehicle, each with a tire to draw.
std::vector<entt::entity> keep_wheels(world::World& world, entt::entity entity,
                                      const VehicleRecipe& recipe) {
    entt::registry& ecs = world.ecs();
    std::vector<entt::entity> wheels;
    if (!world::vehicle_rolls(recipe)) {
        return wheels;
    }
    if (!ecs.all_of<world::VehicleBody3D>(entity)) {
        ecs.emplace<world::VehicleBody3D>(entity);
    }
    const std::string id(world.id_of(entity));
    for (std::size_t i = 0; i < recipe.wheels.size(); ++i) {
        const world::VehicleWheel& made = recipe.wheels[i];
        const std::string name = id.empty() ? std::string{} : id + "_wheel_" + std::to_string(i);
        entt::entity wheel = name.empty() ? entt::null : world.find(name);
        if (wheel == entt::null || !ecs.all_of<world::VehicleWheel3D>(wheel)) {
            if (wheel == entt::null) {
                wheel = create_named(world, name);
            }
            world::Transform3D place;
            place.position = made.position + Vec3{0.0F, recipe.physics.suspension_rest, 0.0F};
            ecs.emplace_or_replace<world::Transform3D>(wheel, place);
            ecs.emplace_or_replace<world::VehicleWheel3D>(wheel, wheel_settings({}, recipe, made));
            ecs.get<world::Transform3D>(entity).children.push_back(wheel);
            // The tire turned so the cylinder's axis is the axle.
            const entt::entity tire = create_named(world, name.empty() ? name : name + "_tire");
            world::Transform3D turned;
            turned.rotation = {0.0F, 0.0F, static_cast<real_t>(1.5707963F)};
            ecs.emplace<world::Transform3D>(tire, turned);
            world::CylinderMesh cylinder;
            cylinder.top_radius = made.radius;
            cylinder.bottom_radius = made.radius;
            cylinder.height = made.width;
            cylinder.radial_segments = 16;
            cylinder.rings = 1;
            ecs.emplace<world::CylinderMesh>(tire, cylinder);
            ecs.emplace<world::MeshInstance3D>(tire);
            world::StandardMaterial3D rubber;
            rubber.albedo_color = recipe.body.trim;
            rubber.roughness = 0.9F;
            ecs.emplace<world::StandardMaterial3D>(tire, rubber);
            ecs.get<world::Transform3D>(wheel).children.push_back(tire);
        } else {
            const auto& old = ecs.get<world::VehicleWheel3D>(wheel);
            if (const world::VehicleWheel3D fresh = wheel_settings(old, recipe, made);
                !same_settings(fresh, old)) {
                ecs.replace<world::VehicleWheel3D>(wheel, fresh);
            }
        }
        wheels.push_back(wheel);
    }
    return wheels;
}

// Who rides each vehicle, in storage order.
std::vector<std::pair<entt::entity, world::VehicleRider>> riders_of(const entt::registry& ecs,
                                                                    entt::entity vehicle) {
    std::vector<std::pair<entt::entity, world::VehicleRider>> out;
    const auto view = ecs.view<const world::VehicleRider>();
    std::vector<entt::entity> all(view.begin(), view.end());
    std::reverse(all.begin(), all.end());
    for (const entt::entity rider : all) {
        const auto& ride = ecs.get<const world::VehicleRider>(rider);
        if (ride.vehicle == vehicle) {
            out.emplace_back(rider, ride);
        }
    }
    return out;
}

const world::VehicleSeat* seat_named(const VehicleRecipe& recipe, std::string_view name) {
    for (const world::VehicleSeat& seat : recipe.seats) {
        if (seat.name == name) {
            return &seat;
        }
    }
    return recipe.seats.empty() ? nullptr : recipe.seats.data();
}

// Places an entity at a global transform, through its parent's.
void place_at(world::World& world, const world::Tree3D& tree, entt::entity entity,
              const Affine3D& global) {
    auto& ecs = world.ecs();
    auto* transform = ecs.try_get<world::Transform3D>(entity);
    if (transform == nullptr) {
        return;
    }
    Affine3D local = global;
    if (const entt::entity parent = tree.transform_parent(entity); parent != entt::null) {
        local = tree.global(parent).inverse() * global;
    }
    world::Transform3D placed = *transform;
    placed.position = local.origin;
    placed.rotation = world::euler(local);
    if (!(placed == *transform)) {
        ecs.replace<world::Transform3D>(entity, placed);
    }
}

// The forces of the water, the air and its own engines and surfaces on a vehicle, in the world.
struct Push {
    Vec3 force;
    Vec3 torque;
};

Push boat_forces(const VehicleRecipe& recipe, const Affine3D& global, Vec3 velocity, Vec3 angular,
                 const world::VehicleControls& controls) {
    Push out;
    const world::VehiclePhysics& p = recipe.physics;
    const world::VehicleBody& b = recipe.body;
    const Vec3 com = global.apply(p.center_of_mass);
    const real_t draft = std::max(p.draft, 0.05F);
    // Each of the hull's four corners floats a quarter of its weight at its draft.
    const real_t spring = p.mass * kGravity / (4.0F * draft);
    const real_t damper = 2.0F * std::sqrt(spring * p.mass * 0.25F) * 0.6F;
    real_t submerged = 0.0F;
    for (const real_t x : {b.width * 0.45F, -b.width * 0.45F}) {
        for (const real_t z : {b.length * 0.45F, -b.length * 0.45F}) {
            const Vec3 corner = global.apply({x, 0.0F, z});
            const real_t depth = std::min<real_t>(p.water_level - corner.y, b.height);
            if (depth <= 0.0F) {
                continue;
            }
            submerged += 0.25F;
            const Vec3 at = corner - com;
            const real_t rising = (velocity + cross(angular, at)).y;
            const Vec3 lift{0.0F, std::max<real_t>((spring * depth) - (damper * rising), 0.0F),
                            0.0F};
            out.force = out.force + lift;
            out.torque = out.torque + cross(at, lift);
        }
    }
    if (submerged <= 0.0F) {
        return out;
    }
    const Vec3 forward = normalized(global.basis({0.0F, 0.0F, 1.0F}));
    const Vec3 side = normalized(global.basis({1.0F, 0.0F, 0.0F}));
    // The water holds it back, much harder sideways than along its keel.
    const real_t along = dot(velocity, forward);
    const real_t across = dot(velocity, side);
    out.force = out.force - (forward * (p.water_drag * p.mass * 0.1F * along * submerged)) -
                (side * (p.water_drag * p.mass * 0.8F * across * submerged));
    // The propeller pushes along the keel and the rudder turns it, more as it goes faster.
    const Vec3 flat = normalized(Vec3{forward.x, 0.0F, forward.z});
    out.force = out.force +
                (flat * (p.engine.thrust * std::clamp(controls.throttle, -0.5F, 1.0F) * submerged));
    const real_t yaw = controls.steer * p.rudder * p.mass * b.length * 0.05F *
                       std::clamp<real_t>(std::abs(along) + 1.0F, 0.0F, 15.0F);
    out.torque = out.torque + (global.basis({0.0F, 1.0F, 0.0F}) * (along >= 0.0F ? yaw : -yaw));
    return out;
}

Push aircraft_forces(const VehicleRecipe& recipe, const Affine3D& global, Vec3 velocity,
                     const world::VehicleControls& controls) {
    Push out;
    const world::VehiclePhysics& p = recipe.physics;
    const Vec3 forward = normalized(global.basis({0.0F, 0.0F, 1.0F}));
    const Vec3 up = normalized(global.basis({0.0F, 1.0F, 0.0F}));
    const Vec3 left = normalized(global.basis({1.0F, 0.0F, 0.0F}));
    out.force = forward * (p.engine.thrust * std::clamp(controls.throttle, 0.0F, 1.0F));
    const real_t along = dot(velocity, forward);
    if (along <= 0.0F) {
        return out;
    }
    const real_t area = recipe.wing_span * recipe.wing_chord;
    const real_t q = 0.5F * kAir * along * along;
    // Lift grows with the angle of attack until the wing stalls.
    const real_t attack = std::atan2(-dot(velocity, up), along);
    const real_t coefficient = p.lift * std::clamp<real_t>(0.25F + (4.0F * attack), -0.6F, 1.4F);
    out.force = out.force + (up * (q * area * coefficient));
    // Elevator, ailerons and rudder: nose up turns about -x (its left), banking left about -z.
    const real_t authority = q * area * p.control * 0.02F;
    out.torque = left * (-controls.pitch * authority * recipe.wing_chord * 4.0F);
    out.torque = out.torque - (forward * (controls.steer * authority * recipe.wing_span * 0.5F));
    out.torque = out.torque + (up * (controls.steer * authority * recipe.wing_chord * 2.0F));
    return out;
}

// Tracks slip sideways as they slew it: a tracked vehicle turns toward a yaw rate of 0.6 times
// its steering angle (radians, eased and less at speed as a car's wheels turn) a second, so full
// over at its max_angle of 1 it pivots 0.6 radians a second, as hard as a sixteenth of a second's
// change of its turning.
Push slew_forces(const VehicleRecipe& recipe, const Affine3D& global, Vec3 angular, float steer) {
    Push out;
    const world::VehiclePhysics& p = recipe.physics;
    const world::VehicleBody& b = recipe.body;
    const Vec3 up = normalized(global.basis({0.0F, 1.0F, 0.0F}));
    const real_t inertia = p.mass * ((b.length * b.length) + (b.width * b.width)) / 12.0F;
    const real_t wanted = steer * 0.6F;
    out.torque = up * ((wanted - dot(angular, up)) * inertia * 16.0F);
    return out;
}

// A bike leans back upright.
Push balance_forces(const VehicleRecipe& recipe, const Affine3D& global, Vec3 angular) {
    Push out;
    const world::VehiclePhysics& p = recipe.physics;
    if (p.balance <= 0.0F) {
        return out;
    }
    const Vec3 forward = normalized(global.basis({0.0F, 0.0F, 1.0F}));
    const Vec3 up = normalized(global.basis({0.0F, 1.0F, 0.0F}));
    const real_t lean = dot(cross(up, Vec3{0.0F, 1.0F, 0.0F}), forward);
    const real_t height = std::max(recipe.body.height, 0.5F);
    const real_t spring = p.balance * p.mass * kGravity * height * 4.0F;
    const real_t damper = p.balance * p.mass * height * height * 2.0F;
    out.torque = forward * ((spring * lean) - (damper * dot(angular, forward)));
    return out;
}

// Runs into characters and creatures in its way.
void hit_people(world::World& world, const world::Tree3D& tree, entt::entity vehicle,
                const VehicleRecipe& recipe, const Affine3D& global, Vec3 velocity, Kept& kept,
                float seconds) {
    for (auto it = kept.hits.begin(); it != kept.hits.end();) {
        it->second -= seconds;
        it = it->second <= 0.0F ? kept.hits.erase(it) : std::next(it);
    }
    const world::VehicleDamage& damage = recipe.damage;
    if (length(velocity) < damage.min_speed) {
        return;
    }
    entt::registry& ecs = world.ecs();
    std::vector<entt::entity> targets;
    for (const entt::entity e : ecs.view<world::Transform3D>()) {
        if (e != vehicle &&
            (ecs.any_of<world::CharacterBody3D, world::Creature3D, world::Ragdoll3D>(e)) &&
            !ecs.all_of<world::VehicleRider>(e) && !kept.hits.contains(e)) {
            targets.push_back(e);
        }
    }
    std::reverse(targets.begin(), targets.end());
    const Affine3D inverse = global.inverse();
    const world::VehicleBody& b = recipe.body;
    const real_t vehicle_mass = recipe.physics.mass;
    for (const entt::entity target : targets) {
        const Affine3D place = tree.global(target);
        const Vec3 local = inverse.apply(place.origin);
        if (std::abs(local.x) > (b.width * 0.5F) + kReach ||
            std::abs(local.z) > (b.length * 0.5F) + kReach || local.y < -1.0F ||
            local.y > b.height + 0.5F) {
            continue;
        }
        Vec3 target_velocity;
        if (const auto* body = ecs.try_get<world::CharacterBody3D>(target); body != nullptr) {
            target_velocity = body->velocity;
        }
        Vec3 away = place.origin - global.origin;
        away.y = 0.0F;
        away = normalized(away);
        const real_t closing = dot(velocity - target_velocity, away);
        if (closing < damage.min_speed) {
            continue;
        }
        const real_t mass = kPersonMass;
        const real_t reduced = (vehicle_mass * mass) / (vehicle_mass + mass);
        // kilogram-meters a second, bouncing off as a collision of this restitution would, so
        // whoever is hit leaves faster than the vehicle and is clear of it
        const real_t impulse = (1.0F + kHitRestitution) * reduced * closing;
        const real_t hurt = impulse * damage.per_impulse;
        kept.hits[target] = kHitCooldown;
        const Vec3 shove = (away + Vec3{0.0F, 0.3F, 0.0F}) * (impulse / mass);
        if (auto* body = ecs.try_get<world::CharacterBody3D>(target); body != nullptr) {
            world::CharacterBody3D pushed = *body;
            pushed.velocity = pushed.velocity + shove;
            ecs.replace<world::CharacterBody3D>(target, pushed);
        }
        if (auto* creature = ecs.try_get<world::Creature3D>(target); creature != nullptr) {
            world::Creature3D scared = *creature;
            scared.mind.mode = "fleeing";
            scared.mind.target = vehicle;
            scared.mind.timer = 3.0F;
            scared.mind.velocity = scared.mind.velocity + shove;
            ecs.replace<world::Creature3D>(target, scared);
        }
        bool knocked = false;
        if (ecs.all_of<world::Ragdoll3D>(target) && impulse >= damage.knockdown) {
            // The bone nearest the vehicle's middle takes it.
            std::string bone;
            real_t nearest = std::numeric_limits<real_t>::max();
            for (const auto& [name, body] : scene3d::ragdoll_bones(world, target)) {
                const real_t d = length(tree.global(body).origin - global.origin);
                if (d < nearest) {
                    nearest = d;
                    bone = name;
                }
            }
            if (!bone.empty()) {
                auto report = scene3d::hit_ragdoll(world, target, bone, away * impulse);
                knocked = report && report.value().knocked_down;
            }
        }
        json::Value data;
        data["vehicle"] = std::string(world.id_of(vehicle));
        data["target"] = std::string(world.id_of(target));
        data["impulse"] = static_cast<double>(impulse);
        data["damage"] = static_cast<double>(hurt);
        data["speed"] = static_cast<double>(closing);
        data["knocked_down"] = knocked;
        world.emit("vehicle_hit", world.id_of(vehicle), std::move(data));
    }
}

void drive_vehicle(world::World& world, const world::Tree3D& tree, entt::entity entity, Kept& kept,
                   float seconds) {
    entt::registry& ecs = world.ecs();
    world::Vehicle3D vehicle = ecs.get<world::Vehicle3D>(entity);
    auto tuned = tuned_of(world, vehicle, &kept);
    if (!tuned) {
        return;
    }
    const VehicleRecipe& recipe = *tuned.value();
    keep_body(ecs, entity, recipe);
    const std::vector<entt::entity> wheels = keep_wheels(world, entity, recipe);
    const Affine3D global = tree.global(entity);
    const auto& body = ecs.get<world::RigidBody3D>(entity);
    const Vec3 velocity = body.linear_velocity;
    const Vec3 angular = body.angular_velocity;
    const Vec3 forward = normalized(global.basis({0.0F, 0.0F, 1.0F}));
    const real_t speed = dot(velocity, forward);
    const world::VehicleControls controls =
        vehicle.enabled ? vehicle.controls : world::VehicleControls{};

    // The drivetrain, from the driven wheels' spin.
    real_t wheel_rpm = 0.0F;
    real_t radius = 0.35F;
    int driven = 0;
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        if (recipe.wheels[i].drive) {
            const auto& wheel = ecs.get<world::VehicleWheel3D>(wheels[i]);
            wheel_rpm += std::abs(wheel.rpm);
            radius = recipe.wheels[i].radius;
            ++driven;
        }
    }
    if (driven > 0) {
        wheel_rpm /= static_cast<real_t>(driven);
    } else if (!recipe.wheels.empty()) {
        radius = recipe.wheels.front().radius;
        wheel_rpm = std::abs(speed) * 60.0F / (6.2831853F * radius);
    }
    world::DriveState state{.gear = vehicle.drive.gear,
                            .rpm = vehicle.drive.rpm,
                            .shift_timer = vehicle.drive.shift_timer,
                            .steer = vehicle.drive.steer};
    const world::DriveInput input{.throttle = controls.throttle,
                                  .brake = controls.brake,
                                  .handbrake = controls.handbrake,
                                  .steer = controls.steer,
                                  .pitch = controls.pitch};
    const bool engine = recipe.physics.engine.max_torque > 0.0F;
    world::DriveOutput out =
        world::drive_step(recipe.physics, state, input, static_cast<float>(wheel_rpm),
                          static_cast<float>(speed), static_cast<float>(radius), seconds);
    if (!engine) {
        out.force = 0.0F; // pulled by thrust, not its wheels
    }
    vehicle.drive = {.gear = state.gear,
                     .rpm = state.rpm,
                     .shift_timer = state.shift_timer,
                     .steer = state.steer,
                     .speed = static_cast<float>(speed)};

    // Each wheel's share: the differential splits the force (a tracked vehicle's wheels don't
    // steer; its tracks slew it, below), and brakes are impulses a step, as Godot's
    // VehicleWheel3D takes them.
    const bool tracked = recipe.kind == "tracked";
    int front_driven = 0;
    for (const world::VehicleWheel& w : recipe.wheels) {
        front_driven += w.drive && w.position.z >= 0.0F ? 1 : 0;
    }
    const int rear_driven = driven - front_driven;
    const float idle_brake = std::abs(controls.throttle) < 0.05F && out.brake <= 0.0F
                                 ? 0.015F * recipe.physics.mass * kGravity /
                                       static_cast<float>(std::max<std::size_t>(wheels.size(), 1))
                                 : 0.0F;
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        const world::VehicleWheel& made = recipe.wheels[i];
        world::VehicleWheel3D wheel = ecs.get<world::VehicleWheel3D>(wheels[i]);
        float force = 0.0F;
        if (made.drive && driven > 0) {
            if (recipe.physics.differential == "all" && front_driven > 0 && rear_driven > 0) {
                force = made.position.z >= 0.0F ? out.force * recipe.physics.front_share /
                                                      static_cast<float>(front_driven)
                                                : out.force * (1.0F - recipe.physics.front_share) /
                                                      static_cast<float>(rear_driven);
            } else {
                force = out.force / static_cast<float>(driven);
            }
        }
        float brake = (out.brake + idle_brake) * seconds;
        if (made.handbrake) {
            brake += out.handbrake * seconds;
        }
        const float steering = made.steer && !tracked ? out.steer : 0.0F;
        if (!vehicle.enabled) {
            force = 0.0F;
        }
        if (wheel.engine_force != force || wheel.brake != brake || wheel.steering != steering) {
            wheel.engine_force = force;
            wheel.brake = brake;
            wheel.steering = steering;
            ecs.replace<world::VehicleWheel3D>(wheels[i], wheel);
        }
    }

    // The air, the water, wings and balance.
    Push push;
    const world::VehicleBody& b = recipe.body;
    const real_t front_area = b.width * std::max<real_t>(b.height - b.clearance, 0.1F);
    push.force = velocity * (-0.5F * kAir * recipe.physics.drag * front_area * length(velocity));
    if (recipe.kind == "boat") {
        const Push water = boat_forces(recipe, global, velocity, angular, controls);
        push.force = push.force + water.force;
        push.torque = push.torque + water.torque;
    } else if (recipe.kind == "aircraft") {
        const Push air = aircraft_forces(recipe, global, velocity, controls);
        push.force = push.force + air.force;
        push.torque = push.torque + air.torque;
    } else if (recipe.kind == "bike") {
        push.torque = push.torque + balance_forces(recipe, global, angular).torque;
    } else if (tracked && vehicle.enabled) {
        push.torque =
            push.torque + slew_forces(recipe, global, angular, vehicle.drive.steer).torque;
    }
    world::RigidBody3D pushed = ecs.get<world::RigidBody3D>(entity);
    if (!(pushed.constant_force == push.force) || !(pushed.constant_torque == push.torque)) {
        pushed.constant_force = push.force;
        pushed.constant_torque = push.torque;
        ecs.replace<world::RigidBody3D>(entity, pushed);
    }
    if (!(vehicle == ecs.get<world::Vehicle3D>(entity))) {
        ecs.replace<world::Vehicle3D>(entity, vehicle);
    }

    // Its riders go where it will be after the step.
    Affine3D ahead = global;
    ahead.origin = ahead.origin + (velocity * static_cast<real_t>(seconds));
    for (const auto& [rider, ride] : riders_of(ecs, entity)) {
        const world::VehicleSeat* seat = seat_named(recipe, ride.seat);
        if (seat == nullptr) {
            continue;
        }
        Affine3D at = ahead;
        at.origin = ahead.apply(seat->position);
        place_at(world, tree, rider, at);
        if (auto* character = ecs.try_get<world::CharacterBody3D>(rider); character != nullptr) {
            if (!(character->velocity == Vec3{}) || character->collision_layer != 0 ||
                character->collision_mask != 0) {
                world::CharacterBody3D still = *character;
                still.velocity = {};
                still.collision_layer = 0;
                still.collision_mask = 0;
                ecs.replace<world::CharacterBody3D>(rider, still);
            }
        }
    }
    hit_people(world, tree, entity, recipe, global, velocity, kept, seconds);
}

} // namespace

void add_vehicle_system(world::World& world) {
    world.add_system(
        "vehicles_3d",
        [](world::World& w, double tick_seconds) {
            entt::registry& ecs = w.ecs();
            VehicleState& state = state_of(w);
            const auto view = ecs.view<world::Vehicle3D, world::Transform3D>();
            std::vector<entt::entity> vehicles(view.begin(), view.end());
            std::reverse(vehicles.begin(), vehicles.end());
            std::erase_if(state.vehicles, [&](const auto& kept) {
                return !ecs.valid(kept.first) || !ecs.all_of<world::Vehicle3D>(kept.first);
            });
            if (vehicles.empty()) {
                return;
            }
            world::prepare_vehicles(w);
            const world::Tree3D tree(w);
            for (const entt::entity entity : vehicles) {
                drive_vehicle(w, tree, entity, state.vehicles[entity],
                              static_cast<float>(tick_seconds));
            }
        },
        {.doc = "Drives each Vehicle3D as its .vehicle.json recipe says: keeps its body and "
                "wheels, runs its drivetrain from its controls, floats boats, flies aircraft, "
                "carries its riders and hits what it runs into.",
         .priority = 945});
}

Result<std::shared_ptr<const VehicleRecipe>> vehicle_recipe(const world::World& world,
                                                            entt::entity vehicle) {
    const auto* found = world.ecs().try_get<world::Vehicle3D>(vehicle);
    if (found == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "it has no Vehicle3D");
    }
    return tuned_of(world, *found, nullptr);
}

Result<std::string> enter_vehicle(world::World& world, entt::entity rider, entt::entity vehicle,
                                  std::string_view seat) {
    entt::registry& ecs = world.ecs();
    if (!ecs.valid(rider) || !ecs.all_of<world::Transform3D>(rider)) {
        return make_error(ErrorCode::InvalidArgument, "the rider has no Transform3D");
    }
    if (ecs.all_of<world::VehicleRider>(rider)) {
        return make_error(ErrorCode::InvalidArgument, "the rider is already in a vehicle");
    }
    auto recipe = vehicle_recipe(world, vehicle);
    if (!recipe) {
        return recipe.error();
    }
    const auto riders = riders_of(ecs, vehicle);
    const auto taken = [&](const std::string& name) {
        return std::ranges::any_of(riders, [&](const auto& r) { return r.second.seat == name; });
    };
    std::string chosen;
    if (!seat.empty()) {
        const auto& seats = recipe.value()->seats;
        if (std::ranges::none_of(seats,
                                 [&](const world::VehicleSeat& s) { return s.name == seat; })) {
            return make_error(ErrorCode::NotFound,
                              "the vehicle has no seat \"" + std::string(seat) + "\"");
        }
        if (taken(std::string(seat))) {
            return make_error(ErrorCode::InvalidArgument,
                              "seat \"" + std::string(seat) + "\" is taken");
        }
        chosen = seat;
    } else {
        // The driver's seat first, then the rest in order.
        for (const bool driver : {true, false}) {
            for (const world::VehicleSeat& s : recipe.value()->seats) {
                if (chosen.empty() && s.driver == driver && !taken(s.name)) {
                    chosen = s.name;
                }
            }
        }
        if (chosen.empty()) {
            return make_error(ErrorCode::InvalidArgument, "every seat is taken");
        }
    }
    world::VehicleRider ride{.vehicle = vehicle, .seat = chosen};
    if (auto* body = ecs.try_get<world::CharacterBody3D>(rider); body != nullptr) {
        ride.collision_layer = body->collision_layer;
        ride.collision_mask = body->collision_mask;
        world::CharacterBody3D quiet = *body;
        quiet.collision_layer = 0;
        quiet.collision_mask = 0;
        quiet.velocity = {};
        ecs.replace<world::CharacterBody3D>(rider, quiet);
    }
    ecs.emplace<world::VehicleRider>(rider, ride);
    const world::Tree3D tree(world);
    const Affine3D global = tree.global(vehicle);
    if (const world::VehicleSeat* s = seat_named(*recipe.value(), chosen); s != nullptr) {
        Affine3D at = global;
        at.origin = global.apply(s->position);
        place_at(world, tree, rider, at);
    }
    json::Value data;
    data["rider"] = std::string(world.id_of(rider));
    data["seat"] = chosen;
    world.emit("vehicle_entered", world.id_of(vehicle), std::move(data));
    return chosen;
}

Result<void> exit_vehicle(world::World& world, entt::entity rider) {
    entt::registry& ecs = world.ecs();
    const auto* ride = ecs.valid(rider) ? ecs.try_get<world::VehicleRider>(rider) : nullptr;
    if (ride == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "it isn't in a vehicle");
    }
    const world::VehicleRider was = *ride;
    ecs.remove<world::VehicleRider>(rider);
    if (auto* body = ecs.try_get<world::CharacterBody3D>(rider); body != nullptr) {
        world::CharacterBody3D back = *body;
        back.collision_layer = was.collision_layer;
        back.collision_mask = was.collision_mask;
        ecs.replace<world::CharacterBody3D>(rider, back);
    }
    if (ecs.valid(was.vehicle) && ecs.all_of<world::Vehicle3D, world::Transform3D>(was.vehicle)) {
        if (auto recipe = vehicle_recipe(world, was.vehicle); recipe) {
            const world::Tree3D tree(world);
            const Affine3D global = tree.global(was.vehicle);
            const world::VehicleSeat* seat = seat_named(*recipe.value(), was.seat);
            if (seat != nullptr) {
                const real_t side = seat->position.x < 0.0F ? -1.0F : 1.0F;
                const Vec3 door{side * ((recipe.value()->body.width * 0.5F) + 0.6F), 0.0F,
                                seat->position.z};
                // Upright, turned as the vehicle is about y.
                const Vec3 forward = global.basis({0.0F, 0.0F, 1.0F});
                world::Transform3D upright;
                upright.rotation = {0.0F, std::atan2(forward.x, forward.z), 0.0F};
                Affine3D at = world::affine(upright);
                at.origin = global.apply(door) + Vec3{0.0F, 0.05F, 0.0F};
                place_at(world, tree, rider, at);
            }
        }
        json::Value data;
        data["rider"] = std::string(world.id_of(rider));
        data["seat"] = was.seat;
        world.emit("vehicle_exited", world.id_of(was.vehicle), std::move(data));
    }
    return {};
}

} // namespace slash::physics3d
