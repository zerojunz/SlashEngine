// The lightmap baker: Godot 4.6's GPU lightmapper (modules/lightmapper_rd: LightmapperRD::bake,
// lm_raster.glsl, lm_compute.glsl and lm_blendseams.glsl) on the CPU. Each mesh's material is
// drawn into its lightmap's texels, the lightmaps are packed into the slices of an atlas, and the
// atlas is rastered to the surfaces' positions and normals; then each texel is pushed out of walls
// it sits in, lit directly by the lights with soft shadows, lit by paths that bounce off other
// surfaces or leave for the environment, denoised, dilated into the empty texels around it and
// blended across the seams where a mesh's lightmap is cut. Godot's grid of triangle clusters is a
// bounding volume hierarchy here, which finds the same nearest hits. Every texel's rays come from
// its own random numbers, as on the GPU, so threads can't change what it gets, and the math goes
// through slash::cos_sin, atan2, exp and ln, so neither can the platform.
//
// Godot's denoiser is a joint non-local means filter based on YoctoImageDenoiser's, which carries
// this notice:
//
// MIT License
//
// Copyright (c) 2020 ManuelPrandini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"
#include "slash/scene3d/lightmap.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <stb_rect_pack.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

constexpr float kPi = std::numbers::pi_v<float>; // as lm_compute.glsl's PI

// Godot's project settings for baking: rendering/lightmapping/bake_performance/
// max_rays_per_pass and max_transparency_rays.
constexpr std::uint32_t kMaxRaysPerPass = 32;
// rendering/lightmapping/bake_performance/max_rays_per_probe_pass.
constexpr std::uint32_t kMaxRaysPerProbePass = 64;
constexpr std::uint32_t kTransparencyRays = 8;
// Godot's AA_SAMPLES: texel samples a light's shadow is traced from.
constexpr std::uint32_t kAaSamples = 16;

// ---- Vectors, as GLSL's

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// A unit vector; the zero vector stays zero, where GLSL's would be NaN.
Vec3 normalize(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

Vec3 mul(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// GLSL's mix: a toward b by t.
Vec3 mix(Vec3 a, Vec3 b, float t) {
    return (a * (1.0F - t)) + (b * t);
}

float max3(Vec3 v) {
    return std::max(std::max(v.x, v.y), v.z);
}

float power(float base, float exponent) {
    return base > 0.0F ? slash::exp(exponent * slash::ln(base)) : 0.0F;
}

// ---- Random numbers, as lm_compute.glsl's

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
std::uint32_t hash(std::uint32_t value) {
    const std::uint32_t state = (value * 747796405U) + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

std::uint32_t random_seed(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return hash(x ^ hash(y ^ hash(z)));
}

// From 0 to 1.
float randomize(std::uint32_t& value) {
    value = hash(value);
    return static_cast<float>(value) / 4294967296.0F;
}

Vec3 hemisphere_cosine_weighted_direction(std::uint32_t& noise) {
    const float noise1 = randomize(noise);
    const float noise2 = randomize(noise) * 2.0F * kPi;
    const Vec2 cs = slash::cos_sin(noise2);
    return {std::sqrt(noise1) * cs.x, std::sqrt(noise1) * cs.y, std::sqrt(1.0F - noise1)};
}

Vec3 sphere_uniform_direction(std::uint32_t& noise) {
    const float theta = 2.0F * kPi * randomize(noise);
    const float z = 1.0F - (2.0F * randomize(noise)); // cos(phi), phi = acos(z)
    const float sin_phi = std::sqrt(std::max(0.0F, (1.0F - z) * (1.0F + z)));
    const Vec2 cs = slash::cos_sin(theta);
    return {sin_phi * cs.x, sin_phi * cs.y, z};
}

Vec3 ray_dir_from_normal(Vec3 normal, std::uint32_t& noise) {
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

// An edge of one mesh's lightmap whose two sides lie in different places in it.
struct Seam {
    std::array<std::uint32_t, 2> a{};
    std::array<std::uint32_t, 2> b{};
    std::uint32_t slice = 0;
};

struct Light {
    render::LightType type = render::LightType::Directional;
    Vec3 position;
    Vec3 direction;
    Vec3 color;
    float energy = 1.0F;
    float indirect_energy = 1.0F;
    float size = 0.0F;
    float range = 0.0F;
    float attenuation = 1.0F;
    float cos_spot_angle = 0.0F;
    float inv_spot_attenuation = 1.0F;
    float shadow_blur = 1.0F;
    bool static_bake = false;
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

// ---- Textures

using Rgba = std::array<float, 4>;

// Slices of width by height texels, as a texture2DArray.
template <typename T>
struct Layers {
    int width = 0;
    int height = 0;
    int slices = 0;
    std::vector<T> texels;

    void resize(int w, int h, int s, T fill = T{}) {
        width = w;
        height = h;
        slices = s;
        texels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                          static_cast<std::size_t>(s),
                      fill);
    }
    [[nodiscard]] bool inside(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }
    [[nodiscard]] std::size_t at(int slice, int x, int y) const {
        return ((static_cast<std::size_t>(slice) * static_cast<std::size_t>(height) +
                 static_cast<std::size_t>(y)) *
                static_cast<std::size_t>(width)) +
               static_cast<std::size_t>(x);
    }
    T& operator()(int slice, int x, int y) { return texels[at(slice, x, y)]; }
    // texelFetch: zero outside, as robust access returns.
    [[nodiscard]] T fetch(int slice, int x, int y) const {
        return inside(x, y) ? texels[at(slice, x, y)] : T{};
    }
};

Rgba lerp4(const Rgba& a, const Rgba& b, float t) {
    return {a[0] + ((b[0] - a[0]) * t), a[1] + ((b[1] - a[1]) * t), a[2] + ((b[2] - a[2]) * t),
            a[3] + ((b[3] - a[3]) * t)};
}

int wrap(int i, int size) {
    const int m = i % size;
    return m < 0 ? m + size : m;
}

// textureLod with the lightmapper's linear sampler, which repeats.
Rgba sample(const Layers<Rgba>& layers, int slice, float u, float v) {
    const float x = (u * static_cast<float>(layers.width)) - 0.5F;
    const float y = (v * static_cast<float>(layers.height)) - 0.5F;
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float tx = x - fx;
    const float ty = y - fy;
    const int x0 = wrap(static_cast<int>(fx), layers.width);
    const int y0 = wrap(static_cast<int>(fy), layers.height);
    const int x1 = wrap(x0 + 1, layers.width);
    const int y1 = wrap(y0 + 1, layers.height);
    const Rgba top =
        lerp4(layers.texels[layers.at(slice, x0, y0)], layers.texels[layers.at(slice, x1, y0)], tx);
    const Rgba bottom =
        lerp4(layers.texels[layers.at(slice, x0, y1)], layers.texels[layers.at(slice, x1, y1)], tx);
    return lerp4(top, bottom, ty);
}

Vec3 rgb(const Rgba& c) {
    return {c[0], c[1], c[2]};
}

void for_each(jobs::Pool* pool, std::size_t count, const std::function<void(std::size_t)>& task) {
    if (pool != nullptr && count > 1) {
        pool->parallel_for(count, task);
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        task(i);
    }
}

// ---- Rasterizing, as the GPU does in lm_raster.glsl's and Godot's UV2 passes

struct Point2 {
    float x = 0.0F;
    float y = 0.0F;
};

float cross2(Point2 a, Point2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

Point2 minus(Point2 a, Point2 b) {
    return {a.x - b.x, a.y - b.y};
}

// Whether a texel center exactly on an edge belongs to the triangle: one of the two triangles
// sharing an edge takes it, as GPUs' top-left rule makes sure.
bool owns_edge(Point2 edge) {
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

Vec3 lerp_barycentric(Vec3 a, Vec3 b, float t) {
    return a + ((b - a) * t);
}

constexpr std::array<Vec3, 3> kCorners{Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                                       Vec3{0.0F, 0.0F, 1.0F}};

// ---- One mesh

struct MeshInstance {
    const LightmapMesh* source = nullptr;
    const LightmapUnwrap* unwrap = nullptr;
    int width = 0; // its lightmap's texels
    int height = 0;
    int offset_x = 0; // where they are in the atlas
    int offset_y = 0;
    int slice = 0;
    std::vector<Vec3> positions; // the unwrap's vertices in the world
    std::vector<Vec3> normals;
};

// Draws a mesh's material into its lightmap's texels, as Godot's bake_render_uv2: its triangles
// in UV2 space, after their edges at the nine places a texel around for texels the triangles
// leave out, so albedo reaches past the charts' edges.
void bake_material(const MeshInstance& mesh, Layers<Rgba>& albedo, Layers<Rgba>& emission) {
    const LightmapMesh& source = *mesh.source;
    const LightmapUnwrap& unwrap = *mesh.unwrap;
    struct Covered {
        std::uint32_t triangle = 0;
        Vec3 barycentric;
        bool set = false;
    };
    std::vector<Covered> covered(static_cast<std::size_t>(mesh.width) *
                                 static_cast<std::size_t>(mesh.height));
    const auto texel = [&](std::uint32_t vertex) {
        const std::array<float, 2>& uv = unwrap.uv2[vertex];
        return Point2{uv[0] * static_cast<float>(mesh.width),
                      uv[1] * static_cast<float>(mesh.height)};
    };
    const std::size_t triangles = unwrap.indices.size() / 3;
    static constexpr std::array<std::array<float, 2>, 9> kOffsets{{{-1.0F, 1.0F},
                                                                   {1.0F, 1.0F},
                                                                   {1.0F, -1.0F},
                                                                   {-1.0F, -1.0F},
                                                                   {-1.0F, 0.0F},
                                                                   {1.0F, 0.0F},
                                                                   {0.0F, -1.0F},
                                                                   {0.0F, 1.0F},
                                                                   {0.0F, 0.0F}}};
    for (const auto& offset : kOffsets) {
        for (std::size_t t = 0; t < triangles; ++t) {
            for (std::size_t k = 0; k < 3; ++k) {
                const std::size_t n = (k + 1) % 3;
                Point2 a = texel(unwrap.indices[(t * 3) + k]);
                Point2 b = texel(unwrap.indices[(t * 3) + n]);
                a = {a.x + offset[0], a.y + offset[1]};
                b = {b.x + offset[0], b.y + offset[1]};
                draw_line(a, b, mesh.width, mesh.height, [&](int x, int y, float along) {
                    covered[(static_cast<std::size_t>(y) * static_cast<std::size_t>(mesh.width)) +
                            static_cast<std::size_t>(x)] = {
                        static_cast<std::uint32_t>(t),
                        lerp_barycentric(kCorners[k], kCorners[n], along), true};
                });
            }
        }
    }
    for (std::size_t t = 0; t < triangles; ++t) {
        const std::array<Point2, 3> s{texel(unwrap.indices[t * 3]),
                                      texel(unwrap.indices[(t * 3) + 1]),
                                      texel(unwrap.indices[(t * 3) + 2])};
        fill_triangle(s, mesh.width, mesh.height, [&](int x, int y, Vec3 barycentric) {
            covered[(static_cast<std::size_t>(y) * static_cast<std::size_t>(mesh.width)) +
                    static_cast<std::size_t>(x)] = {static_cast<std::uint32_t>(t), barycentric,
                                                    true};
        });
    }
    for (int y = 0; y < mesh.height; ++y) {
        for (int x = 0; x < mesh.width; ++x) {
            const Covered& c =
                covered[(static_cast<std::size_t>(y) * static_cast<std::size_t>(mesh.width)) +
                        static_cast<std::size_t>(x)];
            if (!c.set) {
                continue;
            }
            // The surface the triangle is in.
            const std::uint32_t first_index = c.triangle * 3;
            const LightmapMesh::Surface* surface = nullptr;
            for (const LightmapMesh::Surface& s : source.surfaces) {
                if (s.first_index <= first_index) {
                    surface = &s;
                }
            }
            LightmapPoint point{.uv = {0.0F, 0.0F}, .color = {0.0F, 0.0F, 0.0F, 0.0F}};
            const std::array<float, 3> weights{c.barycentric.x, c.barycentric.y, c.barycentric.z};
            for (std::size_t k = 0; k < 3; ++k) {
                const render::Vertex3D& v =
                    source.vertices[unwrap.xref[unwrap.indices[first_index + k]]];
                point.uv[0] += v.uv[0] * weights[k];
                point.uv[1] += v.uv[1] * weights[k];
                for (std::size_t i = 0; i < 4; ++i) {
                    point.color[i] += (static_cast<float>(v.rgba[i]) / 255.0F) * weights[k];
                }
            }
            Rgba a{1.0F, 1.0F, 1.0F, 1.0F};
            std::array<float, 3> e{0.0F, 0.0F, 0.0F};
            if (surface != nullptr && surface->albedo) {
                a = surface->albedo(point);
            }
            if (surface != nullptr && surface->emission) {
                e = surface->emission(point);
            }
            const int ax = mesh.offset_x + x;
            const int ay = mesh.offset_y + y;
            albedo(mesh.slice, ax, ay) = a;
            emission(mesh.slice, ax, ay) = {e[0], e[1], e[2], 1.0F};
        }
    }
}

// The smallest power of two at least n.
int power_of_two(int n) {
    int p = 1;
    while (p < n) {
        p *= 2;
    }
    return p;
}

// Godot's Geometry2D::partial_pack_rects: where stb_rect_pack puts each size in one atlas, and
// whether it fit.
std::vector<std::array<int, 3>> pack(const std::vector<std::array<int, 2>>& sizes, int width,
                                     int height) {
    std::vector<stbrp_node> nodes(static_cast<std::size_t>(width));
    stbrp_context context{};
    stbrp_init_target(&context, width, height, nodes.data(), width);
    std::vector<stbrp_rect> rects(sizes.size());
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        rects[i] = {};
        rects[i].id = static_cast<int>(i);
        rects[i].w = sizes[i][0];
        rects[i].h = sizes[i][1];
    }
    stbrp_pack_rects(&context, rects.data(), static_cast<int>(rects.size()));
    std::vector<std::array<int, 3>> out(sizes.size());
    for (const stbrp_rect& rect : rects) {
        out[static_cast<std::size_t>(rect.id)] = {rect.x, rect.y, rect.was_packed != 0 ? 1 : 0};
    }
    return out;
}

// Floats as Godot's Math::is_equal_approx compares them.
bool equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    float tolerance = 0.00001F * std::abs(a);
    tolerance = std::max(tolerance, 0.00001F);
    return std::abs(a - b) < tolerance;
}

bool before(Vec3 a, Vec3 b) {
    if (a.x != b.x) {
        return a.x < b.x;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    return a.z < b.z;
}

struct EdgeKey {
    std::array<std::uint32_t, 12> bits{};
    friend bool operator==(const EdgeKey&, const EdgeKey&) = default;
};

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& key) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        for (const std::uint32_t b : key.bits) {
            h = (h ^ b) * 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

void put_bits(std::uint32_t* out, Vec3 v) {
    out[0] = std::bit_cast<std::uint32_t>(v.x);
    out[1] = std::bit_cast<std::uint32_t>(v.y);
    out[2] = std::bit_cast<std::uint32_t>(v.z);
}

struct VertexKey {
    std::array<std::uint32_t, 8> bits{};
    friend bool operator==(const VertexKey&, const VertexKey&) = default;
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        for (const std::uint32_t b : key.bits) {
            h = (h ^ b) * 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

// ---- The baker

class Baker {
public:
    Baker(const LightmapSettings& settings, jobs::Pool* pool) : settings_(settings), pool_(pool) {}

    Result<LightmapBake> bake(const std::vector<LightmapMesh>& meshes,
                              const std::vector<LightmapLight>& lights,
                              const render::HdrImage& environment,
                              const LightmapProbeRequest& probes) {
        LightmapBake out;
        probes_ = &probes;
        if (meshes.empty()) {
            return make_error(ErrorCode::InvalidArgument, "there are no static meshes to bake");
        }
        if (auto unwrapped = unwrap(meshes, out); !unwrapped) {
            return unwrapped.error();
        }
        if (auto packed = pack_atlas(); !packed) {
            return packed.error();
        }
        for (const MeshInstance& mesh : instances_) {
            bake_material(mesh, albedo_, emission_);
        }
        add_lights(lights);
        set_environment(environment);
        build_scene();
        raster();
        unocclude();
        direct_light();
        if (settings_.bounces > 0) {
            bounce_light();
        }
        out.probe_sh = light_probes();
        if (settings_.use_denoiser) {
            denoise();
        }
        dilate();
        blend_seams();
        finish(out);
        out.meshes = meshes.size();
        out.lights = lights.size();
        return out;
    }

private:
    // Unwraps each mesh (meshes of one hash once) and works out its lightmap's size, as
    // LightmapGI::bake from the unwrap's size hint.
    Result<void> unwrap(const std::vector<LightmapMesh>& meshes, LightmapBake& out) {
        for (const LightmapMesh& mesh : meshes) {
            std::string hash = lightmap_mesh_hash(mesh.vertices, mesh.indices);
            auto found = out.data.meshes.find(hash);
            if (found == out.data.meshes.end()) {
                auto unwrapped = unwrap_lightmap(mesh.vertices, mesh.indices);
                if (!unwrapped) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "\"" + mesh.entity + "\": " + unwrapped.error().message);
                }
                found = out.data.meshes.emplace(hash, std::move(unwrapped).value()).first;
            }
            MeshInstance instance;
            instance.source = &mesh;
            instance.unwrap = &found->second;
            const float scale = mesh.texel_scale * settings_.texel_scale;
            const auto w = static_cast<int>(static_cast<float>(found->second.width) * scale);
            const auto h = static_cast<int>(static_cast<float>(found->second.height) * scale);
            instance.width = static_cast<int>(static_cast<float>(w) * settings_.supersampling);
            instance.height = static_cast<int>(static_cast<float>(h) * settings_.supersampling);
            if (instance.width <= 0 || instance.height <= 0) {
                return make_error(ErrorCode::InvalidArgument,
                                  "\"" + mesh.entity +
                                      "\"'s lightmap is too small: raise its "
                                      "gi_lightmap_texel_scale or the texel_scale");
            }
            // Normals turn by the inverse transpose of the basis.
            const world::Affine3D inverse = mesh.transform.inverse();
            for (const std::uint32_t source : found->second.xref) {
                const render::Vertex3D& v = mesh.vertices[source];
                const Vec3 p{v.position[0], v.position[1], v.position[2]};
                const Vec3 n{v.normal[0], v.normal[1], v.normal[2]};
                instance.positions.push_back(mesh.transform.apply(p));
                instance.normals.push_back(
                    normalize({dot(inverse.x, n), dot(inverse.y, n), dot(inverse.z, n)}));
            }
            out.data.users.push_back({.entity = mesh.entity,
                                      .part = mesh.part,
                                      .mesh = hash,
                                      .slice = 0,
                                      .uv_rect = {}});
            instances_.push_back(std::move(instance));
        }
        return {};
    }

    // LightmapperRD::_blit_meshes_into_atlas: the atlas size and slices that hold every
    // lightmap, with room for the denoiser between them, in the least memory.
    Result<void> pack_atlas() {
        const int padding = static_cast<int>(
            static_cast<float>(std::max(2, static_cast<int>(settings_.denoiser_range))) *
            settings_.supersampling);
        int width = 0;
        int height = 0;
        std::vector<std::array<int, 2>> sizes;
        for (const MeshInstance& mesh : instances_) {
            sizes.push_back({mesh.width + padding, mesh.height + padding});
            width = std::max(width, mesh.width + padding);
            height = std::max(height, mesh.height + padding);
        }
        const int largest = std::max(power_of_two(width), power_of_two(height));
        const auto max_size = static_cast<int>(settings_.max_texture_size);
        if (largest > max_size) {
            return make_error(ErrorCode::InvalidArgument,
                              "a mesh's lightmap needs " + std::to_string(largest) +
                                  " texels across, more than max_texture_size " +
                                  std::to_string(max_size));
        }
        int atlas_w = largest;
        int atlas_h = largest;
        int best_w = 0;
        int best_h = 0;
        int best_slices = 0;
        std::int64_t best_memory = std::numeric_limits<std::int64_t>::max();
        std::vector<std::array<int, 3>> best_offsets;
        while (atlas_w <= max_size && atlas_h <= max_size) {
            std::vector<std::array<int, 2>> source_sizes = sizes;
            std::vector<std::size_t> source_indices(sizes.size());
            for (std::size_t i = 0; i < source_indices.size(); ++i) {
                source_indices[i] = i;
            }
            std::vector<std::array<int, 3>> offsets(sizes.size());
            int slices = 0;
            while (!source_sizes.empty()) {
                const std::vector<std::array<int, 3>> placed = pack(source_sizes, atlas_w, atlas_h);
                std::vector<std::array<int, 2>> left_sizes;
                std::vector<std::size_t> left_indices;
                for (std::size_t i = 0; i < placed.size(); ++i) {
                    if (placed[i][2] > 0) {
                        // Centered in the room it took.
                        offsets[source_indices[i]] = {placed[i][0] + 1, placed[i][1] + 1, slices};
                    } else {
                        left_sizes.push_back(source_sizes[i]);
                        left_indices.push_back(source_indices[i]);
                    }
                }
                source_sizes = std::move(left_sizes);
                source_indices = std::move(left_indices);
                ++slices;
            }
            const std::int64_t memory = std::int64_t{atlas_w} * atlas_h * slices;
            if (memory < best_memory) {
                best_w = atlas_w;
                best_h = atlas_h;
                best_slices = slices;
                best_memory = memory;
                best_offsets = offsets;
            }
            if (atlas_w == atlas_h) {
                atlas_w *= 2;
            } else {
                atlas_h *= 2;
            }
        }
        albedo_.resize(best_w, best_h, best_slices, Rgba{0.0F, 0.0F, 0.0F, 0.0F});
        emission_.resize(best_w, best_h, best_slices, Rgba{0.0F, 0.0F, 0.0F, 0.0F});
        for (std::size_t i = 0; i < instances_.size(); ++i) {
            instances_[i].offset_x = best_offsets[i][0];
            instances_[i].offset_y = best_offsets[i][1];
            instances_[i].slice = best_offsets[i][2];
        }
        return {};
    }

    void add_lights(const std::vector<LightmapLight>& lights) {
        for (const LightmapLight& from : lights) {
            Light l;
            l.type = from.type;
            l.position = {from.position[0], from.position[1], from.position[2]};
            l.direction = normalize({from.direction[0], from.direction[1], from.direction[2]});
            l.color = {from.color[0], from.color[1], from.color[2]};
            l.energy = from.energy;
            l.indirect_energy = from.indirect_energy;
            l.size = from.size;
            l.range = from.range;
            l.attenuation = from.attenuation;
            l.cos_spot_angle = slash::cos_sin(from.spot_angle * kPi / 180.0F).x;
            l.inv_spot_attenuation = 1.0F / from.spot_attenuation;
            l.shadow_blur = from.shadow_blur;
            l.static_bake = from.static_bake;
            lights_.push_back(l);
        }
        // Directional lights first, then omni, then spot, as Godot sorts them.
        std::stable_sort(lights_.begin(), lights_.end(), [](const Light& a, const Light& b) {
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        });
    }

    void set_environment(const render::HdrImage& environment) {
        if (environment.width == 0 || environment.height == 0) {
            // As Godot's: an 8x8 black panorama.
            environment_.resize(8, 8, 1, Rgba{0.0F, 0.0F, 0.0F, 1.0F});
            return;
        }
        environment_.resize(static_cast<int>(environment.width),
                            static_cast<int>(environment.height), 1);
        for (std::size_t i = 0; i < environment_.texels.size(); ++i) {
            environment_.texels[i] = {environment.rgb[i * 3], environment.rgb[(i * 3) + 1],
                                      environment.rgb[(i * 3) + 2], 1.0F};
        }
    }

    // LightmapperRD::_create_acceleration_structures: the triangles in the atlas, sorted by
    // slice, their vertices shared where they are the same, the seams where a mesh's lightmap is
    // cut, and the world's bounds.
    void build_scene() {
        std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertex_map;
        bool first = true;
        Vec3 lo;
        Vec3 hi;
        const auto atlas_w = static_cast<float>(albedo_.width);
        const auto atlas_h = static_cast<float>(albedo_.height);
        for (const MeshInstance& mesh : instances_) {
            struct EdgeUv {
                std::array<float, 4> uv{};
                std::array<std::uint32_t, 2> indices{};
                bool seam_found = false;
            };
            std::unordered_map<EdgeKey, EdgeUv, EdgeKeyHash> edges;
            const float scale_u = static_cast<float>(mesh.width) / atlas_w;
            const float scale_v = static_cast<float>(mesh.height) / atlas_h;
            const float offset_u = static_cast<float>(mesh.offset_x) / atlas_w;
            const float offset_v = static_cast<float>(mesh.offset_y) / atlas_h;
            const LightmapUnwrap& unwrap = *mesh.unwrap;
            const std::vector<LightmapMesh::Surface>& surfaces = mesh.source->surfaces;
            for (std::size_t i = 0; i + 2 < unwrap.indices.size(); i += 3) {
                std::array<Vec3, 3> p{};
                std::array<Vec3, 3> n{};
                std::array<std::array<float, 2>, 3> uv{};
                Triangle t;
                t.slice = static_cast<std::uint32_t>(mesh.slice);
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::uint32_t v = unwrap.indices[i + k];
                    p[k] = mesh.positions[v];
                    n[k] = mesh.normals[v];
                    uv[k] = {(unwrap.uv2[v][0] * scale_u) + offset_u,
                             (unwrap.uv2[v][1] * scale_v) + offset_v};
                    if (first) {
                        lo = p[k];
                        hi = p[k];
                        first = false;
                    }
                    lo = {std::min(lo.x, p[k].x), std::min(lo.y, p[k].y), std::min(lo.z, p[k].z)};
                    hi = {std::max(hi.x, p[k].x), std::max(hi.y, p[k].y), std::max(hi.z, p[k].z)};
                    VertexKey key;
                    put_bits(key.bits.data(), p[k]);
                    put_bits(key.bits.data() + 3, n[k]);
                    key.bits[6] = std::bit_cast<std::uint32_t>(uv[k][0]);
                    key.bits[7] = std::bit_cast<std::uint32_t>(uv[k][1]);
                    auto [it, added] =
                        vertex_map.try_emplace(key, static_cast<std::uint32_t>(vertices_.size()));
                    if (added) {
                        vertices_.push_back({.position = p[k], .normal = n[k], .uv = uv[k]});
                    }
                    t.indices[k] = it->second;
                }
                // The seams that need blending later.
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::size_t m = (k + 1) % 3;
                    Vec3 a = p[k];
                    Vec3 b = p[m];
                    Vec3 na = n[k];
                    Vec3 nb = n[m];
                    std::array<float, 4> edge_uv{uv[k][0], uv[k][1], uv[m][0], uv[m][1]};
                    std::array<std::uint32_t, 2> indices{t.indices[k], t.indices[m]};
                    if (a == b) {
                        continue; // degenerate
                    }
                    if (before(b, a)) {
                        std::swap(a, b);
                        std::swap(na, nb);
                        edge_uv = {edge_uv[2], edge_uv[3], edge_uv[0], edge_uv[1]};
                        std::swap(indices[0], indices[1]);
                    }
                    EdgeKey key;
                    put_bits(key.bits.data(), a);
                    put_bits(key.bits.data() + 3, b);
                    put_bits(key.bits.data() + 6, na);
                    put_bits(key.bits.data() + 9, nb);
                    auto found = edges.find(key);
                    if (found == edges.end()) {
                        edges.emplace(key, EdgeUv{.uv = edge_uv, .indices = indices});
                        continue;
                    }
                    EdgeUv& other = found->second;
                    bool same = true;
                    for (std::size_t c = 0; c < 4; ++c) {
                        same = same && equal_approx(other.uv[c], edge_uv[c]);
                    }
                    if (same || other.seam_found) {
                        continue; // shared in the lightmap too, or bad geometry
                    }
                    seams_.push_back({.a = indices, .b = other.indices, .slice = t.slice});
                    other.seam_found = true;
                }
                const Vec3 tlo{std::min({p[0].x, p[1].x, p[2].x}),
                               std::min({p[0].y, p[1].y, p[2].y}),
                               std::min({p[0].z, p[1].z, p[2].z})};
                const Vec3 thi{std::max({p[0].x, p[1].x, p[2].x}),
                               std::max({p[0].y, p[1].y, p[2].y}),
                               std::max({p[0].z, p[1].z, p[2].z})};
                t.min = tlo;
                t.max = {tlo.x + std::max(thi.x - tlo.x, 0.0001F),
                         tlo.y + std::max(thi.y - tlo.y, 0.0001F),
                         tlo.z + std::max(thi.z - tlo.z, 0.0001F)};
                for (const LightmapMesh::Surface& surface : surfaces) {
                    if (surface.first_index <= i) {
                        t.cull = surface.cull;
                    }
                }
                triangles_.push_back(t);
            }
        }
        // The light probes count too, as Godot's.
        for (const std::array<float, 3>& point : probes_->points) {
            lo = {std::min(lo.x, point[0]), std::min(lo.y, point[1]), std::min(lo.z, point[2])};
            hi = {std::max(hi.x, point[0]), std::max(hi.y, point[1]), std::max(hi.z, point[2])};
        }
        // Grown a little against rounding, as Godot's.
        lo = lo - Vec3{0.1F, 0.1F, 0.1F};
        hi = hi + Vec3{0.1F, 0.1F, 0.1F};
        world_size_ = length(hi - lo);
        std::stable_sort(triangles_.begin(), triangles_.end(),
                         [](const Triangle& a, const Triangle& b) { return a.slice < b.slice; });
        std::stable_sort(seams_.begin(), seams_.end(),
                         [](const Seam& a, const Seam& b) { return a.slice < b.slice; });
        bvh_ = std::make_unique<Bvh>(vertices_, triangles_);
    }

    // The texel offset Godot's raster gives every triangle, so edges don't fall on texel
    // centers (godotengine/godot#69126): half a texel up and left.
    [[nodiscard]] Point2 raster_texel(std::uint32_t vertex) const {
        const std::array<float, 2>& uv = vertices_[vertex].uv;
        return {(uv[0] * static_cast<float>(albedo_.width)) - 0.5F,
                (uv[1] * static_cast<float>(albedo_.height)) - 0.5F};
    }

    // LightmapperRD::_raster_geometry with lm_raster.glsl: each slice's triangles, then their
    // edges as lines for texels the triangles miss, each texel taking the first that covers it.
    void raster() {
        position_.resize(albedo_.width, albedo_.height, albedo_.slices, Rgba{});
        normal_.resize(albedo_.width, albedo_.height, albedo_.slices, Vec3{});
        unocclude_.resize(albedo_.width, albedo_.height, albedo_.slices, Rgba{});
        std::vector<bool> written(position_.texels.size(), false);
        const auto write = [&](const Triangle& t, int x, int y, Vec3 barycentric) {
            const std::size_t at = position_.at(static_cast<int>(t.slice), x, y);
            if (written[at]) {
                return;
            }
            written[at] = true;
            const Vertex& a = vertices_[t.indices[0]];
            const Vertex& b = vertices_[t.indices[1]];
            const Vertex& c = vertices_[t.indices[2]];
            const Vec3 vertex_interp = (a.position * barycentric.x) + (b.position * barycentric.y) +
                                       (c.position * barycentric.z);
            const Vec3 normal_interp = (a.normal * barycentric.x) + (b.normal * barycentric.y) +
                                       (c.normal * barycentric.z);
            const Vec3 face_normal =
                normalize(cross(a.position - b.position, a.position - c.position)) * -1.0F;
            Vec3 vertex_pos = vertex_interp;
            constexpr float kFlatThreshold = 0.99F;
            if (dot(a.normal, b.normal) < kFlatThreshold ||
                dot(a.normal, c.normal) < kFlatThreshold ||
                dot(b.normal, c.normal) < kFlatThreshold) {
                vertex_pos = smoothed(a, b, c, vertex_interp, barycentric, face_normal);
            }
            // The unocclusion technique of
            // https://ndotl.wordpress.com/2018/08/29/baking-artifact-free-lightmaps/: how far
            // a texel spans in the world, at worst.
            const float texel_size = texel_span(t) * std::numbers::sqrt2_v<float>;
            position_.texels[at] = {vertex_pos.x, vertex_pos.y, vertex_pos.z, 1.0F};
            normal_.texels[at] = normalize(normal_interp);
            unocclude_.texels[at] = {face_normal.x, face_normal.y, face_normal.z, texel_size};
        };
        std::size_t begin = 0;
        for (int slice = 0; slice < albedo_.slices; ++slice) {
            std::size_t end = begin;
            while (end < triangles_.size() &&
                   triangles_[end].slice == static_cast<std::uint32_t>(slice)) {
                ++end;
            }
            for (std::size_t i = begin; i < end; ++i) {
                const Triangle& t = triangles_[i];
                const std::array<Point2, 3> s{raster_texel(t.indices[0]),
                                              raster_texel(t.indices[1]),
                                              raster_texel(t.indices[2])};
                fill_triangle(s, albedo_.width, albedo_.height,
                              [&](int x, int y, Vec3 barycentric) { write(t, x, y, barycentric); });
            }
            for (std::size_t i = begin; i < end; ++i) {
                const Triangle& t = triangles_[i];
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::size_t m = (k + 1) % 3;
                    draw_line(raster_texel(t.indices[k]), raster_texel(t.indices[m]), albedo_.width,
                              albedo_.height, [&](int x, int y, float along) {
                                  write(t, x, y, lerp_barycentric(kCorners[k], kCorners[m], along));
                              });
                }
            }
            begin = end;
        }
    }

    // lm_raster.glsl's FA_SMOOTHEN_POSITION: the position projected onto the three corners'
    // normal planes and blended, where that is further out than the flat triangle.
    static Vec3 smoothed(const Vertex& a, const Vertex& b, const Vertex& c, Vec3 vertex_pos,
                         Vec3 barycentric, Vec3 face_normal) {
        const Vec3 center = (a.position + b.position + c.position) * 0.3333333F;
        std::array<Vec3, 3> norm{a.normal, b.normal, c.normal};
        const std::array<Vec3, 3> pos{a.position, b.position, c.position};
        for (std::size_t i = 0; i < 3; ++i) {
            const Vec3 dir = normalize(pos[i] - center);
            const float d = dot(dir, norm[i]);
            if (d < 0.0F) {
                norm[i] = normalize(norm[i] - (dir * d)); // pointing inwards
            }
        }
        Vec3 smooth;
        const std::array<float, 3> weights{barycentric.x, barycentric.y, barycentric.z};
        for (std::size_t i = 0; i < 3; ++i) {
            const float plane = dot(norm[i], pos[i]);
            const Vec3 projected = vertex_pos - (norm[i] * (dot(norm[i], vertex_pos) - plane));
            smooth = smooth + (projected * weights[i]);
        }
        return dot(face_normal, smooth) > dot(face_normal, vertex_pos) ? smooth : vertex_pos;
    }

    // max(abs(dFdx(vertex_interp)), abs(dFdy(vertex_interp))) for a triangle: the most its
    // position changes across a texel either way.
    [[nodiscard]] float texel_span(const Triangle& t) const {
        const Point2 s0 = raster_texel(t.indices[0]);
        const Point2 s1 = raster_texel(t.indices[1]);
        const Point2 s2 = raster_texel(t.indices[2]);
        const Vec3 p0 = vertices_[t.indices[0]].position;
        const Vec3 d1 = vertices_[t.indices[1]].position - p0;
        const Vec3 d2 = vertices_[t.indices[2]].position - p0;
        const Point2 e1 = minus(s1, s0);
        const Point2 e2 = minus(s2, s0);
        const float det = cross2(e1, e2);
        if (det == 0.0F) {
            return 0.0F;
        }
        const Vec3 ddx = ((d1 * e2.y) - (d2 * e1.y)) * (1.0F / det);
        const Vec3 ddy = ((d2 * e1.x) - (d1 * e2.x)) * (1.0F / det);
        return std::max({std::abs(ddx.x), std::abs(ddx.y), std::abs(ddx.z), std::abs(ddy.x),
                         std::abs(ddy.y), std::abs(ddy.z)});
    }

    // Runs task(slice, x, y) over every texel, a row at a time on the pool.
    template <typename F>
    void each_texel(F&& task) {
        const int rows = albedo_.slices * albedo_.height;
        for_each(pool_, static_cast<std::size_t>(rows), [&](std::size_t row) {
            const int slice = static_cast<int>(row) / albedo_.height;
            const int y = static_cast<int>(row) % albedo_.height;
            for (int x = 0; x < albedo_.width; ++x) {
                task(slice, x, y);
            }
        });
    }

    [[nodiscard]] Bvh::Result trace(Vec3 from, Vec3 to) const {
        return bvh_->trace(from, to, settings_.bias);
    }

    [[nodiscard]] float denoiser_range() const {
        return settings_.use_denoiser ? static_cast<float>(settings_.denoiser_range) : 1.0F;
    }

    // lm_compute.glsl's MODE_UNOCCLUDE: a texel inside a wall nearby, as where a wall meets the
    // floor, moves out to the wall's front face, and is marked so the denoiser leaves it out.
    void unocclude() {
        const float range = denoiser_range();
        each_texel([&](int slice, int x, int y) {
            const std::size_t at = position_.at(slice, x, y);
            Rgba& position_alpha = position_.texels[at];
            if (position_alpha[3] < 0.5F) {
                return;
            }
            Vec3 vertex_pos{position_alpha[0], position_alpha[1], position_alpha[2]};
            const Rgba normal_tsize = unocclude_.texels[at];
            const Vec3 face_normal{normal_tsize[0], normal_tsize[1], normal_tsize[2]};
            const float texel_size = normal_tsize[3];
            const Vec3 v0 =
                std::abs(face_normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
            const Vec3 tangent = normalize(cross(v0, face_normal));
            const Vec3 bitangent = normalize(cross(tangent, face_normal));
            const Vec3 base_pos = vertex_pos + (face_normal * settings_.bias);
            const std::array<Vec3, 4> rays{tangent, bitangent, tangent * -1.0F, bitangent * -1.0F};
            float min_d = 1e20F;
            float mask = 0.0F;
            for (const Vec3& ray : rays) {
                const Vec3 ray_to = base_pos + (ray * (texel_size * range));
                const Bvh::Result hit = trace(base_pos, ray_to);
                if (hit.hit == Hit::Back) {
                    mask = 1.0F;
                    if (hit.distance <= texel_size && hit.distance < min_d) {
                        // Further out than the bias, so rays leaving it don't go back through.
                        vertex_pos = base_pos + (ray * hit.distance) +
                                     (hit.normal * (settings_.bias * 10.0F));
                        min_d = hit.distance;
                    }
                }
            }
            position_alpha = {vertex_pos.x, vertex_pos.y, vertex_pos.z, position_alpha[3]};
            unocclude_.texels[at] = {mask, 0.0F, 0.0F, 0.0F};
        });
    }

    struct AlbedoHit {
        Hit hit = Hit::Miss;
        Rgba albedo{1.0F, 1.0F, 1.0F, 1.0F};
        Vec3 position;
    };

    // trace_ray_closest_hit_triangle_albedo_alpha.
    [[nodiscard]] AlbedoHit trace_albedo(Vec3 from, Vec3 to) const {
        AlbedoHit out;
        const Bvh::Result hit = trace(from, to);
        out.hit = hit.hit;
        if (hit.hit != Hit::Miss) {
            const Triangle& t = triangles_[hit.triangle];
            const Vertex& a = vertices_[t.indices[0]];
            const Vertex& b = vertices_[t.indices[1]];
            const Vertex& c = vertices_[t.indices[2]];
            const Vec3 w = hit.barycentric;
            const float u = (w.x * a.uv[0]) + (w.y * b.uv[0]) + (w.z * c.uv[0]);
            const float v = (w.x * a.uv[1]) + (w.y * b.uv[1]) + (w.z * c.uv[1]);
            out.albedo = sample(albedo_, static_cast<int>(t.slice), u, v);
            out.position = (a.position * w.x) + (b.position * w.y) + (c.position * w.z);
        }
        return out;
    }

    // Godot's get_omni_attenuation.
    static float omni_attenuation(float distance, float inv_range, float decay) {
        float nd = distance * inv_range;
        nd *= nd;
        nd *= nd; // nd^4
        nd = std::max(1.0F - nd, 0.0F);
        nd *= nd; // nd^2
        return nd * power(std::max(distance, 0.0001F), -decay);
    }

    static Point2 vogel_disk(float i, float rotation, float sample_count_sqrt) {
        constexpr float kGoldenAngle = 2.4F;
        const float r = std::sqrt(i + 0.5F) / sample_count_sqrt;
        const float theta = (i * kGoldenAngle) + rotation;
        const Vec2 cs = slash::cos_sin(theta);
        return {cs.x * r, cs.y * r};
    }

    struct Penumbra {
        float power = 0.0F;
        Vec3 color;
    };

    // One shadow ray's light, through surfaces that let some through, as trace_direct_light's
    // loops: from from to to, then on from each surface it passes.
    Penumbra shadow_ray(Vec3 from, Vec3 to, Vec3 light_dir, Vec3 light_color,
                        std::uint32_t* hits) const {
        constexpr float kEpsilon = 0.00001F;
        Penumbra out{.power = 0.0F, .color = light_color};
        bool did_hit = false;
        Vec3 origin = from;
        for (std::uint32_t iter = 0; iter < kTransparencyRays; ++iter) {
            const AlbedoHit hit = trace_albedo(origin, to);
            if (hit.hit == Hit::Miss) {
                if (!did_hit) {
                    out.power = 1.0F;
                }
                if (hits != nullptr) {
                    ++*hits;
                }
                break;
            }
            const bool contribute = hit.hit == Hit::Front || !did_hit;
            if (!did_hit) {
                out.power = 1.0F;
                did_hit = true;
            }
            if (hits != nullptr) {
                ++*hits;
            }
            if (contribute) {
                out.color = mix(out.color, mul(out.color, rgb(hit.albedo)), hit.albedo[3]);
                out.power *= 1.0F - hit.albedo[3];
            }
            origin = hit.position + (light_dir * settings_.bias);
            if (out.power - kEpsilon <= 0.0F) {
                break;
            }
        }
        return out;
    }

    // lm_compute.glsl's trace_direct_light: a light's light reaching a point, shadowed, and the
    // direction to the light. Soft shadowing samples the texel and the light's disk.
    Vec3 direct_light(Vec3 position, Vec3 normal, const Light& light, bool soft_shadowing,
                      std::uint32_t& noise, float texel_size, Vec3& light_dir_out) const {
        Vec3 light_pos;
        float dist = 0.0F;
        float attenuation = 1.0F;
        float disk_size = 0.0F;
        if (light.type == render::LightType::Directional) {
            light_pos = position - (light.direction * world_size_);
            light_dir_out = normalize(light_pos - position);
            dist = world_size_;
            disk_size = light.size;
        } else {
            light_pos = light.position;
            light_dir_out = normalize(light_pos - position);
            dist = length(position - light_pos);
            if (dist > light.range) {
                return {};
            }
            disk_size = light.size / dist;
            attenuation = omni_attenuation(dist, 1.0F / light.range, light.attenuation);
            if (light.type == render::LightType::Spot) {
                const Vec3 rel = normalize(position - light_pos);
                const float cos_angle = dot(rel, light.direction);
                if (cos_angle < light.cos_spot_angle) {
                    return {};
                }
                const float scos = std::max(cos_angle, light.cos_spot_angle);
                const float spot_rim =
                    std::max(0.0001F, (1.0F - scos) / (1.0F - light.cos_spot_angle));
                attenuation *= 1.0F - power(spot_rim, light.inv_spot_attenuation);
            }
        }
        attenuation *= std::max(0.0F, dot(normal, light_dir_out));
        if (attenuation <= 0.0001F) {
            return {};
        }
        const Vec3 r_light_dir = light_dir_out;
        float penumbra = 0.0F;
        Vec3 penumbra_color;
        if (soft_shadowing) {
            const bool use_soft_shadows = light.size > 0.0F;
            const std::uint32_t ray_count = kAaSamples;
            const std::uint32_t total_ray_count = use_soft_shadows ? settings_.rays : ray_count;
            const std::uint32_t shadowing_ray_count = std::max(1U, settings_.rays / ray_count);
            const float shadowing_ray_count_sqrt = std::sqrt(static_cast<float>(total_ray_count));
            // Samples over the texel...
            const Vec3 aux = normal.y < 0.777F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
            const Vec3 tangent = normalize(cross(normal, aux));
            const Vec3 bitan = normalize(cross(normal, tangent));
            // ...and over the light's disk, turned toward the point.
            const Vec3 light_to_point = r_light_dir * -1.0F;
            const Vec3 light_aux =
                light_to_point.y < 0.777F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
            const Vec3 light_to_point_tan = normalize(cross(light_to_point, light_aux));
            const Vec3 light_to_point_bitan = normalize(cross(light_to_point, light_to_point_tan));
            float aa_power = 0.0F;
            for (std::uint32_t i = 0; i < ray_count; ++i) {
                const std::array<float, 2>& halton = kHalton[i];
                const Point2 disk_sample{(halton[0] - 0.5F) * texel_size * light.shadow_blur,
                                         (halton[1] - 0.5F) * texel_size * light.shadow_blur};
                const Vec3 disk_aligned = (tangent * disk_sample.x) + (bitan * disk_sample.y);
                const Vec3 origin = position - disk_aligned;
                const float disk_length =
                    std::sqrt((disk_sample.x * disk_sample.x) + (disk_sample.y * disk_sample.y));
                float power_sum = 0.0F;
                Vec3 light_color;
                std::uint32_t power_accm = 0;
                if (use_soft_shadows) {
                    std::uint32_t soft_shadow_hits = 0;
                    for (std::uint32_t j = 0; j < shadowing_ray_count; ++j) {
                        // Once enough rays are traced, all hits or all misses mean the point
                        // isn't in the penumbra, and the rest would agree.
                        if (j > 0 && j == shadowing_ray_count / 2) {
                            if (soft_shadow_hits == j || soft_shadow_hits == 0) {
                                break;
                            }
                        }
                        const float a = randomize(noise) * 2.0F * kPi;
                        // The outer points first.
                        const auto vogel_index = static_cast<float>(
                            total_ray_count - 1 - ((i * shadowing_ray_count) + j));
                        const Point2 d = vogel_disk(vogel_index, a, shadowing_ray_count_sqrt);
                        const Point2 light_disk{d.x * disk_size * light.shadow_blur,
                                                d.y * disk_size * light.shadow_blur};
                        const Vec3 light_disk_to_point =
                            normalize(light_to_point + (light_to_point_tan * light_disk.x) +
                                      (light_to_point_bitan * light_disk.y));
                        // The ray's origin moved for the texel, its end for the light's disk.
                        const Penumbra sample = shadow_ray(
                            origin - (light_disk_to_point * (settings_.bias + disk_length)),
                            position - (light_disk_to_point * dist), r_light_dir, light.color,
                            &soft_shadow_hits);
                        power_sum += sample.power;
                        light_color = light_color + sample.color;
                        ++power_accm;
                    }
                } else {
                    const Vec3 light_dir = normalize(light_pos - origin);
                    const Penumbra sample =
                        shadow_ray(origin + (light_dir * (settings_.bias + disk_length)), light_pos,
                                   r_light_dir, light.color, nullptr);
                    power_sum = sample.power;
                    light_color = sample.color;
                    power_accm = 1;
                }
                aa_power += power_sum / static_cast<float>(power_accm);
                penumbra_color =
                    penumbra_color + (light_color * (1.0F / static_cast<float>(power_accm)));
            }
            penumbra = aa_power / static_cast<float>(ray_count);
            penumbra_color = penumbra_color * (1.0F / static_cast<float>(ray_count));
        } else {
            const Penumbra sample = shadow_ray(position + (r_light_dir * settings_.bias), light_pos,
                                               r_light_dir, light.color, nullptr);
            penumbra = std::clamp(sample.power, 0.0F, 1.0F);
            penumbra_color = sample.color;
        }
        return penumbra_color * (light.energy * attenuation * penumbra);
    }

    // The world-space size of a texel: the distance to its neighbor's position.
    [[nodiscard]] Vec3 position_at(int slice, int x, int y) const {
        const Rgba p = position_.fetch(slice, x, y);
        return {p[0], p[1], p[2]};
    }

    // MODE_DIRECT_LIGHT: each texel's light from the lights, those that bake whole into the
    // lightmap and all of them, times their indirect energy, for bounces.
    void direct_light() {
        light_source_.resize(albedo_.width, albedo_.height, albedo_.slices, Rgba{});
        accum_.resize(albedo_.width, albedo_.height, albedo_.slices, Rgba{});
        each_texel([&](int slice, int x, int y) {
            const Vec3 normal = normal_.fetch(slice, x, y);
            if (length(normal) < 0.5F) {
                return; // empty
            }
            const Vec3 position = position_at(slice, x, y);
            Rgba neighbor = position_.fetch(slice, x + 1, y);
            if (neighbor[3] < 0.001F) {
                const Rgba left = position_.fetch(slice, x - 1, y);
                neighbor = {left[0], left[1], left[2], neighbor[3]};
            }
            const float texel_size =
                length(position - Vec3{neighbor[0], neighbor[1], neighbor[2]}) *
                settings_.supersampling;
            Vec3 light_for_texture;
            Vec3 light_for_bounces;
            std::uint32_t noise = random_seed(static_cast<std::uint32_t>(x),
                                              static_cast<std::uint32_t>(y), 43573547U);
            for (const Light& light : lights_) {
                Vec3 light_dir;
                const Vec3 lit =
                    direct_light(position, normal, light, true, noise, texel_size, light_dir);
                if (light.static_bake) {
                    light_for_texture = light_for_texture + lit;
                }
                light_for_bounces = light_for_bounces + (lit * light.indirect_energy);
            }
            light_for_bounces = light_for_bounces * settings_.exposure;
            light_for_texture = light_for_texture * settings_.exposure;
            light_source_(slice, x, y) = {light_for_bounces.x, light_for_bounces.y,
                                          light_for_bounces.z, 1.0F};
            accum_(slice, x, y) = {light_for_texture.x, light_for_texture.y, light_for_texture.z,
                                   1.0F};
        });
    }

    // trace_environment_color.
    [[nodiscard]] Vec3 environment_color(Vec3 ray_dir) const {
        const Vec3 sky_dir = normalize(ray_dir);
        float s = slash::atan2(sky_dir.x, sky_dir.z);
        const float y = std::clamp(sky_dir.y, -1.0F, 1.0F);
        const float t = slash::atan2(std::sqrt((1.0F - y) * (1.0F + y)), y); // acos(y)
        if (s < 0.0F) {
            s += kPi * 2.0F;
        }
        return rgb(sample(environment_, 0, s / (kPi * 2.0F), t / kPi));
    }

    // The direct light reaching a point a bounce hits: from the lightmap's light for bounces,
    // or traced from the lights.
    [[nodiscard]] Vec3 bounce_direct(const Triangle& t, float u, float v, Vec3 position,
                                     Vec3 normal, std::uint32_t& noise, float texel_size) const {
        if (settings_.use_texture_for_bounces) {
            return rgb(sample(light_source_, static_cast<int>(t.slice), u, v));
        }
        Vec3 direct;
        for (const Light& light : lights_) {
            Vec3 light_dir;
            const Vec3 lit =
                direct_light(position, normal, light, false, noise, texel_size, light_dir);
            direct = direct + (lit * light.indirect_energy);
        }
        return direct * settings_.exposure;
    }

    // lm_compute.glsl's trace_indirect_light: the light a path brings back from a direction.
    Vec3 indirect_light(Vec3 start, Vec3 start_dir, std::uint32_t& noise, float texel_size) const {
        Vec3 position = start;
        Vec3 ray_dir = start_dir;
        const int max_depth = std::max(static_cast<int>(settings_.bounces), 1);
        int transparency_rays_left = static_cast<int>(kTransparencyRays);
        Vec3 throughput{1.0F, 1.0F, 1.0F};
        Vec3 light;
        for (int depth = 0; depth < max_depth; ++depth) {
            const Bvh::Result hit =
                trace(position + (ray_dir * settings_.bias), position + (ray_dir * world_size_));
            if (hit.hit == Hit::Miss) {
                // The environment's color, and no more bounces.
                light = light + mul(throughput, environment_color(ray_dir));
                break;
            }
            const Triangle& t = triangles_[hit.triangle];
            const Vertex& a = vertices_[t.indices[0]];
            const Vertex& b = vertices_[t.indices[1]];
            const Vertex& c = vertices_[t.indices[2]];
            const Vec3 w = hit.barycentric;
            const float u = (w.x * a.uv[0]) + (w.y * b.uv[0]) + (w.z * c.uv[0]);
            const float v = (w.x * a.uv[1]) + (w.y * b.uv[1]) + (w.z * c.uv[1]);
            position = (a.position * w.x) + (b.position * w.y) + (c.position * w.z);
            Vec3 normal = (a.normal * w.x) + (b.normal * w.y) + (c.normal * w.z);
            const Rgba albedo_alpha = sample(albedo_, static_cast<int>(t.slice), u, v);
            const float alpha = albedo_alpha[3];
            if (hit.hit == Hit::Front) {
                const Vec3 prev_normal = ray_dir;
                const Vec3 direct = bounce_direct(t, u, v, position, normal, noise, texel_size);
                const Vec3 emissive =
                    rgb(sample(emission_, static_cast<int>(t.slice), u, v)) * settings_.exposure;
                light = light + (mul(throughput, emissive) * alpha);
                throughput = mix(throughput, mul(throughput, rgb(albedo_alpha)), alpha);
                light =
                    light + (mul(throughput, direct) * (settings_.bounce_indirect_energy * alpha));
                if (alpha < 1.0F) {
                    --transparency_rays_left;
                    --depth;
                    if (transparency_rays_left <= 0) {
                        break;
                    }
                    // Through the surface, or off it.
                    const float pa = alpha * alpha;
                    if (randomize(noise) > pa) {
                        normal = prev_normal;
                    }
                    position = position + (normal * settings_.bias);
                }
                // Russian roulette ends paths early, by how much light they still carry.
                const float p = max3(throughput);
                if (randomize(noise) > p) {
                    break;
                }
                throughput = throughput * (1.0F / p);
                ray_dir = ray_dir_from_normal(normal, noise);
                continue;
            }
            // A back face: only light through it, and its own glow.
            --transparency_rays_left;
            --depth;
            if (transparency_rays_left <= 0) {
                break;
            }
            const Vec3 direct = bounce_direct(t, u, v, position, normal, noise, texel_size);
            const Vec3 emissive =
                rgb(sample(emission_, static_cast<int>(t.slice), u, v)) * settings_.exposure;
            light = light + (mul(throughput, emissive) * alpha);
            throughput =
                mix(mix(throughput, mul(throughput, rgb(albedo_alpha)), alpha), Vec3{}, alpha);
            light = light + (mul(throughput, direct) * (settings_.bounce_indirect_energy * alpha));
            position = position + (ray_dir * settings_.bias);
        }
        return light;
    }

    // MODE_BOUNCE_LIGHT, in passes of kMaxRaysPerPass rays as Godot's, each seeded afresh.
    void bounce_light() {
        const std::uint32_t ray_count = settings_.rays;
        each_texel([&](int slice, int x, int y) {
            const Vec3 normal = normal_.fetch(slice, x, y);
            if (length(normal) < 0.5F) {
                return;
            }
            const Vec3 position = position_at(slice, x, y);
            const int neighbor_offset = x < albedo_.width - 1 ? 1 : -1;
            const float texel_size = length(position - position_at(slice, x + neighbor_offset, y));
            Rgba& accum = accum_(slice, x, y);
            for (std::uint32_t from = 0; from < ray_count; from += kMaxRaysPerPass) {
                const std::uint32_t to = std::min(from + kMaxRaysPerPass, ray_count);
                std::uint32_t noise =
                    random_seed(from, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                Vec3 light_accum;
                for (std::uint32_t i = from; i < to; ++i) {
                    const Vec3 ray_dir = ray_dir_from_normal(normal, noise);
                    light_accum =
                        light_accum + indirect_light(position, ray_dir, noise, texel_size);
                }
                const Vec3 added = light_accum * (1.0F / static_cast<float>(ray_count));
                accum = {accum[0] + added.x, accum[1] + added.y, accum[2] + added.z, accum[3]};
            }
        });
    }

    // MODE_LIGHT_PROBES: each probe's light as spherical harmonics, from rays in every direction,
    // in passes of kMaxRaysPerProbePass seeded afresh as Godot's. The rays leave in the probes'
    // space, turned into the world's, so the harmonics are kept in the probes' space.
    std::vector<LightmapSh> light_probes() {
        const std::vector<std::array<float, 3>>& points = probes_->points;
        std::vector<LightmapSh> out(points.size());
        if (points.empty()) {
            return out;
        }
        const std::uint32_t ray_count = std::clamp(probes_->rays, 16U, 8192U);
        const world::Affine3D space = world::orthonormalized(probes_->space);
        for_each(pool_, points.size(), [&](std::size_t probe) {
            const Vec3 position{points[probe][0], points[probe][1], points[probe][2]};
            std::array<Vec3, 9> accum{};
            for (std::uint32_t from = 0; from < ray_count; from += kMaxRaysPerProbePass) {
                const std::uint32_t to = std::min(from + kMaxRaysPerProbePass, ray_count);
                std::uint32_t noise =
                    random_seed(from, static_cast<std::uint32_t>(probe), 49502741U);
                for (std::uint32_t i = from; i < to; ++i) {
                    const Vec3 d = sphere_uniform_direction(noise);
                    const Vec3 ray_dir = space.basis(d);
                    const Vec3 light = indirect_light(position, ray_dir, noise, 0.0F);
                    const std::array<float, 9> c{0.282095F,
                                                 0.488603F * d.y,
                                                 0.488603F * d.z,
                                                 0.488603F * d.x,
                                                 1.092548F * d.x * d.y,
                                                 1.092548F * d.y * d.z,
                                                 0.315392F * ((3.0F * d.z * d.z) - 1.0F),
                                                 1.092548F * d.x * d.z,
                                                 0.546274F * ((d.x * d.x) - (d.y * d.y))};
                    for (std::size_t j = 0; j < 9; ++j) {
                        accum.at(j) = accum.at(j) + (light * c.at(j));
                    }
                }
            }
            const float scale = 4.0F / static_cast<float>(ray_count);
            for (std::size_t j = 0; j < 9; ++j) {
                out[probe].at(j) = {accum.at(j).x * scale, accum.at(j).y * scale,
                                    accum.at(j).z * scale};
            }
        });
        return out;
    }

    // MODE_DENOISE: Godot's joint non-local means filter, each texel a weighted mean of those
    // within denoiser_range whose neighborhoods, albedo and normals look like its own. The
    // patches' distances are box sums here, each offset's over the whole slice at once.
    void denoise() {
        constexpr int kHalfPatch = 3;
        constexpr int kPatch = (kHalfPatch * 2) + 1;
        const int half_search = static_cast<int>(settings_.denoiser_range);
        constexpr float kSigmaSpatial = 5.0F;
        const float sigma_light = std::max(settings_.denoiser_strength, 0.001F);
        constexpr float kSigmaAlbedo = 1.0F;
        constexpr float kSigmaNormal = 0.1F;
        const float filter = 10.0F * sigma_light;
        const float two_sigma_spatial_sq = 2.0F * kSigmaSpatial * kSigmaSpatial;
        const float two_sigma_light_sq = 2.0F * sigma_light * sigma_light;
        const float two_sigma_albedo_sq = 2.0F * kSigmaAlbedo * kSigmaAlbedo;
        const float two_sigma_normal_sq = 2.0F * kSigmaNormal * kSigmaNormal;
        const float filter_sq_two_sigma_light_sq = filter * filter * two_sigma_light_sq;
        constexpr float kEpsilon = 1e-6F;
        const int w = albedo_.width;
        const int h = albedo_.height;
        const int pw = w + (2 * kHalfPatch);
        const int ph = h + (2 * kHalfPatch);
        Layers<Rgba> out = accum_;
        std::vector<float> delta(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph));
        std::vector<float> rows(static_cast<std::size_t>(w) * static_cast<std::size_t>(ph));
        std::vector<Vec3> sum(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        std::vector<float> weights(sum.size());
        for (int slice = 0; slice < albedo_.slices; ++slice) {
            std::fill(sum.begin(), sum.end(), Vec3{});
            std::fill(weights.begin(), weights.end(), 0.0F);
            const auto light = [&](int x, int y) { return rgb(accum_.fetch(slice, x, y)); };
            for (int sy = -half_search; sy <= half_search; ++sy) {
                for (int sx = -half_search; sx <= half_search; ++sx) {
                    // Each texel's light against the one at the offset, less the noise expected.
                    for_each(pool_, static_cast<std::size_t>(ph), [&](std::size_t row) {
                        const int y = static_cast<int>(row) - kHalfPatch;
                        for (int i = 0; i < pw; ++i) {
                            const int x = i - kHalfPatch;
                            const Vec3 d = light(x, y) - light(x + sx, y + sy);
                            delta[(row * static_cast<std::size_t>(pw)) +
                                  static_cast<std::size_t>(i)] = dot(d, d) - two_sigma_light_sq;
                        }
                    });
                    for_each(pool_, static_cast<std::size_t>(ph), [&](std::size_t row) {
                        for (int x = 0; x < w; ++x) {
                            float s = 0.0F;
                            for (int k = 0; k < kPatch; ++k) {
                                s += delta[(row * static_cast<std::size_t>(pw)) +
                                           static_cast<std::size_t>(x + k)];
                            }
                            rows[(row * static_cast<std::size_t>(w)) +
                                 static_cast<std::size_t>(x)] = s;
                        }
                    });
                    const float spatial =
                        static_cast<float>((sx * sx) + (sy * sy)) / two_sigma_spatial_sq;
                    for_each(pool_, static_cast<std::size_t>(h), [&](std::size_t row) {
                        const int y = static_cast<int>(row);
                        for (int x = 0; x < w; ++x) {
                            const int qx = x + sx;
                            const int qy = y + sy;
                            if (!accum_.inside(qx, qy)) {
                                continue;
                            }
                            const Vec3 search_normal = normal_.fetch(slice, qx, qy);
                            if (length(search_normal) < kEpsilon) {
                                continue;
                            }
                            const float occlusion = unocclude_.fetch(slice, qx, qy)[0];
                            float patch = 0.0F;
                            for (int k = 0; k < kPatch; ++k) {
                                patch += rows[((row + static_cast<std::size_t>(k)) *
                                               static_cast<std::size_t>(w)) +
                                              static_cast<std::size_t>(x)];
                            }
                            patch = std::max(0.0F, patch / (3.0F * kPatch * kPatch));
                            const Vec3 albedo_delta =
                                rgb(albedo_.fetch(slice, x, y)) - rgb(albedo_.fetch(slice, qx, qy));
                            const Vec3 normal_delta = normal_.fetch(slice, x, y) - search_normal;
                            const float weight =
                                slash::exp(
                                    -spatial - (patch / filter_sq_two_sigma_light_sq) -
                                    (dot(albedo_delta, albedo_delta) / two_sigma_albedo_sq) -
                                    (dot(normal_delta, normal_delta) / two_sigma_normal_sq)) *
                                (1.0F - occlusion);
                            const std::size_t at =
                                (row * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x);
                            sum[at] = sum[at] + (light(qx, qy) * weight);
                            weights[at] += weight;
                        }
                    });
                }
            }
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (length(normal_.fetch(slice, x, y)) <= kEpsilon) {
                        continue; // left as it is
                    }
                    const std::size_t at =
                        (static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) +
                        static_cast<std::size_t>(x);
                    if (weights[at] > kEpsilon) {
                        const Vec3 denoised = sum[at] * (1.0F / weights[at]);
                        Rgba& texel = out(slice, x, y);
                        texel = {denoised.x, denoised.y, denoised.z, texel[3]};
                    }
                }
            }
        }
        accum_ = std::move(out);
    }

    // MODE_DILATE: an empty texel takes the nearest lit one's light, up to 4 texels away (times
    // the supersampling), the sides before the corners at each distance.
    void dilate() {
        const int max_radius = static_cast<int>(4.0F * settings_.supersampling);
        static constexpr std::array<std::array<int, 2>, 8> kDirections{
            {{-1, 0}, {0, 1}, {1, 0}, {0, -1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1}}};
        Layers<Rgba> out = accum_;
        each_texel([&](int slice, int x, int y) {
            if (accum_.texels[accum_.at(slice, x, y)][3] > 0.5F) {
                return;
            }
            for (int radius = 1; radius <= max_radius; ++radius) {
                for (const auto& d : kDirections) {
                    const int qx = x + (d[0] * radius);
                    const int qy = y + (d[1] * radius);
                    if (!accum_.inside(qx, qy)) {
                        continue;
                    }
                    const Rgba& neighbor = accum_.texels[accum_.at(slice, qx, qy)];
                    if (neighbor[3] > 0.5F) {
                        out(slice, x, y) = neighbor;
                        return;
                    }
                }
            }
        });
        accum_ = std::move(out);
    }

    // lm_blendseams.glsl: across each seam, each side's texels are blended half with the other
    // side's light, first along the seam, then in the empty texels around it, each texel once.
    void blend_seams() {
        const Layers<Rgba> source = accum_;
        static constexpr std::array<std::array<float, 2>, 9> kOffsets{{{0.0F, 0.0F},
                                                                       {0.0F, 1.0F},
                                                                       {0.0F, -1.0F},
                                                                       {1.0F, 0.0F},
                                                                       {-1.0F, 0.0F},
                                                                       {-1.0F, -1.0F},
                                                                       {1.0F, -1.0F},
                                                                       {1.0F, 1.0F},
                                                                       {-1.0F, 1.0F}}};
        constexpr float kBlend = 0.5F;
        std::size_t seam_begin = 0;
        std::size_t triangle_begin = 0;
        std::vector<bool> drawn(static_cast<std::size_t>(accum_.width) *
                                static_cast<std::size_t>(accum_.height));
        for (int slice = 0; slice < accum_.slices; ++slice) {
            std::size_t seam_end = seam_begin;
            while (seam_end < seams_.size() &&
                   seams_[seam_end].slice == static_cast<std::uint32_t>(slice)) {
                ++seam_end;
            }
            std::size_t triangle_end = triangle_begin;
            while (triangle_end < triangles_.size() &&
                   triangles_[triangle_end].slice == static_cast<std::uint32_t>(slice)) {
                ++triangle_end;
            }
            if (seam_end == seam_begin) {
                seam_begin = seam_end;
                triangle_begin = triangle_end;
                continue;
            }
            std::fill(drawn.begin(), drawn.end(), false);
            const auto lines = [&](const std::array<float, 2>& offset) {
                for (std::size_t i = seam_begin; i < seam_end; ++i) {
                    const Seam& seam = seams_[i];
                    // Each side drawn over the other: b's light on a's texels, then a's on b's.
                    const std::array<std::array<std::uint32_t, 2>, 2> src{seam.b, seam.a};
                    const std::array<std::array<std::uint32_t, 2>, 2> dst{seam.a, seam.b};
                    for (std::size_t side = 0; side < 2; ++side) {
                        const auto at = [&](std::uint32_t vertex) {
                            const Point2 p = raster_texel(vertex);
                            return Point2{p.x + offset[0], p.y + offset[1]};
                        };
                        const std::array<float, 2>& s0 = vertices_[src[side][0]].uv;
                        const std::array<float, 2>& s1 = vertices_[src[side][1]].uv;
                        draw_line(at(dst[side][0]), at(dst[side][1]), accum_.width, accum_.height,
                                  [&](int x, int y, float along) {
                                      const std::size_t texel =
                                          (static_cast<std::size_t>(y) *
                                           static_cast<std::size_t>(accum_.width)) +
                                          static_cast<std::size_t>(x);
                                      if (drawn[texel]) {
                                          return;
                                      }
                                      drawn[texel] = true;
                                      const Rgba color =
                                          sample(source, slice, s0[0] + ((s1[0] - s0[0]) * along),
                                                 s0[1] + ((s1[1] - s0[1]) * along));
                                      Rgba& out = accum_(slice, x, y);
                                      for (std::size_t c = 0; c < 3; ++c) {
                                          out[c] = (color[c] * kBlend) + (out[c] * (1.0F - kBlend));
                                      }
                                  });
                    }
                }
            };
            lines(kOffsets[0]);
            // The triangles mask their insides, which the lines around the seams leave alone.
            for (std::size_t i = triangle_begin; i < triangle_end; ++i) {
                const Triangle& t = triangles_[i];
                const std::array<Point2, 3> s{raster_texel(t.indices[0]),
                                              raster_texel(t.indices[1]),
                                              raster_texel(t.indices[2])};
                fill_triangle(s, accum_.width, accum_.height, [&](int x, int y, Vec3 /*w*/) {
                    drawn[(static_cast<std::size_t>(y) * static_cast<std::size_t>(accum_.width)) +
                          static_cast<std::size_t>(x)] = true;
                });
            }
            for (std::size_t j = 1; j < kOffsets.size(); ++j) {
                lines(kOffsets[j]);
            }
            seam_begin = seam_end;
            triangle_begin = triangle_end;
        }
    }

    // The texels as the .hdr file holds them, shrunk by the supersampling as Godot resizes them,
    // and each mesh's place.
    void finish(LightmapBake& out) {
        const int w = accum_.width;
        const int h = accum_.height;
        const int tw =
            std::max(1, static_cast<int>(static_cast<float>(w) / settings_.supersampling));
        const int th =
            std::max(1, static_cast<int>(static_cast<float>(h) / settings_.supersampling));
        out.texels.width = static_cast<std::uint32_t>(tw);
        out.texels.height = static_cast<std::uint32_t>(th * accum_.slices);
        out.texels.rgb.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) *
                                  static_cast<std::size_t>(accum_.slices) * 3,
                              0.0F);
        for (int slice = 0; slice < accum_.slices; ++slice) {
            for (int y = 0; y < th; ++y) {
                // The source texels whose centers fall in this one.
                const int y0 = (y * h) / th;
                const int y1 = std::max(y0 + 1, ((y + 1) * h) / th);
                for (int x = 0; x < tw; ++x) {
                    const int x0 = (x * w) / tw;
                    const int x1 = std::max(x0 + 1, ((x + 1) * w) / tw);
                    Vec3 total;
                    for (int sy = y0; sy < y1; ++sy) {
                        for (int sx = x0; sx < x1; ++sx) {
                            total = total + rgb(accum_.texels[accum_.at(slice, sx, sy)]);
                        }
                    }
                    total = total * (1.0F / static_cast<float>((x1 - x0) * (y1 - y0)));
                    const std::size_t at =
                        ((static_cast<std::size_t>(slice * th + y) * static_cast<std::size_t>(tw)) +
                         static_cast<std::size_t>(x)) *
                        3;
                    out.texels.rgb[at] = total.x;
                    out.texels.rgb[at + 1] = total.y;
                    out.texels.rgb[at + 2] = total.z;
                }
            }
        }
        out.data.width = static_cast<std::uint32_t>(tw);
        out.data.height = static_cast<std::uint32_t>(th);
        out.data.slices = static_cast<std::uint32_t>(accum_.slices);
        out.data.baked_exposure = settings_.exposure;
        for (std::size_t i = 0; i < instances_.size(); ++i) {
            const MeshInstance& mesh = instances_[i];
            LightmapUser& user = out.data.users[i];
            user.slice = static_cast<std::uint32_t>(mesh.slice);
            user.uv_rect = {static_cast<float>(mesh.offset_x) / static_cast<float>(w),
                            static_cast<float>(mesh.offset_y) / static_cast<float>(h),
                            static_cast<float>(mesh.width) / static_cast<float>(w),
                            static_cast<float>(mesh.height) / static_cast<float>(h)};
        }
    }

    static constexpr std::array<std::array<float, 2>, kAaSamples> kHalton{
        {{0.5F, 0.33333333F},
         {0.25F, 0.66666667F},
         {0.75F, 0.11111111F},
         {0.125F, 0.44444444F},
         {0.625F, 0.77777778F},
         {0.375F, 0.22222222F},
         {0.875F, 0.55555556F},
         {0.0625F, 0.88888889F},
         {0.5625F, 0.03703704F},
         {0.3125F, 0.37037037F},
         {0.8125F, 0.7037037F},
         {0.1875F, 0.14814815F},
         {0.6875F, 0.48148148F},
         {0.4375F, 0.81481481F},
         {0.9375F, 0.25925926F},
         {0.03125F, 0.59259259F}}};

    LightmapSettings settings_;
    jobs::Pool* pool_;
    std::vector<MeshInstance> instances_;
    std::vector<Light> lights_;
    Layers<Rgba> environment_;
    Layers<Rgba> albedo_;
    Layers<Rgba> emission_;
    std::vector<Vertex> vertices_;
    std::vector<Triangle> triangles_;
    std::vector<Seam> seams_;
    std::unique_ptr<Bvh> bvh_;
    float world_size_ = 0.0F;
    const LightmapProbeRequest* probes_ = nullptr;
    Layers<Rgba> position_;
    Layers<Vec3> normal_;
    Layers<Rgba> unocclude_;
    Layers<Rgba> light_source_;
    Layers<Rgba> accum_;
};

} // namespace

Result<LightmapBake> bake_lightmap(const std::vector<LightmapMesh>& meshes,
                                   const std::vector<LightmapLight>& lights,
                                   const render::HdrImage& environment,
                                   const LightmapSettings& settings, jobs::Pool* pool,
                                   const LightmapProbeRequest& probes) {
    Baker baker(settings, pool);
    return baker.bake(meshes, lights, environment, probes);
}

} // namespace slash::scene3d
