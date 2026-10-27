#include "ray_shadows.hpp"

#include "forward_post.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace slash::render::detail {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
// How far a pixel's rays start off its surface: a share of its distance from the camera, as the
// depth it was put back from is less exact farther away, and a least amount.
constexpr float kOffsetShare = 0.003F;
constexpr float kOffsetLeast = 0.002F;
// Rays toward an omni or spot light stop this share short of it.
constexpr float kShortOfLight = 1e-4F;
// The R2 sequence's steps (Martin Roberts'), which spread a pixel's samples over a light.
constexpr float kR2X = 0.7548776662F;
constexpr float kR2Y = 0.5698402910F;
// What occlusion rays' hashes take in place of a light's slot.
constexpr std::uint32_t kOcclusionSeed = 0xA0U;
// The most a pixel's occlusion takes away, as Godot's SSAO holds it.
constexpr float kMostObscurance = 0.98F;

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
float dot(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}
float length(const Float3& a) {
    return std::sqrt(dot(a, a));
}
Float3 normalize(const Float3& a) {
    const float size = length(a);
    return size > 0.0F ? scale(a, 1.0F / size) : a;
}

// A 32-bit integer hash with good avalanche (Chris Wellons' lowbias32), as the path tracer's.
std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

// From 0 up to but not including 1, in steps of 2^-24.
float unit(std::uint32_t bits) {
    return static_cast<float>(bits >> 8U) * (1.0F / 16777216.0F);
}

float fraction(float x) {
    return x - std::floor(x);
}

// Two unit vectors square to n (unit length) and to each other: Duff and others' basis.
void basis(const Float3& n, Float3& t, Float3& b) {
    const float sign = n[2] >= 0.0F ? 1.0F : -1.0F;
    const float a = -1.0F / (sign + n[2]);
    const float c = n[0] * n[1] * a;
    t = {1.0F + (sign * n[0] * n[0] * a), sign * c, -sign * n[0]};
    b = {c, sign + (n[1] * n[1] * a), -n[1]};
}

// A point of a disk of radius r square to n at u, v from 0 to 1, from its center.
Float3 disk_point(const Float3& n, float r, float u, float v) {
    Float3 t;
    Float3 b;
    basis(n, t, b);
    const float reach = std::sqrt(u) * r;
    const Vec2 turn = slash::cos_sin(2.0F * kPi * v);
    return add(scale(t, reach * turn.x), scale(b, reach * turn.y));
}

} // namespace

std::vector<RayShadowLight> ray_shadow_lights(const Scene3D& scene) {
    const std::vector<int> slots = ray_shadow_slots(scene);
    std::vector<RayShadowLight> out;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i] < 0) {
            continue;
        }
        const Light3D& light = scene.lights[i];
        RayShadowLight ray{.slot = static_cast<std::uint32_t>(slots[i]),
                           .directional = light.type == LightType::Directional,
                           .position = light.position,
                           .toward = {},
                           .size = std::max(0.0F, light.size),
                           .opacity = std::clamp(light.shadow_opacity, 0.0F, 1.0F),
                           .caster_mask = light.shadow_caster_mask};
        if (ray.directional) {
            const Float3 way = normalize(light.direction);
            ray.toward = scale(way, -1.0F);
            const float half = std::clamp(light.size, 0.0F, 90.0F) * (kPi / 360.0F);
            const Vec2 turn = slash::cos_sin(half);
            ray.size = turn.x > 0.0F ? turn.y / turn.x : 0.0F;
        }
        out.push_back(ray);
    }
    return out;
}

RayShadowFrame ray_shadow_frame(const Scene3D& scene, const Mat4& view_projection,
                                std::uint32_t width, std::uint32_t height) {
    const Mat4& c = scene.camera;
    return RayShadowFrame{
        .width = width,
        .height = height,
        .samples =
            std::clamp<std::uint32_t>(scene.ray_tracing.shadow_samples, 1, kMaxRayShadowSamples),
        .to_world = inverse_matrix(view_projection),
        .view_rows = {Float4{c[0], c[4], c[8], c[12]}, Float4{c[1], c[5], c[9], c[13]},
                      Float4{c[2], c[6], c[10], c[14]}}};
}

std::optional<RaySurface> ray_surface(const RayShadowFrame& frame, std::uint32_t x, std::uint32_t y,
                                      float depth, const Float3& normal_texel) {
    if (!(depth > 0.0F)) {
        return std::nullopt;
    }
    const double ndc_x = ((static_cast<double>(x) + 0.5) / frame.width * 2.0) - 1.0;
    const double ndc_y = 1.0 - ((static_cast<double>(y) + 0.5) / frame.height * 2.0);
    const std::array<double, 16>& m = frame.to_world;
    std::array<double, 4> clip{};
    for (std::size_t row = 0; row < 4; ++row) {
        clip.at(row) = (m.at(row) * ndc_x) + (m.at(4 + row) * ndc_y) +
                       (m.at(8 + row) * static_cast<double>(depth)) + m.at(12 + row);
    }
    if (clip[3] == 0.0) {
        return std::nullopt;
    }
    const Float3 world{static_cast<float>(clip[0] / clip[3]), static_cast<float>(clip[1] / clip[3]),
                       static_cast<float>(clip[2] / clip[3])};
    // Its place and normal in the camera's space: the normal turned to face the camera.
    Float3 seen{};
    for (std::size_t row = 0; row < 3; ++row) {
        const Float4& r = frame.view_rows.at(row);
        seen.at(row) = (r[0] * world[0]) + (r[1] * world[1]) + (r[2] * world[2]) + r[3];
    }
    Float3 normal{(normal_texel[0] * 2.0F) - 1.0F, (normal_texel[1] * 2.0F) - 1.0F,
                  (normal_texel[2] * 2.0F) - 1.0F};
    if (dot(normal, seen) > 0.0F) {
        normal = scale(normal, -1.0F);
    }
    const float size = length(normal);
    if (!(size > 0.0F)) {
        return RaySurface{.origin = world, .normal = {}};
    }
    // Back to the world: the rows' transpose.
    Float3 out{};
    for (std::size_t row = 0; row < 3; ++row) {
        const Float4& r = frame.view_rows.at(row);
        out = add(out, scale(Float3{r[0], r[1], r[2]}, normal.at(row) / size));
    }
    const float amount = (kOffsetShare * length(seen)) + kOffsetLeast;
    return RaySurface{.origin = add(world, scale(out, amount)), .normal = out};
}

std::optional<Float3> ray_shadow_origin(const RayShadowFrame& frame, std::uint32_t x,
                                        std::uint32_t y, float depth, const Float3& normal_texel) {
    const std::optional<RaySurface> surface = ray_surface(frame, x, y, depth, normal_texel);
    if (!surface) {
        return std::nullopt;
    }
    return surface->origin;
}

std::optional<ShadowRay> ray_shadow_ray(const RayShadowFrame& frame, const RayShadowLight& light,
                                        const Float3& origin, std::uint32_t x, std::uint32_t y,
                                        std::uint32_t sample) {
    const std::uint32_t seed = mix(((y * frame.width) + x) ^ mix(light.slot));
    const float u = fraction(unit(seed) + (static_cast<float>(sample) * kR2X));
    const float v = fraction(unit(mix(seed)) + (static_cast<float>(sample) * kR2Y));
    if (light.directional) {
        Float3 way = light.toward;
        if (light.size > 0.0F) {
            way = normalize(add(way, disk_point(way, light.size, u, v)));
        }
        return ShadowRay{.direction = way, .distance = std::numeric_limits<float>::max()};
    }
    Float3 target = light.position;
    const Float3 to = sub(target, origin);
    const float reach = length(to);
    if (!(reach > 0.0F)) {
        return std::nullopt;
    }
    if (light.size > 0.0F) {
        target = add(target, disk_point(scale(to, 1.0F / reach), light.size, u, v));
    }
    const Float3 way = sub(target, origin);
    const float distance = length(way);
    if (!(distance > 0.0F)) {
        return std::nullopt;
    }
    return ShadowRay{.direction = scale(way, 1.0F / distance),
                     .distance = distance * (1.0F - kShortOfLight)};
}

std::vector<float> run_ray_shadows(const PathScene& scene, const PathTextures* textures,
                                   std::span<const RayShadowLight> lights,
                                   const RayShadowFrame& frame, std::span<const float> depth,
                                   std::span<const float> normals, jobs::Pool* pool) {
    std::vector<float> out(std::size_t{frame.width} * frame.height * kMaxRayShadowLights, 1.0F);
    const auto row = [&](std::size_t y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t at = (y * frame.width) + x;
            const std::optional<Float3> origin =
                ray_shadow_origin(frame, x, static_cast<std::uint32_t>(y), depth[at],
                                  {normals[at * 4], normals[(at * 4) + 1], normals[(at * 4) + 2]});
            if (!origin) {
                continue;
            }
            for (const RayShadowLight& light : lights) {
                std::uint32_t blocked = 0;
                for (std::uint32_t s = 0; s < frame.samples; ++s) {
                    const std::optional<ShadowRay> ray =
                        ray_shadow_ray(frame, light, *origin, x, static_cast<std::uint32_t>(y), s);
                    if (!ray) {
                        continue;
                    }
                    const std::uint32_t seed =
                        mix(static_cast<std::uint32_t>(at) ^ mix((light.slot << 16U) ^ s));
                    if (path_blocked(scene, textures, *origin, ray->direction, ray->distance,
                                     light.caster_mask, seed)) {
                        ++blocked;
                    }
                }
                const float share = static_cast<float>(blocked) / static_cast<float>(frame.samples);
                out[(at * kMaxRayShadowLights) + light.slot] =
                    round_half(1.0F - (light.opacity * share));
            }
        }
    };
    if (pool != nullptr) {
        pool->parallel_for(frame.height, row);
    } else {
        for (std::size_t y = 0; y < frame.height; ++y) {
            row(y);
        }
    }
    return out;
}

std::optional<RayOcclusion> ray_occlusion(const Scene3D& scene) {
    if (scene.method != RenderMethod::ForwardPlus || !scene.ssao.enabled ||
        !scene.ray_tracing.ambient_occlusion) {
        return std::nullopt;
    }
    return RayOcclusion{.samples = std::clamp<std::uint32_t>(
                            scene.ray_tracing.ambient_occlusion_samples, 1, kMaxRayShadowSamples),
                        .radius = std::max(0.0F, scene.ssao.radius),
                        .intensity = std::max(0.0F, scene.ssao.intensity),
                        .power = std::max(0.0F, scene.ssao.power)};
}

Float3 occlusion_ray(const Float3& normal, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                     std::uint32_t sample) {
    // Its own seed, apart from the lights' (whose slots are below 8).
    const std::uint32_t seed = mix(((y * width) + x) ^ mix(kOcclusionSeed));
    const float u = fraction(unit(seed) + (static_cast<float>(sample) * kR2X));
    const float v = fraction(unit(mix(seed)) + (static_cast<float>(sample) * kR2Y));
    // Malley's method: a point of the unit disk lifted onto the half sphere.
    const Float3 across = disk_point(normal, 1.0F, u, v);
    const float up = std::sqrt(std::max(0.0F, 1.0F - u));
    return normalize(add(across, scale(normal, up)));
}

std::vector<float> run_ray_occlusion(const PathScene& scene, const PathTextures* textures,
                                     const RayOcclusion& occlusion, const RayShadowFrame& frame,
                                     std::span<const float> depth, std::span<const float> normals,
                                     jobs::Pool* pool) {
    std::vector<float> out(std::size_t{frame.width} * frame.height, 1.0F);
    if (!(occlusion.radius > 0.0F)) {
        return out;
    }
    const auto row = [&](std::size_t y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t at = (y * frame.width) + x;
            const std::optional<RaySurface> surface =
                ray_surface(frame, x, static_cast<std::uint32_t>(y), depth[at],
                            {normals[at * 4], normals[(at * 4) + 1], normals[(at * 4) + 2]});
            if (!surface || dot(surface->normal, surface->normal) == 0.0F) {
                continue;
            }
            std::uint32_t blocked = 0;
            for (std::uint32_t s = 0; s < occlusion.samples; ++s) {
                const Float3 way = occlusion_ray(surface->normal, x, static_cast<std::uint32_t>(y),
                                                 frame.width, s);
                const std::uint32_t seed =
                    mix(static_cast<std::uint32_t>(at) ^ mix((kOcclusionSeed << 16U) ^ s));
                if (path_blocked(scene, textures, surface->origin, way, occlusion.radius,
                                 0xFFFFFFFFU, seed)) {
                    ++blocked;
                }
            }
            const float share = static_cast<float>(blocked) / static_cast<float>(occlusion.samples);
            // As ssao.cpp's gather: the obscurance held below 0.98, so nothing turns black.
            const float open = 1.0F - std::min(occlusion.intensity * share, kMostObscurance);
            // slash::exp and ln, the same bits everywhere, rather than std::pow.
            out[at] = round_half(slash::exp(occlusion.power * slash::ln(open)));
        }
    };
    if (pool != nullptr) {
        pool->parallel_for(frame.height, row);
    } else {
        for (std::size_t y = 0; y < frame.height; ++y) {
            row(y);
        }
    }
    return out;
}

} // namespace slash::render::detail
