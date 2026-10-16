#include "space.hpp"

#include "polygon.hpp"

#include "slash/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
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
};

BodyFields fields_of(const entt::registry& registry, entt::entity entity, Kind kind) {
    const auto material = [](const auto& body) {
        return BodyFields{.layer = body.collision_layer,
                          .mask = body.collision_mask,
                          .friction = body.friction,
                          .bounce = body.bounce,
                          .monitoring = true,
                          .monitorable = true};
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
                .monitorable = true};
    }
    case Kind::Area: {
        const auto& area = registry.get<Area2D>(entity);
        return {.layer = area.collision_layer,
                .mask = area.collision_mask,
                .friction = 0.0F,
                .bounce = 0.0F,
                .monitoring = area.monitoring,
                .monitorable = area.monitorable};
    }
    }
    return {};
}

ShapeKey shape_key_of(const entt::registry& registry, const world::Tree2D& tree,
                      entt::entity entity, Kind kind) {
    ShapeKey key;
    if (const auto* shape = registry.try_get<CollisionShape2D>(entity); shape != nullptr) {
        key.shape = *shape;
    }
    if (const auto* polygon = registry.try_get<CollisionPolygon2D>(entity); polygon != nullptr) {
        key.polygon = *polygon;
    }
    key.scale = transform_of(tree, entity).scale;
    const BodyFields fields = fields_of(registry, entity, kind);
    key.layer = fields.layer;
    key.mask = fields.mask;
    key.friction = fields.friction;
    key.bounce = fields.bounce;
    key.monitoring = fields.monitoring;
    key.monitorable = fields.monitorable;
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

b2BodyType type_of(Kind kind) {
    switch (kind) {
    case Kind::Rigid:
        return b2_dynamicBody;
    case Kind::Static:
        return b2_staticBody;
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
    return found != nullptr ? found->get() : nullptr;
}

void Space::create_world() {
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = to_b2(settings_.gravity_vector * settings_.gravity);
    def.frictionCallback = combine_friction;
    def.restitutionCallback = combine_bounce;
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
    for (ShapeInfo& info : body.info) {
        info = ShapeInfo{.entity = body.entity,
                         .kind = body.kind,
                         .layer = key.layer,
                         .mask = key.mask,
                         .monitorable = key.monitorable,
                         .one_way = false,
                         .one_way_margin = 1.0F,
                         .one_way_up = {0.0F, -1.0F}};
    }

    const auto shape_def = [&](ShapeInfo& info, bool one_way, float margin, b2Rot rotation) {
        b2ShapeDef def = b2DefaultShapeDef();
        def.material.friction = key.friction;
        def.material.restitution = key.bounce;
        def.density = 1.0F;
        def.isSensor = area;
        def.enableSensorEvents = !area || key.monitoring || key.monitorable;
        def.updateBodyMass = false;
        def.filter.categoryBits = key.layer | kShapeBit;
        def.filter.maskBits = ~std::uint64_t{0};
        // Areas ignore one_way_collision, as in Godot.
        if (one_way && !area) {
            info.one_way = true;
            info.one_way_margin = margin;
            info.one_way_up = b2RotateVector(rotation, {0.0F, -1.0F});
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

    if (key.shape && !key.shape->disabled) {
        const CollisionShape2D& shape = *key.shape;
        const ShapePlacement place{
            .rotation = b2MakeRot(shape.rotation), .position = shape.position, .scale = key.scale};
        const b2ShapeDef def = shape_def(body.info[0], shape.one_way_collision,
                                         shape.one_way_collision_margin, place.rotation);
        if (shape.shape == "circle") {
            const b2Circle circle{.center = place.point({}), .radius = place.radius(shape.radius)};
            if (circle.radius > 0.0F) {
                body.shapes.push_back(b2CreateCircleShape(body.id, &def, &circle));
            } else {
                warn_shape(world, body.entity, "the circle has no radius");
            }
        } else if (shape.shape == "capsule") {
            const float half = std::max(0.0F, (shape.height * 0.5F) - shape.radius);
            const b2Capsule capsule{.center1 = place.point({0.0F, -half}),
                                    .center2 = place.point({0.0F, half}),
                                    .radius = place.radius(shape.radius)};
            if (capsule.radius <= 0.0F) {
                warn_shape(world, body.entity, "the capsule has no radius");
            } else if (b2Distance(capsule.center1, capsule.center2) <= kLinearSlop) {
                const b2Circle circle{.center = place.point({}), .radius = capsule.radius};
                body.shapes.push_back(b2CreateCircleShape(body.id, &def, &circle));
            } else {
                body.shapes.push_back(b2CreateCapsuleShape(body.id, &def, &capsule));
            }
        } else if (shape.shape == "segment") {
            const b2Segment segment{.point1 = place.point(shape.a), .point2 = place.point(shape.b)};
            if (b2Distance(segment.point1, segment.point2) > kLinearSlop) {
                body.shapes.push_back(b2CreateSegmentShape(body.id, &def, &segment));
            } else {
                warn_shape(world, body.entity, "the segment has no length");
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
                warn_shape(world, body.entity, "the polygon has no area");
            }
        } else {
            const float hx = shape.size.x * 0.5F;
            const float hy = shape.size.y * 0.5F;
            const std::array<b2Vec2, 4> corners{place.point({-hx, -hy}), place.point({hx, -hy}),
                                                place.point({hx, hy}), place.point({-hx, hy})};
            if (!add_polygon(corners, def)) {
                warn_shape(world, body.entity, "the rectangle has no area");
            }
        }
    }

    if (key.polygon && !key.polygon->disabled) {
        const CollisionPolygon2D& polygon = *key.polygon;
        const ShapePlacement place{.rotation = b2Rot_identity, .position = {}, .scale = key.scale};
        const b2ShapeDef def = shape_def(body.info[1], polygon.one_way_collision,
                                         polygon.one_way_collision_margin, b2Rot_identity);
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
                warn_shape(world, body.entity, pieces.error().message);
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

    std::unique_ptr<Body>* found = bodies_.find(entity);
    if (found == nullptr) {
        auto body = std::make_unique<Body>();
        body->entity = entity;
        body->name = world.id_of(entity);
        body->kind = kind;
        b2BodyDef def = b2DefaultBodyDef();
        def.type = type_of(kind);
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
        const Transform2D transform = transform_of(tree, entity);
        const b2BodyId body_a = body_id(node_a, true);
        const b2BodyId body_b = body_id(node_b, false);
        if (const Joint* existing = joints_.find(entity); existing != nullptr) {
            const Joint& joint = *existing;
            if (b2Joint_IsValid(joint.id) && joint.key == key &&
                joint.position == transform.position && joint.rotation == transform.rotation &&
                B2_ID_EQUALS(joint.body_a, body_a) && B2_ID_EQUALS(joint.body_b, body_b)) {
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
                    .position = transform.position,
                    .rotation = transform.rotation,
                    .body_a = body_a,
                    .body_b = body_b};
        const b2Vec2 origin = to_b2(transform.position);
        const b2Vec2 y_axis = b2RotateVector(b2MakeRot(transform.rotation), {0.0F, 1.0F});
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
            def.localAnchorA = b2Body_GetLocalPoint(body_a, origin);
            def.localAnchorB =
                b2Body_GetLocalPoint(body_b, b2MulAdd(origin, spring->length, y_axis));
            def.length = spring->rest_length > 0.0F ? spring->rest_length : spring->length;
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
            def.localAnchorA = b2Body_GetLocalPoint(body_a, origin);
            def.localAnchorB =
                b2Body_GetLocalPoint(body_b, b2MulAdd(origin, groove->initial_offset, y_axis));
            def.localAxisA = b2Body_GetLocalVector(body_a, y_axis);
            def.enableSpring = false;
            def.enableLimit = true;
            def.lowerTranslation = 0.0F;
            def.upperTranslation = std::max(groove->length, 0.0F);
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
}

void Space::sync(world::World& world) {
    const world::Tree2D tree(world);
    sync(world, tree);
}

void Space::sync(world::World& world, const world::Tree2D& tree) {
    entt::registry& registry = world.ecs();
    if (!b2World_IsValid(world_)) {
        if (!has_any<RigidBody2D, CharacterBody2D, AnimatableBody2D, StaticBody2D, Area2D>(
                registry)) {
            return;
        }
        create_world();
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
        move_character(world, world_, body, transform, character, seconds);
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

void Space::step(world::World& world, float seconds) {
    const world::Tree2D tree(world);
    sync(world, tree);
    if (!b2World_IsValid(world_)) {
        impulses_.clear();
        return;
    }
    entt::registry& registry = world.ecs();

    // Animatable bodies move to their Transform2D during the step, pushing what is in the way.
    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Animatable ||
            !registry.get<AnimatableBody2D>(entity).sync_to_physics) {
            continue;
        }
        const Transform2D transform = transform_of(tree, entity);
        if (transform.position != body->position || transform.rotation != body->rotation) {
            b2Body_SetTargetTransform(
                body->id, {to_b2(transform.position), b2MakeRot(transform.rotation)}, seconds);
            body->position = transform.position;
            body->rotation = transform.rotation;
        } else if (b2Body_IsAwake(body->id)) {
            b2Body_SetLinearVelocity(body->id, b2Vec2_zero);
            b2Body_SetAngularVelocity(body->id, 0.0F);
        }
    }

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
    read_back(world, tree);
    move_characters(world, tree, seconds);
    emit_events(world);
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
                (area.info[0].mask & other->layer) == 0U ||
                (other->kind == Kind::Area && !other->monitorable)) {
                continue;
            }
            out.emplace_back(other->entity, other->kind == Kind::Area);
        }
    }
}

} // namespace

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
