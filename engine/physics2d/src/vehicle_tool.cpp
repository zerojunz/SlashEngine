#include "slash/physics2d/vehicle_tool.hpp"

#include "space.hpp"

#include "slash/physics2d/physics2d.hpp"
#include "slash/physics2d/ragdoll.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::physics2d {

namespace {

using world::Affine2D;
using world::VehicleRecipe;

constexpr float kAir = 1.225F;
constexpr float kPersonMass = 80.0F;
constexpr float kHitRestitution = 0.5F;
constexpr float kHitCooldown = 0.5F;
constexpr float kReach = 0.4F;       // meters around the body a hit reaches
constexpr float kWheelShare = 0.03F; // of the vehicle's mass, each wheel
constexpr float kTau = 6.2831853F;

real_t dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

real_t length(Vec2 v) {
    return std::sqrt(dot(v, v));
}

Vec2 normalized(Vec2 v) {
    const real_t l = length(v);
    return l > 0.0F ? Vec2{v.x / l, v.y / l} : v;
}

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
                                                      const world::Vehicle2D& vehicle, Kept* kept) {
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

entt::entity create_named(world::World& world, const std::string& id) {
    if (!id.empty() && world.find(id) == entt::null) {
        if (auto made = world.create(id); made) {
            return made.value();
        }
    }
    return world.create();
}

// How the recipe's side view (meters, y up, nose toward +x) maps into the vehicle's pixels.
struct Scale {
    float ppm = 50.0F;
    float facing = 1.0F; // -1 when flipped

    [[nodiscard]] Vec2 at(Vec2 side) const { return {side.x * ppm * facing, -side.y * ppm}; }
};

Scale scale_of(const world::Vehicle2D& vehicle) {
    return {.ppm = std::max(vehicle.pixels_per_meter, 1.0F),
            .facing = vehicle.flip_h ? -1.0F : 1.0F};
}

float gravity_px(world::World& world, entt::entity entity) {
    if (const auto found = gravity(world, entity); found) {
        return std::max(static_cast<float>(length(found->gravity)), 1.0F);
    }
    return 980.0F;
}

void add_child(entt::registry& ecs, entt::entity parent, entt::entity child) {
    auto& children = ecs.get<world::Transform2D>(parent).children;
    if (std::ranges::find(children, child) == children.end()) {
        children.push_back(child);
    }
}

// The chassis: its body, collision box and pieces.
void keep_body(world::World& world, entt::entity entity, const VehicleRecipe& recipe,
               const Scale& s) {
    entt::registry& ecs = world.ecs();
    const world::VehicleBody& b = recipe.body;
    const world::VehiclePhysics& p = recipe.physics;
    if (!ecs.all_of<world::RigidBody2D>(entity)) {
        ecs.emplace<world::RigidBody2D>(entity);
    }
    const world::RigidBody2D& body = ecs.get<world::RigidBody2D>(entity);
    const Vec2 center = s.at({p.center_of_mass.z, p.center_of_mass.y});
    const float damp = recipe.kind == "boat" ? 2.0F : 0.0F;
    if (body.mass != p.mass || body.center_of_mass_mode != "custom" ||
        body.center_of_mass != center || body.can_sleep || body.angular_damp != damp ||
        body.linear_damp_mode != "replace" || body.linear_damp != 0.0F) {
        ecs.patch<world::RigidBody2D>(entity, [&](world::RigidBody2D& changed) {
            changed.mass = p.mass;
            changed.center_of_mass_mode = "custom";
            changed.center_of_mass = center;
            changed.can_sleep = false;
            changed.angular_damp = damp;
            // Its own drag slows it, not the project's damping on top.
            changed.linear_damp_mode = "replace";
            changed.linear_damp = 0.0F;
        });
    }
    const std::string id(world.id_of(entity));
    if (id.empty()) {
        return;
    }
    const std::string box_id = id + "_collision";
    if (world.find(box_id) == entt::null) {
        const entt::entity box = create_named(world, box_id);
        const real_t bottom = recipe.kind == "boat" ? 0.0F : b.clearance;
        world::Transform2D place;
        place.position = s.at({0.0F, (bottom + b.height) * 0.5F});
        ecs.emplace<world::Transform2D>(box, place);
        world::CollisionShape2D shape;
        shape.shape = "rectangle";
        shape.size = {b.length * s.ppm, (b.height - bottom) * s.ppm};
        ecs.emplace<world::CollisionShape2D>(box, shape);
        add_child(ecs, entity, box);
        const world::VehicleSideView view = world::vehicle_side_view(recipe);
        for (const world::VehicleSideView::Piece& piece : view.body) {
            const entt::entity drawn = create_named(world, id + "_" + piece.name);
            ecs.emplace<world::Transform2D>(drawn);
            world::Polygon2D polygon;
            for (const Vec2 point : piece.points) {
                polygon.polygon.push_back(s.at(point));
            }
            polygon.color = piece.color;
            ecs.emplace<world::Polygon2D>(drawn, std::move(polygon));
            add_child(ecs, entity, drawn);
        }
    }
}

// A circle of points, for a wheel's tire.
std::vector<Vec2> circle(float radius, int sides) {
    std::vector<Vec2> out;
    for (int i = 0; i < sides; ++i) {
        const float angle = kTau * static_cast<float>(i) / static_cast<float>(sides);
        const Vec2f cs = cos_sin(angle);
        out.push_back({radius * cs.x, radius * cs.y});
    }
    return out;
}

// Each side-view wheel's body and joints; the wheels in side-view order.
std::vector<entt::entity> keep_wheels(world::World& world, entt::entity entity,
                                      const VehicleRecipe& recipe, const Scale& s,
                                      const world::Tree2D& tree) {
    entt::registry& ecs = world.ecs();
    std::vector<entt::entity> wheels;
    const std::string id(world.id_of(entity));
    if (id.empty() || !world::vehicle_rolls(recipe)) {
        return wheels;
    }
    const world::VehicleSideView view = world::vehicle_side_view(recipe);
    const world::VehiclePhysics& p = recipe.physics;
    const Affine2D chassis = tree.global_affine(entity);
    const float g = gravity_px(world, entity);
    for (std::size_t i = 0; i < view.wheels.size(); ++i) {
        const auto& [middle, radius] = view.wheels[i];
        const std::string name = id + "_wheel_" + std::to_string(i);
        entt::entity wheel = world.find(name);
        if (wheel != entt::null && ecs.all_of<world::RigidBody2D>(wheel)) {
            wheels.push_back(wheel);
            continue;
        }
        wheel = create_named(world, name);
        const float r = radius * s.ppm;
        world::Transform2D place;
        place.position = chassis.apply(s.at(middle));
        ecs.emplace_or_replace<world::Transform2D>(wheel, place);
        world::RigidBody2D body;
        body.mass = p.mass * kWheelShare;
        body.friction = std::clamp(p.tire_friction / 10.5F, 0.2F, 3.0F);
        body.can_sleep = false;
        body.linear_damp_mode = "replace";
        body.angular_damp_mode = "replace"; // the project's would brake them as they spin
        body.collision_layer = ecs.get<world::RigidBody2D>(entity).collision_layer;
        body.collision_mask = ecs.get<world::RigidBody2D>(entity).collision_mask;
        ecs.emplace_or_replace<world::RigidBody2D>(wheel, body);
        world::CollisionShape2D shape;
        shape.shape = "circle";
        shape.radius = r;
        ecs.emplace_or_replace<world::CollisionShape2D>(wheel, shape);
        world::Polygon2D tire;
        tire.polygon = circle(r, 20);
        tire.color = recipe.body.trim;
        ecs.emplace_or_replace<world::Polygon2D>(wheel, std::move(tire));
        // A hub off the middle, so its turning shows.
        const entt::entity hub = create_named(world, name + "_hub");
        ecs.emplace<world::Transform2D>(hub);
        world::Polygon2D spoke;
        spoke.polygon = {{-r * 0.15F, -r * 0.7F},
                         {r * 0.15F, -r * 0.7F},
                         {r * 0.15F, r * 0.7F},
                         {-r * 0.15F, r * 0.7F}};
        spoke.color = {0.75F, 0.75F, 0.75F, 1.0F};
        ecs.emplace<world::Polygon2D>(hub, std::move(spoke));
        add_child(ecs, wheel, hub);
        // The groove from above the wheel down past it, and the spring that holds it there,
        // along the chassis's down.
        const float travel = p.suspension_travel * s.ppm;
        const float rest = p.suspension_rest * s.ppm;
        const Vec2 top = s.at(middle) - Vec2{0.0F, rest + travel};
        world::Transform2D mount;
        mount.position = chassis.apply(top);
        mount.rotation =
            std::atan2(static_cast<float>(chassis.x.y), static_cast<float>(chassis.x.x));
        const entt::entity groove_entity = create_named(world, name + "_groove");
        ecs.emplace<world::Transform2D>(groove_entity, mount);
        ecs.emplace<world::GrooveJoint2D>(groove_entity,
                                          world::GrooveJoint2D{.node_a = entity,
                                                               .node_b = wheel,
                                                               .disable_collision = true,
                                                               .length = rest + (2.0F * travel),
                                                               .initial_offset = rest + travel});
        // Each side-view wheel holds up its share of the weight a compression as stiff as the
        // recipe's suspension.
        const float share = p.mass * g / static_cast<float>(view.wheels.size());
        const float count = static_cast<float>(std::max<std::size_t>(recipe.wheels.size(), 1));
        const float compression =
            std::max(9.8F / (count * std::max(p.suspension_stiffness, 1.0F)), 0.01F) * s.ppm;
        const float stiffness = share / compression;
        const float damping = std::sqrt(stiffness * body.mass) * 1.4F *
                              std::clamp(p.suspension_damping / 2.5F, 0.1F, 4.0F);
        const entt::entity spring_entity = create_named(world, name + "_spring");
        ecs.emplace<world::Transform2D>(spring_entity, mount);
        ecs.emplace<world::DampedSpringJoint2D>(
            spring_entity, world::DampedSpringJoint2D{.node_a = entity,
                                                      .node_b = wheel,
                                                      .disable_collision = true,
                                                      .length = rest + travel,
                                                      .rest_length = rest + travel + compression,
                                                      .stiffness = stiffness,
                                                      .damping = damping});
        wheels.push_back(wheel);
    }
    return wheels;
}

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

void place_at(world::World& world, const world::Tree2D& tree, entt::entity entity, Vec2 at,
              float rotation) {
    auto& ecs = world.ecs();
    auto* transform = ecs.try_get<world::Transform2D>(entity);
    if (transform == nullptr) {
        return;
    }
    world::Transform2D placed = *transform;
    if (const entt::entity parent = tree.transform_parent(entity); parent != entt::null) {
        world::Transform2D global;
        global.position = at;
        global.rotation = rotation;
        const world::Transform2D local =
            world::parts(tree.global_affine(parent).inverse() * world::affine(global));
        placed.position = local.position;
        placed.rotation = local.rotation;
    } else {
        placed.position = at;
        placed.rotation = rotation;
    }
    if (placed.position != transform->position || placed.rotation != transform->rotation) {
        ecs.replace<world::Transform2D>(entity, placed);
    }
}

float angle_of(const Affine2D& affine) {
    return std::atan2(static_cast<float>(affine.x.y), static_cast<float>(affine.x.x));
}

struct Push {
    Vec2 force;
    float torque = 0.0F;
};

// 2D cross product: the torque of a force at a point, positive clockwise on screen.
real_t turn(Vec2 at, Vec2 force) {
    return (at.x * force.y) - (at.y * force.x);
}

Push boat_forces(const VehicleRecipe& recipe, const Scale& s, const Affine2D& chassis,
                 Vec2 velocity, float spin, const world::VehicleControls& controls, float g) {
    Push out;
    const world::VehiclePhysics& p = recipe.physics;
    const world::VehicleBody& b = recipe.body;
    const Vec2 com = chassis.apply(s.at({p.center_of_mass.z, p.center_of_mass.y}));
    const float surface = -p.water_level; // pixels: y grows down
    const float draft = std::max(p.draft, 0.05F) * s.ppm;
    const float spring = p.mass * g / (2.0F * draft);
    const float damper = 2.0F * std::sqrt(spring * p.mass * 0.5F) * 0.6F;
    float submerged = 0.0F;
    for (const real_t z : {b.length * 0.45F, -b.length * 0.45F}) {
        const Vec2 corner = chassis.apply(s.at({z, 0.0F}));
        const float depth = std::min(static_cast<float>(corner.y) - surface, b.height * s.ppm);
        if (depth <= 0.0F) {
            continue;
        }
        submerged += 0.5F;
        const Vec2 at = corner - com;
        // The corner's velocity: the body's plus its spin's (clockwise positive in y down).
        const real_t sinking = velocity.y + (spin * at.x);
        const Vec2 lift{0.0F, -std::max<real_t>((spring * depth) + (damper * sinking), 0.0F)};
        out.force = out.force + lift;
        out.torque += static_cast<float>(turn(at, lift));
    }
    if (submerged <= 0.0F) {
        return out;
    }
    const Vec2 forward = normalized(chassis.x) * s.facing;
    const real_t along = dot(velocity, forward);
    out.force = out.force - (forward * (p.water_drag * p.mass * 0.1F * along * submerged)) -
                (Vec2{0.0F, velocity.y} * (p.water_drag * p.mass * 0.4F * submerged));
    out.force = out.force + (forward * (p.engine.thrust * s.ppm *
                                        std::clamp(controls.throttle, -0.5F, 1.0F) * submerged));
    return out;
}

Push aircraft_forces(const VehicleRecipe& recipe, const Scale& s, const Affine2D& chassis,
                     Vec2 velocity, const world::VehicleControls& controls) {
    Push out;
    const world::VehiclePhysics& p = recipe.physics;
    const Vec2 forward = normalized(chassis.x) * s.facing;
    const Vec2 up = normalized(chassis.y) * -1.0F;
    out.force = forward * (p.engine.thrust * s.ppm * std::clamp(controls.throttle, 0.0F, 1.0F));
    const real_t along = dot(velocity, forward) / s.ppm; // meters a second
    if (along <= 0.0F) {
        return out;
    }
    const real_t area = recipe.wing_span * recipe.wing_chord;
    const real_t q = 0.5F * kAir * along * along;
    const real_t attack = std::atan2(-dot(velocity, up) / s.ppm, along);
    const real_t coefficient = p.lift * std::clamp<real_t>(0.25F + (4.0F * attack), -0.6F, 1.4F);
    out.force = out.force + (up * (q * area * coefficient * s.ppm));
    // The elevator: nose up turns counterclockwise when facing right.
    const real_t authority = q * area * p.control * 0.02F * recipe.wing_chord * 4.0F;
    out.torque = static_cast<float>(-controls.pitch * authority * s.ppm * s.ppm * s.facing);
    return out;
}

void hit_people(world::World& world, const world::Tree2D& tree, entt::entity vehicle,
                const VehicleRecipe& recipe, const Scale& s, const Affine2D& chassis, Vec2 velocity,
                Kept& kept, float seconds) {
    for (auto it = kept.hits.begin(); it != kept.hits.end();) {
        it->second -= seconds;
        it = it->second <= 0.0F ? kept.hits.erase(it) : std::next(it);
    }
    const world::VehicleDamage& damage = recipe.damage;
    if (length(velocity) / s.ppm < damage.min_speed) {
        return;
    }
    entt::registry& ecs = world.ecs();
    std::vector<entt::entity> targets;
    for (const entt::entity e : ecs.view<world::Transform2D>()) {
        if (e != vehicle &&
            ecs.any_of<world::CharacterBody2D, world::Creature2D, world::Ragdoll2D>(e) &&
            !ecs.all_of<world::VehicleRider>(e) && !kept.hits.contains(e)) {
            targets.push_back(e);
        }
    }
    std::reverse(targets.begin(), targets.end());
    const Affine2D inverse = chassis.inverse();
    const world::VehicleBody& b = recipe.body;
    for (const entt::entity target : targets) {
        const Vec2 where = tree.global_affine(target).origin;
        const Vec2 local = inverse.apply(where);
        const real_t z = local.x / s.ppm;
        const real_t y = -local.y / s.ppm;
        if (std::abs(z) > (b.length * 0.5F) + kReach || y < -1.0F || y > b.height + 1.0F) {
            continue;
        }
        Vec2 target_velocity;
        if (const auto* body = ecs.try_get<world::CharacterBody2D>(target); body != nullptr) {
            target_velocity = body->velocity;
        }
        const Vec2 away{where.x >= chassis.origin.x ? 1.0F : -1.0F, 0.0F};
        const real_t closing = dot(velocity - target_velocity, away) / s.ppm;
        if (closing < damage.min_speed) {
            continue;
        }
        const real_t reduced =
            (recipe.physics.mass * kPersonMass) / (recipe.physics.mass + kPersonMass);
        // kilogram-meters a second, bouncing off as a collision of this restitution would, so
        // whoever is hit leaves faster than the vehicle and is clear of it
        const real_t impulse = (1.0F + kHitRestitution) * reduced * closing;
        kept.hits[target] = kHitCooldown;
        const Vec2 shove = (away + Vec2{0.0F, -0.3F}) * (impulse / kPersonMass * s.ppm);
        if (auto* body = ecs.try_get<world::CharacterBody2D>(target); body != nullptr) {
            world::CharacterBody2D pushed = *body;
            pushed.velocity = pushed.velocity + shove;
            ecs.replace<world::CharacterBody2D>(target, pushed);
        }
        if (auto* creature = ecs.try_get<world::Creature2D>(target); creature != nullptr) {
            world::Creature2D scared = *creature;
            scared.mind.mode = "fleeing";
            scared.mind.target = vehicle;
            scared.mind.timer = 3.0F;
            ecs.replace<world::Creature2D>(target, scared);
        }
        bool knocked = false;
        if (ecs.all_of<world::Ragdoll2D>(target) && impulse >= damage.knockdown) {
            std::string bone;
            real_t nearest = std::numeric_limits<real_t>::max();
            for (const auto& [name, part] : ragdoll_bones(world, target)) {
                const real_t d = length(tree.global_affine(part).origin - chassis.origin);
                if (d < nearest) {
                    nearest = d;
                    bone = name;
                }
            }
            if (!bone.empty()) {
                auto report = hit_ragdoll(world, target, bone, away * (impulse * s.ppm));
                knocked = report && report.value().knocked_down;
            }
        }
        json::Value data;
        data["vehicle"] = std::string(world.id_of(vehicle));
        data["target"] = std::string(world.id_of(target));
        data["impulse"] = static_cast<double>(impulse);
        data["damage"] = static_cast<double>(impulse * damage.per_impulse);
        data["speed"] = static_cast<double>(closing);
        data["knocked_down"] = knocked;
        world.emit("vehicle_hit", world.id_of(vehicle), std::move(data));
    }
}

void drive_vehicle(world::World& world, entt::entity entity, Kept& kept, float seconds) {
    entt::registry& ecs = world.ecs();
    world::Vehicle2D vehicle = ecs.get<world::Vehicle2D>(entity);
    auto tuned = tuned_of(world, vehicle, &kept);
    if (!tuned) {
        return;
    }
    const VehicleRecipe& recipe = *tuned.value();
    const Scale s = scale_of(vehicle);
    keep_body(world, entity, recipe, s);
    const world::Tree2D tree(world);
    const std::vector<entt::entity> wheels = keep_wheels(world, entity, recipe, s, tree);
    const world::VehicleSideView view = world::vehicle_side_view(recipe);
    const Affine2D chassis = tree.global_affine(entity);
    const auto& body = ecs.get<world::RigidBody2D>(entity);
    const Vec2 velocity = body.linear_velocity;
    const Vec2 forward = normalized(chassis.x) * s.facing;
    const real_t speed = dot(velocity, forward) / s.ppm;
    const world::VehicleControls controls =
        vehicle.enabled ? vehicle.controls : world::VehicleControls{};
    const float g = gravity_px(world, entity);

    // The drivetrain, from the driven wheels' spin.
    float wheel_rpm = 0.0F;
    float radius = 0.35F;
    int driven = 0;
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        const world::VehicleWheel& made = recipe.wheels[view.wheel_of[i]];
        if (made.drive) {
            wheel_rpm +=
                std::abs(ecs.get<world::RigidBody2D>(wheels[i]).angular_velocity) * 60.0F / kTau;
            radius = made.radius;
            ++driven;
        }
    }
    if (driven > 0) {
        wheel_rpm /= static_cast<float>(driven);
    }
    world::DriveState state{.gear = vehicle.drive.gear,
                            .rpm = vehicle.drive.rpm,
                            .shift_timer = vehicle.drive.shift_timer,
                            .steer = vehicle.drive.steer};
    world::DriveOutput out =
        world::drive_step(recipe.physics, state,
                          {.throttle = controls.throttle,
                           .brake = controls.brake,
                           .handbrake = controls.handbrake,
                           .steer = controls.steer,
                           .pitch = controls.pitch},
                          wheel_rpm, static_cast<float>(speed), radius, seconds);
    if (recipe.physics.engine.max_torque <= 0.0F) {
        out.force = 0.0F;
    }
    vehicle.drive = {.gear = state.gear,
                     .rpm = state.rpm,
                     .shift_timer = state.shift_timer,
                     .steer = state.steer,
                     .speed = static_cast<float>(speed)};

    // Each side-view wheel stands for those at its z: a share of the force by how many it
    // stands for, as a torque, and brakes against its spin.
    const float idle = std::abs(controls.throttle) < 0.05F && out.brake <= 0.0F
                           ? 0.015F * recipe.physics.mass * 9.8F /
                                 static_cast<float>(std::max<std::size_t>(wheels.size(), 1))
                           : 0.0F;
    const float per_driven = driven > 0 ? out.force / static_cast<float>(driven) : 0.0F;
    const float pairs = static_cast<float>(std::max<std::size_t>(recipe.wheels.size(), 1)) /
                        static_cast<float>(std::max<std::size_t>(wheels.size(), 1));
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        const world::VehicleWheel& made = recipe.wheels[view.wheel_of[i]];
        world::RigidBody2D wheel = ecs.get<world::RigidBody2D>(wheels[i]);
        const float r = made.radius * s.ppm;
        float torque = made.drive && vehicle.enabled ? per_driven * s.ppm * r * s.facing : 0.0F;
        // Brakes push against the spin, no harder than stops it this tick.
        float brake = (out.brake * pairs) + idle;
        if (made.handbrake) {
            brake += out.handbrake * pairs;
        }
        if (brake > 0.0F && wheel.angular_velocity != 0.0F) {
            const float inertia = 0.5F * wheel.mass * r * r;
            const float stop = inertia * std::abs(wheel.angular_velocity) / seconds;
            const float held = std::min(brake * s.ppm * r, stop);
            torque -= wheel.angular_velocity > 0.0F ? held : -held;
        }
        if (wheel.constant_torque != torque) {
            wheel.constant_torque = torque;
            ecs.replace<world::RigidBody2D>(wheels[i], wheel);
        }
    }

    // The air, the water and wings; steering leans the body.
    Push push;
    const world::VehicleBody& b = recipe.body;
    const real_t front_area = b.width * std::max<real_t>(b.height - b.clearance, 0.1F);
    const real_t v = length(velocity) / s.ppm;
    push.force = velocity * (-0.5F * kAir * recipe.physics.drag * front_area * v);
    if (recipe.kind == "boat") {
        const Push water =
            boat_forces(recipe, s, chassis, velocity, body.angular_velocity, controls, g);
        push.force = push.force + water.force;
        push.torque += water.torque;
    } else if (recipe.kind == "aircraft") {
        const Push air = aircraft_forces(recipe, s, chassis, velocity, controls);
        push.force = push.force + air.force;
        push.torque += air.torque;
    } else {
        push.torque -=
            controls.steer * 0.05F * recipe.physics.mass * g * b.length * s.ppm * s.facing;
    }
    world::RigidBody2D pushed = ecs.get<world::RigidBody2D>(entity);
    if (pushed.constant_force != push.force || pushed.constant_torque != push.torque) {
        pushed.constant_force = push.force;
        pushed.constant_torque = push.torque;
        ecs.replace<world::RigidBody2D>(entity, pushed);
    }
    if (!(vehicle == ecs.get<world::Vehicle2D>(entity))) {
        ecs.replace<world::Vehicle2D>(entity, vehicle);
    }

    // Riders go where it will be after the step.
    Affine2D ahead = chassis;
    ahead.origin = ahead.origin + (velocity * static_cast<real_t>(seconds));
    const float rotation = angle_of(chassis);
    for (const auto& [rider, ride] : riders_of(ecs, entity)) {
        const world::VehicleSeat* seat = seat_named(recipe, ride.seat);
        if (seat == nullptr) {
            continue;
        }
        place_at(world, tree, rider, ahead.apply(s.at({seat->position.z, seat->position.y})),
                 rotation);
        if (auto* character = ecs.try_get<world::CharacterBody2D>(rider); character != nullptr) {
            if (character->velocity != Vec2{} || character->collision_layer != 0 ||
                character->collision_mask != 0) {
                world::CharacterBody2D still = *character;
                still.velocity = {};
                still.collision_layer = 0;
                still.collision_mask = 0;
                ecs.replace<world::CharacterBody2D>(rider, still);
            }
        }
    }
    hit_people(world, tree, entity, recipe, s, chassis, velocity, kept, seconds);
}

} // namespace

void add_vehicle_system(world::World& world) {
    world.add_system(
        "vehicles_2d",
        [](world::World& w, double tick_seconds) {
            entt::registry& ecs = w.ecs();
            VehicleState& state = state_of(w);
            const auto view = ecs.view<world::Vehicle2D, world::Transform2D>();
            std::vector<entt::entity> vehicles(view.begin(), view.end());
            std::reverse(vehicles.begin(), vehicles.end());
            std::erase_if(state.vehicles, [&](const auto& kept) {
                return !ecs.valid(kept.first) || !ecs.all_of<world::Vehicle2D>(kept.first);
            });
            if (vehicles.empty()) {
                return;
            }
            world::prepare_vehicles(w);
            for (const entt::entity entity : vehicles) {
                drive_vehicle(w, entity, state.vehicles[entity], static_cast<float>(tick_seconds));
            }
        },
        {.doc = "Drives each Vehicle2D as its .vehicle.json recipe says, seen from the side: "
                "keeps its body, pieces and sprung wheels, runs its drivetrain from its "
                "controls, floats boats, flies aircraft, carries its riders and hits what it "
                "runs into.",
         .priority = 945});
}

namespace detail {

bool is_vehicle_wheel(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    const std::size_t mark = id.rfind("_wheel_");
    if (mark == std::string_view::npos || mark + 7 >= id.size()) {
        return false;
    }
    for (const char c : id.substr(mark + 7)) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const entt::entity vehicle = world.find(id.substr(0, mark));
    return vehicle != entt::null && world.ecs().all_of<world::Vehicle2D>(vehicle);
}

} // namespace detail

Result<std::shared_ptr<const VehicleRecipe>> vehicle_recipe(const world::World& world,
                                                            entt::entity vehicle) {
    const auto* found = world.ecs().try_get<world::Vehicle2D>(vehicle);
    if (found == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "it has no Vehicle2D");
    }
    return tuned_of(world, *found, nullptr);
}

Result<std::string> enter_vehicle(world::World& world, entt::entity rider, entt::entity vehicle,
                                  std::string_view seat) {
    entt::registry& ecs = world.ecs();
    if (!ecs.valid(rider) || !ecs.all_of<world::Transform2D>(rider)) {
        return make_error(ErrorCode::InvalidArgument, "the rider has no Transform2D");
    }
    if (ecs.all_of<world::VehicleRider>(rider)) {
        return make_error(ErrorCode::InvalidArgument, "the rider is already in a vehicle");
    }
    auto recipe = vehicle_recipe(world, vehicle);
    if (!recipe) {
        return recipe.error();
    }
    const auto riders = riders_of(ecs, vehicle);
    const auto taken = [&](std::string_view name) {
        return std::ranges::any_of(riders, [&](const auto& r) { return r.second.seat == name; });
    };
    std::string chosen;
    if (!seat.empty()) {
        if (std::ranges::none_of(recipe.value()->seats,
                                 [&](const world::VehicleSeat& s) { return s.name == seat; })) {
            return make_error(ErrorCode::NotFound,
                              "the vehicle has no seat \"" + std::string(seat) + "\"");
        }
        if (taken(seat)) {
            return make_error(ErrorCode::InvalidArgument,
                              "seat \"" + std::string(seat) + "\" is taken");
        }
        chosen = seat;
    } else {
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
    if (auto* body = ecs.try_get<world::CharacterBody2D>(rider); body != nullptr) {
        ride.collision_layer = body->collision_layer;
        ride.collision_mask = body->collision_mask;
        world::CharacterBody2D quiet = *body;
        quiet.collision_layer = 0;
        quiet.collision_mask = 0;
        quiet.velocity = {};
        ecs.replace<world::CharacterBody2D>(rider, quiet);
    }
    ecs.emplace<world::VehicleRider>(rider, ride);
    const world::Tree2D tree(world);
    const Affine2D chassis = tree.global_affine(vehicle);
    const Scale s = scale_of(ecs.get<world::Vehicle2D>(vehicle));
    if (const world::VehicleSeat* found = seat_named(*recipe.value(), chosen); found != nullptr) {
        place_at(world, tree, rider, chassis.apply(s.at({found->position.z, found->position.y})),
                 angle_of(chassis));
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
    if (auto* body = ecs.try_get<world::CharacterBody2D>(rider); body != nullptr) {
        world::CharacterBody2D back = *body;
        back.collision_layer = was.collision_layer;
        back.collision_mask = was.collision_mask;
        ecs.replace<world::CharacterBody2D>(rider, back);
    }
    if (ecs.valid(was.vehicle) && ecs.all_of<world::Vehicle2D, world::Transform2D>(was.vehicle)) {
        if (auto recipe = vehicle_recipe(world, was.vehicle); recipe) {
            const world::Tree2D tree(world);
            const Affine2D chassis = tree.global_affine(was.vehicle);
            const Scale s = scale_of(ecs.get<world::Vehicle2D>(was.vehicle));
            // Behind its tail, on the ground it stands on.
            const real_t back = -(recipe.value()->body.length * 0.5F) - 0.6F;
            place_at(world, tree, rider, chassis.apply(s.at({back, 0.0F})), 0.0F);
        }
        json::Value data;
        data["rider"] = std::string(world.id_of(rider));
        data["seat"] = was.seat;
        world.emit("vehicle_exited", world.id_of(was.vehicle), std::move(data));
    }
    return {};
}

} // namespace slash::physics2d
