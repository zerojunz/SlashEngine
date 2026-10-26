#pragma once

// Godot's float vector math that the bakes follow bit for bit (core/math: Vector3, Plane, AABB,
// Face3 and Geometry3D's triangle and box test), shared by the lightmap's probes and VoxelGI's
// voxelizer. Godot's code is MIT; its triangle and box test is Tomas Akenine-Möller's, public
// domain.

#include "slash/core/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace slash::scene3d::detail {

// Godot's CMP_EPSILON.
inline constexpr float kEpsilon = 0.00001F;

inline float component(Vec3 v, int axis) {
    if (axis == 0) {
        return v.x;
    }
    return axis == 1 ? v.y : v.z;
}

inline void set_component(Vec3& v, int axis, float value) {
    if (axis == 0) {
        v.x = value;
    } else if (axis == 1) {
        v.y = value;
    } else {
        v.z = value;
    }
}

inline float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

inline float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// As Godot's Vector3::normalize: zero stays zero.
inline Vec3 normalized(Vec3 v) {
    const float squared = dot(v, v);
    if (squared == 0.0F) {
        return {};
    }
    const float l = std::sqrt(squared);
    return {v.x / l, v.y / l, v.z / l};
}

inline Vec3 divide(Vec3 a, Vec3 b) {
    return {a.x / b.x, a.y / b.y, a.z / b.z};
}

inline Vec3 multiply(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// As Godot's Math::is_equal_approx.
inline bool equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    float tolerance = kEpsilon * std::abs(a);
    if (tolerance < kEpsilon) {
        tolerance = kEpsilon;
    }
    return std::abs(a - b) < tolerance;
}

inline bool equal_approx(Vec3 a, Vec3 b) {
    return equal_approx(a.x, b.x) && equal_approx(a.y, b.y) && equal_approx(a.z, b.z);
}

// A plane, as Godot's: the points p with normal·p = d.
struct Plane {
    Vec3 normal;
    float d = 0.0F;

    // Plane(p1, p2, p3), clockwise, as Godot's.
    static Plane through(Vec3 a, Vec3 b, Vec3 c) {
        Plane out;
        out.normal = normalized(cross(a - c, a - b));
        out.d = dot(out.normal, a);
        return out;
    }

    [[nodiscard]] float distance_to(Vec3 p) const { return dot(normal, p) - d; }
    [[nodiscard]] bool is_point_over(Vec3 p) const { return dot(normal, p) > d; }
    [[nodiscard]] bool has_point(Vec3 p, float tolerance) const {
        return std::abs(dot(normal, p) - d) <= tolerance;
    }
    [[nodiscard]] bool equal_approx_any_side(const Plane& o) const {
        return (equal_approx(normal, o.normal) && equal_approx(d, o.d)) ||
               (equal_approx(normal, o.normal * -1.0F) && equal_approx(d, -o.d));
    }
    [[nodiscard]] bool empty() const { return normal == Vec3{} && d == 0.0F; }
    [[nodiscard]] Vec3 project(Vec3 p) const { return p - (normal * distance_to(p)); }
    // Plane::intersects_segment: where the segment from begin to end crosses it, if it does.
    [[nodiscard]] bool intersects_segment(Vec3 begin, Vec3 end, Vec3& intersection) const {
        const Vec3 segment = begin - end;
        const float den = dot(normal, segment);
        if (std::abs(den) < kEpsilon) {
            return false;
        }
        float dist = (dot(normal, begin) - d) / den;
        if (dist < -kEpsilon || dist > 1.0F + kEpsilon) {
            return false;
        }
        dist = -dist;
        intersection = begin + (segment * dist);
        return true;
    }
};

// An AABB, as Godot's.
struct Box {
    Vec3 position;
    Vec3 size;

    void expand_to(Vec3 p) {
        Vec3 begin = position;
        Vec3 end = position + size;
        begin = {std::min(begin.x, p.x), std::min(begin.y, p.y), std::min(begin.z, p.z)};
        end = {std::max(end.x, p.x), std::max(end.y, p.y), std::max(end.z, p.z)};
        position = begin;
        size = end - begin;
    }
    void grow_by(float amount) {
        position = position - Vec3{amount, amount, amount};
        size = size + Vec3{2.0F * amount, 2.0F * amount, 2.0F * amount};
    }
    [[nodiscard]] int longest_axis() const {
        int axis = 0;
        float most = size.x;
        if (size.y > most) {
            axis = 1;
            most = size.y;
        }
        if (size.z > most) {
            axis = 2;
        }
        return axis;
    }
    [[nodiscard]] Vec3 center() const { return position + (size * 0.5F); }
    [[nodiscard]] Vec3 endpoint(int i) const {
        return {(i & 4) != 0 ? position.x + size.x : position.x,
                (i & 2) != 0 ? position.y + size.y : position.y,
                (i & 1) != 0 ? position.z + size.z : position.z};
    }
    [[nodiscard]] bool has_point(Vec3 p) const {
        return p.x >= position.x && p.y >= position.y && p.z >= position.z &&
               p.x <= position.x + size.x && p.y <= position.y + size.y &&
               p.z <= position.z + size.z;
    }
    // AABB::intersects: whether they overlap by more than touching.
    [[nodiscard]] bool intersects(const Box& o) const {
        for (int axis = 0; axis < 3; ++axis) {
            if (component(position, axis) >=
                    component(o.position, axis) + component(o.size, axis) ||
                component(position, axis) + component(size, axis) <= component(o.position, axis)) {
                return false;
            }
        }
        return true;
    }
};

// Geometry3D::planeBoxOverlap.
inline bool plane_box_overlap(Vec3 normal, float d, Vec3 max_box) {
    Vec3 vmin;
    Vec3 vmax;
    for (int q = 0; q < 3; ++q) {
        const float m = component(max_box, q);
        if (component(normal, q) > 0.0F) {
            set_component(vmin, q, -m);
            set_component(vmax, q, m);
        } else {
            set_component(vmin, q, m);
            set_component(vmax, q, -m);
        }
    }
    if (dot(normal, vmin) + d > 0.0F) {
        return false;
    }
    return dot(normal, vmax) + d >= 0.0F;
}

// Geometry3D::triangle_box_overlap: Tomas Akenine-Möller's separating axis test.
inline bool triangle_box_overlap(Vec3 center, Vec3 half, const std::array<Vec3, 3>& triangle) {
    const Vec3 v0 = triangle[0] - center;
    const Vec3 v1 = triangle[1] - center;
    const Vec3 v2 = triangle[2] - center;
    const Vec3 e0 = v1 - v0;
    const Vec3 e1 = v2 - v1;
    const Vec3 e2 = v0 - v2;
    // A test along one edge's cross product with an axis: pa and pb are the two vertices'
    // projections, r the box's.
    const auto separated = [](float pa, float pb, float r) {
        const float lo = std::min(pa, pb);
        const float hi = std::max(pa, pb);
        return lo > r || hi < -r;
    };
    const auto x_test = [&](float a, float b, float fa, float fb, Vec3 p, Vec3 q) {
        return separated((a * p.y) - (b * p.z), (a * q.y) - (b * q.z),
                         (fa * half.y) + (fb * half.z));
    };
    const auto y_test = [&](float a, float b, float fa, float fb, Vec3 p, Vec3 q) {
        return separated((-a * p.x) + (b * p.z), (-a * q.x) + (b * q.z),
                         (fa * half.x) + (fb * half.z));
    };
    const auto z_test = [&](float a, float b, float fa, float fb, Vec3 p, Vec3 q) {
        return separated((a * p.x) - (b * p.y), (a * q.x) - (b * q.y),
                         (fa * half.x) + (fb * half.y));
    };
    Vec3 f{std::abs(e0.x), std::abs(e0.y), std::abs(e0.z)};
    if (x_test(e0.z, e0.y, f.z, f.y, v0, v2) || y_test(e0.z, e0.x, f.z, f.x, v0, v2) ||
        z_test(e0.y, e0.x, f.y, f.x, v1, v2)) {
        return false;
    }
    f = {std::abs(e1.x), std::abs(e1.y), std::abs(e1.z)};
    if (x_test(e1.z, e1.y, f.z, f.y, v0, v2) || y_test(e1.z, e1.x, f.z, f.x, v0, v2) ||
        z_test(e1.y, e1.x, f.y, f.x, v0, v1)) {
        return false;
    }
    f = {std::abs(e2.x), std::abs(e2.y), std::abs(e2.z)};
    if (x_test(e2.z, e2.y, f.z, f.y, v0, v1) || y_test(e2.z, e2.x, f.z, f.x, v0, v1) ||
        z_test(e2.y, e2.x, f.y, f.x, v1, v2)) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        const float a = component(v0, axis);
        const float b = component(v1, axis);
        const float c = component(v2, axis);
        const float h = component(half, axis);
        if (std::min({a, b, c}) > h || std::max({a, b, c}) < -h) {
            return false;
        }
    }
    const Vec3 normal = cross(e0, e1);
    return plane_box_overlap(normal, -dot(normal, v0), half);
}

// Face3::get_closest_point_to: the point of the triangle nearest p.
inline Vec3 closest_point_on_triangle(const std::array<Vec3, 3>& triangle, Vec3 p) {
    const Vec3 edge0 = triangle[1] - triangle[0];
    const Vec3 edge1 = triangle[2] - triangle[0];
    const Vec3 v0 = triangle[0] - p;
    const float a = dot(edge0, edge0);
    const float b = dot(edge0, edge1);
    const float c = dot(edge1, edge1);
    const float d = dot(edge0, v0);
    const float e = dot(edge1, v0);
    const float det = (a * c) - (b * b);
    float s = (b * e) - (c * d);
    float t = (b * d) - (a * e);
    const auto unit = [](float v) { return std::clamp(v, 0.0F, 1.0F); };
    if (s + t < det) {
        if (s < 0.0F) {
            if (t < 0.0F) {
                if (d < 0.0F) {
                    s = unit(-d / a);
                    t = 0.0F;
                } else {
                    s = 0.0F;
                    t = unit(-e / c);
                }
            } else {
                s = 0.0F;
                t = unit(-e / c);
            }
        } else if (t < 0.0F) {
            s = unit(-d / a);
            t = 0.0F;
        } else {
            const float inv_det = 1.0F / det;
            s *= inv_det;
            t *= inv_det;
        }
    } else {
        if (s < 0.0F) {
            const float tmp0 = b + d;
            const float tmp1 = c + e;
            if (tmp1 > tmp0) {
                const float numer = tmp1 - tmp0;
                const float denom = a - (2.0F * b) + c;
                s = unit(numer / denom);
                t = 1.0F - s;
            } else {
                t = unit(-e / c);
                s = 0.0F;
            }
        } else if (t < 0.0F) {
            if (a + d > b + e) {
                const float numer = c + e - b - d;
                const float denom = a - (2.0F * b) + c;
                s = unit(numer / denom);
                t = 1.0F - s;
            } else {
                s = unit(-d / a);
                t = 0.0F;
            }
        } else {
            const float numer = c + e - b - d;
            const float denom = a - (2.0F * b) + c;
            s = unit(numer / denom);
            t = 1.0F - s;
        }
    }
    return triangle[0] + (edge0 * s) + (edge1 * t);
}

} // namespace slash::scene3d::detail
