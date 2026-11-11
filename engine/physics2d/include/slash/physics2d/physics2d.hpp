#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/components.hpp"
#include "slash/world/damage.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 2D physics on Box2D v3, as Godot's 2D physics. The components are in slash/world/components.hpp:
// StaticBody2D, AnimatableBody2D, RigidBody2D, CharacterBody2D and Area2D make an entity a body or
// an area, CollisionShape2D and CollisionPolygon2D give it shapes, and PinJoint2D,
// DampedSpringJoint2D and GrooveJoint2D join two bodies.
//
// A TileMapLayer is solid where its tiles have collision polygons, as Godot's tile physics:
// canvas::install hands this module the polygons (set_tile_shape_source), since tile sets are
// the canvas module's.
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
    // Solver substeps each physics tick takes, as Box2D's: more keep fast, small or stacked
    // bodies steadier, at more cost.
    int substeps = 4;

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
    // The cell hit, when the entity is a TileMapLayer.
    std::optional<Vec2i> cell{}; // NOLINT(readability-redundant-member-init)
    // The surface its physics material names, such as "wood"; empty without one.
    std::string surface;
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

// The gravity on a body and the damping that slows it, as Godot's get_gravity and the body's
// total damping: from the areas whose space overrides reach it, highest priority first, then
// the project's physics_2d settings. A rigid body's gravity_scale and own damping are in them;
// a character body uses the gravity in its own velocity.
struct Gravity {
    Vec2 gravity;                      // pixels per second squared
    float linear_damp = 0.0F;          // per second
    float angular_damp = 0.0F;         // per second
    std::vector<entt::entity> areas{}; // NOLINT(readability-redundant-member-init): in order
};
// nullopt when the entity is no body.
[[nodiscard]] std::optional<Gravity> gravity(world::World& world, entt::entity entity);

// A collision polygon of one cell of a TileMapLayer, in pixels in the layer's own space, with
// its tile set physics layer's settings.
struct TileShape {
    Vec2i cell;
    std::vector<Vec2> points;
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    bool one_way = false;
    float one_way_margin = 1.0F;
};
using TileShapes = std::shared_ptr<const std::vector<TileShape>>;
// A TileMapLayer's shapes, or nullptr for none; the same pointer while they haven't changed, so
// the layer's body is rebuilt only when they do.
using TileShapeSource = std::function<TileShapes(const world::World& world, entt::entity layer)>;

// Where a world's TileMapLayer components get their shapes: canvas::install sets it. Each layer
// whose collision_enabled is on and that has shapes becomes one static body, in the order the
// layers were made, after every other body.
void set_tile_shape_source(world::World& world, TileShapeSource source);
// The shapes a layer has, as the source gives them; nullptr if it has none or there's no source.
[[nodiscard]] TileShapes tile_shapes(const world::World& world, entt::entity layer);

// Pushes a rigid body at the next physics step, as Godot's apply_impulse: impulse in
// kilogram-pixels per second, at offset pixels from its position.
void apply_impulse(world::World& world, entt::entity entity, Vec2 impulse, Vec2 offset = {});
// Turns a rigid body at the next physics step, as Godot's apply_torque_impulse.
void apply_torque_impulse(world::World& world, entt::entity entity, float impulse);

// What damage did to a Destructible2D: how much its health went down (after its profile's
// resistance), its health and damage state after, and whether it broke, into which pieces, and
// whether linked pieces fell.
struct DamageReport2D {
    std::string id;
    float dealt = 0.0F;
    float health = 0.0F;
    int state = 0;
    bool broke = false;
    std::vector<std::string> pieces;
    bool fell = false;
    // How the damage changed its shape while it held: "dent", "hole", "carve" or "".
    std::string deformed;
};

// Carves the cells of a TileMapLayer whose centers are within radius pixels of center, as the
// canvas module sets it (physics2d doesn't depend on canvas); returns how many it took away, or
// without apply how many it would.
using TileCarver = std::function<int(world::World& world, entt::entity layer, Vec2 center,
                                     float radius, bool apply)>;
void set_tile_carver(world::World& world, TileCarver carver);

// Changes a Destructible2D's outline by damage that didn't break it, as its profile's deform
// settings say: blunt, impact and explosive damage dent it, pierce damage holes it and
// explosive damage carves it (a TileMapLayer loses the cells a blast reaches). Parts cut off
// fall away as debris pieces. The settings' meters are 64 pixels each, as Box2D's length unit.
// Returns "dent", "hole", "carve" or "" when nothing changed.
std::string deform_body_2d(world::World& world, entt::entity entity,
                           const world::DamageProfile& profile, float dealt, std::string_view kind,
                           std::optional<Vec2> at, std::optional<Vec2> direction = std::nullopt);

// Breaks a Destructible2D into pieces now, as its profile (or its legacy cells) cuts it, the
// pieces flying push pixels a second from from (else from its center). Returns the pieces.
[[nodiscard]] Result<std::vector<entt::entity>>
break_body_2d(world::World& world, entt::entity entity, float push = 0.0F,
              std::optional<Vec2> from = std::nullopt);
// Tears a Cloth2D's edges whose middles are within radius pixels of center, where physics last
// put its points (else where they rest), as a cut, a shot or a blast does; returns the edges it
// tore (world::cloth_grid_2d's numbers) and emits "torn" with them.
std::vector<int> tear_cloth_2d(world::World& world, entt::entity entity, Vec2 center, float radius);
// Does amount damage of a kind ("blunt", "pierce", "cut", "explosive", "fire", "impact") to a
// Destructible2D at a point in the world, breaking it once its health runs out.
[[nodiscard]] Result<DamageReport2D> damage_body_2d(world::World& world, entt::entity entity,
                                                    float amount, std::string_view kind,
                                                    std::optional<Vec2> at = std::nullopt,
                                                    float push = 0.0F,
                                                    std::optional<Vec2> direction = std::nullopt);
// Damages every Destructible2D within radius pixels of center (its outline's nearest point, a
// tile map layer's place), less with distance.
[[nodiscard]] std::vector<DamageReport2D> damage_area_2d(world::World& world, Vec2 center,
                                                         float radius, float amount,
                                                         std::string_view kind, float push = 0.0F);
// Works out the load through a linked group's pieces now, breaking the links it is too much for;
// returns the pieces that came loose.
std::vector<std::string> settle_group_2d(world::World& world, std::string_view group);

} // namespace slash::physics2d
