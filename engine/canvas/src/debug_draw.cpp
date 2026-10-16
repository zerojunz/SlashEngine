#include "slash/canvas/debug_draw.hpp"

#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <numeric>

namespace slash::canvas {
namespace {

using world::CollisionPolygon2D;
using world::CollisionShape2D;

std::uint8_t to_byte(float channel) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
}

// Twice the signed area of a, b, c: above 0 when they turn one way, below 0 the other.
float cross(Vec2 a, Vec2 b, Vec2 c) {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

Vec2 rotate(Vec2 axis, Vec2 point) {
    return {(axis.x * point.x) - (axis.y * point.y), (axis.y * point.x) + (axis.x * point.y)};
}

float length(Vec2 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

// Colors as Godot's Color methods make them.
Vec4 opaque(Vec4 color) {
    return {color.x, color.y, color.z, 1.0F};
}
Vec4 inverted(Vec4 color) {
    return {1.0F - color.x, 1.0F - color.y, 1.0F - color.z, color.w};
}
Vec4 darkened(Vec4 color, float amount) {
    return {color.x * (1.0F - amount), color.y * (1.0F - amount), color.z * (1.0F - amount),
            color.w};
}
// The gray CollisionShape2D draws a disabled shape with: Color::get_v in every channel.
Vec4 grayed(Vec4 color) {
    const float value = std::max({color.x, color.y, color.z});
    return {value, value, value, color.w * 0.5F};
}

// Where a shape's points go: as physics2d places them in the body (moved and turned by the
// shape's position and rotation, then scaled), then by the body's transform.
struct Place {
    Vec2 position;
    Vec2 shape_axis{1.0F, 0.0F}; // cos and sin of the shape's rotation
    Vec2 scale{1.0F, 1.0F};
    Vec2 origin;
    Vec2 body_axis{1.0F, 0.0F};

    Vec2 operator()(Vec2 local) const {
        const Vec2 turned = position + rotate(shape_axis, local);
        return origin + rotate(body_axis, {turned.x * scale.x, turned.y * scale.y});
    }
    // Radii scale by the larger axis, as physics2d's do.
    [[nodiscard]] float radius(float value) const {
        return value * std::max(std::abs(scale.x), std::abs(scale.y));
    }
    // The shape's +x and +y in the world, unscaled.
    [[nodiscard]] Vec2 x_axis() const { return rotate(body_axis, shape_axis); }
    [[nodiscard]] Vec2 y_axis() const { return rotate(x_axis(), {0.0F, 1.0F}); }
};

class Painter {
public:
    explicit Painter(std::vector<render::Vertex2D>& out) : out_(&out) {}

    void triangle(Vec2 a, Vec2 b, Vec2 c, Vec4 color) {
        const std::array<std::uint8_t, 4> rgba{to_byte(color.x), to_byte(color.y), to_byte(color.z),
                                               to_byte(color.w)};
        for (const Vec2 point : {a, b, c}) {
            out_->push_back({.x = point.x, .y = point.y, .u = 0.0F, .v = 0.0F, .rgba = rgba});
        }
    }

    void quad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec4 color) {
        triangle(a, b, c, color);
        triangle(a, c, d, color);
    }

    // Fills a simple polygon, as canvas_item_add_polygon.
    void fill(std::span<const Vec2> points, Vec4 color) {
        const std::vector<std::uint32_t> triangles = triangulate(points);
        for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
            triangle(points[triangles[i]], points[triangles[i + 1]], points[triangles[i + 2]],
                     color);
        }
    }

    // A line one pixel wide, as Godot's thin lines.
    void hairline(Vec2 a, Vec2 b, Vec4 color) {
        const Vec2 along = b - a;
        const float span = length(along);
        if (span <= 0.0F) {
            return;
        }
        const Vec2 side = Vec2{-along.y, along.x} * (0.5F / span);
        quad(a + side, b + side, b - side, a - side, color);
    }

    // The closed outline of a polygon, as a polyline with its last segment.
    void outline(std::span<const Vec2> points, Vec4 color) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            hairline(points[i], points[(i + 1) % points.size()], color);
        }
    }

    // A line width wide in a shape's own space, as canvas_item_add_line with a width.
    void line(const Place& place, Vec2 a, Vec2 b, float width, Vec4 color) {
        const Vec2 along = b - a;
        const float span = length(along);
        if (span <= 0.0F) {
            return;
        }
        const Vec2 side = Vec2{-along.y, along.x} * (width * 0.5F / span);
        quad(place(a - side), place(b - side), place(b + side), place(a + side), color);
    }

private:
    std::vector<render::Vertex2D>* out_;
};

// The arrow CollisionShape2D and CollisionPolygon2D draw for one-way collision: a line from
// their origin 20 pixels down +y, then a tip 8 pixels long.
void arrow(Painter& paint, const Place& place, float width, Vec4 color) {
    constexpr Vec2 kLineTo{0.0F, 20.0F};
    constexpr float kTip = 8.0F;
    constexpr float kSide = std::numbers::sqrt2_v<float> / 2.0F * kTip;
    paint.line(place, {}, kLineTo, width, color);
    paint.triangle(place(kLineTo + Vec2{0.0F, kTip}), place(kLineTo + Vec2{kSide, 0.0F}),
                   place(kLineTo + Vec2{-kSide, 0.0F}), color);
}

constexpr int kRoundPoints = 24;
constexpr float kTurnStep = 2.0F * std::numbers::pi_v<float> / static_cast<float>(kRoundPoints);

// CircleShape2D::draw's points.
std::vector<Vec2> circle_points(const Place& place, float radius) {
    const Vec2 center = place({});
    const float r = place.radius(radius);
    const Vec2 axis = place.x_axis();
    std::vector<Vec2> points;
    points.reserve(kRoundPoints);
    for (int i = 0; i < kRoundPoints; ++i) {
        points.push_back(center + rotate(axis, cos_sin(static_cast<float>(i) * kTurnStep) * r));
    }
    return points;
}

// CapsuleShape2D::_get_points: half circles round both ends, starting at the bottom.
std::vector<Vec2> capsule_points(const Place& place, float radius, float height) {
    const float half = std::max(0.0F, (height * 0.5F) - radius);
    const Vec2 top = place({0.0F, -half});
    const Vec2 bottom = place({0.0F, half});
    const float r = place.radius(radius);
    Vec2 down = place.y_axis();
    if (const float span = length(bottom - top); span > 0.0F) {
        down = (bottom - top) * (1.0F / span);
    }
    const Vec2 across{down.y, -down.x};
    const Vec2 middle = (top + bottom) * 0.5F;
    const Vec2 offset = bottom - middle;
    std::vector<Vec2> points;
    points.reserve(kRoundPoints + 2);
    for (int i = 0; i < kRoundPoints; ++i) {
        const Vec2 turn = cos_sin(static_cast<float>(i) * kTurnStep);
        const Vec2 rim = middle + (across * (turn.y * r)) + (down * (turn.x * r));
        const bool upper = i > kRoundPoints / 4 && i <= kRoundPoints * 3 / 4;
        points.push_back(upper ? rim - offset : rim + offset);
        if (i == kRoundPoints / 4 || i == kRoundPoints * 3 / 4) {
            points.push_back(upper ? rim + offset : rim - offset);
        }
    }
    return points;
}

// The convex hull of points, by Andrew's monotone chain, without collinear points.
std::vector<Vec2> convex_hull(std::vector<Vec2> points) {
    std::ranges::sort(points, [](Vec2 a, Vec2 b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    const auto [first, last] = std::ranges::unique(points);
    points.erase(first, last);
    if (points.size() < 3) {
        return {};
    }
    std::vector<Vec2> hull(points.size() * 2);
    std::size_t count = 0;
    for (const Vec2 point : points) {
        while (count >= 2 && cross(hull[count - 2], hull[count - 1], point) <= 0.0F) {
            --count;
        }
        hull[count++] = point;
    }
    const std::size_t lower = count + 1;
    for (std::size_t i = points.size() - 1; i > 0; --i) {
        const Vec2 point = points[i - 1];
        while (count >= lower && cross(hull[count - 2], hull[count - 1], point) <= 0.0F) {
            --count;
        }
        hull[count++] = point;
    }
    hull.resize(count - 1);
    return hull.size() >= 3 ? hull : std::vector<Vec2>{};
}

void draw_shape(Painter& paint, const CollisionShape2D& shape, const Place& place,
                Vec4 debug_color) {
    const Vec4 color = shape.disabled ? grayed(debug_color) : debug_color;
    std::vector<Vec2> points;
    if (shape.shape == "circle") {
        points = circle_points(place, shape.radius);
    } else if (shape.shape == "capsule") {
        points = capsule_points(place, shape.radius, shape.height);
    } else if (shape.shape == "segment") {
        paint.line(place, shape.a, shape.b, 3.0F, color);
    } else if (shape.shape == "polygon") {
        std::vector<Vec2> placed;
        placed.reserve(shape.points.size());
        for (const Vec2 point : shape.points) {
            placed.push_back(place(point));
        }
        points = convex_hull(std::move(placed));
    } else {
        const Vec2 half = shape.size * 0.5F;
        points = {place({-half.x, -half.y}), place({half.x, -half.y}), place({half.x, half.y}),
                  place({-half.x, half.y})};
    }
    paint.fill(points, color);
    paint.outline(points, opaque(color));
    if (shape.one_way_collision) {
        const Vec4 tip = inverted(debug_color);
        arrow(paint, place, 2.0F, shape.disabled ? darkened(tip, 0.25F) : tip);
    }
}

void draw_polygon(Painter& paint, const CollisionPolygon2D& polygon, const Place& place,
                  Vec4 debug_color) {
    constexpr Vec4 kStroke{0.9F, 0.2F, 0.0F, 1.0F};
    if (polygon.polygon.size() > 2) {
        std::vector<Vec2> points;
        points.reserve(polygon.polygon.size());
        for (const Vec2 point : polygon.polygon) {
            points.push_back(place(point));
        }
        paint.fill(points, polygon.disabled ? grayed(debug_color) : debug_color);
        paint.outline(points, polygon.disabled ? opaque(grayed(kStroke)) : kStroke);
    }
    if (polygon.one_way_collision) {
        arrow(paint, place, 3.0F, opaque(debug_color));
    }
}

} // namespace

CollisionShapes collision_shapes(const world::World& world, const TransformOf& transform_of,
                                 Vec4 color) {
    CollisionShapes drawn;
    Painter paint(drawn.vertices);
    const auto& ecs = world.ecs();
    std::vector<entt::entity> entities;
    for (const auto entity : ecs.view<const CollisionShape2D>()) {
        entities.push_back(entity);
    }
    for (const auto entity : ecs.view<const CollisionPolygon2D>(entt::exclude<CollisionShape2D>)) {
        entities.push_back(entity);
    }
    std::ranges::sort(entities, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    for (const entt::entity entity : entities) {
        world::Transform2D transform;
        if (const auto* current = ecs.try_get<world::Transform2D>(entity); current != nullptr) {
            transform = transform_of ? transform_of(entity, *current) : *current;
        }
        Place place{.position = {},
                    .shape_axis = {1.0F, 0.0F},
                    .scale = transform.scale,
                    .origin = transform.position,
                    .body_axis = cos_sin(transform.rotation)};
        if (const auto* shape = ecs.try_get<CollisionShape2D>(entity); shape != nullptr) {
            place.position = shape->position;
            place.shape_axis = cos_sin(shape->rotation);
            draw_shape(paint, *shape, place, color);
            ++drawn.shapes;
        }
        if (const auto* polygon = ecs.try_get<CollisionPolygon2D>(entity); polygon != nullptr) {
            place.position = {};
            place.shape_axis = {1.0F, 0.0F};
            draw_polygon(paint, *polygon, place, color);
            ++drawn.shapes;
        }
    }
    return drawn;
}

std::vector<std::uint32_t> triangulate(std::span<const Vec2> points) {
    std::vector<std::uint32_t> triangles;
    if (points.size() < 3) {
        return triangles;
    }
    double area = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec2 a = points[i];
        const Vec2 b = points[(i + 1) % points.size()];
        area += (static_cast<double>(a.x) * b.y) - (static_cast<double>(b.x) * a.y);
    }
    if (area == 0.0) {
        return triangles;
    }
    // Turns the polygon's way are convex.
    const float way = area > 0.0 ? 1.0F : -1.0F;
    const auto turn = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        return cross(points[a], points[b], points[c]) * way;
    };
    std::vector<std::uint32_t> left(points.size());
    std::iota(left.begin(), left.end(), 0U);
    while (left.size() > 3) {
        const std::size_t count = left.size();
        bool cut = false;
        for (std::size_t i = 0; i < count && !cut; ++i) {
            const std::uint32_t a = left[(i + count - 1) % count];
            const std::uint32_t b = left[i];
            const std::uint32_t c = left[(i + 1) % count];
            const float bend = turn(a, b, c);
            if (bend == 0.0F) {
                // A point in line with its neighbors, or on one, adds nothing.
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
                cut = true;
                break;
            }
            if (bend < 0.0F) {
                continue;
            }
            // An ear: no other point inside or on it.
            const bool ear = std::ranges::none_of(left, [&](std::uint32_t other) {
                const Vec2 p = points[other];
                if (p == points[a] || p == points[b] || p == points[c]) {
                    return false;
                }
                return cross(points[a], points[b], p) * way >= 0.0F &&
                       cross(points[b], points[c], p) * way >= 0.0F &&
                       cross(points[c], points[a], p) * way >= 0.0F;
            });
            if (ear) {
                triangles.insert(triangles.end(), {a, b, c});
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
                cut = true;
            }
        }
        if (!cut) {
            return {};
        }
    }
    if (turn(left[0], left[1], left[2]) > 0.0F) {
        triangles.insert(triangles.end(), {left[0], left[1], left[2]});
    }
    return triangles;
}

} // namespace slash::canvas
