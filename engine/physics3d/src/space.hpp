#pragma once

#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/gridmap.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/world/components.hpp"
#include "slash/world/physical_bones.hpp"
#include "slash/world/tree3d.hpp"

// Jolt's headers, which Jolt.h must come before.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// The Jolt physics system behind a slash world, kept in step with its 3D physics components, as
// Godot's JoltSpace3D, JoltBody3D and JoltArea3D keep it in step with Godot's nodes.
namespace slash::physics3d::detail {

// Godot's CMP_EPSILON.
inline constexpr float kEpsilon = 0.00001F;

// Bone is a PhysicalBone3D: a rigid, kinematic or static body as its link says.
enum class Kind : std::uint8_t { Rigid, Character, Animatable, Static, Area, Bone };

// The broad phase layers: Godot's JoltBroadPhaseLayer, with its two static layers as one and
// areas in their own. Areas take no part in Jolt's step; the space finds what is in them itself.
namespace broad {
inline constexpr JPH::BroadPhaseLayer kStatic{0};
inline constexpr JPH::BroadPhaseLayer kMoving{1};
inline constexpr JPH::BroadPhaseLayer kArea{2};
inline constexpr JPH::uint kCount = 3;
} // namespace broad

// Jolt's object layers as Godot's JoltLayers makes them: an object layer is its broad phase layer
// in the top three bits and, below, the index of its collision layer and mask, so two objects
// collide as Godot's do, when either one's mask has a bit of the other's layer.
class Layers final : public JPH::BroadPhaseLayerInterface,
                     public JPH::ObjectVsBroadPhaseLayerFilter,
                     public JPH::ObjectLayerPairFilter {
public:
    Layers();

    [[nodiscard]] JPH::ObjectLayer to_object_layer(JPH::BroadPhaseLayer broad_phase_layer,
                                                   std::uint32_t layer, std::uint32_t mask);
    // The broad phase layer, collision layer and mask of an object layer.
    [[nodiscard]] std::tuple<JPH::BroadPhaseLayer, std::uint32_t, std::uint32_t>
    from_object_layer(JPH::ObjectLayer object_layer) const;

    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return broad::kCount; }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override;
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override;
#endif
    // Which broad phase layers an object's pairs are looked for in.
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer,
                                     JPH::BroadPhaseLayer broad_phase_layer) const override;
    // Whether two objects collide.
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override;

private:
    std::vector<std::uint64_t> collisions_; // layer << 32 | mask, by index
    std::unordered_map<std::uint64_t, JPH::ObjectLayer> by_collision_;
};

// Values by entity, kept in the order they were added. Jolt's results depend on the order bodies
// are made and changed in, and the same entities have other handles in another session, so the
// space goes through its bodies in this order, never by handle.
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

inline JPH::Vec3 to_jolt(Vec3 v) {
    return {v.x, v.y, v.z};
}

inline Vec3 from_jolt(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

inline float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] float length(Vec3 v);
// v scaled to length 1, or zero for zero, as Godot's Vector3::normalized.
[[nodiscard]] Vec3 normalized(Vec3 v);
// Godot's Vector3::slide: v without its part along the unit vector n.
[[nodiscard]] Vec3 slide(Vec3 v, Vec3 n);
// Godot's Vector3::is_zero_approx.
[[nodiscard]] bool zero_approx(Vec3 v);
// acos, as Godot's Math::acos, the same bits on every platform; its argument is clamped to -1 to 1.
[[nodiscard]] float acos(float x);

// A transform without its scale: its basis orthonormalized as Godot's Basis::orthonormalize
// does, and the scale that came off, as Godot's Basis::get_scale.
struct Unscaled {
    world::Affine3D transform; // orthonormal basis
    Vec3 scale{1.0F, 1.0F, 1.0F};
};
[[nodiscard]] Unscaled unscale(const world::Affine3D& transform);
// An orthonormal basis as a unit quaternion.
[[nodiscard]] JPH::Quat rotation_of(const world::Affine3D& basis);
[[nodiscard]] world::Affine3D affine_of(JPH::RVec3Arg position, JPH::QuatArg rotation);
[[nodiscard]] JPH::Mat44 matrix_of(const world::Affine3D& transform);

// A CollisionShape3D's Jolt shape, unscaled and in its own space, as Godot's JoltShape3D
// subclasses build them; an error for a shape that can't be made, such as a hull of too few
// points.
struct BuiltShape {
    JPH::RefConst<JPH::Shape> shape;
    std::string error;
};
[[nodiscard]] BuiltShape build_shape(const world::CollisionShape3D& shape);
// An HTerrain's heights as a Jolt height field in its entity's space, as Godot's
// JoltHeightMapShape3D builds a height map shape (mirrored along z, so Jolt splits each quad as
// Godot does): cell (x, z) at map_scale * (x, height, z), moved back half the terrain when
// centered. Holes don't collide.
[[nodiscard]] BuiltShape build_terrain_shape(const scene3d::TerrainData& data, Vec3 map_scale,
                                             bool centered);
// The same scaled, made valid for the shape as Godot's JOLT_ENSURE_SCALE_VALID does (a sphere
// scales the same along every axis).
[[nodiscard]] JPH::RefConst<JPH::Shape> scaled(const JPH::Shape* shape, Vec3 scale);

// One shape of a body: the CollisionShape3D of the body's entity or of an entity under it,
// placed in the body's scaled space, as Godot's JoltShapeInstance3D.
struct Part {
    entt::entity entity = entt::null; // whose CollisionShape3D it is
    world::CollisionShape3D shape;
    world::Affine3D local; // the shape's transform in the body's scaled space, scale and all

    friend bool operator==(const Part&, const Part&) = default;
};

// What a body's shape is made from; a change rebuilds it.
struct ShapeKey {
    std::vector<Part> parts; // the body's own first, then those under it in tree order
    Vec3 scale{1.0F, 1.0F, 1.0F};
    // A CSG root's triangles, as scene3d keeps them: a new mesh is a new pointer.
    std::shared_ptr<const std::vector<Vec3>> csg;
    // A GridMap's cells' shapes, the same way.
    std::shared_ptr<const std::vector<scene3d::GridShape>> grid;
    // An HTerrain's heights, the same way, and how they are placed.
    std::shared_ptr<const scene3d::TerrainData> terrain;
    Vec3 terrain_scale{1.0F, 1.0F, 1.0F};
    bool terrain_centered = false;

    friend bool operator==(const ShapeKey&, const ShapeKey&) = default;
};

// How a rigid body's Jolt body is set up; a change updates it.
struct RigidKey {
    JPH::EMotionType motion = JPH::EMotionType::Dynamic;
    float mass = 1.0F;
    Vec3 inertia;
    JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
    bool ccd = false;
    bool can_sleep = true;

    friend bool operator==(const RigidKey&, const RigidKey&) = default;
};

// The gravity and damping at a body, before its own gravity scale and damping.
struct AreaForces {
    Vec3 gravity;
    float linear_damp = 0.0F;
    float angular_damp = 0.0F;
    std::vector<entt::entity> areas{}; // NOLINT(readability-redundant-member-init): in order
};

struct Body {
    entt::entity entity = entt::null;
    std::string name; // the entity's ID
    Kind kind = Kind::Static;
    // A CSG root with use_collision, a static body of its triangles as Godot's CSGShape3D's, a
    // GridMap, a static body of its cells' shapes as its octants' bodies, or an HTerrain, a
    // static height field as its collider: what scene3d makes.
    bool generated = false;
    // Physical bones: what their simulator makes of them.
    world::PhysicalBoneMode bone_mode = world::PhysicalBoneMode::Static;
    JPH::BodyID id;
    JPH::RefConst<JPH::Shape> shape; // the body's whole shape, as Jolt has it
    // Each built part's own shape, unscaled, with its index in shape_key.parts: motion tests
    // cast the convex ones one by one, as Godot's do.
    std::vector<std::pair<std::size_t, JPH::RefConst<JPH::Shape>>> parts;
    // A GridMap with one shape: its index in shape_key.grid. With more, each is a sub-shape of
    // the compound whose user data is its index.
    std::optional<std::size_t> grid_part;
    ShapeKey shape_key;
    RigidKey rigid_key;
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    // Static and animatable bodies: their constant velocities, which carry what touches them as
    // Godot's surface velocities do.
    Vec3 constant_linear_velocity;
    Vec3 constant_angular_velocity;
    // Where the components put it after the last sync or step, scale and all, to tell their
    // edits apart.
    world::Affine3D placed;
    // Kinematic bodies (animatable ones that sync to physics, characters and frozen kinematic
    // rigid bodies): where the next step moves them, unscaled, as Godot's kinematic_transform.
    std::optional<world::Affine3D> target;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    bool sleeping = false;
    // Rigid bodies and physical bones: their gravity and total damping, set before each step
    // from the areas they are in, as Godot's JoltBody3D.
    Vec3 gravity;
    float total_linear_damp = 0.0F;
    float total_angular_damp = 0.0F;
    // Rigid bodies with contact_monitor: the bodies touching them in the last step they were
    // awake for.
    std::set<entt::entity> contacts;
    // Characters: what the last move slid along, and the body it stood on or slid along, for
    // its platform velocity, and that body's collision layer.
    std::vector<entt::entity> touching;
    entt::entity platform = entt::null;
    std::uint32_t platform_layer = 0;
    // Areas: what is in them, by entity, after the last step, and whether each is an area.
    std::vector<std::pair<entt::entity, bool>> overlaps;
};

struct Impulse {
    entt::entity entity = entt::null;
    Vec3 impulse;
    Vec3 offset;
    Vec3 torque;
};

// One collision of a motion test, as Godot's PhysicsServer3D::MotionCollision.
struct MotionCollision {
    Vec3 position;
    Vec3 normal;
    Vec3 collider_velocity;
    Vec3 collider_angular_velocity;
    float depth = 0.0F;
    entt::entity collider = entt::null;
    std::uint32_t collider_layer = 0;
    bool collider_is_character = false;

    // Godot's MotionCollision::get_angle.
    [[nodiscard]] float angle(Vec3 up) const { return acos(dot(normal, up)); }
};

// A motion test's result, as Godot's PhysicsServer3D::MotionResult.
struct MotionResult {
    Vec3 travel;
    Vec3 remainder;
    float collision_depth = 0.0F;
    float collision_safe_fraction = 0.0F;
    float collision_unsafe_fraction = 0.0F;
    std::vector<MotionCollision> collisions;
};

// A motion test's parameters, as Godot's PhysicsServer3D::MotionParameters.
struct MotionParameters {
    world::Affine3D from; // where the body starts, scale and all
    Vec3 motion;
    float margin = 0.001F;
    int max_collisions = 1;
    bool recovery_as_collision = false;
    std::vector<entt::entity> exclude{}; // NOLINT(readability-redundant-member-init)
};

// The joint components, as a joint was made from one.
using JointKey = std::variant<world::PinJoint3D, world::HingeJoint3D, world::SliderJoint3D,
                              world::ConeTwistJoint3D, world::Generic6DOFJoint3D>;

// A joint's Jolt constraint, as Godot's JoltJoint3D: made where its entity was then, it stays
// there when a parent carries the entity away; moving the entity's own Transform3D or naming
// other bodies makes it again.
struct Joint {
    JPH::Ref<JPH::Constraint> constraint;
    JointKey key;
    world::Transform3D own; // the entity's own Transform3D when it was made
    entt::entity node_a = entt::null;
    entt::entity node_b = entt::null;
    // The bodies it joins as Godot's Joint3D passes them: body_a is always one, and body_b,
    // when null, is the world.
    entt::entity body_a = entt::null;
    entt::entity body_b = entt::null;
    JPH::BodyID id_a;
    JPH::BodyID id_b;
    // Godot's local_ref_a and local_ref_b: the joint's frame in each body's space, scale and
    // all, or in the world's for no body.
    world::Affine3D local_a;
    world::Affine3D local_b;
    // The bodies' centers of mass the constraint was built for; a new shape rebuilds it.
    Vec3 com_a;
    Vec3 com_b;
};

// A physical bone's joint to the bone above, as Godot's PhysicalBone3D::_reload_joint makes it:
// in the frames the two bodies were in when it was made, again when its type, joint_rotation,
// body offset or bodies change.
struct BoneJoint {
    Joint joint;
    world::PhysicalBone3D made; // the component when its frames were made
};

// What a wheel's Transform3D was made from, so the next step tells the physics' own writes from
// edits, as Godot's VehicleWheel3D keeps where it entered the tree.
struct Wheel {
    world::Affine3D mount; // where it is mounted in its vehicle's unscaled space, orthonormal
    Vec3 mount_scale{1.0F, 1.0F, 1.0F};
    world::Transform3D written;  // the Transform3D the last step gave it
    float delta_rotation = 0.0F; // how far it rolled in the last step, as Godot's m_deltaRotation
};

// A vehicle's engine force, brake and steering when they last went to its wheels.
struct VehicleInputs {
    float engine_force = 0.0F;
    float brake = 0.0F;
    float steering = 0.0F;
};

class Space;

// Contact callbacks during a step, as Godot's JoltContactListener3D: bodies a joint keeps apart
// don't touch, constant velocities of static and animatable bodies carry what touches them, and
// rigid bodies that monitor contacts note what touches them.
class ContactListener final : public JPH::ContactListener {
public:
    explicit ContactListener(Space& space) : space_(&space) {}

    JPH::ValidateResult OnContactValidate(const JPH::Body& a, const JPH::Body& b,
                                          JPH::RVec3Arg base_offset,
                                          const JPH::CollideShapeResult& result) override;

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override;
    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override;

private:
    void contact(const JPH::Body& a, const JPH::Body& b, JPH::ContactSettings& settings);

    Space* space_;
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
    // Forgets the Jolt system, its bodies and contacts; the next sync builds them again from the
    // components, as in a new world.
    void reset();

    // Makes the Jolt system match the components: queries call it so they see edits made since
    // the last step.
    void sync(world::World& world);
    // One physics tick: sync, apply gravity and damping, step Jolt, write rigid bodies back,
    // move characters, find what is in areas and emit contact and area events.
    void step(world::World& world, float seconds);

    // The Jolt system, or nullptr while no entity has a 3D body.
    [[nodiscard]] JPH::PhysicsSystem* system() const noexcept { return system_.get(); }
    [[nodiscard]] const Layers& layers() const noexcept { return layers_; }
    // A body's or area's, or nullptr.
    [[nodiscard]] const Body* body(entt::entity entity) const;
    [[nodiscard]] Body* body(entt::entity entity);
    // The body a Jolt body belongs to.
    [[nodiscard]] static Body* body_of(const JPH::Body& body) {
        return reinterpret_cast<Body*>(body.GetUserData()); // NOLINT
    }

    void queue(const Impulse& impulse) { impulses_.push_back(impulse); }

    [[nodiscard]] Contacts contacts(const world::World& world, entt::entity entity) const;
    // The gravity and damping at a body at position from the override areas it is in and the
    // project's settings, as Godot's JoltBody3D::_update_gravity and _update_damp.
    [[nodiscard]] AreaForces forces(const world::World& world, const Body& body,
                                    Vec3 position) const;

    // Godot's body_test_motion for a body (motion.cpp): recovers it from what it overlaps, casts
    // its convex shapes along the motion and collects what it touches where it stops.
    bool test_motion(const Body& body, const MotionParameters& parameters,
                     MotionResult& result) const;

    // A body's velocity at a point, its constant velocities included, as Godot's
    // JoltBody3D::get_velocity_at_position; nothing for a body Jolt no longer has.
    [[nodiscard]] std::optional<Vec3> velocity_at(const Body& body, Vec3 position) const;

    // The first body or area a ray hits, as Godot's intersect_ray without hitting from inside.
    [[nodiscard]] std::optional<RayHit> cast_ray(Vec3 from, Vec3 to, const QueryFilter& filter,
                                                 bool hit_back_faces) const;

    // The cell of a GridMap's body a sub-shape is in; nothing for other bodies.
    [[nodiscard]] static std::optional<Vec3i> grid_cell(const Body& body,
                                                        const JPH::SubShapeID& id);

    // Whether a joint keeps two bodies from colliding, as Godot's collision exceptions.
    [[nodiscard]] bool excepted(entt::entity a, entt::entity b) const;

    // Contact callbacks call these, on the stepping thread.
    void note_contact(Body& a, Body& b);

    // Jolt's allocations and what its system holds, for World::memory.
    [[nodiscard]] world::MemoryPart memory() const;

private:
    // A static body scene3d makes: a CSG root's triangles, a GridMap's cells' shapes or an
    // HTerrain's heights, with
    // their collision layer and mask and physics material.
    struct MadeShape {
        std::uint32_t layer = 1;
        std::uint32_t mask = 1;
        float friction = 1.0F;
        float bounce = 0.0F;
        std::shared_ptr<const std::vector<Vec3>> faces;
        std::shared_ptr<const std::vector<scene3d::GridShape>> grid;
        std::shared_ptr<const scene3d::TerrainData> terrain;
        Vec3 terrain_scale{1.0F, 1.0F, 1.0F};
        bool terrain_centered = false;
    };

    void create_system();
    // Removes bodies whose entities are gone or are another kind now, and the bodies scene3d
    // makes whose entities aren't in made.
    void remove_stale(world::World& world, const std::vector<entt::entity>& made);
    void sync(world::World& world, const world::Tree3D& tree);
    void sync_body(world::World& world, const world::Tree3D& tree, entt::entity entity, Kind kind,
                   const MadeShape* made = nullptr);
    void create_body(Body& body, const world::Affine3D& global, const world::World& world);
    void build_shape(const world::World& world, Body& body, const ShapeKey& key);
    void destroy(Body& body);
    // Wakes what rests in a box, which Jolt leaves asleep when a static body moves or changes.
    void wake_in(const JPH::AABox& box);
    void pre_step(const world::World& world, float seconds);
    void read_back(world::World& world, const world::Tree3D& tree);
    void move_characters(world::World& world, const world::Tree3D& tree, float seconds);
    void find_overlaps(const world::World& world);
    void emit_events(world::World& world);
    // Makes the Jolt constraints match the joint components and physical bones (joints.cpp).
    void sync_joints(world::World& world, const world::Tree3D& tree);
    void sync_bone_joints(world::World& world, const world::Tree3D& tree);
    // Takes the constraints off a body that is going; the next sync makes them again.
    void drop_joints(const JPH::BodyID& id);
    void remove_joint(Joint& joint);
    // Sets what joints take per tick, such as a hinge motor's torque from its impulse.
    void pre_step_joints(float seconds);
    // Notes where new or moved wheels are mounted and gives vehicles' wheels the inputs that
    // changed (vehicle.cpp).
    void pre_step_vehicles(world::World& world, const world::Tree3D& tree, float seconds);
    // Godot's VehicleBody3D::_body_state_changed for each awake vehicle after the step: casts
    // the wheels' rays, pushes the body with their suspension and friction, and rolls, steers
    // and lifts the wheels.
    void step_vehicles(world::World& world, const world::Tree3D& tree, float seconds);
    // The areas whose space overrides reach a body: those it is in that override something,
    // highest priority first, then in the order the areas were made.
    [[nodiscard]] std::vector<const Body*> override_areas(const world::World& world,
                                                          const Body& body) const;

    Settings settings_;
    Layers layers_;
    ContactListener listener_{*this};
    std::unique_ptr<JPH::TempAllocator> temp_;
    std::unique_ptr<JPH::JobSystem> jobs_;
    std::unique_ptr<JPH::PhysicsSystem> system_;
    EntityMap<std::unique_ptr<Body>> bodies_;
    EntityMap<Joint> joints_;
    EntityMap<BoneJoint> bone_joints_;
    // What each physical bone's simulator makes of it, as the last sync found.
    std::unordered_map<entt::entity, world::PhysicalBoneLink> bone_links_;
    // Pairs of bodies joints keep apart, the lower entity first.
    std::set<std::pair<entt::entity, entt::entity>> exceptions_;
    EntityMap<Wheel> wheels_;
    EntityMap<VehicleInputs> vehicles_;
    std::vector<Impulse> impulses_;
    int added_ = 0; // bodies added since the broad phase was last optimized
    bool warned_full_ = false;
    // Who touched whom after the last step, by entity ID, for entered and exited events:
    // (entity, other, other is an area).
    std::set<std::tuple<std::string, std::string, bool>> touching_;
    // IDs whose bodies were removed since the last step: a returning ID belongs to a replaced
    // entity, whose contacts start over without events.
    std::set<std::string, std::less<>> removed_;
};

// The space installed in a world, or nullptr.
[[nodiscard]] Space* find_space(world::World& world);
[[nodiscard]] const Space* find_space(const world::World& world);

// What a moving body's own component says of its gravity and damping: a RigidBody3D's or a
// PhysicalBone3D's; nothing for other kinds.
struct OwnForces {
    float gravity_scale = 1.0F;
    std::string_view linear_damp_mode;
    float linear_damp = 0.0F;
    std::string_view angular_damp_mode;
    float angular_damp = 0.0F;
    Vec3 constant_force;
    Vec3 constant_torque;
};
[[nodiscard]] std::optional<OwnForces> own_forces(const entt::registry& registry,
                                                  entt::entity entity, Kind kind);
// A body's total damping from the space's and its own, as Godot's _update_damp.
[[nodiscard]] float with_own(float total, std::string_view mode, float own);

// Sets Jolt up once per process: its allocator, factory and shape types.
void set_up_jolt();

// A body's mass properties, as Godot's JoltBody3D::_calculate_mass_properties: mass 0 or less
// takes the shape's, and each axis of inertia 0 or less the shape's scaled to the mass.
[[nodiscard]] JPH::MassProperties mass_properties(const JPH::Shape& shape, float mass,
                                                  Vec3 inertia);

// Moves a character one tick, as Godot's CharacterBody3D::move_and_slide (character.cpp):
// transform is its global transform, which it moves.
void move_character(const Space& space, Body& body, world::Affine3D& transform,
                    world::CharacterBody3D& character, float seconds);

} // namespace slash::physics3d::detail
