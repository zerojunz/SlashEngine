#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// 2D physics on Box2D v3, as Godot's 2D physics. The components are in slash/world/components.hpp:
// StaticBody2D, AnimatableBody2D, RigidBody2D, CharacterBody2D and Area2D make an entity a body or
// an area, CollisionShape2D and CollisionPolygon2D give it shapes, and PinJoint2D,
// DampedSpringJoint2D and GrooveJoint2D join two bodies.
//
// The "physics2d" system keeps a Box2D world in step with the components: each tick it creates,
// changes and removes bodies to match them, moves character bodies, steps the world and writes
// rigid bodies' transforms and velocities back. The components are the truth, so commands, undo
// and snapshots work on physics as on anything else; Box2D's caches (contacts, sleep timers) are
// rebuilt when an entity is replaced, so a restored world may not replay bit for bit.
//
// Box2D is deterministic on every platform for the same calls, and the system makes its calls in
// the same order everywhere, so a scene simulates the same on every machine.
namespace slash::physics2d {

// The project's physics_2d settings, as Godot's physics/2d project settings.
struct Settings {
    float gravity = 980.0F; // pixels per second squared
    Vec2 gravity_vector{0.0F, 1.0F};
    float linear_damp = 0.1F;  // what RigidBody2D's linear_damp combines with
    float angular_damp = 1.0F; // what RigidBody2D's angular_damp combines with

    friend bool operator==(const Settings&, const Settings&) = default;
};

// JSON Schema for the physics_2d object in project.json.
[[nodiscard]] json::Value settings_schema();
// Settings from a physics_2d object that passed the schema; missing fields keep their defaults.
[[nodiscard]] Settings parse_settings(const json::Value& settings);

// Adds the "physics2d" system to a world, which steps 2D physics each tick after gameplay
// systems and animation players and before sprite animation.
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
    Vec2 point;
    Vec2 normal;           // the surface's, toward the ray's start
    float fraction = 0.0F; // how far along the ray, from 0 to 1
};

// The first thing a ray from from to to hits, as Godot's intersect_ray. Shapes the ray starts
// inside are not hit.
[[nodiscard]] std::optional<RayHit> raycast(world::World& world, Vec2 from, Vec2 to,
                                            const QueryFilter& filter = {});

// Entities whose shapes contain a point, as Godot's intersect_point, sorted by ID.
[[nodiscard]] std::vector<entt::entity> overlap_point(world::World& world, Vec2 point,
                                                      const QueryFilter& filter = {},
                                                      std::size_t max_results = 32);

// Entities whose shapes overlap a shape placed at position and rotation, as Godot's
// intersect_shape, sorted by ID. The shape's own position and rotation apply too.
[[nodiscard]] std::vector<entt::entity>
overlap_shape(world::World& world, const world::CollisionShape2D& shape, Vec2 position,
              float rotation, const QueryFilter& filter = {}, std::size_t max_results = 32);

// What touches an entity now, each sorted by ID: for a body, the bodies touching it (for a
// character, what its last move slid along) and the areas it is in; for an area, the bodies and
// areas in it.
struct Contacts {
    std::vector<entt::entity> bodies;
    std::vector<entt::entity> areas;
};
[[nodiscard]] Contacts contacts(world::World& world, entt::entity entity);

// Pushes a rigid body at the next physics step, as Godot's apply_impulse: impulse in
// kilogram-pixels per second, at offset pixels from its position.
void apply_impulse(world::World& world, entt::entity entity, Vec2 impulse, Vec2 offset = {});
// Turns a rigid body at the next physics step, as Godot's apply_torque_impulse.
void apply_torque_impulse(world::World& world, entt::entity entity, float impulse);

} // namespace slash::physics2d
