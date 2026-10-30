#include "path_trace.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>

namespace slash::render::detail {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
// Each leaf holds at most this many triangles.
constexpr std::uint32_t kLeafSize = 4;
// Bounces from this one on may end a path early, as Russian roulette ends them, by how much
// light it still carries.
constexpr std::uint32_t kRouletteBounce = 3;
// The roughness bounces sample and are lit with at least, so that mirrors reflect (light_compute
// leaves the specular light of a surface with none out).
constexpr float kMinBounceRoughness = 0.03F;

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Float3 mul(const Float3& a, const Float3& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}
Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
float dot(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}
Float3 cross(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}
Float3 normalize(const Float3& a) {
    const float length = std::sqrt(dot(a, a));
    return length > 0.0F ? scale(a, 1.0F / length) : Float3{0.0F, 1.0F, 0.0F};
}
float luminance(const Float3& c) {
    return (c[0] * 0.2126F) + (c[1] * 0.7152F) + (c[2] * 0.0722F);
}
Float3 xyz(const Float4& v) {
    return {v[0], v[1], v[2]};
}

// A 32-bit integer hash with good avalanche (Chris Wellons' lowbias32).
std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

// A path's random numbers: a counter hashed with its seed.
struct Random {
    std::uint32_t seed = 0;
    std::uint32_t counter = 0;

    // From 0 up to but not including 1, in steps of 2^-24.
    float next() {
        const std::uint32_t bits = mix(seed ^ mix(counter++));
        return static_cast<float>(bits >> 8U) * (1.0F / 16777216.0F);
    }
};

// A unit vector square to n, and the third axis.
void basis(const Float3& n, Float3& t, Float3& b) {
    const float sign = n[2] >= 0.0F ? 1.0F : -1.0F;
    const float a = -1.0F / (sign + n[2]);
    const float c = n[0] * n[1] * a;
    t = {1.0F + (sign * n[0] * n[0] * a), sign * c, -sign * n[0]};
    b = {c, sign + (n[1] * n[1] * a), -n[1]};
}

Float3 from_basis(const Float3& local, const Float3& t, const Float3& b, const Float3& n) {
    return add(add(scale(t, local[0]), scale(b, local[1])), scale(n, local[2]));
}

// Cosine-weighted directions about n.
Float3 sample_cosine(const Float3& n, float u1, float u2) {
    const float r = std::sqrt(u1);
    const Vec2 cs = slash::cos_sin(2.0F * kPi * u2);
    const Float3 local{r * cs.x, r * cs.y, std::sqrt(std::max(0.0F, 1.0F - u1))};
    Float3 t{};
    Float3 b{};
    basis(n, t, b);
    return normalize(from_basis(local, t, b, n));
}

// GGX's microfacet normals as v sees them (Heitz, "Sampling the GGX Distribution of Visible
// Normals", 2018), about n.
Float3 sample_visible_normal(const Float3& n, const Float3& v, float alpha, float u1, float u2) {
    Float3 t{};
    Float3 b{};
    basis(n, t, b);
    const Float3 vl{dot(v, t), dot(v, b), dot(v, n)};
    const Float3 vh = normalize({alpha * vl[0], alpha * vl[1], vl[2]});
    const float lensq = (vh[0] * vh[0]) + (vh[1] * vh[1]);
    const Float3 t1 = lensq > 0.0F ? scale(Float3{-vh[1], vh[0], 0.0F}, 1.0F / std::sqrt(lensq))
                                   : Float3{1.0F, 0.0F, 0.0F};
    const Float3 t2 = cross(vh, t1);
    const float r = std::sqrt(u1);
    const Vec2 cs = slash::cos_sin(2.0F * kPi * u2);
    const float p1 = r * cs.x;
    float p2 = r * cs.y;
    const float s = 0.5F * (1.0F + vh[2]);
    p2 = ((1.0F - s) * std::sqrt(std::max(0.0F, 1.0F - (p1 * p1)))) + (s * p2);
    const Float3 nh = add(add(scale(t1, p1), scale(t2, p2)),
                          scale(vh, std::sqrt(std::max(0.0F, 1.0F - (p1 * p1) - (p2 * p2)))));
    const Float3 local = normalize({alpha * nh[0], alpha * nh[1], std::max(0.0F, nh[2])});
    return normalize(from_basis(local, t, b, n));
}

// GGX's normal distribution, as shaders/mesh.hlsl's D_GGX.
float ggx(float cos_theta, float alpha) {
    const float a = cos_theta * alpha;
    const float k = alpha / (1.0F - (cos_theta * cos_theta) + (a * a));
    return k * k / kPi;
}

// Smith's masking of GGX for one direction.
float smith(float cos_theta, float alpha) {
    const float a2 = alpha * alpha;
    return 2.0F * cos_theta / (cos_theta + std::sqrt(a2 + ((1.0F - a2) * cos_theta * cos_theta)));
}

// The box around points.
struct Box {
    Float3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()};
    Float3 high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest()};

    void grow(const Float3& p) {
        for (std::size_t i = 0; i < 3; ++i) {
            low.at(i) = std::min(low.at(i), p.at(i));
            high.at(i) = std::max(high.at(i), p.at(i));
        }
    }
};

Float3 corner_position(const PathScene& scene, const PathTriangle& triangle, std::size_t i) {
    return scene.corners[triangle.corners.at(i)].world;
}

// Builds the hierarchy over triangles [first, first + count): each node split at the median of
// its triangles' centers along its box's longest side, ties broken by the triangles' order, so
// the tree is the same everywhere.
void build_node(PathScene& scene, std::vector<Float3>& centers, std::vector<std::uint32_t>& order,
                std::uint32_t node, std::uint32_t first, std::uint32_t count) {
    Box box;
    for (std::uint32_t i = first; i < first + count; ++i) {
        const PathTriangle& triangle = scene.triangles[order[i]];
        for (std::size_t c = 0; c < 3; ++c) {
            box.grow(corner_position(scene, triangle, c));
        }
    }
    scene.nodes[node].low = box.low;
    scene.nodes[node].high = box.high;
    if (count <= kLeafSize) {
        scene.nodes[node].first = first;
        scene.nodes[node].count = count;
        return;
    }
    Box spread;
    for (std::uint32_t i = first; i < first + count; ++i) {
        spread.grow(centers[order[i]]);
    }
    std::size_t axis = 0;
    for (std::size_t i = 1; i < 3; ++i) {
        if (spread.high.at(i) - spread.low.at(i) > spread.high.at(axis) - spread.low.at(axis)) {
            axis = i;
        }
    }
    const auto begin = order.begin() + static_cast<std::ptrdiff_t>(first);
    std::sort(begin, begin + static_cast<std::ptrdiff_t>(count),
              [&](std::uint32_t a, std::uint32_t b) {
                  const float ca = centers[a].at(axis);
                  const float cb = centers[b].at(axis);
                  return ca < cb || (ca == cb && a < b);
              });
    const std::uint32_t half = count / 2;
    const auto left = static_cast<std::uint32_t>(scene.nodes.size());
    scene.nodes.emplace_back();
    scene.nodes.emplace_back();
    scene.nodes[node].first = left;
    scene.nodes[node].count = 0;
    build_node(scene, centers, order, left, first, half);
    build_node(scene, centers, order, left + 1, first + half, count - half);
}

// Where a ray meets a box, as the near and far distances, or false.
bool meets_box(const PathNode& node, const Float3& origin, const Float3& inverse, float limit,
               float& near) {
    float t0 = 0.0F;
    float t1 = limit;
    for (std::size_t i = 0; i < 3; ++i) {
        float a = (node.low.at(i) - origin.at(i)) * inverse.at(i);
        float b = (node.high.at(i) - origin.at(i)) * inverse.at(i);
        if (a > b) {
            std::swap(a, b);
        }
        // A ray along the slab's plane gives NaN: it is inside it or never meets it.
        if (std::isnan(a) || std::isnan(b)) {
            if (origin.at(i) < node.low.at(i) || origin.at(i) > node.high.at(i)) {
                return false;
            }
            continue;
        }
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) {
            return false;
        }
    }
    near = t0;
    return true;
}

// Möller and Trumbore's ray and triangle test: the distance and the second and third corners'
// shares, and whether the ray meets the triangle's front, which winds clockwise as seen by the
// ray, as Godot's front faces wind on the screen.
struct Crossing {
    float distance = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    bool front = true;
};
std::optional<Crossing> cross_triangle(const Float3& origin, const Float3& direction,
                                       const Float3& a, const Float3& b, const Float3& c) {
    const Float3 e1 = sub(b, a);
    const Float3 e2 = sub(c, a);
    const Float3 p = cross(direction, e2);
    const float det = dot(e1, p);
    if (det == 0.0F || !std::isfinite(det)) {
        return std::nullopt;
    }
    const float inverse = 1.0F / det;
    const Float3 s = sub(origin, a);
    const float u = dot(s, p) * inverse;
    if (u < 0.0F || u > 1.0F) {
        return std::nullopt;
    }
    const Float3 q = cross(s, e1);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) {
        return std::nullopt;
    }
    const float t = dot(e2, q) * inverse;
    if (!(t > 0.0F)) {
        return std::nullopt;
    }
    // det is the direction against e1 x e2, the counterclockwise normal: positive when the
    // corners turn clockwise as the ray sees them.
    return Crossing{.distance = t, .u = u, .v = v, .front = det < 0.0F};
}

enum class RayKind : std::uint8_t { Camera, Shadow };

// The corners' values at a point of a triangle, as the rasterizer gives them to a pixel.
MeshPixel pixel_at(const PathScene& scene, const PathTriangle& triangle, float u, float v,
                   bool front) {
    const MeshCorner& a = scene.corners[triangle.corners[0]];
    const MeshCorner& b = scene.corners[triangle.corners[1]];
    const MeshCorner& c = scene.corners[triangle.corners[2]];
    const float w = 1.0F - u - v;
    const auto blend3 = [&](const Float3& x, const Float3& y, const Float3& z) {
        return add(add(scale(x, w), scale(y, u)), scale(z, v));
    };
    MeshPixel pixel;
    pixel.world = blend3(a.world, b.world, c.world);
    pixel.normal = blend3(a.normal, b.normal, c.normal);
    pixel.tangent = blend3(a.tangent, b.tangent, c.tangent);
    pixel.binormal = blend3(a.binormal, b.binormal, c.binormal);
    for (std::size_t i = 0; i < 4; ++i) {
        pixel.color.at(i) = (a.color.at(i) * w) + (b.color.at(i) * u) + (c.color.at(i) * v);
    }
    pixel.front = front;
    return pixel;
}

std::array<float, 2> uv_at(const PathScene& scene, const PathTriangle& triangle, float u, float v) {
    const MeshCorner& a = scene.corners[triangle.corners[0]];
    const MeshCorner& b = scene.corners[triangle.corners[1]];
    const MeshCorner& c = scene.corners[triangle.corners[2]];
    const float w = 1.0F - u - v;
    return {(a.uv[0] * w) + (b.uv[0] * u) + (c.uv[0] * v),
            (a.uv[1] * w) + (b.uv[1] * u) + (c.uv[1] * v)};
}

// Fills a pixel's texels from the draw's textures at uv.
void sample_texels(const PathScene& scene, const PathTextures& textures, std::size_t draw,
                   std::array<float, 2> uv, MeshPixel& pixel) {
    const MeshSurfaceUniforms& surface = scene.draws[draw].surface;
    for (std::size_t slot = 0; slot < kMeshTextures; ++slot) {
        pixel.texels.at(slot) = textures.sample(draw, slot, mesh_texture_uv(surface, slot, uv));
    }
}

// Whether a ray meets a point of an alpha-tested draw: its alpha scissor keeps it, or a blended
// one's alpha is above a number hashed from the path and the triangle, so that the share of rays
// its alpha says stop there whichever order the hierarchy is walked in.
bool alpha_keeps(const PathScene& scene, const PathTextures& textures, std::uint32_t index,
                 const Crossing& crossing, std::uint32_t seed) {
    const PathTriangle& triangle = scene.triangles[index];
    MeshPixel pixel = pixel_at(scene, triangle, crossing.u, crossing.v, crossing.front);
    sample_texels(scene, textures, triangle.draw, uv_at(scene, triangle, crossing.u, crossing.v),
                  pixel);
    const std::optional<PathSurface> surface =
        path_surface(scene.draws[triangle.draw].surface, pixel);
    if (!surface) {
        return false;
    }
    if (surface->alpha >= 1.0F) {
        return true;
    }
    const float u = static_cast<float>(mix(seed ^ mix(index)) >> 8U) * (1.0F / 16777216.0F);
    return u < surface->alpha;
}

// Whether a ray of a kind can meet a triangle from a side.
bool meets(const PathScene& scene, const PathTriangle& triangle, bool front, RayKind kind,
           std::uint32_t caster_mask) {
    const PathDraw& draw = scene.draws[triangle.draw];
    if (kind == RayKind::Shadow) {
        return draw.shadows != ShadowCasting::Off && (draw.layers & caster_mask) != 0;
    }
    if (draw.shadows == ShadowCasting::ShadowsOnly) {
        return false;
    }
    switch (draw.cull) {
    case CullMode::Back:
        return front;
    case CullMode::Front:
        return !front;
    default:
        return true;
    }
}

struct Found {
    std::uint32_t triangle = 0;
    Crossing crossing;
};

// The nearest triangle a ray meets within limit, walking the hierarchy nearer child first; a
// shadow ray stops at the first it meets. Ties go to the triangle that comes first, so the walk's
// order never matters.
std::optional<Found> trace(const PathScene& scene, const PathTextures* textures,
                           const Float3& origin, const Float3& direction, float limit, RayKind kind,
                           std::uint32_t caster_mask, std::uint32_t seed) {
    if (scene.nodes.empty()) {
        return std::nullopt;
    }
    const Float3 inverse{1.0F / direction[0], 1.0F / direction[1], 1.0F / direction[2]};
    std::array<std::uint32_t, 64> stack{};
    std::size_t depth = 0;
    stack[depth++] = 0;
    std::optional<Found> best;
    float reach = limit;
    while (depth > 0) {
        const PathNode& node = scene.nodes[stack[--depth]];
        float near = 0.0F;
        if (!meets_box(node, origin, inverse, reach, near)) {
            continue;
        }
        if (node.count == 0) {
            float near_a = 0.0F;
            float near_b = 0.0F;
            const bool a = meets_box(scene.nodes[node.first], origin, inverse, reach, near_a);
            const bool b = meets_box(scene.nodes[node.first + 1], origin, inverse, reach, near_b);
            // The nearer goes on top.
            if (a && b) {
                const bool a_first = near_a <= near_b;
                stack.at(depth++) = node.first + (a_first ? 1 : 0);
                stack.at(depth++) = node.first + (a_first ? 0 : 1);
            } else if (a) {
                stack.at(depth++) = node.first;
            } else if (b) {
                stack.at(depth++) = node.first + 1;
            }
            continue;
        }
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            const PathTriangle& triangle = scene.triangles[i];
            const std::optional<Crossing> crossing = cross_triangle(
                origin, direction, corner_position(scene, triangle, 0),
                corner_position(scene, triangle, 1), corner_position(scene, triangle, 2));
            if (!crossing || crossing->distance > reach ||
                (best && crossing->distance == reach && i > best->triangle)) {
                continue;
            }
            if (!meets(scene, triangle, crossing->front, kind, caster_mask)) {
                continue;
            }
            if (textures != nullptr && scene.draws[triangle.draw].alpha_tested &&
                !alpha_keeps(scene, *textures, i, *crossing, seed)) {
                continue;
            }
            if (kind == RayKind::Shadow) {
                return Found{.triangle = i, .crossing = *crossing};
            }
            if (!best || crossing->distance < reach || i < best->triangle) {
                best = Found{.triangle = i, .crossing = *crossing};
                reach = crossing->distance;
            }
        }
    }
    return best;
}

// A point to start a ray from at a surface: moved off it along its face's normal, to the side
// the ray leaves by, so the ray doesn't meet the surface it starts on.
Float3 offset(const Float3& point, const Float3& face, const Float3& direction) {
    const float size = std::max({1.0F, std::abs(point[0]), std::abs(point[1]), std::abs(point[2])});
    const float amount = 1e-4F * size;
    return add(point, scale(face, dot(face, direction) >= 0.0F ? amount : -amount));
}

// The environment's light along a direction, for a bounce that meets nothing.
Float3 environment(const PathScene& scene, const PathTextures& textures, const Float3& direction) {
    if (!scene.environment_sky) {
        return scene.ambient;
    }
    const Float3 eye = normalize({dot(scene.to_sky[0], direction), dot(scene.to_sky[1], direction),
                                  dot(scene.to_sky[2], direction)});
    const SkyRay ray = sky_ray_toward(eye);
    const Float4 sky =
        shade_sky(scene.environment_uniforms, ray, textures.sky(true, ray.coords), 0.0F, 0.0F);
    const Float3 light = scale(xyz(sky), scene.environment_scale);
    return add(scale(scene.ambient, 1.0F - scene.sky_share), scale(light, scene.sky_share));
}

// What a camera ray through a point of the target that meets nothing shows.
Float3 background(const PathScene& scene, const PathTextures& textures, float x, float y) {
    if (!scene.sky) {
        return xyz(scene.clear);
    }
    const SkyRay ray = sky_ray(scene.sky_uniforms, x, y);
    return xyz(shade_sky(scene.sky_uniforms, ray, textures.sky(false, ray.coords), x, y));
}

// The light the lights send toward v from a surface point, through their shadows.
Float3 direct_light(const PathScene& scene, const PathTextures& textures, const PathDraw& draw,
                    const PathSurface& surface, const Float3& point, const Float3& face,
                    const Float3& v, const Float3& f0, Random& random) {
    Float3 sum{};
    for (const PathLight& light : scene.lights) {
        if ((static_cast<std::uint32_t>(light.uniforms.params[3]) & draw.surface.options[2]) == 0) {
            continue;
        }
        const std::optional<LightReach> reach = light_reach(light.uniforms, point);
        if (!reach || dot(surface.normal, reach->toward) <= 0.0F) {
            continue;
        }
        float visible = 1.0F;
        if (light.shadow && light.shadow_opacity > 0.0F) {
            const Float3 from = offset(point, face, reach->toward);
            const float limit = std::isfinite(reach->distance) ? reach->distance * (1.0F - 1e-4F)
                                                               : std::numeric_limits<float>::max();
            const std::uint32_t seed = mix(random.seed ^ mix(random.counter++));
            if (trace(scene, &textures, from, reach->toward, limit, RayKind::Shadow,
                      light.caster_mask, seed)) {
                visible = 1.0F - light.shadow_opacity;
            }
        }
        if (visible <= 0.0F) {
            continue;
        }
        const LightResponse response =
            light_response(surface.normal, reach->toward, v, xyz(light.uniforms.color),
                           reach->attenuation * visible, f0, surface.roughness, surface.metallic,
                           light.uniforms.color[3]);
        sum = add(sum, add(mul(response.diffuse, scale(surface.base, 1.0F - surface.metallic)),
                           response.specular));
    }
    return sum;
}

// One path through the target at x, y.
Float3 trace_path(const PathScene& scene, const PathTextures& textures, float x, float y,
                  Random& random) {
    const double ndc_x = (static_cast<double>(x) / scene.width * 2.0) - 1.0;
    const double ndc_y = 1.0 - (static_cast<double>(y) / scene.height * 2.0);
    const auto unproject = [&](double z) {
        const std::array<double, 16>& m = scene.to_world;
        std::array<double, 4> out{};
        for (std::size_t row = 0; row < 4; ++row) {
            out.at(row) = (m.at(row) * ndc_x) + (m.at(4 + row) * ndc_y) + (m.at(8 + row) * z) +
                          m.at(12 + row);
        }
        return Float3{static_cast<float>(out[0] / out[3]), static_cast<float>(out[1] / out[3]),
                      static_cast<float>(out[2] / out[3])};
    };
    // Depth is reversed: the near plane is at 1 and the far plane at 0.
    Float3 origin = unproject(1.0);
    const Float3 far = unproject(0.0);
    Float3 direction = sub(far, origin);
    float limit = std::sqrt(dot(direction, direction));
    if (!(limit > 0.0F) || !std::isfinite(limit)) {
        direction = sub(unproject(0.5), origin);
        limit = std::numeric_limits<float>::max();
    }
    direction = normalize(direction);

    Float3 light{};
    Float3 throughput{1.0F, 1.0F, 1.0F};
    for (std::uint32_t bounce = 0;; ++bounce) {
        const std::uint32_t seed = mix(random.seed ^ mix(random.counter++));
        const std::optional<Found> found =
            trace(scene, &textures, origin, direction, limit, RayKind::Camera, 0, seed);
        if (!found) {
            const Float3 seen = bounce == 0 ? background(scene, textures, x, y)
                                            : environment(scene, textures, direction);
            light = add(light, mul(throughput, seen));
            break;
        }
        const PathTriangle& triangle = scene.triangles[found->triangle];
        const PathDraw& draw = scene.draws[triangle.draw];
        MeshPixel pixel =
            pixel_at(scene, triangle, found->crossing.u, found->crossing.v, found->crossing.front);
        sample_texels(scene, textures, triangle.draw,
                      uv_at(scene, triangle, found->crossing.u, found->crossing.v), pixel);
        std::optional<PathSurface> surface = path_surface(draw.surface, pixel);
        if (!surface) {
            break; // alpha tests kept it, so this doesn't happen
        }
        if (surface->unshaded) {
            light = add(light, mul(throughput, surface->base));
            break;
        }
        light = add(light, mul(throughput, surface->emitted));

        const Float3 point = pixel.world;
        const Float3 v = scale(direction, -1.0F);
        Float3 face = normalize(
            cross(sub(corner_position(scene, triangle, 1), corner_position(scene, triangle, 0)),
                  sub(corner_position(scene, triangle, 2), corner_position(scene, triangle, 0))));
        if (dot(face, v) < 0.0F) {
            face = scale(face, -1.0F);
        }
        if (dot(surface->normal, v) <= 0.0F) {
            surface->normal = face;
        }
        const float specular = draw.surface.surface[2];
        const float dielectric = 0.16F * specular * specular;
        Float3 f0{};
        for (std::size_t i = 0; i < 3; ++i) {
            f0.at(i) = dielectric + ((surface->base.at(i) - dielectric) * surface->metallic);
        }
        light = add(light, mul(throughput, direct_light(scene, textures, draw, *surface, point,
                                                        face, v, f0, random)));
        if (bounce >= scene.bounces) {
            break;
        }

        // The next bounce: from the diffuse or the specular lobe, picked by how much light each
        // reflects, and weighed by both lobes' chances of the direction picked.
        const Float3& n = surface->normal;
        const float roughness = std::max(surface->roughness, kMinBounceRoughness);
        const float alpha = roughness * roughness;
        const float n_dot_v = std::max(dot(n, v), 1e-4F);
        const float diffuse_weight = (1.0F - surface->metallic) * luminance(surface->base);
        const float m = 1.0F - n_dot_v;
        const float fresnel = m * m * m * m * m;
        float specular_weight = 0.0F;
        for (std::size_t i = 0; i < 3; ++i) {
            specular_weight += f0.at(i) + ((1.0F - f0.at(i)) * fresnel);
        }
        specular_weight /= 3.0F;
        const float total = diffuse_weight + specular_weight;
        if (!(total > 0.0F)) {
            break;
        }
        float specular_share = specular_weight / total;
        if (diffuse_weight > 0.0F) {
            specular_share = std::clamp(specular_share, 0.1F, 0.9F);
        }
        const float pick = random.next();
        const float u1 = random.next();
        const float u2 = random.next();
        Float3 next{};
        if (pick < specular_share) {
            const Float3 h = sample_visible_normal(n, v, alpha, u1, u2);
            next = sub(scale(h, 2.0F * dot(v, h)), v);
        } else {
            next = sample_cosine(n, u1, u2);
        }
        const float n_dot_l = dot(n, next);
        if (n_dot_l <= 0.0F || dot(face, next) <= 0.0F) {
            break;
        }
        const Float3 h = normalize(add(v, next));
        const float specular_pdf = ggx(std::clamp(dot(n, h), 0.0F, 1.0F), alpha) *
                                   smith(n_dot_v, alpha) / (4.0F * n_dot_v);
        const float pdf =
            (specular_share * specular_pdf) + ((1.0F - specular_share) * n_dot_l / kPi);
        if (!(pdf > 0.0F)) {
            break;
        }
        const LightResponse response = light_response(n, next, v, {1.0F, 1.0F, 1.0F}, 1.0F, f0,
                                                      roughness, surface->metallic, 1.0F);
        const Float3 reflected =
            add(mul(response.diffuse, scale(surface->base, 1.0F - surface->metallic)),
                response.specular);
        throughput = mul(throughput, scale(reflected, 1.0F / pdf));
        if (bounce + 1 >= kRouletteBounce) {
            const float keep =
                std::min(std::max({throughput[0], throughput[1], throughput[2]}), 0.95F);
            if (!(random.next() < keep)) {
                break;
            }
            throughput = scale(throughput, 1.0F / keep);
        }
        if (!std::isfinite(throughput[0] + throughput[1] + throughput[2])) {
            break;
        }
        origin = offset(point, face, next);
        direction = next;
        limit = std::numeric_limits<float>::max();
    }
    return light;
}

template <typename T>
void hash_bytes(std::uint64_t& hash, const T& value) {
    std::array<unsigned char, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
}

} // namespace

PathScene build_path_scene(const Scene3D& scene, std::span<const PathMesh> meshes,
                           std::uint32_t width, std::uint32_t height, const Float4& clear) {
    PathScene out;
    out.width = width;
    out.height = height;
    out.bounces = scene.path_trace ? scene.path_trace->bounces : 0;
    const MeshCameraUniforms camera = mesh_camera_uniforms(scene);
    for (const PathMesh& mesh : meshes) {
        const MeshDraw& draw = *mesh.draw;
        const auto draw_index = static_cast<std::uint32_t>(out.draws.size());
        PathDraw& path_draw = out.draws.emplace_back();
        path_draw.surface = mesh_surface_uniforms(draw.material, draw.layers);
        path_draw.cull = draw.material.cull;
        path_draw.shadows = draw.shadows;
        path_draw.layers = draw.layers;
        path_draw.alpha_tested = (path_draw.surface.options[0] & kMeshAlphaScissor) != 0 ||
                                 (path_draw.surface.options[0] & kMeshHasAlpha) != 0;
        const MeshInstanceUniforms instance = mesh_instance_uniforms(draw.transform, draw.material);
        const auto base = static_cast<std::uint32_t>(out.corners.size());
        for (const Vertex3D& vertex : mesh.vertices) {
            out.corners.push_back(mesh_vertex(camera, instance, vertex, vertex.position));
        }
        for (std::size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
            out.triangles.push_back(
                PathTriangle{.corners = {base + mesh.triangles[i], base + mesh.triangles[i + 1],
                                         base + mesh.triangles[i + 2]},
                             .draw = draw_index});
        }
    }
    if (!out.triangles.empty()) {
        std::vector<Float3> centers;
        centers.reserve(out.triangles.size());
        for (const PathTriangle& triangle : out.triangles) {
            Float3 center{};
            for (std::size_t c = 0; c < 3; ++c) {
                center = add(center, corner_position(out, triangle, c));
            }
            centers.push_back(scale(center, 1.0F / 3.0F));
        }
        std::vector<std::uint32_t> order(out.triangles.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = static_cast<std::uint32_t>(i);
        }
        out.nodes.reserve(out.triangles.size());
        out.nodes.emplace_back();
        build_node(out, centers, order, 0, 0, static_cast<std::uint32_t>(order.size()));
        std::vector<PathTriangle> sorted;
        sorted.reserve(order.size());
        for (const std::uint32_t i : order) {
            sorted.push_back(out.triangles[i]);
        }
        out.triangles = std::move(sorted);
    }
    for (const Light3D& light : scene.lights) {
        out.lights.push_back(PathLight{.uniforms = mesh_light_uniforms(light, -1),
                                       .shadow = light.shadow,
                                       .shadow_opacity = light.shadow_opacity,
                                       .caster_mask = light.shadow_caster_mask});
    }
    out.to_world = inverse_matrix(multiply(scene.projection, scene.camera));
    out.sky = sky_draws(scene);
    if (out.sky) {
        out.sky_uniforms = sky_uniforms(scene, width, height);
    }
    out.clear = clear;
    const MeshSceneUniforms uniforms = mesh_scene_uniforms(scene);
    out.ambient = xyz(uniforms.ambient);
    if (radiance_used(scene) && scene.radiance.ambient) {
        out.environment_sky = true;
        out.environment_uniforms = radiance_face_uniforms(scene, 0);
        out.environment_uniforms.tonemap[3] = 2.0F; // linear, before the exposure
        out.environment_scale = scene.post.exposure * uniforms.sky[2];
        out.sky_share = uniforms.sky[3];
        for (std::size_t i = 0; i < 3; ++i) {
            out.to_sky.at(i) = xyz(uniforms.sky_rows.at(i));
        }
    }
    return out;
}

std::uint64_t path_scene_key(const PathScene& scene) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    hash_bytes(hash, scene.width);
    hash_bytes(hash, scene.height);
    hash_bytes(hash, scene.bounces);
    for (const MeshCorner& corner : scene.corners) {
        hash_bytes(hash, corner.world);
        hash_bytes(hash, corner.normal);
        hash_bytes(hash, corner.tangent);
        hash_bytes(hash, corner.binormal);
        hash_bytes(hash, corner.uv);
        hash_bytes(hash, corner.color);
    }
    for (const PathTriangle& triangle : scene.triangles) {
        hash_bytes(hash, triangle);
    }
    // Field by field, since padding between them holds anything.
    for (const PathDraw& draw : scene.draws) {
        hash_bytes(hash, draw.surface);
        hash_bytes(hash, draw.cull);
        hash_bytes(hash, draw.shadows);
        hash_bytes(hash, draw.layers);
        hash_bytes(hash, draw.alpha_tested);
    }
    for (const PathLight& light : scene.lights) {
        hash_bytes(hash, light.uniforms);
        hash_bytes(hash, light.shadow);
        hash_bytes(hash, light.shadow_opacity);
        hash_bytes(hash, light.caster_mask);
    }
    hash_bytes(hash, scene.to_world);
    hash_bytes(hash, scene.sky);
    hash_bytes(hash, scene.sky_uniforms);
    hash_bytes(hash, scene.clear);
    hash_bytes(hash, scene.environment_sky);
    hash_bytes(hash, scene.environment_uniforms);
    hash_bytes(hash, scene.to_sky);
    hash_bytes(hash, scene.environment_scale);
    hash_bytes(hash, scene.sky_share);
    hash_bytes(hash, scene.ambient);
    return hash;
}

void trace_paths(const PathScene& scene, const PathTextures& textures, std::uint32_t first,
                 std::uint32_t samples, std::span<float> sums, jobs::Pool* pool) {
    const auto row = [&](std::size_t y) {
        for (std::uint32_t x = 0; x < scene.width; ++x) {
            const std::size_t at = ((y * scene.width) + x) * 3;
            for (std::uint32_t s = first; s < first + samples; ++s) {
                Random random{.seed = mix(mix(mix(x) ^ static_cast<std::uint32_t>(y)) ^ s),
                              .counter = 0};
                const float jx = random.next();
                const float jy = random.next();
                const Float3 light = trace_path(scene, textures, static_cast<float>(x) + jx,
                                                static_cast<float>(y) + jy, random);
                for (std::size_t c = 0; c < 3; ++c) {
                    if (std::isfinite(light.at(c))) {
                        sums[at + c] += light.at(c);
                    }
                }
            }
        }
    };
    if (pool != nullptr) {
        pool->parallel_for(scene.height, row);
    } else {
        for (std::size_t y = 0; y < scene.height; ++y) {
            row(y);
        }
    }
}

std::optional<PathHit> path_hit(const PathScene& scene, const Float3& origin,
                                const Float3& direction, float distance) {
    const std::optional<Found> found =
        trace(scene, nullptr, origin, direction, distance, RayKind::Camera, 0, 0);
    if (!found) {
        return std::nullopt;
    }
    return PathHit{.triangle = found->triangle,
                   .distance = found->crossing.distance,
                   .u = found->crossing.u,
                   .v = found->crossing.v};
}

bool path_blocked(const PathScene& scene, const PathTextures* textures, const Float3& origin,
                  const Float3& direction, float distance, std::uint32_t caster_mask,
                  std::uint32_t seed) {
    return trace(scene, textures, origin, direction, distance, RayKind::Shadow, caster_mask, seed)
        .has_value();
}

} // namespace slash::render::detail
