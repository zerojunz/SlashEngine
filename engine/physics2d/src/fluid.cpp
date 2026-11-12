// Fluids against 2D bodies: each particle a circle collided with the shapes near it by Box2D's
// manifolds, and the push the fluid gives back to rigid bodies as impulses.
#include "slash/world/fluid.hpp"

#include "space.hpp"

#include <algorithm>
#include <box2d/box2d.h>
#include <box2d/collision.h>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace slash::physics2d::detail {

namespace {

// The most shapes one fluid tests in a step.
constexpr std::size_t kMaxFluidShapes = 512;

struct Found {
    std::vector<b2ShapeId> shapes;
};

bool collect(b2ShapeId shape, void* context) {
    auto& out = *static_cast<Found*>(context);
    const ShapeInfo* info = info_of(shape);
    if (info != nullptr && info->kind != Kind::Area && !b2Shape_IsSensor(shape)) {
        out.shapes.push_back(shape);
    }
    return out.shapes.size() < kMaxFluidShapes;
}

// The manifold between a shape and a circle at a point, its normal from the shape.
b2Manifold manifold(b2ShapeId shape, const b2Circle& circle, b2Transform at) {
    const b2Transform place = b2Body_GetTransform(b2Shape_GetBody(shape));
    switch (b2Shape_GetType(shape)) {
    case b2_polygonShape: {
        const b2Polygon other = b2Shape_GetPolygon(shape);
        return b2CollidePolygonAndCircle(&other, place, &circle, at);
    }
    case b2_capsuleShape: {
        const b2Capsule other = b2Shape_GetCapsule(shape);
        return b2CollideCapsuleAndCircle(&other, place, &circle, at);
    }
    case b2_circleShape: {
        const b2Circle other = b2Shape_GetCircle(shape);
        return b2CollideCircles(&other, place, &circle, at);
    }
    case b2_segmentShape: {
        const b2Segment other = b2Shape_GetSegment(shape);
        return b2CollideSegmentAndCircle(&other, place, &circle, at);
    }
    case b2_chainSegmentShape: {
        const b2Segment other = b2Shape_GetChainSegment(shape).segment;
        return b2CollideSegmentAndCircle(&other, place, &circle, at);
    }
    default:
        return {};
    }
}

bool by_id(b2ShapeId a, b2ShapeId b) {
    if (a.world0 != b.world0) {
        return a.world0 < b.world0;
    }
    if (a.index1 != b.index1) {
        return a.index1 < b.index1;
    }
    return a.generation < b.generation;
}

void collide(const Space& space, std::span<const Vec3f> points, float radius,
             std::vector<std::optional<world::FluidContact>>& out) {
    out.assign(points.size(), std::nullopt);
    const b2WorldId world = space.id();
    if (!b2World_IsValid(world) || points.empty()) {
        return;
    }
    b2AABB box{.lowerBound = {points[0].x, points[0].y}, .upperBound = {points[0].x, points[0].y}};
    for (const Vec3f& p : points) {
        box.lowerBound = b2Min(box.lowerBound, {p.x, p.y});
        box.upperBound = b2Max(box.upperBound, {p.x, p.y});
    }
    box.lowerBound = b2Sub(box.lowerBound, {radius, radius});
    box.upperBound = b2Add(box.upperBound, {radius, radius});
    Found found;
    b2World_OverlapAABB(world, box, {.categoryBits = kQueryBit, .maskBits = ~std::uint64_t{0}},
                        collect, &found);
    // In shape order, so steps repeat.
    std::sort(found.shapes.begin(), found.shapes.end(), by_id);
    const b2Circle circle{.center = b2Vec2_zero, .radius = radius};
    for (const b2ShapeId shape : found.shapes) {
        b2AABB bounds = b2Shape_GetAABB(shape);
        bounds.lowerBound = b2Sub(bounds.lowerBound, {radius, radius});
        bounds.upperBound = b2Add(bounds.upperBound, {radius, radius});
        const b2BodyId body = b2Shape_GetBody(shape);
        const ShapeInfo& info = *info_of(shape);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const b2Vec2 at{points[i].x, points[i].y};
            if (at.x < bounds.lowerBound.x || at.y < bounds.lowerBound.y ||
                at.x > bounds.upperBound.x || at.y > bounds.upperBound.y) {
                continue;
            }
            const b2Manifold found_manifold =
                manifold(shape, circle, b2Transform{.p = at, .q = b2Rot_identity});
            if (found_manifold.pointCount == 0 || !b2IsNormalized(found_manifold.normal)) {
                continue;
            }
            float separation = found_manifold.points[0].separation;
            for (int k = 1; k < found_manifold.pointCount; ++k) {
                separation = std::min(separation, found_manifold.points[k].separation);
            }
            const float depth = -separation;
            if (depth <= 0.0F || (out[i] && out[i]->depth >= depth)) {
                continue;
            }
            const b2Vec2 velocity = b2Body_GetWorldPointVelocity(body, at);
            out[i] = world::FluidContact{
                .normal = {found_manifold.normal.x, found_manifold.normal.y, 0.0F},
                .depth = depth,
                .velocity = {velocity.x, velocity.y, 0.0F},
                .body = info.entity};
        }
    }
}

} // namespace

world::FluidHost fluid_host_2d(const std::shared_ptr<Space>& space) {
    world::FluidHost host;
    const std::weak_ptr<Space> weak = space;
    host.collide = [weak](std::span<const Vec3f> points, float radius,
                          std::vector<std::optional<world::FluidContact>>& out) {
        if (const auto held = weak.lock(); held != nullptr) {
            collide(*held, points, radius, out);
        } else {
            out.assign(points.size(), std::nullopt);
        }
    };
    host.push = [weak](entt::entity entity, Vec3f impulse, Vec3f at) {
        const auto held = weak.lock();
        const Body* owner = held != nullptr ? std::as_const(*held).body(entity) : nullptr;
        if (owner == nullptr || owner->kind != Kind::Rigid || !b2Body_IsValid(owner->id)) {
            return;
        }
        const Vec2 origin = from_b2(b2Body_GetPosition(owner->id));
        held->queue({.entity = entity,
                     .impulse = Vec2{impulse.x, impulse.y},
                     .offset = Vec2{at.x, at.y} - origin,
                     .torque = 0.0F});
    };
    host.gravity = [weak]() {
        const auto held = weak.lock();
        if (held == nullptr) {
            return Vec3f{0.0F, 980.0F, 0.0F};
        }
        const Settings& settings = held->settings();
        const Vec2 g = settings.gravity_vector * settings.gravity;
        return Vec3f{static_cast<float>(g.x), static_cast<float>(g.y), 0.0F};
    };
    return host;
}

} // namespace slash::physics2d::detail
