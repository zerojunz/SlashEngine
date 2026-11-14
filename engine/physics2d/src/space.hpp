#pragma once

#include "slash/core/assert.hpp"
#include "slash/core/jobs.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/world/cloth2d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/hair.hpp"
#include "slash/world/physics_debug.hpp"
#include "slash/world/physics_material.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <array>
#include <box2d/box2d.h>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// The Box2D world behind a slash world, kept in step with its physics components.
namespace slash::physics2d::detail {

// Box2D's tolerances are tuned for objects about a meter across; a character 64 pixels tall
// plays that part here, so all lengths go to Box2D in pixels.
inline constexpr float kPixelsPerMeter = 64.0F;
// Box2D's linear slop at that scale: contacts closer than this count as touching.
inline constexpr float kLinearSlop = 0.005F * kPixelsPerMeter;

// Every shape's category has this bit, so Box2D's own filter passes every pair to ours, which
// follows Godot's rule; the low 32 bits are the shape's collision layer.
inline constexpr std::uint64_t kShapeBit = std::uint64_t{1} << 32U;
// Every query's category; a query's mask is the layers it looks in.
inline constexpr std::uint64_t kQueryBit = std::uint64_t{1} << 33U;

enum class Kind : std::uint8_t { Rigid, Character, Animatable, Static, Area };

// Values by entity, kept in the order they were added. Box2D's results depend on the order of
// the calls made to it, and the same entities have other handles in another session (EnTT
// reuses them after a clear), so the space goes through its bodies and joints in this order,
// never by handle: the same world then steps the same in every session.
template <typename T>
class EntityMap {
public:
    using Entry = std::pair<entt::entity, T>;

    [[nodiscard]] T* find(entt::entity entity) {
        const auto found = index_.find(entity);
        return found != index_.end() ? &entries_[found->second].second : nullptr;
    }
    [[nodiscard]] const T* find(entt::entity entity) const {
        const auto found = index_.find(entity);
        return found != index_.end() ? &entries_[found->second].second : nullptr;
    }
    T& emplace(entt::entity entity, T value) {
        SLASH_ASSERT(!index_.contains(entity), "an entity is in the map once");
        index_.emplace(entity, entries_.size());
        return entries_.emplace_back(entity, std::move(value)).second;
    }
    // Removes the entries remove(entity, value) picks, asking in order.
    template <typename Remove>
    void erase_if(Remove remove) {
        std::size_t kept = 0;
        for (Entry& entry : entries_) {
            if (!remove(entry.first, entry.second)) {
                if (&entries_[kept] != &entry) {
                    entries_[kept] = std::move(entry);
                }
                ++kept;
            }
        }
        entries_.resize(kept);
        index_.clear();
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            index_.emplace(entries_[i].first, i);
        }
    }
    void erase(entt::entity entity) {
        erase_if([entity](entt::entity other, const T& /*value*/) { return other == entity; });
    }
    void clear() {
        entries_.clear();
        index_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] auto begin() { return entries_.begin(); }
    [[nodiscard]] auto end() { return entries_.end(); }
    [[nodiscard]] auto begin() const { return entries_.begin(); }
    [[nodiscard]] auto end() const { return entries_.end(); }

private:
    std::vector<Entry> entries_;
    std::unordered_map<entt::entity, std::size_t> index_;
};

inline b2Vec2 to_b2(Vec2 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y)};
}

inline Vec2 from_b2(b2Vec2 v) {
    return {v.x, v.y};
}

// Places a point of a shape, given in the shape's own space, in the body's: moved and turned by
// the shape's position and rotation, placed by the child entity that holds it if it isn't the
// body's own, then scaled by the body's scale.
struct ShapePlacement {
    b2Rot rotation = b2Rot_identity;
    Vec2 position;
    Vec2 scale{1.0F, 1.0F};
    std::optional<world::Affine2D> owner; // a child entity's transform in the body's space

    [[nodiscard]] b2Vec2 point(Vec2 local) const {
        b2Vec2 turned = b2Add(to_b2(position), b2RotateVector(rotation, to_b2(local)));
        if (owner) {
            turned = to_b2(owner->apply(from_b2(turned)));
        }
        return {static_cast<float>(turned.x * scale.x), static_cast<float>(turned.y * scale.y)};
    }
    // Radii scale by the larger axis.
    [[nodiscard]] float radius(float value) const {
        float factor = std::max(std::abs(scale.x), std::abs(scale.y));
        if (owner) {
            const auto length = [](Vec2 v) { return std::sqrt((v.x * v.x) + (v.y * v.y)); };
            factor *= std::max(length(owner->x), length(owner->y));
        }
        return value * factor;
    }
    // The shape's up, for one-way collision, in the body's frame.
    [[nodiscard]] b2Vec2 up() const {
        b2Vec2 turned = b2RotateVector(rotation, {0.0F, -1.0F});
        if (owner) {
            const Vec2 placed{(owner->x.x * turned.x) + (owner->y.x * turned.y),
                              (owner->x.y * turned.x) + (owner->y.y * turned.y)};
            turned = b2Normalize(to_b2(placed));
        }
        return turned;
    }
};

// What each Box2D shape's user data points to.
struct ShapeInfo {
    entt::entity entity = entt::null;
    Kind kind = Kind::Static;
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    bool monitorable = true; // areas
    bool one_way = false;
    float one_way_margin = 1.0F;
    b2Vec2 one_way_up{0.0F, -1.0F}; // in the body's frame
    std::optional<Vec2i> cell{};    // NOLINT(readability-redundant-member-init): a tile's cell
};

// An entity's shapes in a body: the body's own, or a child entity's with no body of its own.
struct ShapeSource {
    entt::entity entity = entt::null;
    std::optional<world::CollisionShape2D> shape;
    std::optional<world::CollisionPolygon2D> polygon;
    std::optional<world::Affine2D> owner; // the child's transform; none for the body's own

    friend bool operator==(const ShapeSource&, const ShapeSource&) = default;
};

// What a body's shapes are made from; a change rebuilds them.
struct ShapeKey {
    std::vector<ShapeSource> sources; // the body's own first, then its children's in order
    Vec2 scale{1.0F, 1.0F};
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    bool monitoring = true;
    bool monitorable = true;
    bool overrides = false; // an area that changes gravity or damping
    bool hits = false;      // a Destructible2D, whose shapes report how hard they are hit

    friend bool operator==(const ShapeKey&, const ShapeKey&) = default;
};

// How a rigid body's Box2D body is set up; a change updates it.
struct RigidKey {
    b2BodyType type = b2_dynamicBody;
    float mass = 1.0F;
    float inertia = 0.0F;
    float gravity_scale = 1.0F;
    float linear_damping = 0.0F;
    float angular_damping = 0.0F;
    bool fixed_rotation = false;
    bool bullet = false;
    bool sleep_enabled = true;
    // Kilograms a square meter from the body's physics material; above 0 it sets the mass.
    float density = 0.0F;
    // Godot's custom center of mass, in pixels in the body's space.
    bool custom_center = false;
    Vec2 center;

    friend bool operator==(const RigidKey&, const RigidKey&) = default;
};

// How a rigid body's Box2D body falls and slows, as last set: by its RigidKey, or by the areas it
// is in, which pull it with a force instead of Box2D's gravity.
struct Environment {
    float gravity_scale = 1.0F;
    float linear_damping = 0.0F;
    float angular_damping = 0.0F;
    Vec2 pull; // pixels per second squared, applied as a force; areas only

    friend bool operator==(const Environment&, const Environment&) = default;
};

// The gravity and damping at a body, before its own gravity scale and damping.
struct AreaForces {
    Vec2 gravity;
    float linear_damp = 0.0F;
    float angular_damp = 0.0F;
    bool gravity_overridden = false;   // an area set the gravity, so Box2D's doesn't apply
    std::vector<entt::entity> areas{}; // NOLINT(readability-redundant-member-init): in order
};

struct Body {
    entt::entity entity = entt::null;
    std::string name; // the entity's ID
    Kind kind = Kind::Static;
    b2BodyId id = b2_nullBodyId;
    // Two for each of shape_key's sources: its CollisionShape2D, then its CollisionPolygon2D.
    // Box2D's shapes point into it, so it is sized before they are made.
    std::vector<ShapeInfo> info;
    std::vector<b2ShapeId> shapes;
    ShapeKey shape_key;
    RigidKey rigid_key;
    Environment environment; // rigid bodies
    // Static and animatable bodies: their constant velocities, which make a static body
    // kinematic in Box2D.
    Vec2 constant_linear_velocity;
    float constant_angular_velocity = 0.0F;
    // What the components said after the last sync or step, to tell their edits apart.
    Vec2 position;
    float rotation = 0.0F;
    Vec2 linear_velocity;
    float angular_velocity = 0.0F;
    bool sleeping = false;
    // Characters: what the last move slid along.
    std::vector<entt::entity> touching;
    // Characters: the body the last move stood on or slid along, for its platform velocity, and
    // its collision layer.
    entt::entity platform = entt::null;
    b2BodyId platform_body = b2_nullBodyId;
    std::uint32_t platform_layer = 0;
    // Tile map layers: the shapes the body was built from.
    TileShapes tiles;
    // The surface its physics material names.
    std::string surface;
    // Rigid bodies an anchor field holds as kinematic ones, and their mass to go back to.
    bool anchored = false;
    b2MassData anchored_mass{};
};

using JointKey = std::variant<world::PinJoint2D, world::DampedSpringJoint2D, world::GrooveJoint2D>;

// A joint, made where its entity was then. As Godot's, it stays there when a parent carries the
// entity away; moving the entity's own Transform2D makes it again.
struct Joint {
    b2JointId id = b2_nullJointId;
    JointKey key;
    Vec2 position; // the entity's own Transform2D when the joint was made
    float rotation = 0.0F;
    b2BodyId body_a = b2_nullBodyId;
    b2BodyId body_b = b2_nullBodyId;
};

struct Impulse {
    entt::entity entity = entt::null;
    Vec2 impulse;
    Vec2 offset;
    float torque = 0.0F;
};

// Box2D's tasks, run on a world's job system (slash/core/jobs.hpp): Box2D hands each task's
// items to enqueue_task and waits for them with finish_task.
struct Tasks {
    jobs::Pool* pool = nullptr;
    // The groups of a step in flight. Box2D keeps pointers to them, which a deque leaves valid.
    std::deque<jobs::GroupId> groups;
};

// A Cloth2D's sheet as the space steps it (cloth.cpp).
struct Cloth2DBody {
    entt::entity entity{entt::null};
    std::string name;
    int columns = 0;
    int rows = 0;
    std::vector<Vec2> velocity;
    std::vector<Vec2> targets; // where the transform put the points at the last step
    std::vector<std::array<int, 2>> edges;
    std::vector<std::array<int, 2>> bends;
    std::vector<std::array<int, 4>> bend_sides; // the edges each bend spans, -1 for none
    world::ClothPoints2D points;
};

// A Hair2D's guide strands as the space steps them (hair.cpp), grown from key.
struct Hair2DBody {
    std::string name;
    std::string key;
    world::Strands strands;
    world::HairStrands out;
};

class Space {
public:
    explicit Space(const Settings& settings);
    ~Space();

    Space(const Space&) = delete;
    Space& operator=(const Space&) = delete;
    Space(Space&&) = delete;
    Space& operator=(Space&&) = delete;

    void set_settings(const Settings& settings);
    // Forgets the Box2D world, its bodies and contacts; the next sync builds them again from the
    // components, as in a new world.
    void reset();

    // Makes the Box2D world match the components: queries call it so they see edits made since
    // the last step.
    void sync(world::World& world);
    // One physics tick: sync, step Box2D, write rigid bodies back, move characters and emit
    // contact and area events.
    void step(world::World& world, float seconds);

    // The Box2D world, or a null ID while no entity has a body.
    [[nodiscard]] b2WorldId id() const noexcept { return world_; }
    // A body's or a tile map layer's, or nullptr.
    [[nodiscard]] const Body* body(entt::entity entity) const;
    // Where a Cloth2D's points are after the last step and the one before, or nullptr.
    [[nodiscard]] const world::ClothPoints2D* cloth_points(entt::entity entity) const;
    // Where a Hair2D's guides are after the last step and the one before, or nullptr.
    [[nodiscard]] const world::HairStrands* hair_strands(entt::entity entity) const;

    void queue(const Impulse& impulse) { impulses_.push_back(impulse); }

    // The hardest hit on each Destructible2D in the last step (destruction.cpp), taken by the
    // destruction pass after it: kilogram-pixels per second, and where, in the world.
    struct Impact {
        float impulse = 0.0F;
        Vec2 point;
    };
    [[nodiscard]] std::unordered_map<entt::entity, Impact> take_impacts() {
        return std::exchange(impacts_, {});
    }
    [[nodiscard]] const Settings& settings() const { return settings_; }

    [[nodiscard]] Contacts contacts(const world::World& world, entt::entity entity) const;

    // Box2D's allocations and what its world holds, for World::memory.
    [[nodiscard]] world::MemoryPart memory() const;
    // The gravity and damping at a body from the override areas it is in and the project's
    // settings, as Godot's GodotBody2D::integrate_forces.
    [[nodiscard]] AreaForces forces(const world::World& world, const world::Tree2D& tree,
                                    entt::entity entity) const;
    // The .physics_material.json file at a path, read through the world's assets and kept until
    // the file changes; nullopt, with a warning once, for one that can't be read or is invalid.
    std::optional<world::PhysicsMaterial> material(const world::World& world,
                                                   const std::string& path);

private:
    void create_world(jobs::Pool& pool);
    void remove_stale(world::World& world);
    void sync(world::World& world, const world::Tree2D& tree);
    void sync_body(world::World& world, const world::Tree2D& tree, entt::entity entity, Kind kind);
    void build_shapes(world::World& world, Body& body, const ShapeKey& key);
    void sync_joints(world::World& world, const world::Tree2D& tree);
    void sync_tiles(world::World& world, const world::Tree2D& tree);
    void build_tile_shapes(world::World& world, Body& body, TileShapes tiles, Vec2 scale);
    void read_back(world::World& world, const world::Tree2D& tree);
    void move_characters(world::World& world, const world::Tree2D& tree, float seconds);
    // The areas that change each body's gravity or damping (Godot's area pairs): those whose
    // shapes overlap the body's and whose mask has its layer, highest priority first, then in
    // the order the areas were made.
    using AreaLists = std::unordered_map<entt::entity, std::vector<const Body*>>;
    [[nodiscard]] AreaLists area_lists(const world::World& world) const;
    [[nodiscard]] AreaForces forces(const world::World& world, const world::Tree2D& tree,
                                    const Body& body, const std::vector<const Body*>* areas) const;
    // Sets each rigid body's gravity and damping from the areas it is in, before a step.
    void apply_areas(world::World& world, const world::Tree2D& tree);
    void emit_events(world::World& world);
    void destroy(Body& body);
    // Pushes, sets the velocity of, holds, removes and puts to sleep rigid bodies in the world's
    // PhysicsField2Ds' reach, before a step (fields.cpp).
    void apply_fields(world::World& world, const world::Tree2D& tree, float seconds);
    // Destroys the entities kill fields took, after a step, emitting field_killed.
    void destroy_doomed(world::World& world);
    // While physics.debug records (debug.cpp): the step's contacts, joints and bodies.
    void finish_debug(const world::World& world, world::PhysicsDebugStep& step) const;
    // Steps every Cloth2D's sheet (cloth.cpp), after the bodies moved.
    void step_cloths(world::World& world, const world::Tree2D& tree, float seconds);
    // Steps every Hair2D's guides (hair.cpp), after the cloths.
    void step_hair_2d(world::World& world, float seconds);
    // Reads the step's hit events into impacts_ (destruction.cpp).
    void collect_hits();

    struct CachedMaterial {
        std::uint64_t version = 0;
        std::optional<world::PhysicsMaterial> material;
    };
    std::map<std::string, CachedMaterial, std::less<>> materials_;
    std::vector<entt::entity> doomed_;
    // The step physics.debug is recording, during step().
    world::PhysicsDebugStep* debug_ = nullptr;

    Settings settings_;
    Tasks tasks_;
    b2WorldId world_ = b2_nullWorldId;
    b2BodyId ground_ = b2_nullBodyId; // what joints with no node_a hold on to
    EntityMap<std::unique_ptr<Body>> bodies_;
    EntityMap<std::unique_ptr<Body>> tiles_; // tile map layers' static bodies
    EntityMap<Joint> joints_;
    EntityMap<std::unique_ptr<Cloth2DBody>> cloths_;
    EntityMap<std::unique_ptr<Hair2DBody>> hairs_;
    std::vector<Impulse> impulses_;
    std::unordered_map<entt::entity, Impact> impacts_;
    // Who touched whom after the last step, by entity ID, for entered and exited events:
    // (entity, other, other is an area).
    std::set<std::tuple<std::string, std::string, bool>> touching_;
    // IDs whose bodies were removed since the last step. An ID that is back belongs to a
    // replaced entity, as when a session restores a copy of the world: its contacts start over
    // without events, since Box2D's were lost.
    std::set<std::string, std::less<>> removed_;
};

inline const ShapeInfo* info_of(b2ShapeId shape) {
    return static_cast<const ShapeInfo*>(b2Shape_GetUserData(shape));
}

// The space installed in a world, or nullptr.
[[nodiscard]] Space* find_space(world::World& world);

// Moves a character one tick, as Godot's move_and_slide (character.cpp).
void move_character(b2WorldId space, Body& body, world::Transform2D& transform,
                    world::CharacterBody2D& character, float seconds);

// Whether an entity is a Vehicle2D's wheel ("<vehicle>_wheel_<n>"), which may spin faster than
// Box2D lets other bodies turn, as its allowFastRotation is for (vehicle_tool.cpp).
[[nodiscard]] bool is_vehicle_wheel(const world::World& world, entt::entity entity);

// Breaks the Destructible2Ds this step's impacts broke, settles linked groups and counts down
// loose pieces' lifetimes, after a step (destruction.cpp).
void break_and_age_2d(world::World& world, Space& space, float seconds);

} // namespace slash::physics2d::detail
