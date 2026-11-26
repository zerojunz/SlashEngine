// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
#pragma once

// What scene3d's CPU bakes share with the GPU's bake shader (engine/render/shaders/
// lightmap_light.comp): GLSL's vector math, its random numbers and ray directions, the bounding
// volume hierarchy whose nearest hits the shader's ray queries match, and rasterizing into texels
// as GPUs do. The lightmapper (lightmapper.cpp) and the mesh map baker (mesh_maps.cpp) use them.

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"
#include "slash/render/mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <utility>
#include <vector>

namespace slash::scene3d::bake {

inline constexpr float kPi = std::numbers::pi_v<float>; // as lm_compute.glsl's PI

// ---- Vectors, as GLSL's

inline float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

inline float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// A unit vector; the zero vector stays zero, where GLSL's would be NaN.
inline Vec3 normalize(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

// ---- Random numbers, as lm_compute.glsl's

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
inline std::uint32_t hash(std::uint32_t value) {
    const std::uint32_t state = (value * 747796405U) + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

inline std::uint32_t random_seed(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return hash(x ^ hash(y ^ hash(z)));
}

// From 0 to 1.
inline float randomize(std::uint32_t& value) {
    value = hash(value);
    return static_cast<float>(value) / 4294967296.0F;
}

inline Vec3 hemisphere_cosine_weighted_direction(std::uint32_t& noise) {
    const float noise1 = randomize(noise);
    const float noise2 = randomize(noise) * 2.0F * kPi;
    const Vec2 cs = slash::cos_sin(noise2);
    return {std::sqrt(noise1) * cs.x, std::sqrt(noise1) * cs.y, std::sqrt(1.0F - noise1)};
}

inline Vec3 sphere_uniform_direction(std::uint32_t& noise) {
    const float theta = 2.0F * kPi * randomize(noise);
    const float z = 1.0F - (2.0F * randomize(noise)); // cos(phi), phi = acos(z)
    const float sin_phi = std::sqrt(std::max(0.0F, (1.0F - z) * (1.0F + z)));
    const Vec2 cs = slash::cos_sin(theta);
    return {sin_phi * cs.x, sin_phi * cs.y, z};
}

inline Vec3 ray_dir_from_normal(Vec3 normal, std::uint32_t& noise) {
    const Vec3 v0 = std::abs(normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
    const Vec3 tangent = normalize(cross(v0, normal));
    const Vec3 bitangent = normalize(cross(tangent, normal));
    const Vec3 h = hemisphere_cosine_weighted_direction(noise);
    return (tangent * h.x) + (bitangent * h.y) + (normal * h.z);
}

// ---- The scene

struct Vertex {
    Vec3 position;
    Vec3 normal;
    std::array<float, 2> uv{}; // in the atlas, from 0 to 1
};

struct Triangle {
    std::array<std::uint32_t, 3> indices{};
    std::uint32_t slice = 0;
    Vec3 min;
    Vec3 max;
    render::CullMode cull = render::CullMode::Back;
};

enum class Hit : std::uint8_t { Miss, Front, Back };

// The nearest triangle a ray hits, as lm_compute.glsl's trace_ray finds it in its grid.
class Bvh {
public:
    Bvh(const std::vector<Vertex>& vertices, const std::vector<Triangle>& triangles)
        : vertices_(&vertices), triangles_(&triangles) {
        order_.resize(triangles.size());
        for (std::uint32_t i = 0; i < order_.size(); ++i) {
            order_[i] = i;
        }
        if (!order_.empty()) {
            nodes_.reserve(2 * order_.size());
            nodes_.emplace_back();
            build(0, 0, static_cast<std::uint32_t>(order_.size()));
        }
    }

    struct Result {
        Hit hit = Hit::Miss;
        float distance = 0.0F;
        Vec3 normal;
        std::uint32_t triangle = 0;
        Vec3 barycentric;
    };

    // The nearest hit from from toward to, no nearer than bias: a front face's distance less the
    // bias, so it wins over a back face in its plane, and its side as the triangle's cull mode
    // makes it.
    [[nodiscard]] Result trace(Vec3 from, Vec3 to, float bias) const {
        Result out;
        if (nodes_.empty()) {
            return out;
        }
        const Vec3 rel = to - from;
        const float rel_len = length(rel);
        const Vec3 dir = normalize(rel);
        float best = 1e20F;
        std::array<std::uint32_t, 64> stack{};
        std::size_t top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& node = nodes_[stack[--top]];
            if (!hits_box(node, from, dir, std::min(rel_len, best + bias))) {
                continue;
            }
            if (node.count == 0) {
                // The nearer child last, so it comes off the stack first.
                const std::uint32_t left = node.first;
                const std::uint32_t right = node.first + 1;
                const bool right_first = axis_of(dir, node.axis) < 0.0F;
                stack[top++] = right_first ? left : right;
                stack[top++] = right_first ? right : left;
                continue;
            }
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                test(order_[i], from, dir, rel_len, bias, best, out);
            }
        }
        return out;
    }

private:
    struct Node {
        Vec3 min;
        Vec3 max;
        std::uint32_t first = 0; // a leaf's first triangle in order_, or an inner node's children
        std::uint32_t count = 0; // a leaf's triangles; 0 for an inner node
        int axis = 0;
    };

    static float axis_of(Vec3 v, int axis) {
        if (axis == 0) {
            return v.x;
        }
        return axis == 1 ? v.y : v.z;
    }

    static bool hits_box(const Node& node, Vec3 from, Vec3 dir, float max_distance) {
        float near = 0.0F;
        float far = max_distance;
        for (int axis = 0; axis < 3; ++axis) {
            const float o = axis_of(from, axis);
            const float d = axis_of(dir, axis);
            const float lo = axis_of(node.min, axis);
            const float hi = axis_of(node.max, axis);
            if (d == 0.0F) {
                if (o < lo || o > hi) {
                    return false;
                }
                continue;
            }
            float t0 = (lo - o) / d;
            float t1 = (hi - o) / d;
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            near = std::max(near, t0);
            far = std::min(far, t1);
            if (near > far) {
                return false;
            }
        }
        return true;
    }

    // Builds the subtree of order_[begin, end) into the node at slot, its children after it.
    void build(std::uint32_t slot, std::uint32_t begin, std::uint32_t end) {
        Vec3 lo{1e30F, 1e30F, 1e30F};
        Vec3 hi{-1e30F, -1e30F, -1e30F};
        Vec3 clo = lo;
        Vec3 chi = hi;
        for (std::uint32_t i = begin; i < end; ++i) {
            const Triangle& t = (*triangles_)[order_[i]];
            lo = {std::min(lo.x, t.min.x), std::min(lo.y, t.min.y), std::min(lo.z, t.min.z)};
            hi = {std::max(hi.x, t.max.x), std::max(hi.y, t.max.y), std::max(hi.z, t.max.z)};
            const Vec3 c = centroid(t);
            clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
            chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
        }
        nodes_[slot].min = lo;
        nodes_[slot].max = hi;
        if (end - begin <= 4) {
            nodes_[slot].first = begin;
            nodes_[slot].count = end - begin;
            return;
        }
        const Vec3 extent = chi - clo;
        int axis = extent.y >= extent.z ? 1 : 2;
        if (extent.x >= extent.y && extent.x >= extent.z) {
            axis = 0;
        }
        // Ordered by centroid, then index: one order on every platform.
        std::sort(order_.begin() + begin, order_.begin() + end,
                  [&](std::uint32_t a, std::uint32_t b) {
                      const float ca = axis_of(centroid((*triangles_)[a]), axis);
                      const float cb = axis_of(centroid((*triangles_)[b]), axis);
                      return ca < cb || (ca == cb && a < b);
                  });
        const std::uint32_t middle = begin + ((end - begin) / 2);
        const auto children = static_cast<std::uint32_t>(nodes_.size());
        nodes_.resize(nodes_.size() + 2);
        nodes_[slot].first = children;
        nodes_[slot].axis = axis;
        build(children, begin, middle);
        build(children + 1, middle, end);
    }

    static Vec3 centroid(const Triangle& t) { return (t.min + t.max) * 0.5F; }

    // lm_compute.glsl's ray_hits_triangle and the rest of trace_ray's test of one triangle.
    void test(std::uint32_t index, Vec3 from, Vec3 dir, float max_distance, float bias, float& best,
              Result& out) const {
        const Triangle& triangle = (*triangles_)[index];
        const Vec3 p0 = (*vertices_)[triangle.indices[0]].position;
        const Vec3 p1 = (*vertices_)[triangle.indices[1]].position;
        const Vec3 p2 = (*vertices_)[triangle.indices[2]].position;
        constexpr float kEpsilon = 0.00001F;
        const Vec3 e0 = p1 - p0;
        const Vec3 e1 = p0 - p2;
        const Vec3 triangle_normal = cross(e1, e0);
        const float n_dot_dir = dot(triangle_normal, dir);
        if (std::abs(n_dot_dir) < kEpsilon) {
            return;
        }
        const Vec3 e2 = (p0 - from) * (1.0F / n_dot_dir);
        const Vec3 i = cross(dir, e2);
        Vec3 barycentric;
        barycentric.y = dot(i, e1);
        barycentric.z = dot(i, e0);
        barycentric.x = 1.0F - (barycentric.z + barycentric.y);
        float distance = dot(triangle_normal, e2);
        // As lm_compute.glsl tests it, so a NaN distance misses too.
        const bool inside = distance > bias && distance < max_distance && barycentric.x >= 0.0F &&
                            barycentric.y >= 0.0F && barycentric.z >= 0.0F;
        if (!inside) {
            return;
        }
        const Vec3 normal = normalize(cross(p0 - p1, p0 - p2)) * -1.0F;
        bool backface = dot(normal, dir) >= 0.0F;
        if (!backface) {
            distance = std::max(bias, distance - bias);
        }
        if (distance < best) {
            if (triangle.cull == render::CullMode::Disabled) {
                backface = false;
            } else if (triangle.cull == render::CullMode::Front) {
                backface = !backface;
            }
            out.hit = backface ? Hit::Back : Hit::Front;
            best = distance;
            out.distance = distance;
            out.normal = normal;
            out.triangle = index;
            out.barycentric = barycentric;
        }
    }

    const std::vector<Vertex>* vertices_;
    const std::vector<Triangle>* triangles_;
    std::vector<std::uint32_t> order_;
    std::vector<Node> nodes_;
};

inline void for_each(jobs::Pool* pool, std::size_t count,
                     const std::function<void(std::size_t)>& task) {
    if (pool != nullptr && count > 1) {
        pool->parallel_for(count, task);
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        task(i);
    }
}

// ---- Rasterizing, as the GPU does ----

struct Point2 {
    float x = 0.0F;
    float y = 0.0F;
};

inline float cross2(Point2 a, Point2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

inline Point2 minus(Point2 a, Point2 b) {
    return {a.x - b.x, a.y - b.y};
}

// Whether a texel center exactly on an edge belongs to the triangle: one of the two triangles
// sharing an edge takes it, as GPUs' top-left rule makes sure.
inline bool owns_edge(Point2 edge) {
    return edge.y < 0.0F || (edge.y == 0.0F && edge.x > 0.0F);
}

// Calls fill(x, y, barycentrics) for each texel of a width by height grid whose center is inside
// the triangle s, given in texels.
template <typename F>
void fill_triangle(const std::array<Point2, 3>& s, int width, int height, F&& fill) {
    const float area = cross2(minus(s[1], s[0]), minus(s[2], s[0]));
    if (area == 0.0F || !std::isfinite(area)) {
        return;
    }
    const float sign = area > 0.0F ? 1.0F : -1.0F;
    const float lo_x = std::min({s[0].x, s[1].x, s[2].x});
    const float hi_x = std::max({s[0].x, s[1].x, s[2].x});
    const float lo_y = std::min({s[0].y, s[1].y, s[2].y});
    const float hi_y = std::max({s[0].y, s[1].y, s[2].y});
    const int x0 = std::max(0, static_cast<int>(std::ceil(lo_x - 0.5F)));
    const int x1 = std::min(width - 1, static_cast<int>(std::floor(hi_x - 0.5F)));
    const int y0 = std::max(0, static_cast<int>(std::ceil(lo_y - 0.5F)));
    const int y1 = std::min(height - 1, static_cast<int>(std::floor(hi_y - 0.5F)));
    // The edge opposite each corner, turned the way the triangle winds.
    std::array<Point2, 3> edges{minus(s[2], s[1]), minus(s[0], s[2]), minus(s[1], s[0])};
    for (Point2& e : edges) {
        e = {e.x * sign, e.y * sign};
    }
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const Point2 p{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
            const std::array<float, 3> w{cross2(minus(s[2], s[1]), minus(p, s[1])) * sign,
                                         cross2(minus(s[0], s[2]), minus(p, s[2])) * sign,
                                         cross2(minus(s[1], s[0]), minus(p, s[0])) * sign};
            bool inside = true;
            for (std::size_t i = 0; i < 3 && inside; ++i) {
                inside = w[i] > 0.0F || (w[i] == 0.0F && owns_edge(edges[i]));
            }
            if (inside) {
                const float total = area * sign;
                fill(x, y, Vec3{w[0] / total, w[1] / total, w[2] / total});
            }
        }
    }
}

// Calls plot(x, y, t) for each texel a line from a to b (in texels) crosses along its longer
// axis, t of the way from a, as GPUs draw lines.
template <typename F>
void draw_line(Point2 a, Point2 b, int width, int height, F&& plot) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const bool x_major = std::abs(dx) >= std::abs(dy);
    const float from = x_major ? a.x : a.y;
    const float to = x_major ? b.x : b.y;
    if (from == to || !std::isfinite(from) || !std::isfinite(to)) {
        return;
    }
    const float lo = std::min(from, to);
    const float hi = std::max(from, to);
    const int first = static_cast<int>(std::ceil(lo - 0.5F));
    const int last = static_cast<int>(std::ceil(hi - 0.5F)) - 1;
    const int limit = x_major ? width : height;
    for (int i = std::max(first, 0); i <= std::min(last, limit - 1); ++i) {
        const float center = static_cast<float>(i) + 0.5F;
        const float t = (center - from) / (to - from);
        const float other = x_major ? a.y + (dy * t) : a.x + (dx * t);
        const int j = static_cast<int>(std::floor(other));
        const int x = x_major ? i : j;
        const int y = x_major ? j : i;
        if (x >= 0 && y >= 0 && x < width && y < height) {
            plot(x, y, t);
        }
    }
}

inline Vec3 lerp_barycentric(Vec3 a, Vec3 b, float t) {
    return a + ((b - a) * t);
}

} // namespace slash::scene3d::bake
