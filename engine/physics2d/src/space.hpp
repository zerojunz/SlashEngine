#pragma once

#include "slash/core/assert.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <box2d/box2d.h>
#include <cmath>
#include <cstdint>
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

    [[nodiscard]] auto begin() { return entries_.begin(); }
    [[nodiscard]] auto end() { return entries_.end(); }
    [[nodiscard]] auto begin() const { return entries_.begin(); }
    [[nodiscard]] auto end() const { return entries_.end(); }

private:
    std::vector<Entry> entries_;
    std::unordered_map<entt::entity, std::size_t> index_;
};

inline b2Vec2 to_b2(Vec2 v) {
    return {v.x, v.y};
}

inline Vec2 from_b2(b2Vec2 v) {
    return {v.x, v.y};
}

// Places a point of a shape, given in the shape's own space, in the body's: moved and turned by
// the shape's position and rotation, then scaled by the entity's scale.
struct ShapePlacement {
    b2Rot rotation = b2Rot_identity;
    Vec2 position;
    Vec2 scale{1.0F, 1.0F};

    [[nodiscard]] b2Vec2 point(Vec2 local) const {
        const b2Vec2 turned = b2Add(to_b2(position), b2RotateVector(rotation, to_b2(local)));
        return {turned.x * scale.x, turned.y * scale.y};
    }
    // Radii scale by the larger axis.
    [[nodiscard]] float radius(float value) const {
        return value * std::max(std::abs(scale.x), std::abs(scale.y));
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
};

// What a body's shapes are made from; a change rebuilds them.
struct ShapeKey {
    std::optional<world::CollisionShape2D> shape;
    std::optional<world::CollisionPolygon2D> polygon;
    Vec2 scale{1.0F, 1.0F};
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    bool monitoring = true;
    bool monitorable = true;

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

    friend bool operator==(const RigidKey&, const RigidKey&) = default;
};

struct Body {
    entt::entity entity = entt::null;
    std::string name; // the entity's ID
    Kind kind = Kind::Static;
    b2BodyId id = b2_nullBodyId;
    // [0] for CollisionShape2D, [1] for CollisionPolygon2D.
    std::array<ShapeInfo, 2> info{};
    std::vector<b2ShapeId> shapes;
    ShapeKey shape_key;
    RigidKey rigid_key;
    // What the components said after the last sync or step, to tell their edits apart.
    Vec2 position;
    float rotation = 0.0F;
    Vec2 linear_velocity;
    float angular_velocity = 0.0F;
    bool sleeping = false;
    // Characters: what the last move slid along.
    std::vector<entt::entity> touching;
    // Characters: the body the last move stood on or slid along, for its platform velocity.
    entt::entity platform = entt::null;
    b2BodyId platform_body = b2_nullBodyId;
};

using JointKey = std::variant<world::PinJoint2D, world::DampedSpringJoint2D, world::GrooveJoint2D>;

struct Joint {
    b2JointId id = b2_nullJointId;
    JointKey key;
    Vec2 position;
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
    [[nodiscard]] const Body* body(entt::entity entity) const;

    void queue(const Impulse& impulse) { impulses_.push_back(impulse); }

    [[nodiscard]] Contacts contacts(const world::World& world, entt::entity entity) const;

private:
    void create_world();
    void remove_stale(world::World& world);
    void sync_body(world::World& world, entt::entity entity, Kind kind);
    void build_shapes(world::World& world, Body& body, const ShapeKey& key);
    void sync_joints(world::World& world);
    void read_back(world::World& world);
    void move_characters(world::World& world, float seconds);
    void emit_events(world::World& world);
    void destroy(Body& body);

    Settings settings_;
    b2WorldId world_ = b2_nullWorldId;
    b2BodyId ground_ = b2_nullBodyId; // what joints with no node_a hold on to
    EntityMap<std::unique_ptr<Body>> bodies_;
    EntityMap<Joint> joints_;
    std::vector<Impulse> impulses_;
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
void move_character(world::World& world, b2WorldId space, Body& body, world::Transform2D& transform,
                    world::CharacterBody2D& character, float seconds);

} // namespace slash::physics2d::detail
