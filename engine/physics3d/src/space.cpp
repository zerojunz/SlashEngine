#include "space.hpp"

#include "slash/core/log.hpp"
#include "slash/core/profile.hpp"
#include "slash/scene3d/csg.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace slash::physics3d::detail {

namespace {

using world::AnimatableBody3D;
using world::Area3D;
using world::CharacterBody3D;
using world::CollisionShape3D;
using world::PhysicalBone3D;
using world::RigidBody3D;
using world::StaticBody3D;
using world::Transform3D;

// Godot's physics/jolt_physics_3d limits: bodies, body pairs and contact constraints.
constexpr JPH::uint kMaxBodies = 10240;
constexpr JPH::uint kMaxPairs = 65536;
constexpr JPH::uint kMaxContacts = 20480;
// Godot's max_linear_velocity and max_angular_velocity (2700 degrees per second).
constexpr float kMaxLinearVelocity = 500.0F;
constexpr float kMaxAngularVelocity = 47.1238898F;
// Jolt's scratch memory for a step; more comes from the heap.
constexpr JPH::uint kTempBytes = 4U * 1024U * 1024U;
// Godot optimizes the broad phase before a query once this many bodies were added since.
constexpr int kOptimizeAfter = 128;

// What Jolt has allocated, for every space in the process, with each block's size kept before
// it. Aligned blocks keep the malloc'd pointer too.
std::atomic<std::uint64_t> g_jolt_bytes{0};
constexpr std::size_t kHeader = 16; // keeps malloc's alignment for what follows

void* jolt_allocate(std::size_t size) {
    auto* raw = static_cast<unsigned char*>(std::malloc(size + kHeader)); // NOLINT
    if (raw == nullptr) {
        return nullptr;
    }
    std::memcpy(raw, &size, sizeof(size));
    g_jolt_bytes += size;
    return raw + kHeader; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

void jolt_free(void* block) {
    if (block == nullptr) {
        return;
    }
    auto* raw = static_cast<unsigned char*>(block) - kHeader; // NOLINT
    std::size_t size = 0;
    std::memcpy(&size, raw, sizeof(size));
    g_jolt_bytes -= size;
    std::free(raw); // NOLINT(cppcoreguidelines-no-malloc)
}

void* jolt_reallocate(void* block, std::size_t old_size, std::size_t new_size) {
    void* out = jolt_allocate(new_size);
    if (out != nullptr && block != nullptr) {
        std::memcpy(out, block, std::min(old_size, new_size));
    }
    jolt_free(block);
    return out;
}

void* jolt_aligned_allocate(std::size_t size, std::size_t alignment) {
    alignment = std::max(alignment, kHeader);
    auto* raw = static_cast<unsigned char*>(std::malloc(size + alignment + kHeader)); // NOLINT
    if (raw == nullptr) {
        return nullptr;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(raw) + kHeader; // NOLINT
    const std::uintptr_t aligned = (address + alignment - 1) & ~(alignment - 1);
    auto* block = reinterpret_cast<unsigned char*>(aligned); // NOLINT
    std::memcpy(block - kHeader, &raw, sizeof(raw));         // NOLINT
    std::memcpy(block - sizeof(size), &size, sizeof(size));  // NOLINT
    g_jolt_bytes += size;
    return block;
}

void jolt_aligned_free(void* block) {
    if (block == nullptr) {
        return;
    }
    auto* bytes = static_cast<unsigned char*>(block);
    unsigned char* raw = nullptr;
    std::size_t size = 0;
    std::memcpy(&raw, bytes - kHeader, sizeof(raw));        // NOLINT
    std::memcpy(&size, bytes - sizeof(size), sizeof(size)); // NOLINT
    g_jolt_bytes -= size;
    std::free(raw); // NOLINT(cppcoreguidelines-no-malloc)
}

// Jolt's messages go to the log.
void jolt_trace(const char* format, ...) {
    std::array<char, 512> text{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text.data(), text.size(), format, args); // NOLINT
    va_end(args);
    log::warn("Jolt Physics", {{"message", std::string(text.data())}});
}

// Godot's rules for two bodies' materials: the smaller friction, the sum of bounces up to 1.
float combine_friction(const JPH::Body& a, const JPH::SubShapeID& /*a_part*/, const JPH::Body& b,
                       const JPH::SubShapeID& /*b_part*/) {
    return std::abs(std::min(a.GetFriction(), b.GetFriction()));
}

float combine_bounce(const JPH::Body& a, const JPH::SubShapeID& /*a_part*/, const JPH::Body& b,
                     const JPH::SubShapeID& /*b_part*/) {
    return std::clamp(a.GetRestitution() + b.GetRestitution(), 0.0F, 1.0F);
}

// The kind of body an entity is, by Godot's node it stands for; the first wins if it has two.
std::optional<Kind> kind_of(const entt::registry& registry, entt::entity entity) {
    if (registry.all_of<RigidBody3D>(entity)) {
        return Kind::Rigid;
    }
    if (registry.all_of<CharacterBody3D>(entity)) {
        return Kind::Character;
    }
    if (registry.all_of<AnimatableBody3D>(entity)) {
        return Kind::Animatable;
    }
    if (registry.all_of<StaticBody3D>(entity)) {
        return Kind::Static;
    }
    if (registry.all_of<Area3D>(entity)) {
        return Kind::Area;
    }
    if (registry.all_of<PhysicalBone3D>(entity)) {
        return Kind::Bone;
    }
    return std::nullopt;
}

template <typename... Components>
bool has_any(const entt::registry& registry) {
    return (!registry.view<Components>().empty() || ...);
}

struct Fields {
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    Vec3 constant_linear_velocity;
    Vec3 constant_angular_velocity;
};

Fields fields_of(const entt::registry& registry, entt::entity entity, Kind kind) {
    switch (kind) {
    case Kind::Rigid: {
        const auto& body = registry.get<RigidBody3D>(entity);
        return {body.collision_layer, body.collision_mask, body.friction, body.bounce, {}, {}};
    }
    case Kind::Animatable: {
        const auto& body = registry.get<AnimatableBody3D>(entity);
        return {
            body.collision_layer, body.collision_mask,           body.friction,
            body.bounce,          body.constant_linear_velocity, body.constant_angular_velocity};
    }
    case Kind::Static: {
        const auto& body = registry.get<StaticBody3D>(entity);
        return {
            body.collision_layer, body.collision_mask,           body.friction,
            body.bounce,          body.constant_linear_velocity, body.constant_angular_velocity};
    }
    case Kind::Character: {
        const auto& body = registry.get<CharacterBody3D>(entity);
        return {body.collision_layer, body.collision_mask, 0.0F, 0.0F, {}, {}};
    }
    case Kind::Area: {
        const auto& area = registry.get<Area3D>(entity);
        return {area.collision_layer, area.collision_mask, 0.0F, 0.0F, {}, {}};
    }
    case Kind::Bone: {
        const auto& bone = registry.get<PhysicalBone3D>(entity);
        return {bone.collision_layer, bone.collision_mask, bone.friction, bone.bounce, {}, {}};
    }
    }
    return {};
}

// Whether an area changes the gravity or damping of the bodies in it.
bool overrides_space(const Area3D& area) {
    return area.gravity_space_override != "disabled" ||
           area.linear_damp_space_override != "disabled" ||
           area.angular_damp_space_override != "disabled";
}

// The scale an entity is drawn at: its own and its parents' Transform3D scales. Read from the
// components rather than the basis, so a body that turns keeps exactly the same scale.
Vec3 scale_of(const entt::registry& registry, const world::Tree3D& tree, entt::entity entity) {
    Vec3 scale{1.0F, 1.0F, 1.0F};
    for (std::size_t steps = 0; entity != entt::null && steps < 4096; ++steps) {
        const auto* transform = registry.try_get<Transform3D>(entity);
        if (transform == nullptr) {
            break;
        }
        scale = {scale.x * transform->scale.x, scale.y * transform->scale.y,
                 scale.z * transform->scale.z};
        entity = tree.transform_parent(entity);
    }
    return scale;
}

world::Affine3D shape_transform(const CollisionShape3D& shape) {
    Transform3D transform;
    transform.position = shape.position;
    transform.rotation = shape.rotation;
    return world::affine(transform);
}

ShapeKey shape_key_of(const entt::registry& registry, const world::Tree3D& tree,
                      entt::entity entity) {
    ShapeKey key;
    if (const auto* shape = registry.try_get<CollisionShape3D>(entity); shape != nullptr) {
        key.parts.push_back({.entity = entity, .shape = *shape, .local = shape_transform(*shape)});
    }
    // As a Godot CollisionObject3D takes the shapes of its CollisionShape3D children: child
    // entities with shapes and no body of their own, placed by their own transforms.
    if (const auto* transform = registry.try_get<Transform3D>(entity); transform != nullptr) {
        for (const entt::entity child : transform->children) {
            const auto* shape =
                registry.valid(child) ? registry.try_get<CollisionShape3D>(child) : nullptr;
            if (shape == nullptr || tree.transform_parent(child) != entity ||
                kind_of(registry, child).has_value()) {
                continue;
            }
            key.parts.push_back({.entity = child,
                                 .shape = *shape,
                                 .local = world::affine(registry.get<Transform3D>(child)) *
                                          shape_transform(*shape)});
        }
    }
    key.scale = scale_of(registry, tree, entity);
    return key;
}

JPH::EAllowedDOFs dofs_of(const RigidBody3D& rigid, JPH::EMotionType motion) {
    using JPH::EAllowedDOFs;
    if (motion == JPH::EMotionType::Static) {
        return EAllowedDOFs::All;
    }
    EAllowedDOFs dofs = EAllowedDOFs::All;
    const auto lock = [&](bool locked, EAllowedDOFs axis) {
        if (locked) {
            dofs = dofs & ~axis;
        }
    };
    lock(rigid.axis_lock_linear_x, EAllowedDOFs::TranslationX);
    lock(rigid.axis_lock_linear_y, EAllowedDOFs::TranslationY);
    lock(rigid.axis_lock_linear_z, EAllowedDOFs::TranslationZ);
    lock(rigid.axis_lock_angular_x || rigid.lock_rotation, EAllowedDOFs::RotationX);
    lock(rigid.axis_lock_angular_y || rigid.lock_rotation, EAllowedDOFs::RotationY);
    lock(rigid.axis_lock_angular_z || rigid.lock_rotation, EAllowedDOFs::RotationZ);
    // Jolt can't lock every axis; Godot unlocks them all, and says to freeze the body instead.
    return dofs == EAllowedDOFs::None ? EAllowedDOFs::All : dofs;
}

RigidKey rigid_key_of(const RigidBody3D& rigid) {
    JPH::EMotionType motion = JPH::EMotionType::Dynamic;
    if (rigid.freeze) {
        motion = rigid.freeze_mode == "kinematic" ? JPH::EMotionType::Kinematic
                                                  : JPH::EMotionType::Static;
    }
    return {.motion = motion,
            .mass = rigid.mass,
            .inertia = rigid.inertia,
            .dofs = dofs_of(rigid, motion),
            .ccd = rigid.continuous_cd,
            .can_sleep = rigid.can_sleep};
}

// A physical bone's body as Godot's PhysicalBone3D sets it up: rigid while it simulates,
// kinematic while it follows its bone and static otherwise, with no inertia of its own.
RigidKey bone_key_of(const PhysicalBone3D& bone, world::PhysicalBoneMode mode) {
    JPH::EMotionType motion = JPH::EMotionType::Static;
    if (mode == world::PhysicalBoneMode::Simulate) {
        motion = JPH::EMotionType::Dynamic;
    } else if (mode == world::PhysicalBoneMode::Follow) {
        motion = JPH::EMotionType::Kinematic;
    }
    return {.motion = motion,
            .mass = bone.mass,
            .inertia = {},
            .dofs = JPH::EAllowedDOFs::All,
            .ccd = false,
            .can_sleep = bone.can_sleep};
}

JPH::EMotionType motion_of(Kind kind, const RigidKey& rigid) {
    switch (kind) {
    case Kind::Rigid:
    case Kind::Bone:
        return rigid.motion;
    case Kind::Character:
    case Kind::Animatable:
        return JPH::EMotionType::Kinematic;
    case Kind::Static:
    case Kind::Area:
        return JPH::EMotionType::Static;
    }
    return JPH::EMotionType::Static;
}

JPH::BroadPhaseLayer broad_of(Kind kind, JPH::EMotionType motion) {
    if (kind == Kind::Area) {
        return broad::kArea;
    }
    return motion == JPH::EMotionType::Static ? broad::kStatic : broad::kMoving;
}

void warn_shape(const world::World& world, entt::entity entity, std::string_view problem) {
    log::warn("physics shape left out",
              {{"entity", std::string(world.id_of(entity))}, {"problem", std::string(problem)}});
}

// Godot's integrate for an area's space override: whether the value is done, so later areas
// and the project's settings don't add to it.
template <typename T, typename Get>
bool integrate(T& value, const std::string& mode, Get get) {
    if (mode == "combine") {
        value = value + get();
        return false;
    }
    if (mode == "combine_replace") {
        value = value + get();
        return true;
    }
    if (mode == "replace") {
        value = get();
        return true;
    }
    if (mode == "replace_combine") {
        value = get();
        return false;
    }
    return false; // disabled
}

// What an area's query looks in: moving bodies, other areas and, when the project says so,
// static bodies.
class AreaBroadFilter final : public JPH::BroadPhaseLayerFilter {
public:
    explicit AreaBroadFilter(bool statics) : statics_(statics) {}
    [[nodiscard]] bool ShouldCollide(JPH::BroadPhaseLayer layer) const override {
        return layer == broad::kMoving || layer == broad::kArea ||
               (statics_ && layer == broad::kStatic);
    }

private:
    bool statics_;
};

// Objects whose collision layer is in a mask.
class MaskFilter final : public JPH::ObjectLayerFilter {
public:
    MaskFilter(const Layers& layers, std::uint32_t mask) : layers_(&layers), mask_(mask) {}
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return (std::get<1>(layers_->from_object_layer(layer)) & mask_) != 0;
    }

private:
    const Layers* layers_;
    std::uint32_t mask_;
};

// Everything but the area itself and areas that can't be seen.
class AreaBodyFilter final : public JPH::BodyFilter {
public:
    AreaBodyFilter(const world::World& world, JPH::BodyID self) : world_(&world), self_(self) {}
    [[nodiscard]] bool ShouldCollide(const JPH::BodyID& id) const override { return id != self_; }
    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        const Body* other = Space::body_of(body);
        if (other == nullptr) {
            return false;
        }
        if (other->kind != Kind::Area) {
            return true;
        }
        const auto* area = world_->ecs().try_get<Area3D>(other->entity);
        return area != nullptr && area->monitorable;
    }

private:
    const world::World* world_;
    JPH::BodyID self_;
};

} // namespace

float with_own(float total, std::string_view mode, float own) {
    return mode == "replace" ? own : total + own;
}

std::optional<OwnForces> own_forces(const entt::registry& registry, entt::entity entity,
                                    Kind kind) {
    if (kind == Kind::Rigid) {
        const auto& rigid = registry.get<RigidBody3D>(entity);
        return OwnForces{.gravity_scale = rigid.gravity_scale,
                         .linear_damp_mode = rigid.linear_damp_mode,
                         .linear_damp = rigid.linear_damp,
                         .angular_damp_mode = rigid.angular_damp_mode,
                         .angular_damp = rigid.angular_damp,
                         .constant_force = rigid.constant_force,
                         .constant_torque = rigid.constant_torque};
    }
    if (kind == Kind::Bone) {
        const auto& bone = registry.get<PhysicalBone3D>(entity);
        return OwnForces{.gravity_scale = bone.gravity_scale,
                         .linear_damp_mode = bone.linear_damp_mode,
                         .linear_damp = bone.linear_damp,
                         .angular_damp_mode = bone.angular_damp_mode,
                         .angular_damp = bone.angular_damp,
                         .constant_force = {},
                         .constant_torque = {}};
    }
    return std::nullopt;
}

// ---- Layers

Layers::Layers() {
    collisions_.push_back(0);
    by_collision_.emplace(0, JPH::ObjectLayer{0});
}

JPH::ObjectLayer Layers::to_object_layer(JPH::BroadPhaseLayer broad_phase_layer,
                                         std::uint32_t layer, std::uint32_t mask) {
    constexpr std::size_t kIndexes = 1U << 13U;
    const std::uint64_t collision = (std::uint64_t{layer} << 32U) | mask;
    JPH::ObjectLayer index = 0;
    if (const auto found = by_collision_.find(collision); found != by_collision_.end()) {
        index = found->second;
    } else if (collisions_.size() < kIndexes) {
        index = static_cast<JPH::ObjectLayer>(collisions_.size());
        collisions_.push_back(collision);
        by_collision_.emplace(collision, index);
    } else {
        log::warn("too many collision layer and mask pairs for 3D physics",
                  {{"limit", static_cast<int>(kIndexes)}});
    }
    const auto upper =
        static_cast<unsigned>(static_cast<JPH::BroadPhaseLayer::Type>(broad_phase_layer));
    return static_cast<JPH::ObjectLayer>((upper << 13U) | index);
}

std::tuple<JPH::BroadPhaseLayer, std::uint32_t, std::uint32_t>
Layers::from_object_layer(JPH::ObjectLayer object_layer) const {
    const auto broad_phase_layer = JPH::BroadPhaseLayer(
        static_cast<JPH::BroadPhaseLayer::Type>(static_cast<unsigned>(object_layer) >> 13U));
    const std::size_t index = object_layer & 0x1FFFU;
    const std::uint64_t collision = index < collisions_.size() ? collisions_[index] : 0;
    return {broad_phase_layer, static_cast<std::uint32_t>(collision >> 32U),
            static_cast<std::uint32_t>(collision & 0xFFFFFFFFU)};
}

JPH::BroadPhaseLayer Layers::GetBroadPhaseLayer(JPH::ObjectLayer layer) const {
    return std::get<0>(from_object_layer(layer));
}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
const char* Layers::GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const {
    if (layer == broad::kStatic) {
        return "static";
    }
    return layer == broad::kMoving ? "moving" : "area";
}
#endif

bool Layers::ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase_layer) const {
    // Static bodies meet moving ones, moving ones everything but areas; areas take no part.
    const JPH::BroadPhaseLayer own = GetBroadPhaseLayer(layer);
    if (own == broad::kStatic) {
        return broad_phase_layer == broad::kMoving;
    }
    if (own == broad::kMoving) {
        return broad_phase_layer == broad::kMoving || broad_phase_layer == broad::kStatic;
    }
    return false;
}

bool Layers::ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const {
    const auto [broad_a, layer_a, mask_a] = from_object_layer(a);
    const auto [broad_b, layer_b, mask_b] = from_object_layer(b);
    return (mask_a & layer_b) != 0 || (mask_b & layer_a) != 0;
}

// ---- Contacts

JPH::ValidateResult ContactListener::OnContactValidate(const JPH::Body& a, const JPH::Body& b,
                                                       JPH::RVec3Arg /*base_offset*/,
                                                       const JPH::CollideShapeResult& /*result*/) {
    const Body* first = Space::body_of(a);
    const Body* second = Space::body_of(b);
    if (first != nullptr && second != nullptr && space_->excepted(first->entity, second->entity)) {
        return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
    }
    return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
}

bool Space::excepted(entt::entity a, entt::entity b) const {
    return !exceptions_.empty() && exceptions_.contains(std::minmax(a, b));
}

void ContactListener::OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                                     const JPH::ContactManifold& /*manifold*/,
                                     JPH::ContactSettings& settings) {
    contact(a, b, settings);
}

void ContactListener::OnContactPersisted(const JPH::Body& a, const JPH::Body& b,
                                         const JPH::ContactManifold& /*manifold*/,
                                         JPH::ContactSettings& settings) {
    contact(a, b, settings);
}

void ContactListener::contact(const JPH::Body& a, const JPH::Body& b,
                              JPH::ContactSettings& settings) {
    Body* first = Space::body_of(a);
    Body* second = Space::body_of(b);
    if (first == nullptr || second == nullptr || a.IsSensor() || b.IsSensor()) {
        return;
    }
    // Godot's _try_override_collision_response: when only one of the two has the other's layer
    // in its mask, the other one isn't pushed.
    if (a.IsDynamic() || b.IsDynamic()) {
        const bool first_sees = (first->mask & second->layer) != 0;
        const bool second_sees = (second->mask & first->layer) != 0;
        if (first_sees && !second_sees) {
            settings.mInvMassScale2 = 0.0F;
            settings.mInvInertiaScale2 = 0.0F;
        } else if (second_sees && !first_sees) {
            settings.mInvMassScale1 = 0.0F;
            settings.mInvInertiaScale1 = 0.0F;
        }
    }
    // Godot's _try_apply_surface_velocities: a static or kinematic body's constant velocities
    // carry what touches it.
    const bool surface_a = !a.IsDynamic();
    const bool surface_b = !b.IsDynamic();
    if (surface_a != surface_b) {
        const auto moving = [](const Body& body) {
            return body.constant_linear_velocity != Vec3{} ||
                   body.constant_angular_velocity != Vec3{};
        };
        const bool has_a = surface_a && moving(*first);
        const bool has_b = surface_b && moving(*second);
        if (has_a != has_b) {
            const JPH::Vec3 linear_a = to_jolt(first->constant_linear_velocity);
            const JPH::Vec3 angular_a = to_jolt(first->constant_angular_velocity);
            const JPH::Vec3 linear_b = to_jolt(second->constant_linear_velocity);
            const JPH::Vec3 angular_b = to_jolt(second->constant_angular_velocity);
            const JPH::Vec3 offset_b =
                JPH::Vec3(b.GetCenterOfMassPosition() - a.GetCenterOfMassPosition());
            settings.mRelativeLinearSurfaceVelocity =
                (linear_b + offset_b.Cross(angular_b)) - linear_a;
            settings.mRelativeAngularSurfaceVelocity = angular_b - angular_a;
        }
    }
    space_->note_contact(*first, *second);
}

void Space::note_contact(Body& a, Body& b) {
    if (a.kind == Kind::Rigid) {
        a.contacts.insert(b.entity);
    }
    if (b.kind == Kind::Rigid) {
        b.contacts.insert(a.entity);
    }
}

// ---- Set up

void set_up_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::Allocate = jolt_allocate;
        JPH::Reallocate = jolt_reallocate;
        JPH::Free = jolt_free;
        JPH::AlignedAllocate = jolt_aligned_allocate;
        JPH::AlignedFree = jolt_aligned_free;
        JPH::Trace = jolt_trace;
        // For the process's life, as Godot's JoltPhysicsServer3D keeps it.
        JPH::Factory::sInstance = new JPH::Factory(); // NOLINT(cppcoreguidelines-owning-memory)
        JPH::RegisterTypes();
    });
}

Space* find_space(world::World& world) {
    auto* handle = world.ecs().ctx().find<std::shared_ptr<Space>>();
    return handle != nullptr ? handle->get() : nullptr;
}

const Space* find_space(const world::World& world) {
    const auto* handle = world.ecs().ctx().find<std::shared_ptr<Space>>();
    return handle != nullptr ? handle->get() : nullptr;
}

Space::Space(const Settings& settings) : settings_(settings) {
    set_up_jolt();
}

Space::~Space() {
    for (auto& [entity, joint] : joints_) {
        remove_joint(joint);
    }
    for (auto& [entity, bone] : bone_joints_) {
        remove_joint(bone.joint);
    }
    joints_.clear();
    bone_joints_.clear();
    bodies_.clear();
    system_.reset();
}

void Space::reset() {
    for (auto& [entity, joint] : joints_) {
        remove_joint(joint);
    }
    for (auto& [entity, bone] : bone_joints_) {
        remove_joint(bone.joint);
    }
    joints_.clear();
    bone_joints_.clear();
    bone_links_.clear();
    exceptions_.clear();
    wheels_.clear();
    vehicles_.clear();
    bodies_.clear();
    system_.reset();
    jobs_.reset();
    temp_.reset();
    impulses_.clear();
    touching_.clear();
    removed_.clear();
    added_ = 0;
}

void Space::set_settings(const Settings& settings) {
    const bool gravity_changed = settings.gravity != settings_.gravity ||
                                 settings.gravity_vector != settings_.gravity_vector;
    settings_ = settings;
    if (gravity_changed && system_ != nullptr) {
        JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
        for (auto& [entity, body] : bodies_) {
            if ((body->kind == Kind::Rigid || body->kind == Kind::Bone) && !body->id.IsInvalid() &&
                !body->sleeping) {
                interface.ActivateBody(body->id);
            }
        }
    }
}

const Body* Space::body(entt::entity entity) const {
    const auto* found = bodies_.find(entity);
    return found != nullptr ? found->get() : nullptr;
}

Body* Space::body(entt::entity entity) {
    auto* found = bodies_.find(entity);
    return found != nullptr ? found->get() : nullptr;
}

void Space::create_system() {
    temp_ = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(kTempBytes);
    jobs_ = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    system_ = std::make_unique<JPH::PhysicsSystem>();
    system_->Init(kMaxBodies, 0, kMaxPairs, kMaxContacts, layers_, layers_, layers_);
    // Godot's physics/jolt_physics_3d simulation settings.
    JPH::PhysicsSettings physics;
    physics.mBaumgarte = 0.2F;
    physics.mSpeculativeContactDistance = 0.02F;
    physics.mPenetrationSlop = 0.02F;
    physics.mLinearCastThreshold = 0.75F;
    physics.mLinearCastMaxPenetration = 0.25F;
    physics.mBodyPairCacheMaxDeltaPositionSq = 0.001F * 0.001F;
    physics.mBodyPairCacheCosMaxDeltaRotationDiv2 = 0.99984770F; // cos(2 degrees / 2)
    physics.mNumVelocitySteps = 10;
    physics.mNumPositionSteps = 2;
    physics.mMinVelocityForRestitution = 1.0F;
    physics.mTimeBeforeSleep = 0.5F;
    physics.mPointVelocitySleepThreshold = 0.03F;
    physics.mUseBodyPairContactCache = true;
    physics.mAllowSleeping = true;
    system_->SetPhysicsSettings(physics);
    // Gravity and damping are the space's own, from the areas each body is in.
    system_->SetGravity(JPH::Vec3::sZero());
    system_->SetContactListener(&listener_);
    system_->SetCombineFriction(combine_friction);
    system_->SetCombineRestitution(combine_bounce);
}

void Space::destroy(Body& body) {
    if (system_ != nullptr && !body.id.IsInvalid()) {
        drop_joints(body.id);
        JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
        interface.RemoveBody(body.id);
        interface.DestroyBody(body.id);
    }
    body.id = JPH::BodyID();
}

void Space::remove_stale(world::World& world, const std::vector<entt::entity>& made) {
    const entt::registry& registry = world.ecs();
    bodies_.erase_if([&](entt::entity entity, const std::unique_ptr<Body>& body) {
        if (body->generated ? std::ranges::find(made, entity) != made.end()
                            : world.valid(entity) && kind_of(registry, entity) == body->kind) {
            return false;
        }
        removed_.insert(body->name);
        destroy(*body);
        return true;
    });
}

void Space::wake_in(const JPH::AABox& box) {
    system_->GetBodyInterfaceNoLock().ActivateBodiesInAABox(box, JPH::BroadPhaseLayerFilter(),
                                                            JPH::ObjectLayerFilter());
}

void Space::build_shape(const world::World& world, Body& body, const ShapeKey& key) {
    body.parts.clear();
    body.grid_part.reset();
    body.shape_key = key;
    JPH::StaticCompoundShapeSettings compound;
    JPH::RefConst<JPH::Shape> single;
    world::Affine3D single_transform;
    std::size_t built = 0;
    for (std::size_t i = 0; i < key.parts.size(); ++i) {
        const Part& part = key.parts[i];
        if (part.shape.disabled) {
            continue;
        }
        if (part.shape.shape == "world_boundary" && body.kind != Kind::Static) {
            warn_shape(world, part.entity, "a world boundary only works on a static body");
            continue;
        }
        const BuiltShape made = detail::build_shape(part.shape);
        if (!made.error.empty()) {
            warn_shape(world, part.entity, made.error);
            continue;
        }
        if (made.shape == nullptr) {
            continue;
        }
        const Unscaled local = unscale(part.local);
        const JPH::RefConst<JPH::Shape> shape = scaled(made.shape.GetPtr(), local.scale);
        body.parts.emplace_back(i, made.shape);
        compound.AddShape(to_jolt(local.transform.origin), rotation_of(local.transform),
                          shape.GetPtr());
        single = shape;
        single_transform = local.transform;
        ++built;
    }
    // A CSG root's triangles, as Godot's ConcavePolygonShape3D of them, in the root's space.
    if (key.csg != nullptr && !key.csg->empty()) {
        world::CollisionShape3D faces;
        faces.shape = "concave_polygon";
        faces.faces = *key.csg;
        const BuiltShape made = detail::build_shape(faces);
        if (!made.error.empty()) {
            warn_shape(world, body.entity, made.error);
        } else if (made.shape != nullptr) {
            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), made.shape.GetPtr());
            single = made.shape;
            single_transform = {};
            ++built;
        }
    }
    // A GridMap's cells' shapes, as Godot's GridMap adds each to its octant's body: cells of
    // the same item share each built shape.
    if (key.grid != nullptr) {
        std::vector<std::pair<const world::CollisionShape3D*, JPH::RefConst<JPH::Shape>>> shapes;
        for (std::size_t i = 0; i < key.grid->size(); ++i) {
            const scene3d::GridShape& part = (*key.grid)[i];
            JPH::RefConst<JPH::Shape> unscaled;
            const auto found = std::ranges::find_if(
                shapes, [&](const auto& each) { return *each.first == part.shape; });
            if (found != shapes.end()) {
                unscaled = found->second;
            } else {
                const BuiltShape made = detail::build_shape(part.shape);
                if (!made.error.empty()) {
                    warn_shape(world, body.entity, made.error);
                }
                unscaled = made.shape;
                shapes.emplace_back(&part.shape, unscaled);
            }
            if (unscaled == nullptr) {
                continue;
            }
            const Unscaled local = unscale(part.local);
            const JPH::RefConst<JPH::Shape> shape = scaled(unscaled.GetPtr(), local.scale);
            compound.AddShape(to_jolt(local.transform.origin), rotation_of(local.transform),
                              shape.GetPtr(), static_cast<std::uint32_t>(i));
            single = shape;
            single_transform = local.transform;
            body.grid_part = i;
            ++built;
        }
    }
    if (built != 1) {
        body.grid_part.reset();
    }
    JPH::RefConst<JPH::Shape> whole;
    if (built == 1) {
        whole = single;
        if (single_transform != world::Affine3D{}) {
            const JPH::ShapeSettings::ShapeResult result =
                JPH::RotatedTranslatedShapeSettings(to_jolt(single_transform.origin),
                                                    rotation_of(single_transform), single.GetPtr())
                    .Create();
            whole = result.HasError() ? single : result.Get();
        }
    } else if (built > 1) {
        const JPH::ShapeSettings::ShapeResult result = compound.Create();
        if (result.HasError()) {
            const JPH::String& error = result.GetError();
            warn_shape(world, body.entity, std::string(error.data(), error.size()));
            body.parts.clear();
        } else {
            whole = result.Get();
        }
    }
    if (whole == nullptr) {
        whole = new JPH::EmptyShape(); // NOLINT(cppcoreguidelines-owning-memory): ref counted
    } else {
        whole = scaled(whole.GetPtr(), key.scale);
    }
    body.shape = whole;
}

void Space::create_body(Body& body, const world::Affine3D& global, const world::World& world) {
    const entt::registry& registry = world.ecs();
    const Unscaled placed = unscale(global);
    const JPH::EMotionType motion = motion_of(body.kind, body.rigid_key);
    JPH::BodyCreationSettings settings(
        body.shape.GetPtr(), to_jolt(placed.transform.origin), rotation_of(placed.transform),
        motion, layers_.to_object_layer(broad_of(body.kind, motion), body.layer, body.mask));
    settings.mUserData = reinterpret_cast<std::uint64_t>(&body); // NOLINT
    settings.mFriction = body.friction;
    settings.mRestitution = body.bounce;
    // Damping is the space's, before each step, as Godot's.
    settings.mLinearDamping = 0.0F;
    settings.mAngularDamping = 0.0F;
    settings.mMaxLinearVelocity = kMaxLinearVelocity;
    settings.mMaxAngularVelocity = kMaxAngularVelocity;
    settings.mEnhancedInternalEdgeRemoval = true;
    bool awake = false;
    if (body.kind == Kind::Area) {
        settings.mIsSensor = true;
    } else if (body.kind != Kind::Static) {
        // Rigid bodies freeze and thaw, so every moving body keeps what a dynamic one needs.
        settings.mAllowDynamicOrKinematic = true;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        float mass = 1.0F;
        Vec3 inertia;
        if (body.kind == Kind::Rigid) {
            const auto& rigid = registry.get<RigidBody3D>(body.entity);
            mass = rigid.mass;
            inertia = rigid.inertia;
            settings.mAllowedDOFs = body.rigid_key.dofs;
            settings.mMotionQuality = body.rigid_key.ccd ? JPH::EMotionQuality::LinearCast
                                                         : JPH::EMotionQuality::Discrete;
            settings.mAllowSleeping = body.rigid_key.can_sleep;
            settings.mUseManifoldReduction = !rigid.contact_monitor;
            settings.mLinearVelocity = to_jolt(rigid.linear_velocity);
            settings.mAngularVelocity = to_jolt(rigid.angular_velocity);
            awake = motion == JPH::EMotionType::Dynamic && !rigid.sleeping;
        } else if (body.kind == Kind::Bone) {
            const auto& bone = registry.get<PhysicalBone3D>(body.entity);
            mass = bone.mass;
            settings.mAllowSleeping = body.rigid_key.can_sleep;
            settings.mLinearVelocity = to_jolt(bone.linear_velocity);
            settings.mAngularVelocity = to_jolt(bone.angular_velocity);
            awake = motion == JPH::EMotionType::Dynamic;
        }
        settings.mMassPropertiesOverride = mass_properties(*body.shape, mass, inertia);
    }
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::Body* created = interface.CreateBody(settings);
    if (created == nullptr) {
        log::warn("3D physics body left out",
                  {{"entity", body.name}, {"problem", "too many 3D physics bodies"}});
        return;
    }
    body.id = created->GetID();
    interface.AddBody(body.id, awake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    ++added_;
}

void Space::sync_body(world::World& world, const world::Tree3D& tree, entt::entity entity,
                      Kind kind, const MadeShape* made_shape) {
    entt::registry& registry = world.ecs();
    const world::Affine3D global = tree.global(entity);
    Fields fields = made_shape != nullptr ? Fields{made_shape->layer,
                                                   made_shape->mask,
                                                   made_shape->friction,
                                                   made_shape->bounce,
                                                   {},
                                                   {}}
                                          : fields_of(registry, entity, kind);
    auto bone_mode = world::PhysicalBoneMode::Static;
    if (kind == Kind::Bone) {
        const auto link = bone_links_.find(entity);
        bone_mode = link != bone_links_.end() ? link->second.mode : world::PhysicalBoneMode::Static;
        // Godot's PhysicalBone3D::_stop_physics_simulation: a bone its simulator leaves out is
        // in no layer and sees none.
        if (bone_mode == world::PhysicalBoneMode::Off) {
            fields.layer = 0;
            fields.mask = 0;
        }
    }
    ShapeKey shape_key;
    if (made_shape != nullptr) {
        shape_key.scale = scale_of(registry, tree, entity);
        shape_key.csg = made_shape->faces;
        shape_key.grid = made_shape->grid;
    } else {
        shape_key = shape_key_of(registry, tree, entity);
    }
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();

    std::unique_ptr<Body>* found = bodies_.find(entity);
    if (found == nullptr) {
        auto made = std::make_unique<Body>();
        made->entity = entity;
        made->name = world.id_of(entity);
        made->kind = kind;
        made->generated = made_shape != nullptr;
        made->layer = fields.layer;
        made->mask = fields.mask;
        made->friction = fields.friction;
        made->bounce = fields.bounce;
        made->constant_linear_velocity = fields.constant_linear_velocity;
        made->constant_angular_velocity = fields.constant_angular_velocity;
        made->placed = global;
        made->bone_mode = bone_mode;
        if (kind == Kind::Rigid) {
            const auto& rigid = registry.get<RigidBody3D>(entity);
            made->rigid_key = rigid_key_of(rigid);
            made->linear_velocity = rigid.linear_velocity;
            made->angular_velocity = rigid.angular_velocity;
            made->sleeping = rigid.sleeping;
        } else if (kind == Kind::Bone) {
            const auto& bone = registry.get<PhysicalBone3D>(entity);
            made->rigid_key = bone_key_of(bone, bone_mode);
            made->linear_velocity = bone.linear_velocity;
            made->angular_velocity = bone.angular_velocity;
        }
        found = &bodies_.emplace(entity, std::move(made));
        build_shape(world, **found, shape_key);
        create_body(**found, global, world);
        return;
    }
    Body& body = **found;
    if (body.id.IsInvalid()) {
        return;
    }
    JPH::Body* jolt = locks.TryGetBody(body.id);
    if (jolt == nullptr) {
        return;
    }

    if (shape_key != body.shape_key) {
        const JPH::AABox before = jolt->GetWorldSpaceBounds();
        build_shape(world, body, shape_key);
        interface.SetShape(body.id, body.shape.GetPtr(), false, JPH::EActivation::DontActivate);
        if (jolt->IsDynamic()) {
            interface.ActivateBody(body.id);
        } else if (kind != Kind::Area) {
            JPH::AABox box = before;
            box.Encapsulate(jolt->GetWorldSpaceBounds());
            wake_in(box);
        }
        // The mass follows the shape.
        body.rigid_key.mass = -1.0F;
    }

    if (fields.layer != body.layer || fields.mask != body.mask) {
        body.layer = fields.layer;
        body.mask = fields.mask;
        interface.SetObjectLayer(
            body.id,
            layers_.to_object_layer(broad_of(kind, jolt->GetMotionType()), body.layer, body.mask));
    }
    if (fields.friction != body.friction) {
        body.friction = fields.friction;
        interface.SetFriction(body.id, body.friction);
    }
    if (fields.bounce != body.bounce) {
        body.bounce = fields.bounce;
        interface.SetRestitution(body.id, body.bounce);
    }
    body.constant_linear_velocity = fields.constant_linear_velocity;
    body.constant_angular_velocity = fields.constant_angular_velocity;

    // Rigid bodies and physical bones: how Jolt moves them, and the velocities and sleep an
    // edit gives them.
    const auto update_motion = [&](const RigidKey& key, Vec3 linear_velocity, Vec3 angular_velocity,
                                   bool asleep) {
        if (key != body.rigid_key) {
            if (key.motion != jolt->GetMotionType()) {
                interface.SetMotionType(body.id, key.motion, JPH::EActivation::DontActivate);
                interface.SetObjectLayer(
                    body.id,
                    layers_.to_object_layer(broad_of(kind, key.motion), body.layer, body.mask));
            }
            jolt->GetMotionPropertiesUnchecked()->SetMassProperties(
                key.dofs, mass_properties(*body.shape, key.mass, key.inertia));
            interface.SetMotionQuality(body.id, key.ccd ? JPH::EMotionQuality::LinearCast
                                                        : JPH::EMotionQuality::Discrete);
            jolt->SetAllowSleeping(key.can_sleep);
            if (key.motion == JPH::EMotionType::Dynamic && !asleep) {
                interface.ActivateBody(body.id);
            }
            body.rigid_key = key;
        }
        const bool dynamic = jolt->IsDynamic();
        if (linear_velocity != body.linear_velocity) {
            if (dynamic) {
                interface.SetLinearVelocity(body.id, to_jolt(linear_velocity));
            }
            body.linear_velocity = linear_velocity;
        }
        if (angular_velocity != body.angular_velocity) {
            if (dynamic) {
                interface.SetAngularVelocity(body.id, to_jolt(angular_velocity));
            }
            body.angular_velocity = angular_velocity;
        }
    };
    if (kind == Kind::Rigid) {
        const auto& rigid = registry.get<RigidBody3D>(entity);
        update_motion(rigid_key_of(rigid), rigid.linear_velocity, rigid.angular_velocity,
                      rigid.sleeping);
        if (jolt->GetUseManifoldReduction() == rigid.contact_monitor) {
            jolt->SetUseManifoldReduction(!rigid.contact_monitor);
        }
        const bool dynamic = jolt->IsDynamic();
        if (rigid.sleeping != body.sleeping) {
            if (dynamic && rigid.sleeping) {
                interface.DeactivateBody(body.id);
            } else if (dynamic) {
                interface.ActivateBody(body.id);
            }
            body.sleeping = rigid.sleeping;
        }
    } else if (kind == Kind::Bone) {
        const auto& bone = registry.get<PhysicalBone3D>(entity);
        body.bone_mode = bone_mode;
        update_motion(bone_key_of(bone, bone_mode), bone.linear_velocity, bone.angular_velocity,
                      false);
        if (bone_mode != world::PhysicalBoneMode::Follow) {
            body.target.reset();
        }
    }

    if (global == body.placed) {
        return;
    }
    const Unscaled placed = unscale(global);
    body.placed = global;
    if ((kind == Kind::Animatable && registry.get<AnimatableBody3D>(entity).sync_to_physics) ||
        (kind == Kind::Bone && bone_mode == world::PhysicalBoneMode::Follow)) {
        // Moved during the step, so it pushes what is in its way.
        body.target = placed.transform;
        return;
    }
    // Anything else jumps there, as an edit puts it.
    const JPH::AABox before = jolt->GetWorldSpaceBounds();
    const bool dynamic = jolt->IsDynamic();
    interface.SetPositionAndRotation(
        body.id, to_jolt(placed.transform.origin), rotation_of(placed.transform),
        dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    body.target.reset();
    if (!dynamic && kind != Kind::Area) {
        JPH::AABox box = before;
        box.Encapsulate(jolt->GetWorldSpaceBounds());
        wake_in(box);
    }
}

void Space::sync(world::World& world) {
    const world::Tree3D tree(world);
    sync(world, tree);
}

void Space::sync(world::World& world, const world::Tree3D& tree) {
    entt::registry& registry = world.ecs();
    // CSG roots with use_collision and GridMaps with shapes, unless the entity is a body of its
    // own; a GridMap that is a CSG root too is left to the CSG.
    std::vector<scene3d::CsgBody> csg = scene3d::csg_bodies(world, tree);
    std::erase_if(csg, [&](const scene3d::CsgBody& body) {
        return kind_of(registry, body.entity).has_value();
    });
    std::vector<scene3d::GridBody> grids = scene3d::grid_bodies(world);
    std::erase_if(grids, [&](const scene3d::GridBody& body) {
        return kind_of(registry, body.entity).has_value() ||
               std::ranges::any_of(
                   csg, [&](const scene3d::CsgBody& root) { return root.entity == body.entity; });
    });
    if (system_ == nullptr) {
        if (csg.empty() && grids.empty() &&
            !has_any<RigidBody3D, CharacterBody3D, AnimatableBody3D, StaticBody3D, Area3D,
                     PhysicalBone3D>(registry)) {
            return;
        }
        create_system();
    }
    bone_links_.clear();
    if (!registry.view<PhysicalBone3D>().empty()) {
        for (auto& [entity, link] : world::physical_bone_links(world)) {
            bone_links_.insert_or_assign(entity, std::move(link));
        }
    }
    std::vector<entt::entity> made;
    made.reserve(csg.size() + grids.size());
    for (const scene3d::CsgBody& body : csg) {
        made.push_back(body.entity);
    }
    for (const scene3d::GridBody& body : grids) {
        made.push_back(body.entity);
    }
    remove_stale(world, made);
    // New bodies are made in the same order on every machine: by kind, then in each
    // component's storage order, then CSG roots in tree order, then GridMaps in the order they
    // were made.
    const auto each = [&]<typename Component>(Kind kind) {
        for (const entt::entity entity : registry.view<Component>()) {
            if (kind_of(registry, entity) == kind) {
                sync_body(world, tree, entity, kind);
            }
        }
    };
    each.template operator()<RigidBody3D>(Kind::Rigid);
    each.template operator()<CharacterBody3D>(Kind::Character);
    each.template operator()<AnimatableBody3D>(Kind::Animatable);
    each.template operator()<StaticBody3D>(Kind::Static);
    each.template operator()<Area3D>(Kind::Area);
    each.template operator()<PhysicalBone3D>(Kind::Bone);
    // A CSG root collides as a static body with no physics material: friction 1, no bounce.
    for (scene3d::CsgBody& body : csg) {
        const MadeShape shape{.layer = body.layer,
                              .mask = body.mask,
                              .friction = 1.0F,
                              .bounce = 0.0F,
                              .faces = std::move(body.faces),
                              .grid = nullptr};
        sync_body(world, tree, body.entity, Kind::Static, &shape);
    }
    for (scene3d::GridBody& body : grids) {
        const MadeShape shape{.layer = body.layer,
                              .mask = body.mask,
                              .friction = body.friction,
                              .bounce = body.bounce,
                              .faces = nullptr,
                              .grid = std::move(body.shapes)};
        sync_body(world, tree, body.entity, Kind::Static, &shape);
    }
    sync_joints(world, tree);
    if (added_ >= kOptimizeAfter) {
        system_->OptimizeBroadPhase();
        added_ = 0;
    }
}

std::vector<const Body*> Space::override_areas(const world::World& world, const Body& body) const {
    std::vector<const Body*> areas;
    for (const auto& [entity, area] : bodies_) {
        if (area->kind != Kind::Area) {
            continue;
        }
        const auto* component = world.ecs().try_get<Area3D>(entity);
        if (component == nullptr || !overrides_space(*component)) {
            continue;
        }
        if (std::ranges::any_of(area->overlaps, [&](const auto& overlap) {
                return overlap.first == body.entity;
            })) {
            areas.push_back(area.get());
        }
    }
    std::ranges::stable_sort(areas, std::greater<>(), [&](const Body* area) {
        return world.ecs().get<Area3D>(area->entity).priority;
    });
    return areas;
}

AreaForces Space::forces(const world::World& world, const Body& body, Vec3 position) const {
    AreaForces out;
    const std::vector<const Body*> areas = override_areas(world, body);
    bool gravity_done = false;
    bool linear_done = false;
    bool angular_done = false;
    for (const Body* area : areas) {
        const auto& component = world.ecs().get<Area3D>(area->entity);
        out.areas.push_back(area->entity);
        if (!gravity_done) {
            // Godot's JoltArea3D::compute_gravity.
            gravity_done = integrate(out.gravity, component.gravity_space_override, [&] {
                if (!component.gravity_point) {
                    return component.gravity_direction * component.gravity;
                }
                const Vec3 point = area->placed.apply(component.gravity_point_center);
                const Vec3 to_point = point - position;
                const float distance_squared = std::max(dot(to_point, to_point), kEpsilon);
                const Vec3 direction = to_point * (1.0F / std::sqrt(distance_squared));
                if (component.gravity_point_unit_distance == 0.0F) {
                    return direction * component.gravity;
                }
                const float unit_squared =
                    component.gravity_point_unit_distance * component.gravity_point_unit_distance;
                return direction * (component.gravity * unit_squared / distance_squared);
            });
        }
        if (!linear_done) {
            linear_done = integrate(out.linear_damp, component.linear_damp_space_override,
                                    [&] { return component.linear_damp; });
        }
        if (!angular_done) {
            angular_done = integrate(out.angular_damp, component.angular_damp_space_override,
                                     [&] { return component.angular_damp; });
        }
    }
    if (!gravity_done) {
        out.gravity = out.gravity + (settings_.gravity_vector * settings_.gravity);
    }
    if (!linear_done) {
        out.linear_damp += settings_.linear_damp;
    }
    if (!angular_done) {
        out.angular_damp += settings_.angular_damp;
    }
    return out;
}

void Space::pre_step(const world::World& world, float seconds) {
    const entt::registry& registry = world.ecs();
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    pre_step_joints(seconds);

    // Impulses queued since the last step, as Godot applies them when they're called.
    for (const Impulse& impulse : impulses_) {
        const Body* target = body(impulse.entity);
        const JPH::Body* jolt =
            target != nullptr && (target->kind == Kind::Rigid || target->kind == Kind::Bone) &&
                    !target->id.IsInvalid()
                ? locks.TryGetBody(target->id)
                : nullptr;
        if (jolt == nullptr || !jolt->IsDynamic()) {
            continue;
        }
        if (impulse.impulse != Vec3{}) {
            interface.AddImpulse(target->id, to_jolt(impulse.impulse),
                                 jolt->GetPosition() + to_jolt(impulse.offset));
        }
        if (impulse.torque != Vec3{}) {
            interface.AddAngularImpulse(target->id, to_jolt(impulse.torque));
        }
    }
    impulses_.clear();

    for (auto& [entity, body] : bodies_) {
        if (body->id.IsInvalid() || body->kind == Kind::Area || body->kind == Kind::Static) {
            continue;
        }
        JPH::Body* jolt = locks.TryGetBody(body->id);
        if (jolt == nullptr) {
            continue;
        }
        if (jolt->IsKinematic()) {
            // Godot's JoltBody3D::_move_kinematic.
            if (jolt->IsActive()) {
                jolt->SetLinearVelocity(JPH::Vec3::sZero());
                jolt->SetAngularVelocity(JPH::Vec3::sZero());
            }
            if (body->kind == Kind::Rigid) {
                body->contacts.clear();
            }
            if (const std::optional<world::Affine3D>& target = body->target; target.has_value()) {
                const JPH::RVec3 position = to_jolt(target->origin);
                const JPH::Quat rotation = rotation_of(*target);
                if (position != jolt->GetPosition() || rotation != jolt->GetRotation()) {
                    interface.MoveKinematic(body->id, position, rotation, seconds);
                }
            }
            continue;
        }
        const std::optional<OwnForces> own = own_forces(registry, entity, body->kind);
        if (!own) {
            continue;
        }
        if (!jolt->IsDynamic()) {
            body->contacts.clear();
            continue;
        }
        if (!jolt->IsActive()) {
            if (own->constant_force == Vec3{} && own->constant_torque == Vec3{}) {
                continue; // asleep: it keeps its contacts, as Jolt reports none for it
            }
            interface.ActivateBody(body->id);
        }
        body->contacts.clear();
        // Godot's JoltBody3D::_integrate_forces: damping first, then gravity, then the
        // constant forces.
        const AreaForces forces = this->forces(world, *body, from_jolt(jolt->GetPosition()));
        body->gravity = forces.gravity * own->gravity_scale;
        body->total_linear_damp =
            with_own(forces.linear_damp, own->linear_damp_mode, own->linear_damp);
        body->total_angular_damp =
            with_own(forces.angular_damp, own->angular_damp_mode, own->angular_damp);
        JPH::MotionProperties& motion = *jolt->GetMotionProperties();
        JPH::Vec3 linear = motion.GetLinearVelocity();
        JPH::Vec3 angular = motion.GetAngularVelocity();
        linear *= std::max(1.0F - (body->total_linear_damp * seconds), 0.0F);
        angular *= std::max(1.0F - (body->total_angular_damp * seconds), 0.0F);
        linear += to_jolt(body->gravity) * seconds;
        motion.SetLinearVelocityClamped(linear);
        motion.SetAngularVelocityClamped(angular);
        jolt->AddForce(to_jolt(own->constant_force));
        jolt->AddTorque(to_jolt(own->constant_torque));
    }
}

void Space::read_back(world::World& world, const world::Tree3D& tree) {
    entt::registry& registry = world.ecs();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    std::vector<Body*> moved;
    for (auto& [entity, body] : bodies_) {
        if ((body->kind != Kind::Rigid && body->kind != Kind::Bone) || body->id.IsInvalid() ||
            !world.valid(entity)) {
            continue;
        }
        const JPH::Body* jolt = locks.TryGetBody(body->id);
        if (jolt == nullptr || !jolt->IsDynamic() || (!jolt->IsActive() && body->sleeping)) {
            continue;
        }
        moved.push_back(body.get());
    }
    // Parents first, so a body inside another is placed in its parent's new space.
    const auto depth = [&](const Body* body) {
        std::size_t count = 0;
        for (entt::entity up = tree.transform_parent(body->entity);
             up != entt::null && count < 4096; up = tree.transform_parent(up)) {
            ++count;
        }
        return count;
    };
    std::ranges::stable_sort(moved, {}, depth);
    for (Body* body : moved) {
        const JPH::Body& jolt = *locks.TryGetBody(body->id);
        const world::Affine3D global = affine_of(jolt.GetPosition(), jolt.GetRotation());
        auto& transform = registry.get_or_emplace<Transform3D>(body->entity);
        const entt::entity parent = tree.transform_parent(body->entity);
        if (parent == entt::null) {
            transform.position = global.origin;
            transform.rotation = world::euler(global);
        } else {
            // As Godot sets a body's global transform: its own is what puts it there.
            const Vec3 scale = body->shape_key.scale;
            world::Affine3D scaled_global = global;
            scaled_global.x = global.x * scale.x;
            scaled_global.y = global.y * scale.y;
            scaled_global.z = global.z * scale.z;
            const world::Affine3D local = tree.global(parent).inverse() * scaled_global;
            transform.position = local.origin;
            transform.rotation = world::euler(unscale(local).transform);
        }
        body->linear_velocity = from_jolt(jolt.GetLinearVelocity());
        body->angular_velocity = from_jolt(jolt.GetAngularVelocity());
        body->sleeping = !jolt.IsActive();
        if (body->kind == Kind::Rigid) {
            auto& rigid = registry.get<RigidBody3D>(body->entity);
            rigid.linear_velocity = body->linear_velocity;
            rigid.angular_velocity = body->angular_velocity;
            rigid.sleeping = body->sleeping;
        } else {
            auto& bone = registry.get<PhysicalBone3D>(body->entity);
            bone.linear_velocity = body->linear_velocity;
            bone.angular_velocity = body->angular_velocity;
        }
        // The body keeps the transform the components give, rounding and all, so the next sync
        // doesn't take the rounding for a move.
        body->placed = tree.global(body->entity);
    }
}

void Space::move_characters(world::World& world, const world::Tree3D& tree, float seconds) {
    entt::registry& registry = world.ecs();
    for (const entt::entity entity : registry.view<CharacterBody3D>()) {
        Body* found = body(entity);
        if (found == nullptr || found->kind != Kind::Character || found->id.IsInvalid()) {
            continue;
        }
        Body& character_body = *found;
        world::Affine3D global = tree.global(entity);
        const Vec3 before = global.origin;
        auto& character = registry.get<CharacterBody3D>(entity);
        move_character(*this, character_body, global, character, seconds);
        if (global.origin != before) {
            auto& transform = registry.get_or_emplace<Transform3D>(entity);
            const entt::entity parent = tree.transform_parent(entity);
            transform.position = parent == entt::null
                                     ? global.origin
                                     : tree.global(parent).inverse().apply(global.origin);
            character_body.placed = tree.global(entity);
        }
        // Its Jolt body follows during the next step, pushing what is in its way.
        character_body.target = unscale(character_body.placed).transform;
    }
}

void Space::find_overlaps(const world::World& world) {
    const entt::registry& registry = world.ecs();
    const JPH::NarrowPhaseQuery& query = system_->GetNarrowPhaseQueryNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    const AreaBroadFilter broad_filter(settings_.areas_detect_static_bodies);
    for (auto& [entity, area] : bodies_) {
        if (area->kind != Kind::Area) {
            continue;
        }
        area->overlaps.clear();
        const auto& component = registry.get<Area3D>(entity);
        if ((!component.monitoring && !overrides_space(component)) || area->id.IsInvalid() ||
            area->parts.empty()) {
            continue;
        }
        const JPH::Body* jolt = locks.TryGetBody(area->id);
        if (jolt == nullptr) {
            continue;
        }
        const MaskFilter object_filter(layers_, area->mask);
        const AreaBodyFilter body_filter(world, area->id);
        JPH::CollideShapeSettings settings;
        settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
        query.CollideShape(jolt->GetShape(), JPH::Vec3::sOne(), jolt->GetCenterOfMassTransform(),
                           settings, JPH::RVec3::sZero(), hits, broad_filter, object_filter,
                           body_filter);
        std::vector<JPH::BodyID> ids;
        ids.reserve(hits.mHits.size());
        for (const JPH::CollideShapeResult& hit : hits.mHits) {
            ids.push_back(hit.mBodyID2);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const JPH::BodyID id : ids) {
            const JPH::Body* other = locks.TryGetBody(id);
            const Body* found = other != nullptr ? body_of(*other) : nullptr;
            if (found != nullptr) {
                area->overlaps.emplace_back(found->entity, found->kind == Kind::Area);
            }
        }
    }
}

void Space::step(world::World& world, float seconds) {
    const world::Tree3D tree(world);
    sync(world, tree);
    if (system_ == nullptr) {
        impulses_.clear();
        return;
    }
    pre_step_vehicles(world, tree, seconds);
    pre_step(world, seconds);
    {
        SLASH_PROFILE_ZONE("Jolt step");
        const JPH::EPhysicsUpdateError errors =
            system_->Update(seconds, 1, temp_.get(), jobs_.get());
        if (errors != JPH::EPhysicsUpdateError::None && !warned_full_) {
            warned_full_ = true;
            log::warn("3D physics ran out of room for contacts; some were ignored",
                      {{"max_body_pairs", static_cast<int>(kMaxPairs)},
                       {"max_contact_constraints", static_cast<int>(kMaxContacts)}});
        }
    }
    step_vehicles(world, tree, seconds);
    read_back(world, tree);
    move_characters(world, tree, seconds);
    find_overlaps(world);
    emit_events(world);
}

void Space::emit_events(world::World& world) {
    const entt::registry& registry = world.ecs();
    std::set<std::tuple<std::string, std::string, bool>> now;
    for (const auto& [entity, body] : bodies_) {
        const std::string_view id = world.id_of(entity);
        if (id.empty()) {
            continue;
        }
        if (body->kind == Kind::Rigid && registry.get<RigidBody3D>(entity).contact_monitor) {
            for (const entt::entity other : body->contacts) {
                if (const std::string_view other_id = world.id_of(other);
                    !other_id.empty() && this->body(other) != nullptr) {
                    now.emplace(id, other_id, false);
                }
            }
        } else if (body->kind == Kind::Area && registry.get<Area3D>(entity).monitoring) {
            for (const auto& [other, is_area] : body->overlaps) {
                if (const std::string_view other_id = world.id_of(other); !other_id.empty()) {
                    now.emplace(id, other_id, is_area);
                }
            }
        }
    }
    const auto emit = [&](const std::tuple<std::string, std::string, bool>& pair, bool entered) {
        const auto& [id, other, is_area] = pair;
        const std::string what = is_area ? "area" : "body";
        json::Value data;
        data[what] = other;
        world.emit(what + (entered ? "_entered" : "_exited"), id, std::move(data));
    };
    const auto replaced = [&](const std::tuple<std::string, std::string, bool>& pair) {
        const auto& [id, other, is_area] = pair;
        const auto back = [&](const std::string& name) {
            return removed_.contains(name) && world.find(name) != entt::null;
        };
        return back(id) || back(other);
    };
    for (const auto& pair : touching_) {
        // Nothing reports for an entity that is gone.
        if (!now.contains(pair) && !replaced(pair) && world.find(std::get<0>(pair)) != entt::null) {
            emit(pair, false);
        }
    }
    for (const auto& pair : now) {
        if (!touching_.contains(pair) && !replaced(pair)) {
            emit(pair, true);
        }
    }
    touching_ = std::move(now);
    removed_.clear();
}

world::MemoryPart Space::memory() const {
    world::MemoryPart part{"physics3d", 0, {}, {}};
    std::uint64_t shapes = 0;
    for (const auto& [entity, body] : bodies_) {
        shapes += body->parts.size();
    }
    part.counts = {
        {"active", 0}, {"bodies", bodies_.size()}, {"shapes", shapes}, {"joints", joints_.size()}};
    if (system_ != nullptr) {
        // What Jolt allocated for every space in the process; a session or game has one.
        part.bytes = g_jolt_bytes.load();
        part.counts["active"] = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    }
    part.bytes += bodies_.size() * (sizeof(Body) + sizeof(EntityMap<std::unique_ptr<Body>>::Entry));
    part.bytes += (joints_.size() * sizeof(EntityMap<Joint>::Entry)) +
                  (wheels_.size() * sizeof(EntityMap<Wheel>::Entry));
    return part;
}

Contacts Space::contacts(const world::World& world, entt::entity entity) const {
    Contacts out;
    const Body* target = body(entity);
    if (target == nullptr || system_ == nullptr) {
        return out;
    }
    if (target->kind == Kind::Area) {
        for (const auto& [other, is_area] : target->overlaps) {
            (is_area ? out.areas : out.bodies).push_back(other);
        }
    } else {
        if (target->kind == Kind::Character) {
            // What its last move slid along, and the floor (or wall) it found, which a resting
            // character finds by snapping rather than by moving.
            out.bodies = target->touching;
            if (target->platform != entt::null) {
                out.bodies.push_back(target->platform);
            }
        }
        out.bodies.insert(out.bodies.end(), target->contacts.begin(), target->contacts.end());
        for (const auto& [other, body] : bodies_) {
            if (body->kind == Kind::Rigid && body->contacts.contains(entity)) {
                out.bodies.push_back(other);
            }
            if (body->kind == Kind::Area &&
                std::ranges::any_of(body->overlaps,
                                    [&](const auto& overlap) { return overlap.first == entity; })) {
                out.areas.push_back(other);
            }
        }
    }
    const auto tidy = [&](std::vector<entt::entity>& list) {
        std::erase_if(list, [&](entt::entity other) { return body(other) == nullptr; });
        std::ranges::sort(
            list, [&](entt::entity a, entt::entity b) { return world.id_of(a) < world.id_of(b); });
        list.erase(std::unique(list.begin(), list.end()), list.end());
    };
    tidy(out.bodies);
    tidy(out.areas);
    return out;
}

} // namespace slash::physics3d::detail
