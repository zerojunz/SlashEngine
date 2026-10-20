#include "space.hpp"

#include "polygon.hpp"

#include "slash/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace slash::physics2d::detail {

namespace {

using world::AnimatableBody2D;
using world::Area2D;
using world::CharacterBody2D;
using world::CollisionPolygon2D;
using world::CollisionShape2D;
using world::DampedSpringJoint2D;
using world::GrooveJoint2D;
using world::PinJoint2D;
using world::RigidBody2D;
using world::StaticBody2D;
using world::TileMapLayer;
using world::Transform2D;

// Box2D substeps per tick, as its documentation recommends.
constexpr int kSubSteps = 4;
// Joint motors push as hard as it takes, as Godot's do.
constexpr float kMaxMotorTorque = 1.0e9F;
// Box2D's revolute limits stop just short of half a turn.
constexpr float kMaxJointAngle = 0.99F * std::numbers::pi_v<float>;

// Godot's rule: two objects collide when either one's mask shares a bit with the other's layer.
bool should_collide(b2ShapeId a, b2ShapeId b, void* /*context*/) {
    const ShapeInfo* first = info_of(a);
    const ShapeInfo* second = info_of(b);
    if (first == nullptr || second == nullptr) {
        return true;
    }
    return (first->layer & second->mask) != 0U || (second->layer & first->mask) != 0U;
}

// A one-way shape only stops what comes from its up side and isn't already deep inside it.
bool one_way_allows(b2ShapeId shape, const ShapeInfo& info, b2Vec2 normal,
                    const b2Manifold& manifold) {
    const b2Vec2 up = b2RotateVector(b2Body_GetRotation(b2Shape_GetBody(shape)), info.one_way_up);
    if (b2Dot(normal, up) <= 0.0F) {
        return false;
    }
    float separation = std::numeric_limits<float>::max();
    for (int i = 0; i < manifold.pointCount; ++i) {
        separation = std::min(separation, manifold.points[i].separation);
    }
    return separation >= -info.one_way_margin;
}

bool pre_solve(b2ShapeId a, b2ShapeId b, b2Manifold* manifold, void* /*context*/) {
    // The normal points from a to b.
    const ShapeInfo* first = info_of(a);
    if (first != nullptr && first->one_way &&
        !one_way_allows(a, *first, manifold->normal, *manifold)) {
        return false;
    }
    const ShapeInfo* second = info_of(b);
    return second == nullptr || !second->one_way ||
           one_way_allows(b, *second, b2Neg(manifold->normal), *manifold);
}

// Splits a Box2D task's items into ranges, one per thread of the pool at most, as Box2D's samples
// do with enkiTS. Even a task of one item goes to a worker: the solver's are, and they wait for
// each other. A thread's number in the pool is the worker index Box2D keeps its scratch space by.
void* enqueue_task(b2TaskCallback* task, int item_count, int min_range, void* task_context,
                   void* user_context) {
    auto& tasks = *static_cast<Tasks*>(user_context);
    jobs::Pool& pool = *tasks.pool;
    const auto items = static_cast<std::int64_t>(item_count);
    const auto ranges = static_cast<std::int64_t>(
        std::clamp(item_count / std::max(min_range, 1), 1, pool.thread_count() + 1));
    const jobs::GroupId id = pool.add_group_task(
        [task, task_context, items, ranges, &pool](std::size_t range) {
            const auto first = static_cast<std::int64_t>(range);
            task(static_cast<int>(items * first / ranges),
                 static_cast<int>(items * (first + 1) / ranges),
                 static_cast<std::uint32_t>(pool.thread_index()), task_context);
        },
        static_cast<std::size_t>(ranges));
    tasks.groups.push_back(id);
    return &tasks.groups.back();
}

void finish_task(void* user_task, void* user_context) {
    auto& tasks = *static_cast<Tasks*>(user_context);
    tasks.pool->wait_for_group_task_completion(*static_cast<const jobs::GroupId*>(user_task));
}

// Godot's rules for two bodies' materials: the smaller friction, the sum of bounces up to 1.
float combine_friction(float a, int /*material_a*/, float b, int /*material_b*/) {
    return std::min(a, b);
}

float combine_bounce(float a, int /*material_a*/, float b, int /*material_b*/) {
    return std::clamp(a + b, 0.0F, 1.0F);
}

// The kind of body an entity is, by Godot's node it stands for; the first wins if it has two.
std::optional<Kind> kind_of(const entt::registry& registry, entt::entity entity) {
    if (registry.all_of<RigidBody2D>(entity)) {
        return Kind::Rigid;
    }
    if (registry.all_of<CharacterBody2D>(entity)) {
        return Kind::Character;
    }
    if (registry.all_of<AnimatableBody2D>(entity)) {
        return Kind::Animatable;
    }
    if (registry.all_of<StaticBody2D>(entity)) {
        return Kind::Static;
    }
    if (registry.all_of<Area2D>(entity)) {
        return Kind::Area;
    }
    return std::nullopt;
}

template <typename... Components>
bool has_any(const entt::registry& registry) {
    return (!registry.view<Components>().empty() || ...);
}

// Where an entity is after its parents: bodies, areas and joints are placed by it.
Transform2D transform_of(const world::Tree2D& tree, entt::entity entity) {
    return tree.global(entity);
}

struct BodyFields {
    std::uint32_t layer = 1;
    std::uint32_t mask = 1;
    float friction = 1.0F;
    float bounce = 0.0F;
    bool monitoring = true;
    bool monitorable = true;
    bool overrides = false;
};

// Whether an area changes the gravity or damping of the bodies in it.
bool overrides_space(const Area2D& area) {
    return area.gravity_space_override != "disabled" ||
           area.linear_damp_space_override != "disabled" ||
           area.angular_damp_space_override != "disabled";
}

BodyFields fields_of(const entt::registry& registry, entt::entity entity, Kind kind) {
    const auto material = [](const auto& body) {
        return BodyFields{.layer = body.collision_layer,
                          .mask = body.collision_mask,
                          .friction = body.friction,
                          .bounce = body.bounce,
                          .monitoring = true,
                          .monitorable = true,
                          .overrides = false};
    };
    switch (kind) {
    case Kind::Rigid:
        return material(registry.get<RigidBody2D>(entity));
    case Kind::Animatable:
        return material(registry.get<AnimatableBody2D>(entity));
    case Kind::Static:
        return material(registry.get<StaticBody2D>(entity));
    case Kind::Character: {
        const auto& character = registry.get<CharacterBody2D>(entity);
        return {.layer = character.collision_layer,
                .mask = character.collision_mask,
                .friction = 0.0F,
                .bounce = 0.0F,
                .monitoring = true,
                .monitorable = true,
                .overrides = false};
    }
    case Kind::Area: {
        const auto& area = registry.get<Area2D>(entity);
        return {.layer = area.collision_layer,
                .mask = area.collision_mask,
                .friction = 0.0F,
                .bounce = 0.0F,
                .monitoring = area.monitoring,
                .monitorable = area.monitorable,
                .overrides = overrides_space(area)};
    }
    }
    return {};
}

ShapeKey shape_key_of(const entt::registry& registry, const world::Tree2D& tree,
                      entt::entity entity, Kind kind) {
    ShapeKey key;
    const auto add = [&](entt::entity owner, std::optional<world::Affine2D> frame) {
        ShapeSource source{.entity = owner, .shape = {}, .polygon = {}, .owner = frame};
        if (const auto* shape = registry.try_get<CollisionShape2D>(owner); shape != nullptr) {
            source.shape = *shape;
        }
        if (const auto* polygon = registry.try_get<CollisionPolygon2D>(owner); polygon != nullptr) {
            source.polygon = *polygon;
        }
        if (source.shape || source.polygon) {
            key.sources.push_back(std::move(source));
        }
    };
    add(entity, std::nullopt);
    // As a Godot CollisionObject2D takes the shapes of its CollisionShape2D and
    // CollisionPolygon2D children: child entities with shapes and no body of their own, placed
    // by their own transforms.
    if (const auto* transform = registry.try_get<Transform2D>(entity);
        transform != nullptr && tree.nested()) {
        for (const entt::entity child : transform->children) {
            if (tree.parent(child) == entity && !kind_of(registry, child).has_value()) {
                add(child, world::affine(registry.get<Transform2D>(child)));
            }
        }
    }
    key.scale = transform_of(tree, entity).scale;
    const BodyFields fields = fields_of(registry, entity, kind);
    key.layer = fields.layer;
    key.mask = fields.mask;
    key.friction = fields.friction;
    key.bounce = fields.bounce;
    key.monitoring = fields.monitoring;
    key.monitorable = fields.monitorable;
    key.overrides = fields.overrides;
    return key;
}

b2BodyType frozen_type(const RigidBody2D& rigid) {
    if (!rigid.freeze) {
        return b2_dynamicBody;
    }
    return rigid.freeze_mode == "kinematic" ? b2_kinematicBody : b2_staticBody;
}

RigidKey rigid_key_of(const RigidBody2D& rigid, const Settings& settings) {
    const bool combine_linear = rigid.linear_damp_mode != "replace";
    const bool combine_angular = rigid.angular_damp_mode != "replace";
    return {.type = frozen_type(rigid),
            .mass = rigid.mass,
            .inertia = rigid.inertia,
            .gravity_scale = rigid.gravity_scale,
            .linear_damping = rigid.linear_damp + (combine_linear ? settings.linear_damp : 0.0F),
            .angular_damping =
                rigid.angular_damp + (combine_angular ? settings.angular_damp : 0.0F),
            .fixed_rotation = rigid.lock_rotation,
            .bullet = rigid.continuous_cd != "disabled",
            .sleep_enabled = rigid.can_sleep};
}

// A static or animatable body's constant velocities, as Godot's StaticBody2D's; none for others.
std::pair<Vec2, float> constant_velocity_of(const entt::registry& registry, entt::entity entity,
                                            Kind kind) {
    if (kind == Kind::Static) {
        const auto& body = registry.get<StaticBody2D>(entity);
        return {body.constant_linear_velocity, body.constant_angular_velocity};
    }
    if (kind == Kind::Animatable) {
        const auto& body = registry.get<AnimatableBody2D>(entity);
        return {body.constant_linear_velocity, body.constant_angular_velocity};
    }
    return {};
}

bool carries(Vec2 linear, float angular) {
    return linear != Vec2{} || angular != 0.0F;
}

bool carries(const Body& body) {
    return carries(body.constant_linear_velocity, body.constant_angular_velocity);
}

// Box2D's type for a kind of body. A static body that carries what touches it is kinematic in
// Box2D, since only moving bodies' velocities reach Box2D's contact solver.
b2BodyType type_of(Kind kind, bool carrying) {
    switch (kind) {
    case Kind::Rigid:
        return b2_dynamicBody;
    case Kind::Static:
        return carrying ? b2_kinematicBody : b2_staticBody;
    case Kind::Character:
    case Kind::Animatable:
    case Kind::Area:
        return b2_kinematicBody;
    }
    return b2_staticBody;
}

std::string_view id_text(const world::World& world, entt::entity entity) {
    return world.id_of(entity);
}

void warn_shape(const world::World& world, entt::entity entity, std::string_view problem) {
    log::warn("physics shape left out",
              {{"entity", std::string(id_text(world, entity))}, {"problem", std::string(problem)}});
}

bool wake_dynamic(b2ShapeId shape, void* context) {
    const b2BodyId body = b2Shape_GetBody(shape);
    const b2BodyId self = *static_cast<const b2BodyId*>(context);
    const bool is_self = B2_ID_EQUALS(body, self);
    if (!is_self && b2Body_GetType(body) == b2_dynamicBody && !b2Body_IsAwake(body)) {
        b2Body_SetAwake(body, true);
    }
    return true;
}

// Wakes the rigid bodies near a box: Box2D leaves sleeping bodies asleep when what they rest on
// is moved or changed.
void wake_near(b2WorldId world, b2BodyId self, b2AABB box) {
    constexpr float kGrow = 4.0F * kLinearSlop;
    box.lowerBound = b2Sub(box.lowerBound, {kGrow, kGrow});
    box.upperBound = b2Add(box.upperBound, {kGrow, kGrow});
    b2World_OverlapAABB(world, box, {.categoryBits = kQueryBit, .maskBits = ~std::uint64_t{0}},
                        wake_dynamic, &self);
}

} // namespace

Space* find_space(world::World& world) {
    auto* handle = world.ecs().ctx().find<std::shared_ptr<Space>>();
    return handle != nullptr ? handle->get() : nullptr;
}

Space::Space(const Settings& settings) : settings_(settings) {}

Space::~Space() {
    if (b2World_IsValid(world_)) {
        b2DestroyWorld(world_);
    }
}

void Space::reset() {
    if (b2World_IsValid(world_)) {
        b2DestroyWorld(world_);
    }
    bodies_.clear();
    tiles_.clear();
    joints_.clear();
    world_ = b2_nullWorldId;
    ground_ = b2_nullBodyId;
    impulses_.clear();
    touching_.clear();
    removed_.clear();
}

void Space::set_settings(const Settings& settings) {
    const bool gravity_changed = settings.gravity != settings_.gravity ||
                                 settings.gravity_vector != settings_.gravity_vector;
    settings_ = settings;
    if (b2World_IsValid(world_)) {
        b2World_SetGravity(world_, to_b2(settings.gravity_vector * settings.gravity));
    }
    if (gravity_changed) {
        for (auto& [entity, body] : bodies_) {
            if (body->kind == Kind::Rigid && b2Body_GetType(body->id) == b2_dynamicBody) {
                b2Body_SetAwake(body->id, true);
            }
        }
    }
    // Rigid bodies pick up the new damping at the next sync.
    for (auto& [entity, body] : bodies_) {
        body->rigid_key.linear_damping = -1.0F;
    }
}

const Body* Space::body(entt::entity entity) const {
    const auto* found = bodies_.find(entity);
    if (found == nullptr) {
        found = tiles_.find(entity);
    }
    return found != nullptr ? found->get() : nullptr;
}

void Space::create_world(jobs::Pool& pool) {
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = to_b2(settings_.gravity_vector * settings_.gravity);
    def.frictionCallback = combine_friction;
    def.restitutionCallback = combine_bounce;
    // On the job system's threads and the one stepping the world, which Box2D steps the same
    // with any number of them.
    if (pool.thread_count() > 0) {
        tasks_.pool = &pool;
        def.workerCount = pool.thread_count() + 1;
        def.enqueueTask = enqueue_task;
        def.finishTask = finish_task;
        def.userTaskContext = &tasks_;
    }
    world_ = b2CreateWorld(&def);
    b2World_SetCustomFilterCallback(world_, should_collide, nullptr);
    b2World_SetPreSolveCallback(world_, pre_solve, nullptr);
    const b2BodyDef ground = b2DefaultBodyDef();
    ground_ = b2CreateBody(world_, &ground);
}

void Space::destroy(Body& body) {
    if (b2Body_IsValid(body.id)) {
        b2DestroyBody(body.id);
    }
    body.id = b2_nullBodyId;
    body.shapes.clear();
}

void Space::remove_stale(world::World& world) {
    const entt::registry& registry = world.ecs();
    bodies_.erase_if([&](entt::entity entity, const std::unique_ptr<Body>& body) {
        if (world.valid(entity) && kind_of(registry, entity) == body->kind) {
            return false;
        }
        removed_.insert(body->name);
        destroy(*body);
        return true;
    });
}

void Space::build_shapes(world::World& world, Body& body, const ShapeKey& key) {
    const bool rebuilt = !body.shapes.empty();
    if (rebuilt) {
        wake_near(world_, body.id, b2Body_ComputeAABB(body.id));
    }
    for (const b2ShapeId shape : body.shapes) {
        if (b2Shape_IsValid(shape)) {
            b2DestroyShape(shape, false);
        }
    }
    body.shapes.clear();
    body.shape_key = key;

    const bool area = body.kind == Kind::Area;
    body.info.assign(key.sources.size() * 2, ShapeInfo{.entity = body.entity,
                                                       .kind = body.kind,
                                                       .layer = key.layer,
                                                       .mask = key.mask,
                                                       .monitorable = key.monitorable,
                                                       .one_way = false,
                                                       .one_way_margin = 1.0F,
                                                       .one_way_up = {0.0F, -1.0F}});

    const auto shape_def = [&](ShapeInfo& info, bool one_way, float margin, b2Vec2 up) {
        b2ShapeDef def = b2DefaultShapeDef();
        def.material.friction = key.friction;
        def.material.restitution = key.bounce;
        def.density = 1.0F;
        def.isSensor = area;
        // Areas that change gravity or damping need their overlaps even when they monitor
        // nothing.
        def.enableSensorEvents = !area || key.monitoring || key.monitorable || key.overrides;
        def.updateBodyMass = false;
        def.filter.categoryBits = key.layer | kShapeBit;
        def.filter.maskBits = ~std::uint64_t{0};
        // Areas ignore one_way_collision, as in Godot.
        if (one_way && !area) {
            info.one_way = true;
            info.one_way_margin = margin;
            info.one_way_up = up;
            def.enablePreSolveEvents = true;
        }
        def.userData = &info;
        return def;
    };

    const auto add_polygon = [&](std::span<const b2Vec2> points, const b2ShapeDef& def) {
        if (points.size() < 3 || points.size() > B2_MAX_POLYGON_VERTICES) {
            return false;
        }
        const b2Hull hull = b2ComputeHull(points.data(), static_cast<int>(points.size()));
        if (hull.count == 0) {
            return false;
        }
        const b2Polygon polygon = b2MakePolygon(&hull, 0.0F);
        body.shapes.push_back(b2CreatePolygonShape(body.id, &def, &polygon));
        return true;
    };

    for (std::size_t index = 0; index < key.sources.size(); ++index) {
        const ShapeSource& source = key.sources[index];
        if (source.shape && !source.shape->disabled) {
            const CollisionShape2D& shape = *source.shape;
            const ShapePlacement place{.rotation = b2MakeRot(shape.rotation),
                                       .position = shape.position,
                                       .scale = key.scale,
                                       .owner = source.owner};
            const b2ShapeDef def = shape_def(body.info[index * 2], shape.one_way_collision,
                                             shape.one_way_collision_margin, place.up());
            if (shape.shape == "circle") {
                const b2Circle circle{.center = place.point({}),
                                      .radius = place.radius(shape.radius)};
                if (circle.radius > 0.0F) {
                    body.shapes.push_back(b2CreateCircleShape(body.id, &def, &circle));
                } else {
                    warn_shape(world, source.entity, "the circle has no radius");
                }
            } else if (shape.shape == "capsule") {
                const float half = std::max(0.0F, (shape.height * 0.5F) - shape.radius);
                const b2Capsule capsule{.center1 = place.point({0.0F, -half}),
                                        .center2 = place.point({0.0F, half}),
                                        .radius = place.radius(shape.radius)};
                if (capsule.radius <= 0.0F) {
                    warn_shape(world, source.entity, "the capsule has no radius");
                } else if (b2Distance(capsule.center1, capsule.center2) <= kLinearSlop) {
                    const b2Circle circle{.center = place.point({}), .radius = capsule.radius};
                    body.shapes.push_back(b2CreateCircleShape(body.id, &def, &circle));
                } else {
                    body.shapes.push_back(b2CreateCapsuleShape(body.id, &def, &capsule));
                }
            } else if (shape.shape == "segment") {
                const b2Segment segment{.point1 = place.point(shape.a),
                                        .point2 = place.point(shape.b)};
                if (b2Distance(segment.point1, segment.point2) > kLinearSlop) {
                    body.shapes.push_back(b2CreateSegmentShape(body.id, &def, &segment));
                } else {
                    warn_shape(world, source.entity, "the segment has no length");
                }
            } else if (shape.shape == "polygon") {
                std::vector<Vec2> points;
                points.reserve(shape.points.size());
                for (const Vec2 point : shape.points) {
                    points.push_back(from_b2(place.point(point)));
                }
                const Polygon hull = convex_hull(points);
                bool added = hull.size() >= 3;
                for (const Polygon& piece : split_convex(hull, B2_MAX_POLYGON_VERTICES)) {
                    std::vector<b2Vec2> corners;
                    for (const Vec2 point : piece) {
                        corners.push_back(to_b2(point));
                    }
                    added = add_polygon(corners, def) && added;
                }
                if (!added) {
                    warn_shape(world, source.entity, "the polygon has no area");
                }
            } else {
                const float hx = shape.size.x * 0.5F;
                const float hy = shape.size.y * 0.5F;
                const std::array<b2Vec2, 4> corners{place.point({-hx, -hy}), place.point({hx, -hy}),
                                                    place.point({hx, hy}), place.point({-hx, hy})};
                if (!add_polygon(corners, def)) {
                    warn_shape(world, source.entity, "the rectangle has no area");
                }
            }
        }

        if (source.polygon && !source.polygon->disabled) {
            const CollisionPolygon2D& polygon = *source.polygon;
            const ShapePlacement place{.rotation = b2Rot_identity,
                                       .position = {},
                                       .scale = key.scale,
                                       .owner = source.owner};
            const b2ShapeDef def = shape_def(body.info[(index * 2) + 1], polygon.one_way_collision,
                                             polygon.one_way_collision_margin, place.up());
            std::vector<Vec2> points;
            points.reserve(polygon.polygon.size());
            for (const Vec2 point : polygon.polygon) {
                points.push_back(from_b2(place.point(point)));
            }
            if (polygon.build_mode == "segments") {
                for (std::size_t i = 0; i < points.size() && points.size() >= 2; ++i) {
                    const b2Segment segment{.point1 = to_b2(points[i]),
                                            .point2 = to_b2(points[(i + 1) % points.size()])};
                    if (b2Distance(segment.point1, segment.point2) > kLinearSlop &&
                        (points.size() > 2 || i == 0)) {
                        body.shapes.push_back(b2CreateSegmentShape(body.id, &def, &segment));
                    }
                }
            } else {
                auto pieces = decompose(points, B2_MAX_POLYGON_VERTICES);
                if (!pieces) {
                    warn_shape(world, source.entity, pieces.error().message);
                } else {
                    for (const Polygon& piece : pieces.value()) {
                        std::vector<b2Vec2> corners;
                        for (const Vec2 point : piece) {
                            corners.push_back(to_b2(point));
                        }
                        // Slivers too thin for Box2D are left out; their neighbors cover them.
                        add_polygon(corners, def);
                    }
                }
            }
        }
    }

    if (body.kind == Kind::Rigid) {
        body.rigid_key.mass = -1.0F; // set the mass again from the new shapes
    }
    if (rebuilt && !body.shapes.empty()) {
        wake_near(world_, body.id, b2Body_ComputeAABB(body.id));
    }
}

void Space::sync_body(world::World& world, const world::Tree2D& tree, entt::entity entity,
                      Kind kind) {
    entt::registry& registry = world.ecs();
    const Transform2D transform = transform_of(tree, entity);
    const ShapeKey shape_key = shape_key_of(registry, tree, entity, kind);

    const auto [constant_linear, constant_angular] = constant_velocity_of(registry, entity, kind);
    std::unique_ptr<Body>* found = bodies_.find(entity);
    if (found == nullptr) {
        auto body = std::make_unique<Body>();
        body->entity = entity;
        body->name = world.id_of(entity);
        body->kind = kind;
        b2BodyDef def = b2DefaultBodyDef();
        def.type = type_of(kind, carries(constant_linear, constant_angular));
        def.position = to_b2(transform.position);
        def.rotation = b2MakeRot(transform.rotation);
        def.userData = body.get();
        if (kind == Kind::Rigid) {
            const auto& rigid = registry.get<RigidBody2D>(entity);
            def.linearVelocity = to_b2(rigid.linear_velocity);
            def.angularVelocity = rigid.angular_velocity;
            def.isAwake = !rigid.sleeping;
            body->linear_velocity = rigid.linear_velocity;
            body->angular_velocity = rigid.angular_velocity;
            body->sleeping = rigid.sleeping;
            body->rigid_key.type = b2_dynamicBody;
            body->rigid_key.mass = -1.0F; // set below
        }
        body->id = b2CreateBody(world_, &def);
        body->position = transform.position;
        body->rotation = transform.rotation;
        found = &bodies_.emplace(entity, std::move(body));
        build_shapes(world, **found, shape_key);
    } else if (shape_key != (*found)->shape_key) {
        build_shapes(world, **found, shape_key);
    }
    Body& body = **found;

    if (kind == Kind::Rigid) {
        auto& rigid = registry.get<RigidBody2D>(entity);
        const RigidKey key = rigid_key_of(rigid, settings_);
        if (key != body.rigid_key) {
            if (key.type != b2Body_GetType(body.id)) {
                b2Body_SetType(body.id, key.type);
            }
            b2Body_SetGravityScale(body.id, key.gravity_scale);
            b2Body_SetLinearDamping(body.id, key.linear_damping);
            b2Body_SetAngularDamping(body.id, key.angular_damping);
            b2Body_SetFixedRotation(body.id, key.fixed_rotation);
            b2Body_SetBullet(body.id, key.bullet);
            b2Body_EnableSleep(body.id, key.sleep_enabled);
            if (key.type == b2_dynamicBody) {
                // Box2D finds the center and inertia from the shapes at density 1; the mass
                // scales them.
                b2Body_ApplyMassFromShapes(body.id);
                const b2MassData shapes = b2Body_GetMassData(body.id);
                b2MassData mass{.mass = key.mass,
                                .center = shapes.mass > 0.0F ? shapes.center : b2Vec2_zero,
                                .rotationalInertia = key.inertia};
                if (key.inertia <= 0.0F) {
                    mass.rotationalInertia = shapes.mass > 0.0F
                                                 ? shapes.rotationalInertia * key.mass / shapes.mass
                                                 : 0.0F;
                }
                b2Body_SetMassData(body.id, mass);
            }
            body.rigid_key = key;
            body.environment = {.gravity_scale = key.gravity_scale,
                                .linear_damping = key.linear_damping,
                                .angular_damping = key.angular_damping,
                                .pull = {}};
        }
        if (rigid.linear_velocity != body.linear_velocity) {
            b2Body_SetLinearVelocity(body.id, to_b2(rigid.linear_velocity));
            body.linear_velocity = rigid.linear_velocity;
        }
        if (rigid.angular_velocity != body.angular_velocity) {
            b2Body_SetAngularVelocity(body.id, rigid.angular_velocity);
            body.angular_velocity = rigid.angular_velocity;
        }
        if (rigid.sleeping != body.sleeping) {
            b2Body_SetAwake(body.id, !rigid.sleeping);
            body.sleeping = rigid.sleeping;
        }
    }

    if (kind == Kind::Static || kind == Kind::Animatable) {
        // A body that starts or stops carrying what touches it: Box2D's static bodies have no
        // velocity, so it becomes kinematic, and it never sleeps, since a sleeping body has none.
        body.constant_linear_velocity = constant_linear;
        body.constant_angular_velocity = constant_angular;
        const bool carrying = carries(body);
        const b2BodyType type = type_of(kind, carrying);
        if (type != b2Body_GetType(body.id)) {
            b2Body_SetType(body.id, type);
        }
        if (b2Body_IsSleepEnabled(body.id) == carrying) {
            b2Body_EnableSleep(body.id, !carrying);
        }
        // An animatable body that jumps where it goes keeps no velocity once it stops carrying.
        if (!carrying && kind == Kind::Animatable &&
            !registry.get<AnimatableBody2D>(entity).sync_to_physics &&
            (b2LengthSquared(b2Body_GetLinearVelocity(body.id)) > 0.0F ||
             b2Body_GetAngularVelocity(body.id) != 0.0F)) {
            b2Body_SetLinearVelocity(body.id, b2Vec2_zero);
            b2Body_SetAngularVelocity(body.id, 0.0F);
        }
    }

    const bool moved = transform.position != body.position || transform.rotation != body.rotation;
    if (kind == Kind::Animatable && registry.get<AnimatableBody2D>(entity).sync_to_physics) {
        // Moved during the step (step sets the target), so it pushes what is in its way.
        return;
    }
    if (moved) {
        const bool has_shapes = !body.shapes.empty();
        const b2AABB before = has_shapes ? b2Body_ComputeAABB(body.id) : b2AABB{};
        b2Body_SetTransform(body.id, to_b2(transform.position), b2MakeRot(transform.rotation));
        body.position = transform.position;
        body.rotation = transform.rotation;
        if (b2Body_GetType(body.id) == b2_dynamicBody) {
            b2Body_SetAwake(body.id, true);
        } else if (has_shapes) {
            wake_near(world_, body.id, b2AABB_Union(before, b2Body_ComputeAABB(body.id)));
        }
    }
}

void Space::sync_joints(world::World& world, const world::Tree2D& tree) {
    entt::registry& registry = world.ecs();
    // Joints whose entity is gone.
    joints_.erase_if([&](entt::entity entity, const Joint& joint) {
        if (world.valid(entity) &&
            registry.any_of<PinJoint2D, DampedSpringJoint2D, GrooveJoint2D>(entity)) {
            return false;
        }
        if (b2Joint_IsValid(joint.id)) {
            b2DestroyJoint(joint.id);
        }
        return true;
    });

    const auto body_id = [&](entt::entity node, bool world_if_null) -> b2BodyId {
        if (node == entt::null) {
            return world_if_null ? ground_ : b2_nullBodyId;
        }
        const Body* found = body(node);
        return found != nullptr && world.valid(node) ? found->id : b2_nullBodyId;
    };

    const auto place = [&](entt::entity entity, const JointKey& key, entt::entity node_a,
                           entt::entity node_b) {
        const auto* own_transform = registry.try_get<Transform2D>(entity);
        const Transform2D own = own_transform != nullptr ? *own_transform : Transform2D{};
        const b2BodyId body_a = body_id(node_a, true);
        const b2BodyId body_b = body_id(node_b, false);
        if (const Joint* existing = joints_.find(entity); existing != nullptr) {
            const Joint& joint = *existing;
            if (b2Joint_IsValid(joint.id) && joint.key == key && joint.position == own.position &&
                joint.rotation == own.rotation && B2_ID_EQUALS(joint.body_a, body_a) &&
                B2_ID_EQUALS(joint.body_b, body_b)) {
                return;
            }
            if (b2Joint_IsValid(joint.id)) {
                b2DestroyJoint(joint.id);
            }
            joints_.erase(entity);
        }
        if (B2_IS_NULL(body_a) || B2_IS_NULL(body_b) || B2_ID_EQUALS(body_a, body_b)) {
            return; // the next sync tries again, once both bodies exist
        }
        Joint joint{.id = b2_nullJointId,
                    .key = key,
                    .position = own.position,
                    .rotation = own.rotation,
                    .body_a = body_a,
                    .body_b = body_b};
        // Points along the joint's y axis in its own space, after its parents', as Godot's
        // get_global_transform().xform(Vector2(0, distance)).
        const world::Affine2D global = tree.global_affine(entity);
        const b2Vec2 origin = to_b2(global.origin);
        const auto along = [&](float distance) { return to_b2(global.apply({0.0F, distance})); };
        if (const auto* pin = std::get_if<PinJoint2D>(&key); pin != nullptr) {
            b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
            def.bodyIdA = body_a;
            def.bodyIdB = body_b;
            def.localAnchorA = b2Body_GetLocalPoint(body_a, origin);
            def.localAnchorB = b2Body_GetLocalPoint(body_b, origin);
            def.referenceAngle =
                b2Rot_GetAngle(b2InvMulRot(b2Body_GetRotation(body_a), b2Body_GetRotation(body_b)));
            def.enableLimit = pin->angular_limit_enabled;
            def.lowerAngle = std::clamp(pin->angular_limit_lower, -kMaxJointAngle, 0.0F);
            def.upperAngle = std::clamp(pin->angular_limit_upper, 0.0F, kMaxJointAngle);
            def.enableMotor = pin->motor_enabled;
            def.motorSpeed = pin->motor_target_velocity;
            def.maxMotorTorque = kMaxMotorTorque;
            def.collideConnected = !pin->disable_collision;
            joint.id = b2CreateRevoluteJoint(world_, &def);
        } else if (const auto* spring = std::get_if<DampedSpringJoint2D>(&key); spring != nullptr) {
            // Godot's stiffness and damping, as Box2D's frequency and damping ratio for the
            // bodies' masses.
            const float mass_a = b2Body_GetMass(body_a);
            const float mass_b = b2Body_GetMass(body_b);
            float mass = mass_a > 0.0F && mass_b > 0.0F ? (mass_a * mass_b) / (mass_a + mass_b)
                                                        : std::max(mass_a, mass_b);
            mass = std::max(mass, 1.0e-3F);
            const float stiffness = std::max(spring->stiffness, 0.0F);
            b2DistanceJointDef def = b2DefaultDistanceJointDef();
            def.bodyIdA = body_a;
            def.bodyIdB = body_b;
            const b2Vec2 end = along(spring->length);
            def.localAnchorA = b2Body_GetLocalPoint(body_a, origin);
            def.localAnchorB = b2Body_GetLocalPoint(body_b, end);
            def.length = spring->rest_length > 0.0F ? spring->rest_length : b2Distance(origin, end);
            def.enableSpring = true;
            def.hertz = std::sqrt(stiffness / mass) / (2.0F * std::numbers::pi_v<float>);
            def.dampingRatio =
                stiffness > 0.0F ? spring->damping / (2.0F * std::sqrt(stiffness * mass)) : 0.0F;
            def.collideConnected = !spring->disable_collision;
            joint.id = b2CreateDistanceJoint(world_, &def);
        } else if (const auto* groove = std::get_if<GrooveJoint2D>(&key); groove != nullptr) {
            // A wheel joint without its spring: node_b slides along the axis and turns freely.
            b2WheelJointDef def = b2DefaultWheelJointDef();
            def.bodyIdA = body_a;
            def.bodyIdB = body_b;
            const b2Vec2 groove_end = along(std::max(groove->length, 0.0F));
            def.localAnchorA = b2Body_GetLocalPoint(body_a, origin);
            def.localAnchorB = b2Body_GetLocalPoint(body_b, along(groove->initial_offset));
            b2Vec2 axis = b2Sub(groove_end, origin);
            const float groove_length = b2Length(axis);
            if (groove_length <= 0.0F) {
                axis = b2Sub(along(1.0F), origin);
            }
            def.localAxisA = b2Body_GetLocalVector(body_a, b2Normalize(axis));
            def.enableSpring = false;
            def.enableLimit = true;
            def.lowerTranslation = 0.0F;
            def.upperTranslation = groove_length;
            def.enableMotor = false;
            def.collideConnected = !groove->disable_collision;
            joint.id = b2CreateWheelJoint(world_, &def);
        }
        joints_.emplace(entity, joint);
    };

    for (const entt::entity entity : registry.view<PinJoint2D>()) {
        const auto& pin = registry.get<PinJoint2D>(entity);
        place(entity, pin, pin.node_a, pin.node_b);
    }
    for (const entt::entity entity : registry.view<DampedSpringJoint2D>()) {
        if (registry.all_of<PinJoint2D>(entity)) {
            continue;
        }
        const auto& spring = registry.get<DampedSpringJoint2D>(entity);
        place(entity, spring, spring.node_a, spring.node_b);
    }
    for (const entt::entity entity : registry.view<GrooveJoint2D>()) {
        if (registry.any_of<PinJoint2D, DampedSpringJoint2D>(entity)) {
            continue;
        }
        const auto& groove = registry.get<GrooveJoint2D>(entity);
        place(entity, groove, groove.node_a, groove.node_b);
    }
    // Past a limit a pin's motor stops pushing, as in Godot's GodotPinJoint2D::solve, so it
    // can't drive through the limit.
    for (const entt::entity entity : registry.view<PinJoint2D>()) {
        const auto& pin = registry.get<PinJoint2D>(entity);
        const Joint* joint = joints_.find(entity);
        if (!pin.motor_enabled || !pin.angular_limit_enabled || joint == nullptr ||
            !b2Joint_IsValid(joint->id)) {
            continue;
        }
        const float angle = b2RevoluteJoint_GetAngle(joint->id);
        const bool past = angle > std::clamp(pin.angular_limit_upper, 0.0F, kMaxJointAngle) ||
                          angle < std::clamp(pin.angular_limit_lower, -kMaxJointAngle, 0.0F);
        if (b2RevoluteJoint_IsMotorEnabled(joint->id) == past) {
            b2RevoluteJoint_EnableMotor(joint->id, !past);
        }
    }
}

void Space::build_tile_shapes(world::World& world, Body& body, TileShapes tiles, Vec2 scale) {
    const bool rebuilt = !body.shapes.empty();
    if (rebuilt) {
        wake_near(world_, body.id, b2Body_ComputeAABB(body.id));
    }
    for (const b2ShapeId shape : body.shapes) {
        if (b2Shape_IsValid(shape)) {
            b2DestroyShape(shape, false);
        }
    }
    body.shapes.clear();
    body.tiles = std::move(tiles);
    body.shape_key.scale = scale;
    body.info.clear();
    body.info.reserve(body.tiles->size()); // Box2D's shapes point into it
    bool warned = false;
    for (const TileShape& tile : *body.tiles) {
        ShapeInfo& info = body.info.emplace_back(ShapeInfo{.entity = body.entity,
                                                           .kind = Kind::Static,
                                                           .layer = tile.collision_layer,
                                                           .mask = tile.collision_mask,
                                                           .monitorable = true,
                                                           .one_way = tile.one_way,
                                                           .one_way_margin = tile.one_way_margin,
                                                           .one_way_up = {0.0F, -1.0F},
                                                           .cell = tile.cell});
        b2ShapeDef def = b2DefaultShapeDef();
        def.material.friction = tile.friction;
        def.material.restitution = tile.bounce;
        def.density = 1.0F;
        def.enableSensorEvents = true;
        def.updateBodyMass = false;
        def.filter.categoryBits = tile.collision_layer | kShapeBit;
        def.filter.maskBits = ~std::uint64_t{0};
        def.enablePreSolveEvents = tile.one_way;
        def.userData = &info;
        std::vector<Vec2> points;
        points.reserve(tile.points.size());
        for (const Vec2 point : tile.points) {
            points.push_back({point.x * scale.x, point.y * scale.y});
        }
        auto pieces = decompose(points, B2_MAX_POLYGON_VERTICES);
        if (!pieces) {
            if (!warned) {
                warn_shape(world, body.entity,
                           "the tile at [" + std::to_string(tile.cell.x) + ", " +
                               std::to_string(tile.cell.y) + "]: " + pieces.error().message);
                warned = true;
            }
            continue;
        }
        for (const Polygon& piece : pieces.value()) {
            std::vector<b2Vec2> corners;
            corners.reserve(piece.size());
            for (const Vec2 point : piece) {
                corners.push_back(to_b2(point));
            }
            const b2Hull hull = b2ComputeHull(corners.data(), static_cast<int>(corners.size()));
            if (hull.count == 0) {
                continue; // a sliver too thin for Box2D; its neighbors cover it
            }
            const b2Polygon polygon = b2MakePolygon(&hull, 0.0F);
            body.shapes.push_back(b2CreatePolygonShape(body.id, &def, &polygon));
        }
    }
    if (rebuilt && !body.shapes.empty()) {
        wake_near(world_, body.id, b2Body_ComputeAABB(body.id));
    }
}

void Space::sync_tiles(world::World& world, const world::Tree2D& tree) {
    entt::registry& registry = world.ecs();
    const auto* source = registry.ctx().find<TileShapeSource>();
    const auto shapes_of = [&](entt::entity entity) -> TileShapes {
        if (source == nullptr || !*source || !world.valid(entity) ||
            !registry.all_of<TileMapLayer>(entity) ||
            !registry.get<TileMapLayer>(entity).collision_enabled) {
            return nullptr;
        }
        const TileShapes shapes = (*source)(world, entity);
        return shapes != nullptr && !shapes->empty() ? shapes : nullptr;
    };
    std::vector<std::pair<entt::entity, TileShapes>> layers;
    if (source != nullptr) {
        for (const entt::entity entity : registry.view<TileMapLayer>()) {
            if (TileShapes shapes = shapes_of(entity); shapes != nullptr) {
                layers.emplace_back(entity, std::move(shapes));
            }
        }
    }
    tiles_.erase_if([&](entt::entity entity, const std::unique_ptr<Body>& body) {
        const bool kept = std::ranges::any_of(
            layers, [entity](const auto& layer) { return layer.first == entity; });
        if (!kept) {
            if (!body->shapes.empty()) {
                wake_near(world_, body->id, b2Body_ComputeAABB(body->id));
            }
            removed_.insert(body->name);
            destroy(*body);
        }
        return !kept;
    });
    for (auto& [entity, shapes] : layers) {
        const Transform2D transform = transform_of(tree, entity);
        std::unique_ptr<Body>* found = tiles_.find(entity);
        if (found == nullptr) {
            auto body = std::make_unique<Body>();
            body->entity = entity;
            body->name = world.id_of(entity);
            body->kind = Kind::Static;
            b2BodyDef def = b2DefaultBodyDef();
            def.type = b2_staticBody;
            def.position = to_b2(transform.position);
            def.rotation = b2MakeRot(transform.rotation);
            def.userData = body.get();
            body->id = b2CreateBody(world_, &def);
            body->position = transform.position;
            body->rotation = transform.rotation;
            found = &tiles_.emplace(entity, std::move(body));
            build_tile_shapes(world, **found, std::move(shapes), transform.scale);
            continue;
        }
        Body& body = **found;
        if (body.tiles != shapes || body.shape_key.scale != transform.scale) {
            build_tile_shapes(world, body, std::move(shapes), transform.scale);
        }
        if (transform.position != body.position || transform.rotation != body.rotation) {
            const b2AABB before = b2Body_ComputeAABB(body.id);
            b2Body_SetTransform(body.id, to_b2(transform.position), b2MakeRot(transform.rotation));
            body.position = transform.position;
            body.rotation = transform.rotation;
            wake_near(world_, body.id, b2AABB_Union(before, b2Body_ComputeAABB(body.id)));
        }
    }
}

void Space::sync(world::World& world) {
    const world::Tree2D tree(world);
    sync(world, tree);
}

void Space::sync(world::World& world, const world::Tree2D& tree) {
    entt::registry& registry = world.ecs();
    if (!b2World_IsValid(world_)) {
        const bool tiles =
            registry.ctx().contains<TileShapeSource>() && has_any<TileMapLayer>(registry);
        if (!tiles &&
            !has_any<RigidBody2D, CharacterBody2D, AnimatableBody2D, StaticBody2D, Area2D>(
                registry)) {
            return;
        }
        create_world(world.pool());
    }
    remove_stale(world);
    // New bodies are made in the same order on every machine: by kind, then in each
    // component's storage order.
    const auto each = [&]<typename Component>(Kind kind) {
        for (const entt::entity entity : registry.view<Component>()) {
            if (kind_of(registry, entity) == kind) {
                sync_body(world, tree, entity, kind);
            }
        }
    };
    each.template operator()<RigidBody2D>(Kind::Rigid);
    each.template operator()<CharacterBody2D>(Kind::Character);
    each.template operator()<AnimatableBody2D>(Kind::Animatable);
    each.template operator()<StaticBody2D>(Kind::Static);
    each.template operator()<Area2D>(Kind::Area);
    sync_tiles(world, tree);
    sync_joints(world, tree);
}

void Space::read_back(world::World& world, const world::Tree2D& tree) {
    entt::registry& registry = world.ecs();
    const b2BodyEvents events = b2World_GetBodyEvents(world_);
    std::vector<const b2BodyMoveEvent*> moves;
    moves.reserve(static_cast<std::size_t>(events.moveCount));
    for (int i = 0; i < events.moveCount; ++i) {
        moves.push_back(&events.moveEvents[i]);
    }
    if (tree.nested()) {
        // Parents first, so a body inside another is placed in its parent's new space.
        const auto depth = [&](const b2BodyMoveEvent* event) {
            const auto* body = static_cast<const Body*>(event->userData);
            std::size_t count = 0;
            if (body != nullptr) {
                for (entt::entity up = tree.parent(body->entity); up != entt::null;
                     up = tree.parent(up)) {
                    ++count;
                }
            }
            return count;
        };
        std::ranges::stable_sort(moves, {}, depth);
    }
    for (const b2BodyMoveEvent* move : moves) {
        const b2BodyMoveEvent& event = *move;
        auto* body = static_cast<Body*>(event.userData);
        if (body == nullptr || body->kind != Kind::Rigid || !world.valid(body->entity)) {
            continue;
        }
        Vec2 position = from_b2(event.transform.p);
        float rotation = b2Rot_GetAngle(event.transform.q);
        if (tree.parent(body->entity) == entt::null) {
            auto& transform = registry.get_or_emplace<Transform2D>(body->entity);
            transform.position = position;
            transform.rotation = rotation;
        } else {
            // As Godot sets a body's global transform: its own is what puts it there. The body
            // keeps the global transform that comes back, rounding and all, so the next sync
            // doesn't take the rounding for a move.
            tree.set_global_position(world, body->entity, position);
            tree.set_global_rotation(world, body->entity, rotation);
            const Transform2D placed = tree.global(body->entity);
            position = placed.position;
            rotation = placed.rotation;
        }
        auto& rigid = registry.get<RigidBody2D>(body->entity);
        rigid.linear_velocity = from_b2(b2Body_GetLinearVelocity(body->id));
        rigid.angular_velocity = b2Body_GetAngularVelocity(body->id);
        rigid.sleeping = event.fellAsleep;
        body->position = position;
        body->rotation = rotation;
        body->linear_velocity = rigid.linear_velocity;
        body->angular_velocity = rigid.angular_velocity;
        body->sleeping = rigid.sleeping;
    }
}

void Space::move_characters(world::World& world, const world::Tree2D& tree, float seconds) {
    entt::registry& registry = world.ecs();
    for (const entt::entity entity : registry.view<CharacterBody2D>()) {
        const auto* found = bodies_.find(entity);
        if (found == nullptr || (*found)->kind != Kind::Character) {
            continue;
        }
        Body& body = **found;
        auto& own = registry.get_or_emplace<Transform2D>(entity);
        // A character inside another entity moves in the world, then is put back in its
        // parent's space.
        const bool nested = tree.parent(entity) != entt::null;
        Transform2D placed = nested ? tree.global(entity) : Transform2D{};
        Transform2D& transform = nested ? placed : own;
        auto& character = registry.get<CharacterBody2D>(entity);
        move_character(world_, body, transform, character, seconds);
        if (nested) {
            tree.set_global_position(world, entity, placed.position);
            placed.position = tree.global(entity).position; // as the next sync will see it
        }
        if (transform.position != body.position || transform.rotation != body.rotation) {
            b2Body_SetTransform(body.id, to_b2(transform.position), b2MakeRot(transform.rotation));
            body.position = transform.position;
            body.rotation = transform.rotation;
        }
    }
}

namespace {

// The entities a body touches, by its contacts.
void touching_bodies(b2BodyId body, std::vector<entt::entity>& out) {
    const int capacity = b2Body_GetContactCapacity(body);
    if (capacity == 0) {
        return;
    }
    std::vector<b2ContactData> contacts(static_cast<std::size_t>(capacity));
    const int count = b2Body_GetContactData(body, contacts.data(), capacity);
    for (int i = 0; i < count; ++i) {
        const b2ContactData& contact = contacts[static_cast<std::size_t>(i)];
        float separation = std::numeric_limits<float>::max();
        for (int point = 0; point < contact.manifold.pointCount; ++point) {
            separation = std::min(separation, contact.manifold.points[point].separation);
        }
        if (separation > kLinearSlop) {
            continue; // only near: Box2D keeps contacts a little ahead
        }
        const bool mine = B2_ID_EQUALS(b2Shape_GetBody(contact.shapeIdA), body);
        const ShapeInfo* other = info_of(mine ? contact.shapeIdB : contact.shapeIdA);
        if (other != nullptr) {
            out.push_back(other->entity);
        }
    }
}

// What an area's shapes overlap, by Godot's rule: its mask has the other's layer, and another
// area must be monitorable.
void area_overlaps(const Body& area, std::vector<std::pair<entt::entity, bool>>& out) {
    for (const b2ShapeId shape : area.shapes) {
        const int capacity = b2Shape_GetSensorCapacity(shape);
        if (capacity == 0) {
            continue;
        }
        std::vector<b2ShapeId> visitors(static_cast<std::size_t>(capacity));
        const int count = b2Shape_GetSensorOverlaps(shape, visitors.data(), capacity);
        for (int i = 0; i < count; ++i) {
            const b2ShapeId visitor = visitors[static_cast<std::size_t>(i)];
            if (!b2Shape_IsValid(visitor)) {
                continue;
            }
            const ShapeInfo* other = info_of(visitor);
            if (other == nullptr || other->entity == area.entity ||
                (area.shape_key.mask & other->layer) == 0U ||
                (other->kind == Kind::Area && !other->monitorable)) {
                continue;
            }
            out.emplace_back(other->entity, other->kind == Kind::Area);
        }
    }
}

} // namespace

namespace {

// Godot's Vector2::normalized: unchanged when zero.
Vec2 normalized(Vec2 v) {
    const float length_squared = (v.x * v.x) + (v.y * v.y);
    if (length_squared == 0.0F) {
        return v;
    }
    const float length = std::sqrt(length_squared);
    return {v.x / length, v.y / length};
}

// An area's gravity at a point, as Godot's GodotArea2D::compute_gravity: along its direction,
// or toward its center, falling off with distance squared past a unit distance.
Vec2 area_gravity(const world::Tree2D& tree, entt::entity entity, const Area2D& area,
                  Vec2 position) {
    if (!area.gravity_point) {
        return area.gravity_direction * area.gravity;
    }
    const Vec2 to_center = tree.global_affine(entity).apply(area.gravity_point_center) - position;
    const float unit = area.gravity_point_unit_distance;
    if (unit > 0.0F) {
        const float length_squared = (to_center.x * to_center.x) + (to_center.y * to_center.y);
        if (length_squared <= 0.0F) {
            return {};
        }
        return normalized(to_center) * (area.gravity * unit * unit / length_squared);
    }
    return normalized(to_center) * area.gravity;
}

// A body's damping with its own, as Godot's damp modes.
float with_own(float total, const std::string& mode, float own) {
    return mode == "replace" ? own : total + own;
}

} // namespace

Space::AreaLists Space::area_lists(const world::World& world) const {
    const entt::registry& registry = world.ecs();
    AreaLists lists;
    std::vector<std::pair<entt::entity, bool>> overlaps;
    for (const auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Area || !overrides_space(registry.get<Area2D>(entity))) {
            continue;
        }
        overlaps.clear();
        area_overlaps(*body, overlaps);
        for (const auto& [other, is_area] : overlaps) {
            if (is_area) {
                continue;
            }
            std::vector<const Body*>& list = lists[other];
            if (list.empty() || list.back() != body.get()) {
                list.push_back(body.get());
            }
        }
    }
    // Highest priority first, then in the order the areas were made.
    for (auto& [entity, list] : lists) {
        std::ranges::stable_sort(list, std::greater<>(), [&](const Body* area) {
            return registry.get<Area2D>(area->entity).priority;
        });
    }
    return lists;
}

AreaForces Space::forces(const world::World& world, const world::Tree2D& tree, const Body& body,
                         const std::vector<const Body*>* areas) const {
    const entt::registry& registry = world.ecs();
    AreaForces out;
    const Vec2 position = from_b2(b2Body_GetPosition(body.id));
    bool gravity_done = false;
    bool linear_done = false;
    bool angular_done = false;
    // Godot's override modes, each from the highest priority down.
    const auto apply = [](const std::string& mode, auto value, auto& total, bool& done) {
        if (done || mode == "disabled") {
            return false;
        }
        if (mode == "combine" || mode == "combine_replace") {
            total = total + value;
            done = mode == "combine_replace";
        } else {
            total = value;
            done = mode == "replace";
        }
        return true;
    };
    const std::span<const Body* const> list =
        areas != nullptr ? std::span<const Body* const>(*areas) : std::span<const Body* const>();
    for (const Body* area_body : list) {
        if (gravity_done && linear_done && angular_done) {
            break;
        }
        const auto& area = registry.get<Area2D>(area_body->entity);
        bool used = false;
        if (!gravity_done && area.gravity_space_override != "disabled") {
            out.gravity_overridden = true;
            used = apply(area.gravity_space_override,
                         area_gravity(tree, area_body->entity, area, position), out.gravity,
                         gravity_done);
        }
        used = apply(area.linear_damp_space_override, area.linear_damp, out.linear_damp,
                     linear_done) ||
               used;
        used = apply(area.angular_damp_space_override, area.angular_damp, out.angular_damp,
                     angular_done) ||
               used;
        if (used) {
            out.areas.push_back(area_body->entity);
        }
    }
    // Then the project's, as Godot's default area.
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

AreaForces Space::forces(const world::World& world, const world::Tree2D& tree,
                         entt::entity entity) const {
    const Body* target = body(entity);
    if (target == nullptr || !b2World_IsValid(world_)) {
        return {};
    }
    const AreaLists lists = area_lists(world);
    const auto found = lists.find(entity);
    return forces(world, tree, *target, found != lists.end() ? &found->second : nullptr);
}

void Space::apply_areas(world::World& world, const world::Tree2D& tree) {
    const entt::registry& registry = world.ecs();
    const AreaLists lists = area_lists(world);
    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Rigid || b2Body_GetType(body->id) != b2_dynamicBody) {
            continue;
        }
        const RigidKey& key = body->rigid_key;
        Environment environment{.gravity_scale = key.gravity_scale,
                                .linear_damping = key.linear_damping,
                                .angular_damping = key.angular_damping,
                                .pull = {}};
        // A body in no area keeps Box2D's gravity, as before areas: the same calls, the same
        // results.
        if (const auto found = lists.find(entity); found != lists.end()) {
            const AreaForces area = forces(world, tree, *body, &found->second);
            const auto& rigid = registry.get<RigidBody2D>(entity);
            environment.linear_damping =
                with_own(area.linear_damp, rigid.linear_damp_mode, rigid.linear_damp);
            environment.angular_damping =
                with_own(area.angular_damp, rigid.angular_damp_mode, rigid.angular_damp);
            if (area.gravity_overridden) {
                environment.gravity_scale = 0.0F;
                environment.pull = area.gravity * rigid.gravity_scale;
            }
        }
        if (environment != body->environment) {
            const Environment& was = body->environment;
            if (environment.gravity_scale != was.gravity_scale) {
                b2Body_SetGravityScale(body->id, environment.gravity_scale);
            }
            if (environment.linear_damping != was.linear_damping) {
                b2Body_SetLinearDamping(body->id, environment.linear_damping);
            }
            if (environment.angular_damping != was.angular_damping) {
                b2Body_SetAngularDamping(body->id, environment.angular_damping);
            }
            // What it rests in changed, so it may not rest any more.
            if (!b2Body_IsAwake(body->id)) {
                b2Body_SetAwake(body->id, true);
            }
            body->environment = environment;
        }
        if (environment.pull != Vec2{}) {
            b2Body_ApplyForceToCenter(body->id, to_b2(environment.pull * b2Body_GetMass(body->id)),
                                      false);
        }
    }
}

void Space::step(world::World& world, float seconds) {
    const world::Tree2D tree(world);
    sync(world, tree);
    if (!b2World_IsValid(world_)) {
        impulses_.clear();
        return;
    }
    entt::registry& registry = world.ecs();

    // Animatable bodies move to their Transform2D during the step, pushing what is in the way.
    // Bodies with constant velocities move at them too, from half a step behind where they are
    // to half a step ahead, so what touches them moves as if they moved; after the step they go
    // back.
    std::vector<Body*> carriers;
    for (auto& [entity, body] : bodies_) {
        const bool animated = body->kind == Kind::Animatable &&
                              registry.get<AnimatableBody2D>(entity).sync_to_physics;
        const bool carrying =
            (body->kind == Kind::Static || body->kind == Kind::Animatable) && carries(*body);
        if (!animated && !carrying) {
            continue;
        }
        const Vec2 half = body->constant_linear_velocity * (0.5F * seconds);
        const float half_turn = body->constant_angular_velocity * (0.5F * seconds);
        if (carrying) {
            b2Body_SetTransform(body->id, to_b2(body->position - half),
                                b2MakeRot(body->rotation - half_turn));
            carriers.push_back(body.get());
        }
        const Transform2D transform = animated ? transform_of(tree, entity) : Transform2D{};
        const bool moving = animated && (transform.position != body->position ||
                                         transform.rotation != body->rotation);
        if (carrying) {
            // As Godot's kinematic bodies: the constant velocities plus the move.
            Vec2 linear = body->constant_linear_velocity;
            float angular = body->constant_angular_velocity;
            if (moving) {
                linear = linear + ((transform.position - body->position) * (1.0F / seconds));
                angular += std::remainder(transform.rotation - body->rotation,
                                          2.0F * std::numbers::pi_v<float>) /
                           seconds;
                body->position = transform.position;
                body->rotation = transform.rotation;
            }
            b2Body_SetLinearVelocity(body->id, to_b2(linear));
            b2Body_SetAngularVelocity(body->id, angular);
        } else if (moving) {
            b2Body_SetTargetTransform(
                body->id, {to_b2(transform.position), b2MakeRot(transform.rotation)}, seconds);
            body->position = transform.position;
            body->rotation = transform.rotation;
        } else if (b2Body_IsAwake(body->id)) {
            b2Body_SetLinearVelocity(body->id, b2Vec2_zero);
            b2Body_SetAngularVelocity(body->id, 0.0F);
        }
    }

    apply_areas(world, tree);

    // Constant forces every tick, and impulses queued since the last.
    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Rigid) {
            continue;
        }
        const auto& rigid = registry.get<RigidBody2D>(entity);
        if (rigid.constant_force != Vec2{}) {
            b2Body_ApplyForceToCenter(body->id, to_b2(rigid.constant_force), true);
        }
        if (rigid.constant_torque != 0.0F) {
            b2Body_ApplyTorque(body->id, rigid.constant_torque, true);
        }
    }
    for (const Impulse& impulse : impulses_) {
        const Body* target = body(impulse.entity);
        if (target == nullptr || target->kind != Kind::Rigid) {
            continue;
        }
        if (impulse.impulse != Vec2{}) {
            const b2Vec2 point = b2Add(b2Body_GetPosition(target->id), to_b2(impulse.offset));
            b2Body_ApplyLinearImpulse(target->id, to_b2(impulse.impulse), point, true);
        }
        if (impulse.torque != 0.0F) {
            b2Body_ApplyAngularImpulse(target->id, impulse.torque, true);
        }
    }
    impulses_.clear();

    b2World_Step(world_, seconds, kSubSteps);
    tasks_.groups.clear();
    for (const Body* body : carriers) {
        b2Body_SetTransform(body->id, to_b2(body->position), b2MakeRot(body->rotation));
    }
    read_back(world, tree);
    move_characters(world, tree, seconds);
    emit_events(world);
}

void Space::emit_events(world::World& world) {
    const entt::registry& registry = world.ecs();
    std::set<std::tuple<std::string, std::string, bool>> now;
    std::vector<entt::entity> bodies;
    std::vector<std::pair<entt::entity, bool>> overlaps;
    for (const auto& [entity, body] : bodies_) {
        const std::string_view id = world.id_of(entity);
        if (id.empty()) {
            continue;
        }
        if (body->kind == Kind::Rigid && registry.get<RigidBody2D>(entity).contact_monitor) {
            bodies.clear();
            touching_bodies(body->id, bodies);
            for (const entt::entity other : bodies) {
                if (const std::string_view other_id = world.id_of(other); !other_id.empty()) {
                    now.emplace(id, other_id, false);
                }
            }
        } else if (body->kind == Kind::Area && registry.get<Area2D>(entity).monitoring) {
            overlaps.clear();
            area_overlaps(*body, overlaps);
            for (const auto& [other, is_area] : overlaps) {
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

Contacts Space::contacts(const world::World& world, entt::entity entity) const {
    Contacts out;
    const Body* target = body(entity);
    if (target == nullptr || !b2World_IsValid(world_)) {
        return out;
    }
    std::vector<std::pair<entt::entity, bool>> overlaps;
    if (target->kind == Kind::Area) {
        area_overlaps(*target, overlaps);
        for (const auto& [other, is_area] : overlaps) {
            (is_area ? out.areas : out.bodies).push_back(other);
        }
    } else {
        if (target->kind == Kind::Character) {
            out.bodies = target->touching;
        } else {
            touching_bodies(target->id, out.bodies);
        }
        for (const auto& [other, area] : bodies_) {
            if (area->kind != Kind::Area) {
                continue;
            }
            overlaps.clear();
            area_overlaps(*area, overlaps);
            if (std::any_of(overlaps.begin(), overlaps.end(),
                            [&](const auto& overlap) { return overlap.first == entity; })) {
                out.areas.push_back(other);
            }
        }
    }
    const auto tidy = [&](std::vector<entt::entity>& list) {
        std::sort(list.begin(), list.end(),
                  [&](entt::entity a, entt::entity b) { return world.id_of(a) < world.id_of(b); });
        list.erase(std::unique(list.begin(), list.end()), list.end());
    };
    tidy(out.bodies);
    tidy(out.areas);
    return out;
}

} // namespace slash::physics2d::detail
