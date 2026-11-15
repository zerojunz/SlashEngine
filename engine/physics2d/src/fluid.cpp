// Fluids against 2D bodies: each particle a circle collided with the shapes near it by Box2D's
// manifolds, and the push the fluid gives back to rigid bodies as impulses.
#include "slash/world/fluid.hpp"

#include "space.hpp"

#include <algorithm>
#include <array>
#include <box2d/box2d.h>
#include <box2d/collision.h>
#include <cmath>
#include <cstddef>
#include <limits>
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

// How thick a cloth's outline is to the fluid, past its points' radius, in pixels.
constexpr float kSheetThickness = 1.0F;

real_t dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

real_t length(Vec2 v) {
    return std::sqrt(dot(v, v));
}

// The side of a sheet's outline nearest a point, with the weight of its second end and how far
// it is; nullopt where none is within reach.
struct NearestSide {
    std::size_t side = 0;
    float along = 0.0F;
    Vec2 point;
    float distance = 0.0F;
};
std::optional<NearestSide> nearest_side(const Cloth2DBody& cloth, Vec2 p, float reach) {
    std::optional<NearestSide> best;
    const std::vector<Vec2>& x = cloth.points.current;
    for (std::size_t s = 0; s < cloth.outline.size(); ++s) {
        const Vec2 a = x[static_cast<std::size_t>(cloth.outline[s][0])];
        const Vec2 b = x[static_cast<std::size_t>(cloth.outline[s][1])];
        if (p.x < std::min(a.x, b.x) - reach || p.x > std::max(a.x, b.x) + reach ||
            p.y < std::min(a.y, b.y) - reach || p.y > std::max(a.y, b.y) + reach) {
            continue;
        }
        const Vec2 ab = b - a;
        const real_t span = dot(ab, ab);
        real_t t = span > 0.0F ? dot(p - a, ab) / span : 0.0F;
        t = std::clamp<real_t>(t, 0.0F, 1.0F);
        const Vec2 on = a + (ab * t);
        const auto distance = static_cast<float>(length(p - on));
        if (distance < reach && (!best || distance < best->distance)) {
            best = NearestSide{
                .side = s, .along = static_cast<float>(t), .point = on, .distance = distance};
        }
    }
    return best;
}

// Whether a point is inside one of the triangles a sheet draws.
bool inside_cloth(const Cloth2DBody& cloth, Vec2 p) {
    const std::vector<Vec2>& x = cloth.points.current;
    const auto cross = [](Vec2 a, Vec2 b, Vec2 c) {
        return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
    };
    return std::ranges::any_of(cloth.triangles, [&](const std::array<int, 3>& corners) {
        const Vec2 a = x[static_cast<std::size_t>(corners[0])];
        const Vec2 b = x[static_cast<std::size_t>(corners[1])];
        const Vec2 c = x[static_cast<std::size_t>(corners[2])];
        const real_t ab = cross(a, b, p);
        const real_t bc = cross(b, c, p);
        const real_t ca = cross(c, a, p);
        return (ab >= 0.0F && bc >= 0.0F && ca >= 0.0F) || (ab <= 0.0F && bc <= 0.0F && ca <= 0.0F);
    });
}

// Particles against each Cloth2D sheet's outline, as thick as its points' radius; a particle
// that got inside the sheet goes back out through the nearest side.
void collide_cloths(const Space& space, std::span<const Vec3f> points, float radius,
                    std::vector<std::optional<world::FluidContact>>& out) {
    for (const auto& [entity, cloth] : space.cloth_bodies()) {
        if (cloth->outline.empty() || cloth->points.current.empty()) {
            continue;
        }
        const float reach = radius + cloth->radius + kSheetThickness;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Vec2 p{points[i].x, points[i].y};
            const bool inside = inside_cloth(*cloth, p);
            const std::optional<NearestSide> near =
                nearest_side(*cloth, p, inside ? std::numeric_limits<float>::max() : reach);
            if (!near) {
                continue;
            }
            const float depth = inside ? reach + near->distance : reach - near->distance;
            if (out[i] && out[i]->depth >= depth) {
                continue;
            }
            const auto& ends = cloth->outline[near->side];
            Vec2 normal;
            if (near->distance > 1e-4F) {
                normal = (p - near->point) * ((inside ? -1.0F : 1.0F) / near->distance);
            } else {
                const Vec2 ab = cloth->points.current[static_cast<std::size_t>(ends[1])] -
                                cloth->points.current[static_cast<std::size_t>(ends[0])];
                const real_t span = length(ab);
                normal = span > 0.0F ? Vec2{-ab.y / span, ab.x / span} : Vec2{0.0F, -1.0F};
            }
            const Vec2 velocity =
                (cloth->velocity[static_cast<std::size_t>(ends[0])] * (1.0F - near->along)) +
                (cloth->velocity[static_cast<std::size_t>(ends[1])] * near->along);
            out[i] = world::FluidContact{
                .normal = {static_cast<float>(normal.x), static_cast<float>(normal.y), 0.0F},
                .depth = depth,
                .velocity = {static_cast<float>(velocity.x), static_cast<float>(velocity.y), 0.0F},
                .body = cloth->entity};
        }
    }
}

// Hands a sheet's points the push a particle of a mass gave it at a place: the nearest side's two
// ends by how near each is, each as if it and the particle moved together.
void push_cloth(Cloth2DBody& cloth, Vec2 impulse, Vec2 at, float mass) {
    if (cloth.inverse_mass.size() != cloth.points.current.size() ||
        cloth.velocity.size() != cloth.points.current.size()) {
        return;
    }
    const float reach = (2.0F * (cloth.radius + kSheetThickness)) + 1.0F;
    const std::optional<NearestSide> near = nearest_side(cloth, at, reach);
    if (!near) {
        return;
    }
    const auto& ends = cloth.outline[near->side];
    const std::array<float, 2> weights{1.0F - near->along, near->along};
    for (std::size_t e = 0; e < 2; ++e) {
        const auto point = static_cast<std::size_t>(ends[e]);
        if (cloth.inverse_mass[point] <= 0.0F || weights[e] <= 0.0F) {
            continue;
        }
        const float together = (1.0F / cloth.inverse_mass[point]) + std::max(mass, 0.0F);
        cloth.velocity[point] = cloth.velocity[point] + (impulse * (weights[e] / together));
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
    if (points.empty()) {
        return;
    }
    if (!b2World_IsValid(world)) { // no bodies yet, but there may be cloth
        collide_cloths(space, points, radius, out);
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
    collide_cloths(space, points, radius, out);
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
    host.push = [weak](entt::entity entity, Vec3f impulse, Vec3f at, float mass) {
        const auto held = weak.lock();
        if (held == nullptr) {
            return;
        }
        if (Cloth2DBody* cloth = held->cloth_body(entity); cloth != nullptr) {
            push_cloth(*cloth, Vec2{impulse.x, impulse.y}, Vec2{at.x, at.y}, mass);
            return;
        }
        const Body* owner = std::as_const(*held).body(entity);
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
