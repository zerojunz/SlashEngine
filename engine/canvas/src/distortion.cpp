#include "slash/canvas/distortion.hpp"

#include "slash/scene3d/scene3d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/canvas_layer.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace slash::canvas {

namespace {

constexpr float kTau = 6.28318530718F;

std::uint32_t hash_id(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

// A value from 0 to 1 at a lattice point, hashed with a seed.
float lattice(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    std::uint32_t h = seed ^ (static_cast<std::uint32_t>(x) * 0x8DA6B343U) ^
                      (static_cast<std::uint32_t>(y) * 0xD8163841U);
    h = (h ^ (h >> 16U)) * 0x7FEB352DU;
    h = (h ^ (h >> 15U)) * 0x846CA68BU;
    h ^= h >> 16U;
    return static_cast<float>(h >> 8U) * (1.0F / 16777216.0F);
}

// Smooth noise from -1 to 1 over the plane.
float smooth_noise(float x, float y, std::uint32_t seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float tx = x - fx;
    const float ty = y - fy;
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const float ex = tx * tx * (3.0F - (2.0F * tx));
    const float ey = ty * ty * (3.0F - (2.0F * ty));
    const float a = lattice(ix, iy, seed);
    const float b = lattice(ix + 1, iy, seed);
    const float c = lattice(ix, iy + 1, seed);
    const float d = lattice(ix + 1, iy + 1, seed);
    const float top = a + ((b - a) * ex);
    const float bottom = c + ((d - c) * ex);
    return ((top + ((bottom - top) * ey)) * 2.0F) - 1.0F;
}

// The time, wrapped to an hour so float math keeps its precision.
float wrapped(double time) {
    return static_cast<float>(time - (std::floor(time / 3600.0) * 3600.0));
}

DistortionMode mode_of(std::string_view mode) {
    if (mode == "shockwave") {
        return DistortionMode::Shockwave;
    }
    if (mode == "ripple") {
        return DistortionMode::Ripple;
    }
    if (mode == "lens") {
        return DistortionMode::Lens;
    }
    return DistortionMode::Heat;
}

Distortion from(const world::ScreenDistortion& settings, Vec2f center, float radius,
                std::string_view id) {
    return {.center = center,
            .radius = radius,
            .mode = mode_of(settings.mode),
            .strength = settings.strength,
            .progress = settings.progress,
            .width = settings.width,
            .frequency = settings.frequency,
            .speed = settings.speed,
            .seed = hash_id(id)};
}

bool touches(const Distortion& d, Vec2f size) {
    return d.radius > 0.0F && d.strength != 0.0F && d.center.x + d.radius > 0.0F &&
           d.center.y + d.radius > 0.0F && d.center.x - d.radius < size.x &&
           d.center.y - d.radius < size.y;
}

float length(Vec2f v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

} // namespace

std::vector<Distortion> distortions(const world::World& world, Vec2f size, bool with_3d) {
    std::vector<Distortion> out;
    const auto& ecs = world.ecs();
    if (size.x <= 0.0F || size.y <= 0.0F) {
        return out;
    }
    if (!ecs.view<const world::ScreenDistortion2D>().empty()) {
        const world::Tree2D tree(world);
        const world::Canvases canvases = world::canvases(world, tree, ui::settings(world).viewport);
        for (const auto [entity, settings] : ecs.view<const world::ScreenDistortion2D>().each()) {
            if (!settings.enabled || !ecs.all_of<world::Transform2D>(entity)) {
                continue;
            }
            const world::Affine2D screen = world::screen_transform(world, canvases, entity);
            const Vec2 place = tree.global(entity).position;
            const Vec2 at = screen.apply(place);
            const Vec2 edge = screen.apply(place + Vec2{settings.radius, 0.0F});
            const Distortion d =
                from(settings, to_float(at), length(to_float(edge - at)), world.id_of(entity));
            if (touches(d, size)) {
                out.push_back(d);
            }
        }
    }
    if (with_3d && !ecs.view<const world::ScreenDistortion3D>().empty()) {
        const world::Tree3D tree(world);
        const entt::entity camera_entity = scene3d::current_camera(world, tree);
        if (camera_entity != entt::null) {
            const auto& camera = ecs.get<const world::Camera3D>(camera_entity);
            // As the camera's projection: its place moved by its offsets, the view's half
            // sizes at a distance of 1 (or across, for an orthogonal one).
            world::Affine3D eye = world::orthonormalized(tree.global(camera_entity));
            eye.origin = eye.origin + (eye.y * camera.v_offset) + (eye.x * camera.h_offset);
            const world::Affine3D to_eye = eye.inverse();
            const bool orthogonal = camera.projection == "orthogonal";
            const float aspect = size.x / size.y;
            const bool keep_width = camera.keep_aspect == "width";
            float half_width = 1.0F;
            float half_height = 1.0F;
            if (orthogonal) {
                const float span = camera.size * 0.5F;
                half_height = keep_width ? span / aspect : span;
                half_width = keep_width ? span : span * aspect;
            } else {
                const Vec2 half = cos_sin(camera.fov * 0.5F * 0.0174532925199F);
                const float tangent = half.x != 0.0F ? to_float(half.y / half.x) : 1.0F;
                half_height = keep_width ? tangent / aspect : tangent;
                half_width = keep_width ? tangent : tangent * aspect;
            }
            for (const auto [entity, settings] :
                 ecs.view<const world::ScreenDistortion3D>().each()) {
                if (!settings.enabled || !ecs.all_of<world::Transform3D>(entity)) {
                    continue;
                }
                const Vec3f local = to_float(to_eye.apply(tree.global(entity).origin));
                const float depth = -local.z;
                if (!orthogonal && depth <= std::max(camera.near, 0.0001F)) {
                    continue;
                }
                const float scale = orthogonal ? 1.0F : 1.0F / depth;
                const Vec2f center{((local.x * scale / half_width) + 1.0F) * 0.5F * size.x,
                                   (1.0F - (((local.y * scale / half_height) + 1.0F) * 0.5F)) *
                                       size.y};
                const float radius = settings.radius * scale / half_height * 0.5F * size.y;
                const Distortion d = from(settings, center, radius, world.id_of(entity));
                if (touches(d, size)) {
                    out.push_back(d);
                }
            }
        }
    }
    return out;
}

Vec2f distortion_offset(std::span<const Distortion> distortions, Vec2f point, double time) {
    const float t = wrapped(time);
    Vec2f out;
    for (const Distortion& d : distortions) {
        const Vec2f apart = point - d.center;
        const float distance = length(apart);
        const float r = distance / d.radius;
        if (r >= 1.0F) {
            continue;
        }
        const Vec2f way = distance > 1e-6F ? apart * (1.0F / distance) : Vec2f{};
        const float edge = 1.0F - (r * r);
        const float falloff = edge * edge;
        switch (d.mode) {
        case DistortionMode::Heat: {
            // Shimmer that rises: noise scrolled up the screen.
            const float cells = std::max(d.frequency, 0.01F) / d.radius;
            const float x = point.x * cells;
            const float y = (point.y * cells) + (t * d.speed);
            const Vec2f wobble{smooth_noise(x, y, d.seed),
                               smooth_noise(x, y, d.seed ^ 0x5BD1E995U)};
            out = out + (wobble * (d.strength * falloff));
            break;
        }
        case DistortionMode::Shockwave: {
            // A ring at progress: what is just outside it is pushed out, what is just inside
            // pulled in, as a refracting wave front.
            const float half = std::max(d.width, 0.001F) * 0.5F;
            const float x = (r - d.progress) / half;
            if (x > -1.0F && x < 1.0F && d.progress <= 1.0F + half) {
                const float bump = 1.0F - (x * x);
                // x (1 - x^2)^2 peaks at about 0.2862; scaled so strength is the most it moves.
                out = out - (way * (d.strength * x * bump * bump * 3.4939F));
            }
            break;
        }
        case DistortionMode::Ripple: {
            const Vec2f wave = cos_sin(kTau * ((r * d.frequency) - (t * d.speed)));
            out = out + (way * (d.strength * falloff * wave.y));
            break;
        }
        case DistortionMode::Lens: {
            // Drawn from nearer the middle, so it bulges; r (1 - r^2)^2 peaks at about 0.2862, so
            // strength is the most it moves.
            out = out - (way * (d.strength * r * falloff * 3.4939F));
            break;
        }
        }
    }
    return out;
}

std::vector<render::Vertex2D> distortion_mesh(std::span<const Distortion> distortions, Vec2f size,
                                              double time, std::uint32_t columns) {
    std::vector<render::Vertex2D> out;
    if (distortions.empty() || size.x <= 0.0F || size.y <= 0.0F || columns == 0) {
        return out;
    }
    // Rows to keep the cells square, rounded to the nearest whole number.
    const float tall = static_cast<float>(columns) * size.y / size.x;
    const std::uint32_t rows =
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::floor(tall + 0.5F)));
    std::vector<render::Vertex2D> corners;
    corners.reserve(std::size_t{columns + 1} * (rows + 1));
    for (std::uint32_t j = 0; j <= rows; ++j) {
        for (std::uint32_t i = 0; i <= columns; ++i) {
            const Vec2f p{size.x * static_cast<float>(i) / static_cast<float>(columns),
                          size.y * static_cast<float>(j) / static_cast<float>(rows)};
            const Vec2f from_point = p + distortion_offset(distortions, p, time);
            corners.push_back({.x = p.x,
                               .y = p.y,
                               .u = std::clamp(from_point.x / size.x, 0.0F, 1.0F),
                               .v = std::clamp(from_point.y / size.y, 0.0F, 1.0F),
                               .rgba = {255, 255, 255, 255}});
        }
    }
    out.reserve(std::size_t{columns} * rows * 6);
    const std::uint32_t stride = columns + 1;
    for (std::uint32_t j = 0; j < rows; ++j) {
        for (std::uint32_t i = 0; i < columns; ++i) {
            const render::Vertex2D& a = corners[(j * stride) + i];
            const render::Vertex2D& b = corners[(j * stride) + i + 1];
            const render::Vertex2D& c = corners[((j + 1) * stride) + i + 1];
            const render::Vertex2D& d = corners[((j + 1) * stride) + i];
            out.insert(out.end(), {a, b, c, a, c, d});
        }
    }
    return out;
}

} // namespace slash::canvas
