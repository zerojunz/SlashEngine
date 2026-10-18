// Character movement, as Godot's CharacterBody2D.move_and_slide, on Box2D's collision tools: its
// manifolds find what the character touches, its plane solver pushes it out of what it overlaps
// and its shape casts find what it would hit.

#include "polygon.hpp"
#include "space.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace slash::physics2d::detail {

namespace {

using world::CharacterBody2D;
using world::CollisionPolygon2D;
using world::CollisionShape2D;
using world::Transform2D;

// Godot's CMP_EPSILON.
constexpr float kEpsilon = 0.00001F;
// Godot adds this to floor_max_angle and wall_min_slide_angle, so a surface right at the limit
// counts.
constexpr float kFloorAngleThreshold = 0.01F;
// How rounded a polygon character's corners are as it moves, in pixels: enough for its sweeps to
// stop it just touching what it hits, too little to see.
constexpr float kSkin = 1.0F;
// How far from a one-way shape's up side a surface may face and still hold a character, as
// Godot's: 45 degrees.
constexpr float kOneWayCos = 0.7071F;
// How much a motion must go into a surface it touches for that surface to stop it.
constexpr float kOppose = 0.001F;

bool zero_approx(b2Vec2 v) {
    return std::abs(v.x) < kEpsilon && std::abs(v.y) < kEpsilon;
}

// Godot's Vector2::slide: v without its part along the unit vector n.
b2Vec2 slide(b2Vec2 v, b2Vec2 n) {
    return b2MulSub(v, b2Dot(v, n), n);
}

// The character's shape as it moves, around its position and turned as it is: a rounded polygon
// (with two points, a capsule) or a circle. It is a linear slop larger than the body's shape,
// because Box2D leaves what it moves a slop deep in what it stops against: that way the body's
// own shape just touches.
struct MoverShape {
    b2Polygon polygon{};
    b2Circle circle{};
    bool is_circle = false;

    [[nodiscard]] b2ShapeProxy proxy() const {
        return is_circle ? b2MakeProxy(&circle.center, 1, circle.radius)
                         : b2MakeProxy(polygon.vertices, polygon.count, polygon.radius);
    }
};

MoverShape circle_mover(b2Vec2 center, float radius) {
    MoverShape mover;
    mover.is_circle = true;
    mover.circle = {.center = center, .radius = radius + kLinearSlop};
    return mover;
}

// A capsule as a two-point polygon, as Box2D's own collision code makes one.
MoverShape capsule_mover(b2Vec2 a, b2Vec2 b, float radius) {
    if (b2Distance(a, b) <= kLinearSlop) {
        return circle_mover(b2Lerp(a, b, 0.5F), radius);
    }
    MoverShape mover;
    b2Polygon& polygon = mover.polygon;
    polygon.vertices[0] = a;
    polygon.vertices[1] = b;
    polygon.centroid = b2Lerp(a, b, 0.5F);
    const b2Vec2 normal = b2RightPerp(b2Normalize(b2Sub(b, a)));
    polygon.normals[0] = normal;
    polygon.normals[1] = b2Neg(normal);
    polygon.count = 2;
    polygon.radius = radius + kLinearSlop;
    return mover;
}

// A convex polygon as the same outline with corners rounded by the skin: its corners move in by
// the skin, then a radius of the skin goes round them.
std::optional<MoverShape> polygon_mover(std::span<const b2Vec2> points) {
    if (points.size() < 3 || points.size() > B2_MAX_POLYGON_VERTICES) {
        return std::nullopt;
    }
    const b2Hull hull = b2ComputeHull(points.data(), static_cast<int>(points.size()));
    if (hull.count < 3) {
        return std::nullopt;
    }
    const auto count = static_cast<std::size_t>(hull.count);
    const auto normal = [&](std::size_t i) {
        return b2Normalize(b2CrossVS(b2Sub(hull.points[(i + 1) % count], hull.points[i]), 1.0F));
    };
    // A small polygon gets a smaller skin, though no thinner than Box2D's linear slop.
    static constexpr std::array kSkins{kSkin, kSkin * 0.5F};
    static_assert(kSkin * 0.5F >= kLinearSlop);
    for (const float skin : kSkins) {
        std::array<b2Vec2, B2_MAX_POLYGON_VERTICES> inset{};
        bool fits = true;
        for (std::size_t i = 0; i < count && fits; ++i) {
            const b2Vec2 before = normal((i + count - 1) % count);
            const b2Vec2 after = normal(i);
            const float bend = 1.0F + b2Dot(before, after);
            fits = bend > 0.1F;
            inset[i] = b2MulSub(hull.points[i], skin / std::max(bend, 0.1F), b2Add(before, after));
        }
        // Every new corner must be at least the skin inside every edge.
        for (std::size_t i = 0; i < count && fits; ++i) {
            for (std::size_t edge = 0; edge < count && fits; ++edge) {
                fits = b2Dot(normal(edge), b2Sub(inset[i], hull.points[edge])) <= -0.99F * skin;
            }
        }
        const b2Hull inner = b2ComputeHull(inset.data(), hull.count);
        if (fits && inner.count == hull.count) {
            MoverShape mover;
            mover.polygon = b2MakePolygon(&inner, skin + kLinearSlop);
            return mover;
        }
    }
    MoverShape mover;
    mover.polygon = b2MakePolygon(&hull, kLinearSlop);
    return mover;
}

// One source's shape to move as: its CollisionShape2D, else the convex hull of its
// CollisionPolygon2D; none if it has neither.
std::optional<MoverShape> mover_shape(const ShapeSource& source, const Transform2D& transform) {
    const b2Rot turn = b2MakeRot(transform.rotation);
    const std::optional<CollisionShape2D>& shape = source.shape;
    if (shape && !shape->disabled) {
        const ShapePlacement place{.rotation = b2MakeRot(shape->rotation),
                                   .position = shape->position,
                                   .scale = transform.scale,
                                   .owner = source.owner};
        const auto point = [&](Vec2 local) { return b2RotateVector(turn, place.point(local)); };
        if (shape->shape == "circle") {
            return shape->radius > 0.0F
                       ? std::optional(circle_mover(point({}), place.radius(shape->radius)))
                       : std::nullopt;
        }
        if (shape->shape == "capsule") {
            const float half = std::max(0.0F, (shape->height * 0.5F) - shape->radius);
            return shape->radius > 0.0F
                       ? std::optional(capsule_mover(point({0.0F, -half}), point({0.0F, half}),
                                                     place.radius(shape->radius)))
                       : std::nullopt;
        }
        if (shape->shape == "segment") {
            return capsule_mover(point(shape->a), point(shape->b), 0.0F);
        }
        std::vector<b2Vec2> points;
        if (shape->shape == "polygon") {
            points.reserve(shape->points.size());
            for (const Vec2 local : shape->points) {
                points.push_back(point(local));
            }
        } else {
            const float hx = shape->size.x * 0.5F;
            const float hy = shape->size.y * 0.5F;
            points = {point({-hx, -hy}), point({hx, -hy}), point({hx, hy}), point({-hx, hy})};
        }
        if (points.size() > B2_MAX_POLYGON_VERTICES) {
            std::vector<Vec2> outline;
            outline.reserve(points.size());
            for (const b2Vec2 corner : points) {
                outline.push_back(from_b2(corner));
            }
            points.clear();
            for (const Vec2 corner : convex_hull(outline)) {
                points.push_back(to_b2(corner));
            }
        }
        return polygon_mover(points);
    }
    const std::optional<CollisionPolygon2D>& polygon = source.polygon;
    if (polygon && !polygon->disabled) {
        const ShapePlacement place{.rotation = b2Rot_identity,
                                   .position = {},
                                   .scale = transform.scale,
                                   .owner = source.owner};
        std::vector<Vec2> outline;
        outline.reserve(polygon->polygon.size());
        for (const Vec2 local : polygon->polygon) {
            outline.push_back(from_b2(b2RotateVector(turn, place.point(local))));
        }
        std::vector<b2Vec2> points;
        for (const Vec2 corner : convex_hull(outline)) {
            points.push_back(to_b2(corner));
        }
        return polygon_mover(points);
    }
    return std::nullopt;
}

// The shape a character moves as: the first of its shapes (its own, then its children's) that
// isn't disabled, a CollisionPolygon2D as its convex hull; none if it has none.
std::optional<MoverShape> mover_shape(const ShapeKey& key, const Transform2D& transform) {
    for (const ShapeSource& source : key.sources) {
        if (auto mover = mover_shape(source, transform)) {
            return mover;
        }
    }
    return std::nullopt;
}

// A shape's outline for Box2D's distance and cast functions, in its body's frame.
b2ShapeProxy proxy_of(b2ShapeId shape) {
    switch (b2Shape_GetType(shape)) {
    case b2_circleShape: {
        const b2Circle circle = b2Shape_GetCircle(shape);
        return b2MakeProxy(&circle.center, 1, circle.radius);
    }
    case b2_capsuleShape: {
        const b2Capsule capsule = b2Shape_GetCapsule(shape);
        const std::array<b2Vec2, 2> points{capsule.center1, capsule.center2};
        return b2MakeProxy(points.data(), 2, capsule.radius);
    }
    case b2_segmentShape: {
        const b2Segment segment = b2Shape_GetSegment(shape);
        const std::array<b2Vec2, 2> points{segment.point1, segment.point2};
        return b2MakeProxy(points.data(), 2, 0.0F);
    }
    case b2_polygonShape: {
        const b2Polygon polygon = b2Shape_GetPolygon(shape);
        return b2MakeProxy(polygon.vertices, polygon.count, polygon.radius);
    }
    default:
        return b2MakeProxy(&b2Vec2_zero, 1, 0.0F);
    }
}

float min_separation(const b2Manifold& manifold, int* deepest = nullptr) {
    float separation = FLT_MAX;
    for (int i = 0; i < manifold.pointCount; ++i) {
        if (manifold.points[i].separation < separation) {
            separation = manifold.points[i].separation;
            if (deepest != nullptr) {
                *deepest = i;
            }
        }
    }
    return separation;
}

// Whether a one-way shape holds a character touching it along normal (from the shape) at a
// depth, as Godot's: the character comes from within 45 degrees of its up side and is no deeper
// than the shape's margin or its own, whichever is more.
bool one_way_holds(b2ShapeId shape, const ShapeInfo& info, b2Vec2 normal, float depth,
                   float margin) {
    const b2Vec2 up = b2RotateVector(b2Body_GetRotation(b2Shape_GetBody(shape)), info.one_way_up);
    return b2Dot(normal, up) >= kOneWayCos && depth <= std::max(info.one_way_margin, margin);
}

// What one move found, as Godot's KinematicCollision2D.
struct MotionResult {
    bool collided = false;
    b2Vec2 travel = b2Vec2_zero;    // how far it moved, recovery included
    b2Vec2 remainder = b2Vec2_zero; // the part of the motion left
    b2Vec2 normal = b2Vec2_zero;    // the surface's, toward the character
    entt::entity collider = entt::null;
    b2BodyId collider_body = b2_nullBodyId;
    b2Vec2 collider_velocity = b2Vec2_zero; // where they touch
    std::uint32_t collider_layer = 0;
    bool collider_is_character = false;
};

// A surface the character touches: plane.normal points from the shape to the character, and
// plane.offset is how deep the grown shape is in it.
struct Contact {
    b2Plane plane{};
    b2ShapeId shape = b2_nullShapeId;
    b2Vec2 point = b2Vec2_zero;
};

struct Hit {
    float fraction = 1.0F;
    b2Vec2 normal = b2Vec2_zero;
    b2Vec2 point = b2Vec2_zero;
    b2ShapeId shape = b2_nullShapeId;
};

struct CandidateQuery {
    entt::entity self = entt::null;
    entt::entity exclude = entt::null;
    std::vector<b2ShapeId>* found = nullptr;
};

// Shapes that may stop a character: not its own, not areas.
bool collect_candidate(b2ShapeId shape, void* context) {
    const auto& query = *static_cast<CandidateQuery*>(context);
    const ShapeInfo* info = info_of(shape);
    if (info != nullptr && info->entity != query.self && info->entity != query.exclude &&
        info->kind != Kind::Area && !b2Shape_IsSensor(shape)) {
        query.found->push_back(shape);
    }
    return true;
}

// Moves a character's shape through a Box2D world, as Godot's move_and_collide. What is within
// the margin counts as touching, though never less than Box2D's linear slop: its shape casts
// stop that close.
class Mover {
public:
    Mover(b2WorldId world, entt::entity self, const MoverShape& shape, std::uint32_t mask,
          b2Vec2 start, float margin)
        : position(start), world_(world), self_(self), shape_(shape),
          filter_{.categoryBits = kQueryBit, .maskBits = mask}, margin_(margin) {}

    // Moves by motion, or stops at what it hits: first out of what it overlaps, then a surface
    // it touches and moves into stops it at once, else it sweeps along motion. With test_only it
    // stays where it was. exclude is an entity to pass through.
    MotionResult move(b2Vec2 motion, bool test_only, entt::entity exclude = entt::null) {
        const b2Vec2 start = position;
        std::vector<Contact> contacts = touching(exclude);
        if (!contacts.empty()) {
            std::vector<b2CollisionPlane> planes;
            planes.reserve(contacts.size());
            for (const Contact& contact : contacts) {
                planes.push_back({.plane = contact.plane,
                                  .pushLimit = FLT_MAX,
                                  .push = 0.0F,
                                  .clipVelocity = true});
            }
            const b2Vec2 recovery =
                b2SolvePlanes(b2Vec2_zero, planes.data(), static_cast<int>(planes.size()))
                    .translation;
            if (b2LengthSquared(recovery) > 0.0F) {
                const std::optional<Hit> hit = cast(recovery, exclude);
                position = b2MulAdd(position, hit ? hit->fraction : 1.0F, recovery);
                contacts = touching(exclude);
            }
        }

        MotionResult result;
        const b2Vec2 direction = b2Normalize(motion);
        const Contact* against = nullptr;
        float most = -kOppose;
        for (const Contact& contact : contacts) {
            const float into = b2Dot(contact.plane.normal, direction);
            if (into < most) {
                most = into;
                against = &contact;
            }
        }
        if (against != nullptr) {
            result = collided(against->shape, against->plane.normal, against->point);
            result.remainder = motion;
        } else {
            const std::optional<Hit> hit = cast(motion, exclude);
            const float fraction = hit ? hit->fraction : 1.0F;
            position = b2MulAdd(position, fraction, motion);
            if (hit) {
                result = collided(hit->shape, hit->normal, hit->point);
                result.remainder = b2MulSV(1.0F - fraction, motion);
            } else {
                // Resting against something counts too, as Godot's recovery_as_collision; what
                // it moves away from doesn't.
                contacts = touching(exclude);
                const Contact* rest = nullptr;
                for (const Contact& contact : contacts) {
                    if (b2Dot(contact.plane.normal, direction) <= kOppose &&
                        (rest == nullptr || contact.plane.offset > rest->plane.offset)) {
                        rest = &contact;
                    }
                }
                if (rest != nullptr) {
                    result = collided(rest->shape, rest->plane.normal, rest->point);
                }
            }
        }
        result.travel = b2Sub(position, start);
        if (test_only) {
            position = start;
        }
        return result;
    }

    b2Vec2 position;

private:
    [[nodiscard]] b2AABB bounds(b2Vec2 motion) const {
        const b2ShapeProxy proxy = shape_.proxy();
        b2AABB box{.lowerBound = proxy.points[0], .upperBound = proxy.points[0]};
        for (int i = 1; i < proxy.count; ++i) {
            box.lowerBound = b2Min(box.lowerBound, proxy.points[i]);
            box.upperBound = b2Max(box.upperBound, proxy.points[i]);
        }
        const float grow = proxy.radius + (4.0F * kLinearSlop) + margin_;
        box.lowerBound = b2Sub(b2Add(box.lowerBound, b2Min(motion, b2Vec2_zero)), {grow, grow});
        box.upperBound = b2Add(b2Add(box.upperBound, b2Max(motion, b2Vec2_zero)), {grow, grow});
        box.lowerBound = b2Add(box.lowerBound, position);
        box.upperBound = b2Add(box.upperBound, position);
        return box;
    }

    [[nodiscard]] std::vector<b2ShapeId> candidates(b2Vec2 motion, entt::entity exclude) const {
        std::vector<b2ShapeId> found;
        CandidateQuery query{.self = self_, .exclude = exclude, .found = &found};
        b2World_OverlapAABB(world_, bounds(motion), filter_, collect_candidate, &query);
        return found;
    }

    // The contact manifold between a shape and the character, its normal from the shape.
    [[nodiscard]] b2Manifold manifold(b2ShapeId shape) const {
        const b2Transform obstacle = b2Body_GetTransform(b2Shape_GetBody(shape));
        const b2Transform mover{.p = position, .q = b2Rot_identity};
        const b2Polygon& polygon = shape_.polygon;
        const b2Circle& circle = shape_.circle;
        const auto flipped = [](b2Manifold flip) {
            flip.normal = b2Neg(flip.normal);
            return flip;
        };
        switch (b2Shape_GetType(shape)) {
        case b2_polygonShape: {
            const b2Polygon other = b2Shape_GetPolygon(shape);
            return shape_.is_circle ? b2CollidePolygonAndCircle(&other, obstacle, &circle, mover)
                                    : b2CollidePolygons(&other, obstacle, &polygon, mover);
        }
        case b2_capsuleShape: {
            const b2Capsule other = b2Shape_GetCapsule(shape);
            return shape_.is_circle
                       ? b2CollideCapsuleAndCircle(&other, obstacle, &circle, mover)
                       : flipped(b2CollidePolygonAndCapsule(&polygon, mover, &other, obstacle));
        }
        case b2_circleShape: {
            const b2Circle other = b2Shape_GetCircle(shape);
            return shape_.is_circle
                       ? b2CollideCircles(&other, obstacle, &circle, mover)
                       : flipped(b2CollidePolygonAndCircle(&polygon, mover, &other, obstacle));
        }
        case b2_segmentShape: {
            const b2Segment other = b2Shape_GetSegment(shape);
            return shape_.is_circle ? b2CollideSegmentAndCircle(&other, obstacle, &circle, mover)
                                    : b2CollideSegmentAndPolygon(&other, obstacle, &polygon, mover);
        }
        default:
            return {};
        }
    }

    // What the character touches or overlaps now: the grown shape overlaps it, or the body's
    // own shape is within the margin of it.
    [[nodiscard]] std::vector<Contact> touching(entt::entity exclude) const {
        std::vector<Contact> contacts;
        const float within = std::max(0.0F, margin_ - kLinearSlop);
        for (const b2ShapeId shape : candidates(b2Vec2_zero, exclude)) {
            const b2Manifold found = manifold(shape);
            int deepest = 0;
            const float separation = min_separation(found, &deepest);
            if (found.pointCount == 0 || separation > within || !b2IsNormalized(found.normal)) {
                continue;
            }
            const ShapeInfo& info = *info_of(shape);
            if (info.one_way &&
                !one_way_holds(shape, info, found.normal, -(separation + kLinearSlop), margin_)) {
                continue;
            }
            contacts.push_back({.plane = {.normal = found.normal, .offset = -separation},
                                .shape = shape,
                                .point = found.points[deepest].point});
        }
        return contacts;
    }

    // The first thing a sweep along motion hits. What the character already overlaps is not hit:
    // recovery pushes it out.
    [[nodiscard]] std::optional<Hit> cast(b2Vec2 motion, entt::entity exclude) const {
        if (zero_approx(motion)) {
            return std::nullopt;
        }
        std::optional<Hit> best;
        float fraction = 1.0F;
        const b2ShapeProxy mover = shape_.proxy();
        for (const b2ShapeId shape : candidates(motion, exclude)) {
            const ShapeInfo& info = *info_of(shape);
            if (info.one_way) {
                // A one-way shape the character is already in doesn't stop it.
                const float separation = min_separation(manifold(shape));
                if (separation < FLT_MAX &&
                    -(separation + kLinearSlop) > std::max(info.one_way_margin, margin_)) {
                    continue;
                }
            }
            const b2ShapeCastPairInput input{.proxyA = proxy_of(shape),
                                             .proxyB = mover,
                                             .transformA =
                                                 b2Body_GetTransform(b2Shape_GetBody(shape)),
                                             .transformB = {.p = position, .q = b2Rot_identity},
                                             .translationB = motion,
                                             .maxFraction = fraction,
                                             .canEncroach = true};
            const b2CastOutput output = b2ShapeCast(&input);
            if (!output.hit || output.fraction <= 0.0F || output.fraction >= fraction ||
                !b2IsNormalized(output.normal)) {
                continue;
            }
            if (info.one_way && !one_way_holds(shape, info, output.normal, 0.0F, margin_)) {
                continue;
            }
            fraction = output.fraction;
            best = Hit{.fraction = fraction,
                       .normal = output.normal,
                       .point = output.point,
                       .shape = shape};
        }
        return best;
    }

    [[nodiscard]] static MotionResult collided(b2ShapeId shape, b2Vec2 normal, b2Vec2 point) {
        const ShapeInfo& info = *info_of(shape);
        const b2BodyId body = b2Shape_GetBody(shape);
        return {.collided = true,
                .travel = b2Vec2_zero,
                .remainder = b2Vec2_zero,
                .normal = normal,
                .collider = info.entity,
                .collider_body = body,
                .collider_velocity = b2Body_GetWorldPointVelocity(body, point),
                .collider_layer = info.layer,
                .collider_is_character = info.kind == Kind::Character};
    }

    b2WorldId world_;
    entt::entity self_;
    MoverShape shape_;
    b2QueryFilter filter_;
    float margin_;
};

// One move_and_slide: Godot's algorithm (scene/2d/physics/character_body_2d.cpp), step for step.
class Slide {
public:
    Slide(Mover& mover, const CharacterBody2D& character, float seconds)
        : mover_(mover), character_(character), delta_(seconds), margin_(character.safe_margin),
          velocity_(to_b2(character.velocity)), up_(b2Normalize(to_b2(character.up_direction))),
          grounded_(character.motion_mode != "floating" && !zero_approx(up_)),
          floor_cos_(b2ComputeCosSin(character.floor_max_angle + kFloorAngleThreshold).cosine),
          floor_normal_(to_b2(character.floor_normal)) {}

    void run(const Body& body) {
        b2Vec2 platform_velocity = to_b2(character_.platform_velocity);
        if ((character_.on_floor || character_.on_wall) && body.platform != entt::null) {
            // Only platforms in the layers it rides take it along.
            const std::uint32_t layers = character_.on_floor ? character_.platform_floor_layers
                                                             : character_.platform_wall_layers;
            if ((layers & body.platform_layer) != 0U && b2Body_IsValid(body.platform_body)) {
                platform_velocity =
                    b2Body_GetWorldPointVelocity(body.platform_body, mover_.position);
            } else {
                platform_velocity = b2Vec2_zero;
            }
        }

        const bool was_on_floor = character_.on_floor;
        if (!zero_approx(platform_velocity)) {
            const MotionResult result =
                mover_.move(b2MulSV(delta_, platform_velocity), false, body.platform);
            if (result.collided) {
                results_.push_back(result);
                set_direction(result);
            }
        }

        if (grounded_) {
            move_grounded(was_on_floor);
        } else {
            move_floating();
        }

        const std::string& on_leave = character_.platform_on_leave;
        if (on_leave != "do_nothing" && !floor_ && !wall_) {
            if (on_leave == "add_upward_velocity" && b2Dot(platform_velocity, up_) < 0.0F) {
                platform_velocity = slide(platform_velocity, up_);
            }
            velocity_ = b2Add(velocity_, platform_velocity);
        }
    }

    void write(world::Transform2D& transform, CharacterBody2D& character, Body& body) const {
        transform.position = from_b2(mover_.position);
        character.velocity = from_b2(velocity_);
        character.on_floor = floor_;
        character.on_wall = wall_;
        character.on_ceiling = ceiling_;
        character.floor_normal = from_b2(floor_normal_);
        character.wall_normal = from_b2(wall_normal_);
        character.platform_velocity = from_b2(platform_velocity_);
        body.touching.clear();
        for (const MotionResult& result : results_) {
            if (result.collider != entt::null) {
                body.touching.push_back(result.collider);
            }
        }
        // The floor it snapped to counts too.
        if (floor_ && platform_ != entt::null) {
            body.touching.push_back(platform_);
        }
        body.platform = platform_;
        body.platform_body = platform_body_;
        body.platform_layer = platform_layer_;
    }

private:
    void set_platform(const MotionResult& result) {
        platform_ = result.collider;
        platform_body_ = result.collider_body;
        platform_velocity_ = result.collider_velocity;
        platform_layer_ = result.collider_layer;
    }

    // Godot's _set_collision_direction: whether the surface is a floor, a wall or a ceiling.
    void set_direction(const MotionResult& result) {
        if (grounded_ && b2Dot(result.normal, up_) >= floor_cos_) {
            floor_ = true;
            floor_normal_ = result.normal;
            set_platform(result);
        } else if (grounded_ && b2Dot(result.normal, b2Neg(up_)) >= floor_cos_) {
            ceiling_ = true;
        } else {
            wall_ = true;
            wall_normal_ = result.normal;
            // A character it bumps into is no platform.
            if (!result.collider_is_character) {
                set_platform(result);
            }
        }
    }

    void reset_platform() {
        platform_ = entt::null;
        platform_body_ = b2_nullBodyId;
        platform_layer_ = 0;
        floor_normal_ = b2Vec2_zero;
        platform_velocity_ = b2Vec2_zero;
    }

    [[nodiscard]] bool floor_only() const { return floor_ && !wall_ && !ceiling_; }
    [[nodiscard]] bool wall_only() const { return wall_ && !floor_ && !ceiling_; }

    void move_grounded(bool was_on_floor) {
        b2Vec2 motion = b2MulSV(delta_, velocity_);
        const b2Vec2 motion_slide_up = slide(motion, up_);
        const b2Vec2 prev_floor_normal = floor_normal_;
        reset_platform();

        // No sliding on the first try, so standing on a slope stays put.
        bool sliding_enabled = !character_.floor_stop_on_slope;
        // Constant speed applies only the first time sliding is on.
        bool can_apply_constant_speed = sliding_enabled;
        bool apply_ceiling_velocity = false;
        bool first_slide = true;
        const bool vel_dir_facing_up = b2Dot(velocity_, up_) > 0.0F;
        b2Vec2 last_travel = b2Vec2_zero;

        for (int iteration = 0; iteration < character_.max_slides; ++iteration) {
            const b2Vec2 prev_position = mover_.position;
            const MotionResult result = mover_.move(motion, false);
            bool collided = result.collided;
            if (collided) {
                results_.push_back(result);
                set_direction(result);

                // A ceiling platform moving down pushes the character down with it.
                if (ceiling_ && result.collider_velocity != b2Vec2_zero &&
                    b2Dot(result.collider_velocity, up_) < 0.0F) {
                    if (!character_.slide_on_ceiling || b2Dot(motion, up_) < 0.0F ||
                        b2Length(b2Add(result.normal, up_)) < 0.01F) {
                        apply_ceiling_velocity = true;
                        const b2Vec2 ceiling_vertical =
                            b2MulSV(b2Dot(up_, result.collider_velocity), up_);
                        const b2Vec2 motion_vertical = b2MulSV(b2Dot(up_, velocity_), up_);
                        if (b2Dot(motion_vertical, up_) > 0.0F ||
                            b2LengthSquared(ceiling_vertical) > b2LengthSquared(motion_vertical)) {
                            velocity_ = b2Add(ceiling_vertical, slide(velocity_, up_));
                        }
                    }
                }

                // Falling straight onto a floor with stop on slope: stop there.
                if (floor_ && character_.floor_stop_on_slope &&
                    b2Length(b2Add(b2Normalize(velocity_), up_)) < 0.01F) {
                    if (b2Length(result.travel) <= margin_ + kEpsilon) {
                        mover_.position = b2Sub(mover_.position, result.travel);
                    }
                    velocity_ = b2Vec2_zero;
                    motion = b2Vec2_zero;
                    break;
                }

                if (zero_approx(result.remainder)) {
                    motion = b2Vec2_zero;
                    break;
                }

                if (character_.floor_block_on_wall && wall_ &&
                    b2Dot(motion_slide_up, result.normal) <= 0.0F) {
                    // Walking into a wall too steep to climb.
                    if (was_on_floor && !floor_ && !vel_dir_facing_up) {
                        // A large move may stop short of the wall, so only a small one undoes.
                        if (b2Length(result.travel) <= margin_ + kEpsilon) {
                            mover_.position = b2Sub(mover_.position, result.travel);
                        }
                        snap_on_floor(true, false, true);
                        velocity_ = b2Vec2_zero;
                        motion = b2Vec2_zero;
                        break;
                    }
                    if (!floor_) {
                        // Off the floor, it may only slide along the wall up or down.
                        motion = slide(b2MulSV(b2Dot(up_, result.remainder), up_), result.normal);
                    } else {
                        motion = result.remainder;
                    }
                } else if (character_.floor_constant_speed && floor_only() &&
                           can_apply_constant_speed && was_on_floor &&
                           b2Dot(motion, result.normal) < 0.0F) {
                    // Up a slope at the speed it walks on the flat.
                    can_apply_constant_speed = false;
                    const b2Vec2 along = b2Normalize(slide(result.remainder, result.normal));
                    motion =
                        b2MulSV(b2Length(motion_slide_up) - b2Length(slide(result.travel, up_)) -
                                    b2Length(slide(last_travel, up_)),
                                along);
                } else if ((sliding_enabled || !floor_) &&
                           (!ceiling_ || character_.slide_on_ceiling || !vel_dir_facing_up) &&
                           !apply_ceiling_velocity) {
                    const b2Vec2 slide_motion = slide(result.remainder, result.normal);
                    motion = b2Dot(slide_motion, velocity_) > 0.0F ? slide_motion : b2Vec2_zero;
                    if (character_.slide_on_ceiling && ceiling_) {
                        // Slide only along the input's direction, else stop, to avoid jitter
                        // against a wall.
                        velocity_ = vel_dir_facing_up ? slide(velocity_, result.normal)
                                                      : b2MulSV(b2Dot(up_, velocity_), up_);
                    }
                } else {
                    motion = result.remainder;
                    if (ceiling_ && !character_.slide_on_ceiling && vel_dir_facing_up) {
                        velocity_ = slide(velocity_, up_);
                        motion = slide(motion, up_);
                    }
                }
                last_travel = result.travel;
            } else if (character_.floor_constant_speed && first_slide &&
                       on_floor_if_snapped(was_on_floor, vel_dir_facing_up)) {
                // Down a slope it would leave by going straight: along the floor at its speed.
                can_apply_constant_speed = false;
                sliding_enabled = true;
                mover_.position = prev_position;
                const b2Vec2 along = b2Normalize(slide(motion, prev_floor_normal));
                motion = b2MulSV(b2Length(motion_slide_up), along);
                collided = true;
            }

            can_apply_constant_speed = !can_apply_constant_speed && !sliding_enabled;
            sliding_enabled = true;
            first_slide = false;
            if (!collided || zero_approx(motion)) {
                break;
            }
        }

        snap_on_floor(was_on_floor, vel_dir_facing_up, false);

        // Against a wall in the air, keep only the velocity along it.
        if (wall_only() && b2Dot(motion_slide_up, results_.front().normal) < 0) {
            const b2Vec2 slide_motion = slide(velocity_, results_.front().normal);
            const b2Vec2 vertical = b2MulSV(b2Dot(up_, velocity_), up_);
            velocity_ = b2Dot(motion_slide_up, slide_motion) < 0.0F
                            ? vertical
                            : b2Add(vertical, slide(slide_motion, up_));
        }

        // Gravity stops adding up on a floor.
        if (floor_ && !vel_dir_facing_up) {
            velocity_ = slide(velocity_, up_);
        }
    }

    // Where a floor snap would land, as Godot's _apply_floor_snap test: none if nothing is
    // within floor_snap_length below.
    [[nodiscard]] MotionResult snap_test() const {
        const float length = std::max(character_.floor_snap_length, margin_);
        return mover_.move(b2MulSV(-length, up_), true);
    }

    // Godot's _on_floor_if_snapped: whether a snap would land on a floor.
    [[nodiscard]] bool on_floor_if_snapped(bool was_on_floor, bool vel_dir_facing_up) const {
        if (floor_ || !was_on_floor || vel_dir_facing_up) {
            return false;
        }
        const MotionResult result = snap_test();
        return result.collided && b2Dot(result.normal, up_) >= floor_cos_;
    }

    // Godot's _snap_on_floor: a character that was on a floor and isn't going up stays on it
    // if it is within floor_snap_length; with wall_as_floor, on anything but a ceiling.
    void snap_on_floor(bool was_on_floor, bool vel_dir_facing_up, bool wall_as_floor) {
        if (floor_ || !was_on_floor || vel_dir_facing_up) {
            return;
        }
        const MotionResult result = snap_test();
        if (!result.collided) {
            return;
        }
        const bool floor = b2Dot(result.normal, up_) >= floor_cos_;
        const bool ceiling = b2Dot(result.normal, b2Neg(up_)) >= floor_cos_;
        if (!floor && (!wall_as_floor || ceiling)) {
            return;
        }
        floor_ = true;
        floor_normal_ = result.normal;
        set_platform(result);
        // Straight down only, so getting unstuck doesn't move it sideways.
        const b2Vec2 travel = b2Length(result.travel) > margin_
                                  ? b2MulSV(b2Dot(up_, result.travel), up_)
                                  : b2Vec2_zero;
        mover_.position = b2Add(mover_.position, travel);
    }

    void move_floating() {
        b2Vec2 motion = b2MulSV(delta_, velocity_);
        reset_platform();
        const float min_slide_angle = character_.wall_min_slide_angle + kFloorAngleThreshold;
        const float min_slide_cos = b2ComputeCosSin(min_slide_angle).cosine;
        bool first_slide = true;
        for (int iteration = 0; iteration < character_.max_slides; ++iteration) {
            const MotionResult result = mover_.move(motion, false);
            if (result.collided) {
                results_.push_back(result);
                set_direction(result);
                if (zero_approx(result.remainder)) {
                    break;
                }
                const float head_on = b2Dot(result.normal, b2Neg(b2Normalize(velocity_)));
                if (character_.wall_min_slide_angle != 0.0F &&
                    (min_slide_angle >= std::numbers::pi_v<float> || head_on > min_slide_cos)) {
                    motion = b2Vec2_zero; // too head-on to slide
                } else if (first_slide) {
                    // The first slide keeps the motion's length.
                    const b2Vec2 along = b2Normalize(slide(result.remainder, result.normal));
                    motion = b2MulSV(b2Length(motion) - b2Length(result.travel), along);
                } else {
                    motion = slide(result.remainder, result.normal);
                }
                if (b2Dot(motion, velocity_) <= 0.0F) {
                    motion = b2Vec2_zero;
                }
            }
            if (!result.collided || zero_approx(motion)) {
                break;
            }
            first_slide = false;
        }
    }

    Mover& mover_;
    const CharacterBody2D& character_;
    float delta_;
    float margin_;
    b2Vec2 velocity_;
    b2Vec2 up_;
    bool grounded_;
    float floor_cos_;

    bool floor_ = false;
    bool wall_ = false;
    bool ceiling_ = false;
    // Godot's floor_normal is the last move's until this one resets it, so the constant speed
    // down a slope can follow it.
    b2Vec2 floor_normal_;
    b2Vec2 wall_normal_ = b2Vec2_zero;
    entt::entity platform_ = entt::null;
    b2BodyId platform_body_ = b2_nullBodyId;
    std::uint32_t platform_layer_ = 0;
    b2Vec2 platform_velocity_ = b2Vec2_zero;
    std::vector<MotionResult> results_;
};

} // namespace

void move_character(b2WorldId space, Body& body, Transform2D& transform, CharacterBody2D& character,
                    float seconds) {
    const std::optional<MoverShape> shape = mover_shape(body.shape_key, transform);
    if (!shape) {
        // With no shape it hits nothing.
        transform.position = transform.position + (character.velocity * seconds);
        character.on_floor = false;
        character.on_wall = false;
        character.on_ceiling = false;
        character.floor_normal = {};
        character.wall_normal = {};
        character.platform_velocity = {};
        body.touching.clear();
        body.platform = entt::null;
        body.platform_body = b2_nullBodyId;
        body.platform_layer = 0;
        return;
    }
    Mover mover(space, body.entity, *shape, character.collision_mask, to_b2(transform.position),
                character.safe_margin);
    Slide slide(mover, character, seconds);
    slide.run(body);
    slide.write(transform, character, body);
}

} // namespace slash::physics2d::detail
