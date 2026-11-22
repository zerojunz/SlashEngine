#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/damage.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 3D physics on Jolt Physics, as Godot 4.4's Jolt module (modules/jolt_physics). The components
// are in slash/world/components.hpp: StaticBody3D, MovingBody3D, RigidBody3D, CharacterMover3D
// and Zone3D make an entity a body or an area, and Collider3D gives it shapes.
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
    // Whether Zone3D detects static bodies, which Godot's Jolt module leaves off.
    bool areas_detect_static_bodies = false;
    // Collision steps each physics tick takes, as Chaos's substeps: more keep fast, small or
    // stacked bodies steadier, at that many times the cost.
    int substeps = 1;
    // The most pieces breaks from impacts may cut a tick, so a chain of breaks spreads over
    // ticks rather than stalling one (the first break of a tick always cuts); 0 for no limit.
    // Counted in pieces, not time, so replays repeat.
    int break_budget = 0;

    // Godot's physics/jolt_physics_3d/simulation settings, by their names there.
    int velocity_steps = 10; // solver iterations for velocities each step
    int position_steps = 2;  // solver iterations for positions each step
    // How much of the penetration the position steps take out each step (0 to 1).
    float baumgarte_stabilization_factor = 0.2F;
    // How close bodies come, in meters, before contacts are made for them, so fast ones don't
    // jitter or tunnel.
    float speculative_contact_distance = 0.02F;
    // How far bodies may sink into each other, in meters, before they are pushed apart.
    float penetration_slop = 0.02F;
    // How far a body with continuous collision detection must move in a step, as a fraction of
    // its inner radius, before it is cast rather than moved.
    float continuous_cd_movement_threshold = 0.75F;
    // How far, as a fraction of its inner radius, a cast body may still sink into what it hits.
    float continuous_cd_max_penetration = 0.25F;
    // Whether contacts are reused while two bodies barely move against each other.
    bool body_pair_contact_cache_enabled = true;
    // How far, in meters, and how far turned, in radians, two bodies may move apart before
    // their cached contacts are found again.
    float body_pair_contact_cache_distance_threshold = 0.001F;
    float body_pair_contact_cache_angle_threshold = 0.034906585F; // 2 degrees
    // The speed, in meters per second, below which colliding bodies don't bounce.
    float bounce_velocity_threshold = 1.0F;
    // Whether resting bodies sleep, after sleep_time_threshold seconds with every point moving
    // slower than sleep_velocity_threshold meters per second.
    bool allow_sleep = true;
    float sleep_velocity_threshold = 0.03F;
    float sleep_time_threshold = 0.5F;
    // Whether bodies skip contacts with the inner edges of meshes and compound shapes, so they
    // slide over seams smoothly.
    bool use_enhanced_internal_edge_removal = true;
    // The radius, in meters, of a soft body's or cloth's points when they collide.
    float soft_body_point_radius = 0.01F;
    // Godot's motion_queries settings: the passes and the share of each overlap a character's
    // move takes to push out of what it overlaps, and whether it skips inner edges.
    int recovery_iterations = 4;
    float recovery_amount = 0.4F;
    bool motion_use_enhanced_internal_edge_removal = true;
    // Godot's collisions settings: the share of a convex shape's smallest extent its margin may
    // take, and the angle in radians between faces under which a mesh's edge counts as smooth.
    float collision_margin_fraction = 0.08F;
    float active_edge_threshold = 0.87266463F; // 50 degrees
    // Godot's limits settings: a world boundary shape's size in meters, the fastest bodies may
    // move (meters and radians per second), how many bodies, body pairs and contact
    // constraints a space holds, and Jolt's scratch memory for a step in megabytes (more comes
    // from the heap).
    float world_boundary_shape_size = 2000.0F;
    float max_linear_velocity = 500.0F;
    float max_angular_velocity = 47.1238898F; // 2700 degrees
    int max_bodies = 10240;
    int max_body_pairs = 65536;
    int max_contact_constraints = 20480;
    int temporary_memory_buffer_size = 4;

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
    // The cell hit, when the entity is a BlockMap3D.
    std::optional<Vec3i> cell{}; // NOLINT(readability-redundant-member-init)
    // The surface its physics material names, such as "wood"; empty without one.
    std::string surface;
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
[[nodiscard]] std::vector<entt::entity> overlap_shape(world::World& world,
                                                      const world::Collider3D& shape, Vec3 position,
                                                      Vec3 rotation, const QueryFilter& filter = {},
                                                      std::size_t max_results = 32);

// The point of an area's or body's shapes nearest to point, as Godot's
// get_closest_point_to_object_volume: point itself when it is inside a convex shape, else the
// nearest point on one; the entity's position when it has no convex shapes. nullopt when it is
// no 3D body or area.
[[nodiscard]] std::optional<Vec3> closest_point(world::World& world, entt::entity entity,
                                                Vec3 point);

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

// Where a soft body's mesh is, as Godot's SoftBody3D::get_point_transform gives each point:
// each mesh vertex's place in world space after the last step, and whether its body sleeps.
// nullopt when the entity has no soft body.
struct SoftBodyPoints {
    std::vector<Vec3> points;
    bool sleeping = false;
};
[[nodiscard]] std::optional<SoftBodyPoints> soft_body_points(world::World& world,
                                                             entt::entity entity);

// Tears out the triangles of a SoftBody3D's mesh whose middles are within radius meters of
// center, where physics last put them (else where the mesh rests), as a cut, a shot or a blast
// does; returns the triangles it tore (by number in the mesh) and emits "torn" with them. The
// body is made again without them at the next step, its points kept where they are.
std::vector<int> tear_soft_body(world::World& world, entt::entity entity, Vec3 center,
                                float radius);
// The same for a Cloth3D: its section's triangles (by number in its surfaces' index order)
// within radius meters of center, where physics last put them. A piece torn free of every point
// the skin holds still falls free.
std::vector<int> tear_cloth(world::World& world, entt::entity entity, Vec3 center, float radius);

// Whether an entity is a 3D body or area now, as the next step sees it.
[[nodiscard]] bool has_body(const world::World& world, entt::entity entity);

// Pushes a rigid body at the next physics step, as Godot's apply_impulse: impulse in
// kilogram-meters per second, at offset meters from its position.
void apply_impulse(world::World& world, entt::entity entity, Vec3 impulse, Vec3 offset = {});
// Turns a rigid body at the next physics step, as Godot's apply_torque_impulse.
void apply_torque_impulse(world::World& world, entt::entity entity, Vec3 impulse);

// Breaks a Destructible3D into its pieces now, as running out of health does: the entity goes,
// and a rigid body per piece of its mesh's fracture takes its place (FracturePiece3D), each
// moving as that part of the body moved, sharing its mass by volume (by its profile's density,
// when it has one and isn't a rigid body), and emitting a "broken" event with the pieces' IDs.
// A profile cuts it by its pattern where `from` is (world space), gives each piece health and
// a Destructible3D of its own while under its levels, and a connected one that stood leaves
// its pieces standing, frozen and linked, but those near the hit. push moves loose pieces away
// from `from` (the body's center when nullopt) at that many meters per second more.
// Fails, changing nothing, for an entity with no Destructible3D or body, or a mesh it can't cut.
[[nodiscard]] Result<std::vector<entt::entity>> break_body(world::World& world, entt::entity entity,
                                                           float push = 0.0F,
                                                           std::optional<Vec3> from = std::nullopt);

// What damage did to a Destructible3D.
struct DamageReport {
    std::string id;
    float dealt = 0.0F;  // health taken, after its profile's resistance
    float health = 0.0F; // left
    int state = 0;       // the damage state it is in
    bool broke = false;  // whether it broke, into pieces
    std::vector<std::string> pieces;
    bool fell = false; // whether a standing piece that breaks no more came loose
    // What it did to its shape, short of breaking it: "dent", "hole", "carve", "crater" or "".
    std::string deformed;
    bool killed = false;              // someone or something living died of it
    bool wrecked = false;             // a vehicle was wrecked by it
    bool queued = false;              // it breaks in a later tick, past this one's break budget
    std::vector<std::string> effects; // what the hit or break left (debris, dust, decal, sound)
};

// Damages a Destructible3D by amount of a kind ("blunt", "pierce", "cut", "explosive", "fire",
// "impact"), as its profile resists it, at a point in the world: its damage state changes its
// looks, and at no health left it breaks where it was hit (break_body), or comes loose when it
// is a standing piece that breaks no more. Emits "damaged" with what it took (and by, the ID of
// whoever dealt it, when given). A hit at a point leaves the profile's hit effects, a break its
// break effects (each an entity with a DamageEffect). On someone or something living (a
// Ragdoll3D, Creature3D, Flesh3D or NpcAgent, or a character's or creature's recipe) a hit
// strikes its ragdoll's nearest bone, and no health left kills it instead of breaking it: its
// ragdoll goes limp for good, a creature stops, and "killed" is emitted. A Vehicle3D is wrecked
// instead: it drives no more, and "wrecked" is emitted. With no profile, damage at least its
// damage_threshold breaks it. push is the speed loose pieces fly from the hit at; 0 takes the
// profile's.
[[nodiscard]] Result<DamageReport> damage_body(world::World& world, entt::entity entity,
                                               float amount, std::string_view kind,
                                               std::optional<Vec3> at, float push = 0.0F,
                                               std::optional<Vec3> direction = std::nullopt,
                                               std::string_view by = {});
// What damage dealt of a kind does to a body's shape short of breaking it, by its profile's
// deform: explosive damage carves a sphere out of a profile that carves (and digs a crater into
// an Terrain3D, the entity's Destructible3D naming a soil profile), pierce damage holes it through
// along direction (from at toward its middle when nullopt), and blunt, impact and explosive
// damage dent a profile that dents, deepening a dent near the last. at is in the world, and the
// dent or hole starts at the nearest point of its bounds. A carved or holed body's own
// Collider3D becomes what is left (a static body's triangles, a rigid body's hull).
// damage_body calls it while health is left. Returns "dent", "hole", "carve", "crater" or "".
std::string deform_body(world::World& world, entt::entity entity,
                        const world::DamageProfile& profile, float dealt, std::string_view kind,
                        std::optional<Vec3> at, std::optional<Vec3> direction = std::nullopt);
// Damages every Destructible3D within radius of center, as an explosion: each by amount less
// the share of radius it is away, nearest the center its pieces' middles count from.
[[nodiscard]] std::vector<DamageReport> damage_area(world::World& world, Vec3 center, float radius,
                                                    float amount, std::string_view kind,
                                                    float push = 0.0F, std::string_view by = {});
// Works out the load through the links of a group of standing pieces now (settle_stress),
// breaking those that give way and setting loose what no link holds to an anchored piece, and
// returns those. Physics does it after each step for groups that changed, emitting "collapsed".
std::vector<std::string> settle_group(world::World& world, std::string_view group);

// The convex hull of points as triangles, three points each, wound clockwise as seen from
// outside, as Godot's faces are: what a convex_polygon shape bakes into a navigation mesh as.
// Empty when the points don't span a volume.
[[nodiscard]] std::vector<Vec3> convex_hull_triangles(std::span<const Vec3> points);

} // namespace slash::physics3d
