// Godot's body_test_motion on Jolt (modules/jolt_physics/spaces/
// jolt_physics_direct_space_state_3d.cpp): a body is first pushed out of what it overlaps, then
// each of its convex shapes is swept along the motion by a binary search, then what it touches
// where it stops is collected, deepest first.

#include "space.hpp"

#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/InternalEdgeRemovingCollector.h>
#include <Jolt/Physics/Collision/ManifoldBetweenTwoFaces.h>
#include <Jolt/Physics/Collision/Shape/ConvexShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <algorithm>
#include <cmath>
#include <new>
#include <optional>

namespace slash::physics3d::detail {

namespace {

// Godot's physics/jolt_physics_3d/motion_queries settings.
constexpr int kRecoveryIterations = 4;
constexpr float kRecoveryAmount = 0.4F;
constexpr int kMaxCollisions = 32;
constexpr int kMaxCastBodies = 1024;

// The first hits of a query, up to max, as Godot's JoltQueryCollectorAnyMulti.
template <typename Base>
class AnyMulti final : public Base {
public:
    using Hit = typename Base::ResultType;
    explicit AnyMulti(int max) : max_(max) {}
    void AddHit(const Hit& hit) override {
        if (static_cast<int>(hits.size()) < max_) {
            hits.push_back(hit);
        }
        if (static_cast<int>(hits.size()) == max_) {
            Base::ForceEarlyOut();
        }
    }
    void Reset() override {
        Base::Reset();
        hits.clear();
    }

    std::vector<Hit> hits;

private:
    int max_;
};

// Whether a query hit anything, as Godot's JoltQueryCollectorAny.
template <typename Base>
class Any final : public Base {
public:
    void AddHit(const typename Base::ResultType& /*hit*/) override {
        hit = true;
        Base::ForceEarlyOut();
    }

    bool hit = false;
};

// The deepest hits of a query, up to max, deepest first, as Godot's
// JoltQueryCollectorClosestMulti.
class ClosestMulti final : public JPH::CollideShapeCollector {
public:
    explicit ClosestMulti(int max) : max_(max) {}
    void AddHit(const JPH::CollideShapeResult& hit) override {
        auto at = hits.begin();
        for (; at != hits.end(); ++at) {
            if (hit.GetEarlyOutFraction() < at->GetEarlyOutFraction()) {
                break;
            }
        }
        hits.insert(at, hit);
        if (static_cast<int>(hits.size()) > max_) {
            hits.resize(static_cast<std::size_t>(max_));
        }
    }

    std::vector<JPH::CollideShapeResult> hits;

private:
    int max_;
};

// What a moving body hits, as Godot's JoltMotionFilter3D: bodies, not areas, whose collision
// layer is in its mask, but not itself, bodies a joint keeps it apart from or what the motion
// excludes.
class MotionFilter final : public JPH::BroadPhaseLayerFilter,
                           public JPH::ObjectLayerFilter,
                           public JPH::BodyFilter {
public:
    MotionFilter(const Space& space, const Body& self, const std::vector<entt::entity>& exclude)
        : space_(&space), self_(&self), exclude_(&exclude) {}

    [[nodiscard]] bool ShouldCollide(JPH::BroadPhaseLayer layer) const override {
        return layer == broad::kStatic || layer == broad::kMoving;
    }
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return (self_->mask & std::get<1>(space_->layers().from_object_layer(layer))) != 0;
    }
    [[nodiscard]] bool ShouldCollide(const JPH::BodyID& id) const override {
        return id != self_->id;
    }
    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        const Body* other = Space::body_of(body);
        return other != nullptr && !space_->excepted(self_->entity, other->entity) &&
               std::find(exclude_->begin(), exclude_->end(), other->entity) == exclude_->end();
    }

private:
    const Space* space_;
    const Body* self_;
    const std::vector<entt::entity>* exclude_;
};

// A convex shape swept along a motion, as Godot's JoltCustomMotionShape: its support in any
// direction the motion points along is moved by the motion. Only what collision needs of a
// convex shape is there; it lives on the stack for one query.
class MotionShape final : public JPH::ConvexShape {
public:
    explicit MotionShape(const JPH::ConvexShape& inner)
        : JPH::ConvexShape(JPH::EShapeSubType::UserConvex2), inner_(&inner) {}

    void set_motion(JPH::Vec3Arg motion) { motion_ = motion; }

    [[nodiscard]] JPH::AABox GetLocalBounds() const override {
        JPH::AABox box = inner_->GetLocalBounds();
        JPH::AABox moved = box;
        moved.Translate(motion_);
        box.Encapsulate(moved);
        return box;
    }
    [[nodiscard]] float GetInnerRadius() const override { return inner_->GetInnerRadius(); }
    [[nodiscard]] JPH::MassProperties GetMassProperties() const override { return {}; }
    [[nodiscard]] JPH::Vec3 GetSurfaceNormal(const JPH::SubShapeID& /*id*/,
                                             JPH::Vec3Arg /*position*/) const override {
        return JPH::Vec3::sZero();
    }
    // Internal edge removal only asks for the second shape's faces, and this is always first.
    void GetSupportingFace(const JPH::SubShapeID& /*id*/, JPH::Vec3Arg /*direction*/,
                           JPH::Vec3Arg /*scale*/, JPH::Mat44Arg /*transform*/,
                           SupportingFace& /*vertices*/) const override {}
    [[nodiscard]] const Support* GetSupportFunction(ESupportMode mode, SupportBuffer& buffer,
                                                    JPH::Vec3Arg scale) const override {
        return new (&buffer)
            MotionSupport(motion_, inner_->GetSupportFunction(mode, inner_buffer_, scale));
    }
    void CollideSoftBodyVertices(JPH::Mat44Arg /*transform*/, JPH::Vec3Arg /*scale*/,
                                 const JPH::CollideSoftBodyVertexIterator& /*vertices*/,
                                 JPH::uint /*count*/, int /*index*/) const override {}
    [[nodiscard]] Stats GetStats() const override { return {sizeof(*this), 0}; }
    [[nodiscard]] float GetVolume() const override { return 0.0F; }
#ifdef JPH_DEBUG_RENDERER
    void Draw(JPH::DebugRenderer* /*renderer*/, JPH::RMat44Arg /*transform*/,
              JPH::Vec3Arg /*scale*/, JPH::ColorArg /*color*/, bool /*materials*/,
              bool /*wireframe*/) const override {}
#endif

private:
    class MotionSupport final : public Support {
    public:
        MotionSupport(JPH::Vec3Arg motion, const Support* inner) : motion_(motion), inner_(inner) {}
        [[nodiscard]] JPH::Vec3 GetSupport(JPH::Vec3Arg direction) const override {
            JPH::Vec3 support = inner_->GetSupport(direction);
            if (direction.Dot(motion_) > 0.0F) {
                support += motion_;
            }
            return support;
        }
        [[nodiscard]] float GetConvexRadius() const override { return inner_->GetConvexRadius(); }

    private:
        JPH::Vec3 motion_;
        const Support* inner_;
    };

    const JPH::ConvexShape* inner_;
    JPH::Vec3 motion_ = JPH::Vec3::sZero();
    mutable SupportBuffer inner_buffer_{};
};

world::Affine3D translated_local(world::Affine3D transform, Vec3 offset) {
    transform.origin = transform.origin + transform.basis(offset);
    return transform;
}

world::Affine3D scaled_local(world::Affine3D transform, Vec3 scale) {
    transform.x = transform.x * scale.x;
    transform.y = transform.y * scale.y;
    transform.z = transform.z * scale.z;
    return transform;
}

// floor(log2(x)) for x at least 1, as Godot's int(logf(x) / Math_LN2), without libm.
int log2_floor(float x) {
    int exponent = 0;
    (void)std::frexp(x, &exponent);
    return exponent - 1;
}

// Godot's _body_motion_recover: pushes the body out of what it overlaps, a little at a time.
bool recover(const JPH::PhysicsSystem& system, const Body& body, const world::Affine3D& transform,
             float margin, const MotionFilter& filter, Vec3& recovery) {
    const JPH::NarrowPhaseQuery& query = system.GetNarrowPhaseQueryNoLock();
    world::Affine3D transform_com =
        translated_local(transform, from_jolt(body.shape->GetCenterOfMass()));
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = margin;
    AnyMulti<JPH::CollideShapeCollector> collector(32);
    bool recovered = false;
    for (int i = 0; i < kRecoveryIterations; ++i) {
        collector.Reset();
        const Vec3 base = transform_com.origin;
        query.CollideShapeWithInternalEdgeRemoval(body.shape.GetPtr(), JPH::Vec3::sOne(),
                                                  matrix_of(transform_com), settings, to_jolt(base),
                                                  collector, filter, filter, filter);
        if (collector.hits.empty()) {
            break;
        }
        recovered = true;
        // Every body's collision priority is Godot's default, 1, so each pushes alike.
        Vec3 step;
        for (const JPH::CollideShapeResult& hit : collector.hits) {
            const Vec3 axis = from_jolt(hit.mPenetrationAxis.Normalized());
            const Vec3 on_1 = base + from_jolt(hit.mContactPointOn1) + (axis * margin);
            const Vec3 on_2 = base + from_jolt(hit.mContactPointOn2);
            const float depth = dot(axis, on_1 + step) - dot(axis, on_2);
            if (depth <= 0.0F) {
                continue;
            }
            step = step - (axis * (depth * kRecoveryAmount));
        }
        if (step == Vec3{}) {
            break;
        }
        recovery = recovery + step;
        transform_com.origin = transform_com.origin + step;
    }
    return recovered;
}

// Godot's _cast_motion_impl: how far a convex shape moves before it hits something, by a
// binary search to about a millimeter against each body in its way.
bool cast_shape(const JPH::PhysicsSystem& system, const JPH::ConvexShape& shape,
                const world::Affine3D& transform_com, Vec3 scale, Vec3 motion,
                const MotionFilter& filter, float& closest_safe, float& closest_unsafe) {
    closest_safe = 1.0F;
    closest_unsafe = 1.0F;
    const float motion_length = length(motion);
    const JPH::Mat44 com = matrix_of(transform_com);
    const JPH::Vec3 jolt_scale = to_jolt(scale);
    const JPH::Vec3 jolt_motion = to_jolt(motion);
    const JPH::Vec3 motion_local = com.Multiply3x3Transposed(jolt_motion);

    JPH::AABox box = shape.GetWorldSpaceBounds(com, jolt_scale);
    JPH::AABox moved = box;
    moved.Translate(jolt_motion);
    box.Encapsulate(moved);
    AnyMulti<JPH::CollideShapeBodyCollector> bodies(kMaxCastBodies);
    system.GetBroadPhaseQuery().CollideAABox(box, bodies, filter, filter);
    if (bodies.hits.empty()) {
        return false;
    }

    const JPH::RVec3 base = com.GetTranslation();
    MotionShape motion_shape(shape);
    JPH::CollideShapeSettings settings;
    settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
    settings.mCollectFacesMode = JPH::ECollectFacesMode::CollectFaces;
    const auto collides = [&](const JPH::TransformedShape& other_shape, float fraction) {
        motion_shape.set_motion(motion_local * fraction);
        Any<JPH::CollideShapeCollector> collector;
        JPH::InternalEdgeRemovingCollector edges(collector,
                                                 settings.mInternalEdgeRemovalVertexToleranceSq);
        other_shape.CollideShape(&motion_shape, jolt_scale, com, settings, base, edges);
        edges.Flush();
        return collector.hit;
    };

    const int steps =
        motion_length > 0.0F ? std::clamp(log2_floor(1000.0F * motion_length), 4, 16) : 4;
    const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock();
    bool collided = false;
    for (const JPH::BodyID id : bodies.hits) {
        if (!filter.ShouldCollide(id)) {
            continue;
        }
        const JPH::Body* other = locks.TryGetBody(id);
        if (other == nullptr || !filter.ShouldCollideLocked(*other)) {
            continue;
        }
        const JPH::TransformedShape other_shape = other->GetTransformedShape();
        if (!collides(other_shape, 1.0F)) {
            continue;
        }
        float lo = 0.0F;
        float hi = 1.0F;
        float coefficient = 0.5F;
        for (int j = 0; j < steps; ++j) {
            const float fraction = lo + ((hi - lo) * coefficient);
            if (collides(other_shape, fraction)) {
                collided = true;
                hi = fraction;
                coefficient = j == 0 || lo > 0.0F ? 0.5F : 0.25F;
            } else {
                lo = fraction;
                coefficient = j == 0 || hi < 1.0F ? 0.5F : 0.75F;
            }
        }
        if (lo < closest_safe) {
            closest_safe = lo;
            closest_unsafe = hi;
        }
    }
    return collided;
}

// Godot's _body_motion_cast: each convex shape of the body swept on its own.
bool cast(const JPH::PhysicsSystem& system, const Body& body, const world::Affine3D& transform,
          Vec3 scale, Vec3 motion, const MotionFilter& filter, float& safe, float& unsafe) {
    const world::Affine3D body_transform = scaled_local(transform, scale);
    bool collided = false;
    for (const auto& [index, shape] : body.parts) {
        if (shape->GetType() != JPH::EShapeType::Convex) {
            continue;
        }
        const world::Affine3D local = translated_local(body.shape_key.parts[index].local,
                                                       from_jolt(shape->GetCenterOfMass()));
        const Unscaled com = unscale(body_transform * local);
        const Vec3 valid = from_jolt(shape->MakeScaleValid(to_jolt(com.scale)));
        float shape_safe = 1.0F;
        float shape_unsafe = 1.0F;
        collided |= cast_shape(system, static_cast<const JPH::ConvexShape&>(*shape), com.transform,
                               valid, motion, filter, shape_safe, shape_unsafe);
        safe = std::min(safe, shape_safe);
        unsafe = std::min(unsafe, shape_unsafe);
    }
    return collided;
}

// The velocity of a body at a point, as Godot's JoltBody3D::get_velocity_at_position, with a
// static or animatable body's constant velocities.
Vec3 point_velocity(const JPH::Body& jolt, const Body& body, Vec3 position) {
    const Vec3 linear = from_jolt(jolt.GetLinearVelocity()) + body.constant_linear_velocity;
    const Vec3 angular = from_jolt(jolt.GetAngularVelocity()) + body.constant_angular_velocity;
    return linear + cross(angular, position - from_jolt(jolt.GetCenterOfMassPosition()));
}

// Godot's _body_motion_collide: what the body touches at transform, deepest first.
bool collide(const JPH::PhysicsSystem& system, const Body& body, const world::Affine3D& transform,
             Vec3 motion, float margin, int max_collisions, const MotionFilter& filter,
             MotionResult& result) {
    if (max_collisions <= 0) {
        return false;
    }
    const world::Affine3D transform_com =
        translated_local(transform, from_jolt(body.shape->GetCenterOfMass()));
    JPH::CollideShapeSettings settings;
    settings.mCollectFacesMode = JPH::ECollectFacesMode::CollectFaces;
    settings.mMaxSeparationDistance = margin;
    const Vec3 base = transform_com.origin;
    ClosestMulti collector(max_collisions);
    system.GetNarrowPhaseQueryNoLock().CollideShapeWithInternalEdgeRemoval(
        body.shape.GetPtr(), JPH::Vec3::sOne(), matrix_of(transform_com), settings, to_jolt(base),
        collector, filter, filter, filter);
    const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock();
    const float tolerance_squared = system.GetPhysicsSettings().mManifoldTolerance *
                                    system.GetPhysicsSettings().mManifoldTolerance;
    const Vec3 direction = normalized(motion);
    for (const JPH::CollideShapeResult& hit : collector.hits) {
        const float depth = hit.mPenetrationDepth + margin;
        if (depth <= 0.0F) {
            continue;
        }
        const JPH::Vec3 axis = hit.mPenetrationAxis.Normalized();
        const Vec3 normal = from_jolt(-axis);
        if (dot(motion, motion) > 0.0F && dot(direction, normal) >= -kEpsilon) {
            continue;
        }
        JPH::ContactPoints points_1;
        JPH::ContactPoints points_2;
        if (max_collisions > 1) {
            // Godot's _generate_manifold.
            JPH::ManifoldBetweenTwoFaces(hit.mContactPointOn1, hit.mContactPointOn2, axis,
                                         tolerance_squared, hit.mShape1Face, hit.mShape2Face,
                                         points_1, points_2);
            if (points_1.size() > 4) {
                JPH::PruneContactPoints(axis, points_1, points_2);
            }
        } else {
            points_2.push_back(hit.mContactPointOn2);
        }
        const JPH::Body* other = locks.TryGetBody(hit.mBodyID2);
        const Body* collider = other != nullptr ? Space::body_of(*other) : nullptr;
        if (collider == nullptr) {
            continue;
        }
        for (const JPH::Vec3 point : points_2) {
            const Vec3 position = base + from_jolt(point);
            result.collisions.push_back(
                {.position = position,
                 .normal = normal,
                 .collider_velocity = point_velocity(*other, *collider, position),
                 .collider_angular_velocity =
                     from_jolt(other->GetAngularVelocity()) + collider->constant_angular_velocity,
                 .depth = depth,
                 .collider = collider->entity,
                 .collider_layer = collider->layer,
                 .collider_is_character = collider->kind == Kind::Character});
            if (static_cast<int>(result.collisions.size()) == max_collisions) {
                break;
            }
        }
        if (static_cast<int>(result.collisions.size()) == max_collisions) {
            break;
        }
    }
    return !result.collisions.empty();
}

} // namespace

std::optional<Vec3> Space::velocity_at(const Body& body, Vec3 position) const {
    if (system_ == nullptr || body.id.IsInvalid()) {
        return std::nullopt;
    }
    const JPH::Body* jolt = system_->GetBodyLockInterfaceNoLock().TryGetBody(body.id);
    if (jolt == nullptr) {
        return std::nullopt;
    }
    return point_velocity(*jolt, body, position);
}

bool Space::test_motion(const Body& body, const MotionParameters& parameters,
                        MotionResult& result) const {
    result = {};
    result.travel = parameters.motion;
    result.collision_safe_fraction = 1.0F;
    result.collision_unsafe_fraction = 1.0F;
    if (system_ == nullptr || body.id.IsInvalid() || body.shape == nullptr) {
        return false;
    }
    const float margin = std::max(parameters.margin, 0.0001F);
    const int max_collisions = std::min(parameters.max_collisions, kMaxCollisions);
    const Unscaled from = unscale(parameters.from);
    world::Affine3D transform = from.transform;
    const MotionFilter filter(*this, body, parameters.exclude);

    Vec3 recovery;
    const bool recovered = recover(*system_, body, transform, margin, filter, recovery);
    transform.origin = transform.origin + recovery;

    float safe = 1.0F;
    float unsafe = 1.0F;
    const bool hit =
        cast(*system_, body, transform, from.scale, parameters.motion, filter, safe, unsafe);

    bool collided = false;
    if (hit || (recovered && parameters.recovery_as_collision)) {
        world::Affine3D at = transform;
        at.origin = at.origin + (parameters.motion * unsafe);
        collided =
            collide(*system_, body, at, parameters.motion, margin, max_collisions, filter, result);
    }
    if (collided) {
        result.travel = recovery + (parameters.motion * safe);
        result.remainder = parameters.motion - (parameters.motion * safe);
        result.collision_depth = result.collisions.front().depth;
        result.collision_safe_fraction = safe;
        result.collision_unsafe_fraction = unsafe;
    } else {
        result.travel = recovery + parameters.motion;
        result.collisions.clear();
    }
    return collided;
}

} // namespace slash::physics3d::detail
