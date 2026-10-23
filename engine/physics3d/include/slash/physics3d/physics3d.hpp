#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// 3D physics on Jolt Physics, as Godot 4.4's Jolt module (modules/jolt_physics). The components
// are in slash/world/components.hpp: StaticBody3D, AnimatableBody3D, RigidBody3D, CharacterBody3D
// and Area3D make an entity a body or an area, and CollisionShape3D gives it shapes.
//
// The "physics3d" system keeps a Jolt physics system in step with the components, as the 2D one
// does Box2D's world: each tick it creates, changes and removes bodies to match them, steps Jolt,
// writes rigid bodies' transforms and velocities back, moves character bodies with Godot's
// move_and_slide (on Godot's Jolt motion tests) and emits body_entered, body_exited,
// area_entered and area_exited events. The components are the truth, so commands, undo and
// snapshots work on physics as on anything else.
//
// Jolt is built cross-platform deterministic and steps on one thread, and the system makes its
// calls in the same order everywhere, so a scene simulates the same on every machine.
namespace slash::physics3d {

// The project's physics_3d settings, as Godot's physics/3d and physics/jolt_physics_3d project
// settings.
struct Settings {
    float gravity = 9.8F; // meters per second squared
    Vec3 gravity_vector{0.0F, -1.0F, 0.0F};
    float linear_damp = 0.1F;  // what RigidBody3D's linear_damp combines with
    float angular_damp = 0.1F; // what RigidBody3D's angular_damp combines with
    // Whether Area3D detects static bodies, which Godot's Jolt module leaves off.
    bool areas_detect_static_bodies = false;

    friend bool operator==(const Settings&, const Settings&) = default;
};

// JSON Schema for the physics_3d object in project.json.
[[nodiscard]] json::Value settings_schema();
// Settings from a physics_3d object that passed the schema; missing fields keep their defaults.
[[nodiscard]] Settings parse_settings(const json::Value& settings);

// Adds the "physics3d" system to a world, which steps 3D physics each tick after gameplay
// systems and animation players, beside 2D physics.
void install(world::World& world, const Settings& settings = {});
// Changes the settings of a world the system was installed in.
void set_settings(world::World& world, const Settings& settings);

// What a query looks for.
struct QueryFilter {
    std::uint32_t collision_mask = 0xFFFFFFFFU; // layers to look in
    bool collide_with_bodies = true;
    bool collide_with_areas = false;
    std::vector<entt::entity> exclude{}; // NOLINT(readability-redundant-member-init)
};

struct RayHit {
    entt::entity entity = entt::null;
    Vec3 point;
    Vec3 normal;           // the surface's, toward the ray's start
    float fraction = 0.0F; // how far along the ray, from 0 to 1
};

// The first thing a ray from from to to hits, as Godot's intersect_ray. Shapes the ray starts
// inside and the backs of concave faces are not hit.
[[nodiscard]] std::optional<RayHit> raycast(world::World& world, Vec3 from, Vec3 to,
                                            const QueryFilter& filter = {});

// Entities whose shapes contain a point, as Godot's intersect_point, sorted by ID.
[[nodiscard]] std::vector<entt::entity> overlap_point(world::World& world, Vec3 point,
                                                      const QueryFilter& filter = {},
                                                      std::size_t max_results = 32);

// Entities whose shapes overlap a shape placed at position and rotation (radians, as
// Transform3D's), as Godot's intersect_shape, sorted by ID. The shape's own position and
// rotation apply too.
[[nodiscard]] std::vector<entt::entity>
overlap_shape(world::World& world, const world::CollisionShape3D& shape, Vec3 position,
              Vec3 rotation, const QueryFilter& filter = {}, std::size_t max_results = 32);

// What touches an entity now, each sorted by ID: for a body, the bodies touching it (for a
// character, what its last move slid along) and the areas it is in; for an area, the bodies and
// areas in it.
struct Contacts {
    std::vector<entt::entity> bodies;
    std::vector<entt::entity> areas;
};
[[nodiscard]] Contacts contacts(world::World& world, entt::entity entity);

// The gravity on a body and the damping that slows it, as Godot's get_gravity and the body's
// total damping: from the areas whose space overrides reach it, highest priority first, then
// the project's physics_3d settings. A rigid body's gravity_scale and own damping are in them.
struct Gravity {
    Vec3 gravity;                      // meters per second squared
    float linear_damp = 0.0F;          // per second
    float angular_damp = 0.0F;         // per second
    std::vector<entt::entity> areas{}; // NOLINT(readability-redundant-member-init): in order
};
// nullopt when the entity is no 3D body.
[[nodiscard]] std::optional<Gravity> gravity(world::World& world, entt::entity entity);

// Whether an entity is a 3D body or area now, as the next step sees it.
[[nodiscard]] bool has_body(const world::World& world, entt::entity entity);

// Pushes a rigid body at the next physics step, as Godot's apply_impulse: impulse in
// kilogram-meters per second, at offset meters from its position.
void apply_impulse(world::World& world, entt::entity entity, Vec3 impulse, Vec3 offset = {});
// Turns a rigid body at the next physics step, as Godot's apply_torque_impulse.
void apply_torque_impulse(world::World& world, entt::entity entity, Vec3 impulse);

} // namespace slash::physics3d
