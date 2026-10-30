#include "slash/render/particles3d.hpp"

#include "checks.hpp"
#include "particle_layout.hpp"
#include "particle_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

// Godot's particles shader (servers/rendering/renderer_rd/shaders/particles.glsl), the code
// ParticleProcessMaterial::_update_shader generates for 3D (scene/resources/
// particle_process_material.cpp), its particles copy shader (particles_copy.glsl) and
// BaseMaterial3D's billboard and particle animation code (scene/resources/material.cpp), MIT,
// step for step, on the CPU.
namespace slash::render {
namespace {

using particle_math::curl_3d;
using particle_math::dot;
using particle_math::hash;
using particle_math::kDegreesToRadians;
using particle_math::kPi;
using particle_math::length;
using particle_math::mix;
using particle_math::normalize;
using particle_math::rand_from_seed;
using particle_math::rand_m1_p1;
using particle_math::rotate_hue;
using particle_math::sign;
using particle_math::times;
using Coord = particle_math::Coord;

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 times(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

Vec3 xyz(Vec4 v) {
    return {v.x, v.y, v.z};
}

Vec4 with_w(Vec3 v, float w = 0.0F) {
    return {v.x, v.y, v.z, w};
}

// Godot's normalize_or_else: v at length 1, or fallback for exactly zero.
Vec3 normalize_or_else(Vec3 v, Vec3 fallback) {
    if (v.x == 0.0F && v.y == 0.0F && v.z == 0.0F) {
        return fallback;
    }
    return normalize(v);
}

// tan from the engine's own sine and cosine, the same on every machine.
float tangent_of(float radians) {
    const Vec2 cs = cos_sin(radians);
    return cs.y / cs.x;
}

// GLSL's pow for a base from 0 to 1, through the engine's own exp and ln.
float power(float base, float exponent) {
    if (base <= 0.0F) {
        return 0.0F;
    }
    return slash::exp(exponent * slash::ln(base));
}

// A transform's basis applied to a vector, and the whole of it to a point.
struct Affine {
    Vec3 x{1.0F, 0.0F, 0.0F};
    Vec3 y{0.0F, 1.0F, 0.0F};
    Vec3 z{0.0F, 0.0F, 1.0F};
    Vec3 origin;

    static Affine of(const ParticleTransform& t) {
        return {.x = {t[0], t[1], t[2]},
                .y = {t[3], t[4], t[5]},
                .z = {t[6], t[7], t[8]},
                .origin = {t[9], t[10], t[11]}};
    }

    [[nodiscard]] Vec3 basis(Vec3 v) const { return (x * v.x) + (y * v.y) + (z * v.z); }
    [[nodiscard]] Vec3 apply(Vec3 p) const { return basis(p) + origin; }

    // The inverse, as GLSL's inverse() of the 4x4 matrix.
    [[nodiscard]] Affine inverse() const {
        const float det = dot(x, cross(y, z));
        const float inv = det != 0.0F ? 1.0F / det : 0.0F;
        // Rows of the inverse are the cross products of the columns, over the determinant.
        const Vec3 r0 = cross(y, z) * inv;
        const Vec3 r1 = cross(z, x) * inv;
        const Vec3 r2 = cross(x, y) * inv;
        Affine out;
        out.x = {r0.x, r1.x, r2.x};
        out.y = {r0.y, r1.y, r2.y};
        out.z = {r0.z, r1.z, r2.z};
        out.origin = out.basis(origin) * -1.0F;
        return out;
    }

    Affine operator*(const Affine& b) const {
        return {.x = basis(b.x), .y = basis(b.y), .z = basis(b.z), .origin = apply(b.origin)};
    }
};

float param_min(const ParticleProcess3D& p, ParticleParam param) {
    return p.param_min[static_cast<std::size_t>(param)];
}

float param_max(const ParticleProcess3D& p, ParticleParam param) {
    return p.param_max[static_cast<std::size_t>(param)];
}

float random_param(const ParticleProcess3D& p, ParticleParam param, std::uint32_t& seed) {
    return mix(param_min(p, param), param_max(p, param), rand_from_seed(seed));
}

const ParticleCurve& curve(const ParticleProcess3D& p, ParticleParam param) {
    return p.param_curve[static_cast<std::size_t>(param)];
}

// A curve's x, y and z at an offset, as Godot's texture(curve, ...).rgb; fallback for an axis
// with no points.
Vec3 sample3(const ParticleCurve3& c, float offset, float fallback) {
    return {sample_curve(c.axes[0], offset, fallback), sample_curve(c.axes[1], offset, fallback),
            sample_curve(c.axes[2], offset, fallback)};
}

// Godot's DisplayParameters, DynamicsParameters and PhysicalParameters.
struct Display {
    Vec3 scale{1.0F, 1.0F, 1.0F};
    float hue_rotation = 0.0F;
    float animation_speed = 0.0F;
    float animation_offset = 0.0F;
    float lifetime = 1.0F;
    Vec4 color;
    float emission_position = 0.0F;
};

struct Dynamics {
    float angle = 0.0F;
    float angular_velocity = 0.0F;
    float initial_velocity = 0.0F;
    float directional_velocity = 0.0F;
    float radial_velocity = 0.0F;
    float orbit_velocity = 0.0F;
    float turbulence_influence = 0.0F;
};

struct Physical {
    float linear_accel = 0.0F;
    float radial_accel = 0.0F;
    float tangent_accel = 0.0F;
    float damping = 0.0F;
};

bool point_shape(const ParticleProcess3D& p) {
    return p.emission_shape == EmissionShape::Points ||
           p.emission_shape == EmissionShape::DirectedPoints;
}

// The emission point a display's emission_position picks, or none when there are no points.
std::optional<std::size_t> emission_point(const ParticleProcess3D& p, const Display& display) {
    if (p.emission_points.empty()) {
        return std::nullopt;
    }
    const auto count = static_cast<int>(p.emission_points.size());
    return static_cast<std::size_t>(
        std::max(std::min(count - 1,
                          static_cast<int>(display.emission_position * static_cast<float>(count))),
                 0));
}

// calculate_initial_display_params, calculate_initial_dynamics_params and
// calculate_initial_physical_params, in that order, from the particle's seed.
void initial_params(const ParticleProcess3D& p, std::uint32_t& seed, Display& display,
                    Dynamics& dynamics, Physical& physical) {
    float scale = random_param(p, ParticleParam::Scale, seed);
    scale = sign(scale) * std::max(std::abs(scale), 0.001F);
    display.scale = {scale, scale, scale};
    display.hue_rotation = kPi * 2.0F * random_param(p, ParticleParam::HueVariation, seed);
    display.animation_speed = random_param(p, ParticleParam::AnimSpeed, seed);
    display.animation_offset = random_param(p, ParticleParam::AnimOffset, seed);
    display.lifetime = 1.0F - (p.lifetime_randomness * rand_from_seed(seed));
    display.color = p.color;
    if (!p.color_initial_ramp.empty()) {
        display.color =
            times(display.color, sample_gradient(p.color_initial_ramp, rand_from_seed(seed)));
    }
    if (point_shape(p)) {
        display.emission_position = rand_from_seed(seed);
        if (!p.emission_colors.empty()) {
            if (const auto point = emission_point(p, display);
                point && *point < p.emission_colors.size()) {
                display.color = times(display.color, p.emission_colors[*point]);
            }
        }
    }

    dynamics.angle = random_param(p, ParticleParam::Angle, seed);
    dynamics.angular_velocity = random_param(p, ParticleParam::AngularVelocity, seed);
    dynamics.initial_velocity = random_param(p, ParticleParam::InitialVelocity, seed);
    dynamics.directional_velocity =
        mix(p.directional_velocity_min, p.directional_velocity_max, rand_from_seed(seed));
    dynamics.radial_velocity = random_param(p, ParticleParam::RadialVelocity, seed);
    dynamics.orbit_velocity = random_param(p, ParticleParam::OrbitVelocity, seed);
    if (p.turbulence_enabled) {
        dynamics.turbulence_influence = random_param(p, ParticleParam::TurbulenceInfluence, seed);
    }

    physical.linear_accel = random_param(p, ParticleParam::LinearAccel, seed);
    physical.radial_accel = random_param(p, ParticleParam::RadialAccel, seed);
    physical.tangent_accel = random_param(p, ParticleParam::TangentialAccel, seed);
    physical.damping = random_param(p, ParticleParam::Damping, seed);
}

// process_display_param.
void display_at(const ParticleProcess3D& p, Display& display, float life) {
    if (!p.scale_curve.empty()) {
        display.scale = times(display.scale, sample3(p.scale_curve, life, 0.0F));
    }
    display.hue_rotation *= sample_curve(curve(p, ParticleParam::HueVariation), life, 1.0F);
    display.animation_offset += sample_curve(curve(p, ParticleParam::AnimOffset), life, 0.0F);
    display.animation_speed *= sample_curve(curve(p, ParticleParam::AnimSpeed), life, 1.0F);
    if (!p.color_ramp.empty()) {
        display.color = times(display.color, sample_gradient(p.color_ramp, life));
    }
    display.color.w *= sample_curve(p.alpha_curve, life, 1.0F);
    display.color = rotate_hue(display.color, display.hue_rotation);
    if (!p.emission_curve.empty()) {
        const float bright = 1.0F + sample_curve(p.emission_curve, life, 0.0F);
        display.color = {display.color.x * bright, display.color.y * bright,
                         display.color.z * bright, display.color.w};
    }
}

// get_random_direction_from_spread.
Vec3 random_direction(const ParticleProcess3D& p, std::uint32_t& seed, float spread) {
    const float spread_rad = spread * kDegreesToRadians;
    if (p.disable_z) {
        float angle = rand_m1_p1(seed) * spread_rad;
        angle += p.direction.x != 0.0F ? slash::atan2(p.direction.y, p.direction.x)
                                       : sign(p.direction.y) * (kPi / 2.0F);
        const Vec2 turn = cos_sin(angle);
        return {turn.x, turn.y, 0.0F};
    }
    const float angle1 = rand_m1_p1(seed) * spread_rad;
    const float angle2 = rand_m1_p1(seed) * spread_rad * (1.0F - p.flatness);
    const Vec2 turn1 = cos_sin(angle1);
    const Vec2 turn2 = cos_sin(angle2);
    const Vec3 xz{turn1.y, 0.0F, turn1.x};
    Vec3 yz{0.0F, turn2.y, turn2.x};
    yz.z = yz.z / std::max(0.0001F, std::sqrt(std::abs(yz.z)));
    const Vec3 spread_direction{xz.x * yz.z, yz.y, xz.z * yz.z};
    const Vec3 forward = length(p.direction) > 0.0F ? normalize(p.direction) : Vec3{0, 0, 1};
    Vec3 binormal = cross({0.0F, 1.0F, 0.0F}, forward);
    if (length(binormal) < 0.0001F) {
        binormal = {0.0F, 0.0F, 1.0F};
    }
    binormal = normalize(binormal);
    const Vec3 normal = cross(binormal, forward);
    return normalize((binormal * spread_direction.x) + (normal * spread_direction.y) +
                     (forward * spread_direction.z));
}

// calculate_initial_position.
Vec3 initial_position(const ParticleProcess3D& p, const Display& display, std::uint32_t& seed) {
    Vec3 pos;
    switch (p.emission_shape) {
    case EmissionShape::Point:
        break;
    case EmissionShape::Sphere: {
        const float s = (rand_from_seed(seed) * 2.0F) - 1.0F;
        const float t = rand_from_seed(seed) * 2.0F * kPi;
        const float along = rand_from_seed(seed);
        const float radius = p.emission_sphere_radius * std::sqrt(1.0F - (s * s));
        const Vec2 turn = cos_sin(t);
        pos = Vec3{radius * turn.x, radius * turn.y, p.emission_sphere_radius * s} * along;
        break;
    }
    case EmissionShape::SphereSurface: {
        const float s = (rand_from_seed(seed) * 2.0F) - 1.0F;
        const float t = rand_from_seed(seed) * 2.0F * kPi;
        const float radius = p.emission_sphere_radius * std::sqrt(1.0F - (s * s));
        const Vec2 turn = cos_sin(t);
        pos = {radius * turn.x, radius * turn.y, p.emission_sphere_radius * s};
        break;
    }
    case EmissionShape::Box: {
        const float x = (rand_from_seed(seed) * 2.0F) - 1.0F;
        const float y = (rand_from_seed(seed) * 2.0F) - 1.0F;
        const float z = (rand_from_seed(seed) * 2.0F) - 1.0F;
        pos = times(Vec3{x, y, z}, p.emission_box_extents);
        break;
    }
    case EmissionShape::Points:
    case EmissionShape::DirectedPoints:
        if (const auto point = emission_point(p, display)) {
            pos = p.emission_points[*point];
        }
        break;
    case EmissionShape::Ring: {
        const float radius = std::max(0.001F, p.emission_ring_radius);
        const float inner = p.emission_ring_inner_radius;
        const float top = std::max(
            radius - (tangent_of((90.0F - p.emission_ring_cone_angle) * kDegreesToRadians) *
                      p.emission_ring_height),
            0.0F);
        float y_pos = rand_from_seed(seed);
        const float skew = std::max(std::min(radius, top) / std::max(radius, top), 0.5F);
        y_pos = radius < top ? power(y_pos, skew) : 1.0F - power(y_pos, skew);
        const float angle = rand_from_seed(seed) * 2.0F * kPi;
        float r = std::sqrt((rand_from_seed(seed) * ((radius * radius) - (inner * inner))) +
                            (inner * inner));
        r = mix(r, r * (top / radius), y_pos);
        const Vec3 axis = p.emission_ring_axis == Vec3{} ? Vec3{0.0F, 0.0F, 1.0F}
                                                         : normalize(p.emission_ring_axis);
        Vec3 ortho = std::abs(axis.x) == 1.0F && axis.y == 0.0F && axis.z == 0.0F
                         ? cross(axis, {0.0F, 1.0F, 0.0F})
                         : cross(axis, {1.0F, 0.0F, 0.0F});
        ortho = normalize(ortho);
        const Vec2 turn = cos_sin(angle);
        const float s = turn.y;
        const float c = turn.x;
        const float oc = 1.0F - c;
        // Godot's mat3 of these three columns, times ortho: a turn of angle about axis.
        const Vec3 c0{c + (axis.x * axis.x * oc), (axis.x * axis.y * oc) - (axis.z * s),
                      (axis.x * axis.z * oc) + (axis.y * s)};
        const Vec3 c1{(axis.x * axis.y * oc) + (s * axis.z), c + (axis.y * axis.y * oc),
                      (axis.y * axis.z * oc) - (axis.x * s)};
        const Vec3 c2{(axis.z * axis.x * oc) - (axis.y * s), (axis.z * axis.y * oc) + (axis.x * s),
                      c + (axis.z * axis.z * oc)};
        ortho = normalize((c0 * ortho.x) + (c1 * ortho.y) + (c2 * ortho.z));
        pos = (ortho * r) +
              (axis * ((y_pos * p.emission_ring_height) - (p.emission_ring_height / 2.0F)));
        break;
    }
    }
    return times(pos, p.emission_shape_scale) + p.emission_shape_offset;
}

// Godot's get_noise_direction.
Vec3 noise_direction(const ParticleProcess3D& p, const ParticleStep3D& step, Vec3 pos) {
    const float contrast = std::max(p.turbulence_noise_strength - 1.0F, 0.0F) * 70.0F;
    const float frequency = turbulence_frequency(p.turbulence_noise_scale);
    const Coord at{(pos.x * frequency) + (step.time * p.turbulence_noise_speed.x),
                   (pos.y * frequency) + (step.time * p.turbulence_noise_speed.y),
                   (pos.z * frequency) + (step.time * p.turbulence_noise_speed.z),
                   0.0F + (step.time * p.turbulence_noise_speed_random)};
    const Vec3 direction = curl_3d(at, contrast);
    return particle_math::mix(direction * 0.9F, direction, p.turbulence_noise_strength - 9.0F);
}

// The generated start(): a particle's place, velocity, color and life at its restart.
void start(Particle3D& particle, const ParticleProcess3D& p, const ParticleStep3D& step,
           std::uint32_t number, bool& active) {
    std::uint32_t seed = hash(number + 1U + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(p, seed, display, dynamics, physical);
    display_at(p, display, 0.0F);
    if (rand_from_seed(seed) > step.amount_ratio) {
        active = false;
    }
    particle.custom = {dynamics.angle, 0.0F, 0.0F, display.lifetime};
    particle.color = display.color;
    Vec3 origin = initial_position(p, display, seed);
    if (p.turbulence_enabled) {
        const float displacement = random_param(p, ParticleParam::TurbulenceDisplacement, seed);
        origin = origin + (noise_direction(p, step, origin) * displacement);
    }
    // TRANSFORM = EMISSION_TRANSFORM * TRANSFORM, from an identity basis.
    const Affine emission = Affine::of(step.emission);
    particle.x = with_w(emission.x);
    particle.y = with_w(emission.y);
    particle.z = with_w(emission.z);
    origin = emission.apply(origin);
    Vec3 velocity = random_direction(p, seed, p.spread) * dynamics.initial_velocity;
    if (p.emission_shape == EmissionShape::DirectedPoints) {
        if (const auto point = emission_point(p, display)) {
            // As Godot's black default texture, a point with no normal doesn't move.
            const Vec3 normal =
                *point < p.emission_normals.size() ? p.emission_normals[*point] : Vec3{};
            if (p.disable_z) {
                const Vec2 v{velocity.x, velocity.y};
                velocity = {(normal.x * v.x) + (normal.y * v.y),
                            (normal.y * v.x) - (normal.x * v.y), velocity.z};
            } else if (normal == Vec3{}) {
                velocity = {};
            } else {
                const Vec3 v0 = std::abs(normal.z) < 0.999F ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
                const Vec3 tangent = normalize(cross(v0, normal));
                const Vec3 bitangent = normalize(cross(tangent, normal));
                velocity =
                    (tangent * velocity.x) + (bitangent * velocity.y) + (normal * velocity.z);
            }
        }
    }
    velocity = emission.basis(velocity);
    velocity = velocity + (step.emitter_velocity * p.inherit_velocity_ratio);
    if (p.disable_z) {
        velocity.z = 0.0F;
        origin.z = 0.0F;
    }
    particle.velocity = velocity;
    particle.origin = with_w(origin);
}

// process_orbit_displacement: 2D's with disable_z, and in 3D only with a curve, as Godot's.
Vec3 orbit_displacement(const ParticleProcess3D& p, const Dynamics& dynamics, float life,
                        const Vec3& origin, const Affine& emission, float delta) {
    if ((p.orbit_velocity_curve.empty() && !p.disable_z) ||
        std::abs(dynamics.orbit_velocity) < 0.01F || delta < 0.001F) {
        return {};
    }
    Vec3 displacement;
    if (p.disable_z) {
        float amount = dynamics.orbit_velocity;
        if (!p.orbit_velocity_curve.empty()) {
            // A CurveTexture's red, or a CurveXYZTexture's blue: its z.
            amount *= sample_curve(p.orbit_velocity_curve.axes[2], life, 0.0F);
        }
        if (amount != 0.0F) {
            const Vec3 diff = origin - emission.origin;
            const Vec2 turn = cos_sin(amount * kPi * 2.0F * delta);
            displacement = {(turn.x * diff.x) + (turn.y * diff.y) - diff.x,
                            (-turn.y * diff.x) + (turn.x * diff.y) - diff.y, 0.0F};
        }
    } else {
        const Vec3 turns = sample3(p.orbit_velocity_curve, life, 0.0F) *
                           (dynamics.orbit_velocity * kPi * 2.0F * delta);
        const Vec3 at = emission.inverse().apply(origin) - p.velocity_pivot;
        // About x, y and z in turn, each from where the particle is.
        Vec3 local{0.0F, at.y, at.z};
        Vec2 cs = cos_sin(turns.x);
        displacement = Vec3{local.x, (cs.x * local.y) - (cs.y * local.z),
                            (cs.y * local.y) + (cs.x * local.z)} -
                       local;
        local = {at.x, 0.0F, at.z};
        cs = cos_sin(turns.y);
        displacement = displacement + (Vec3{(cs.x * local.x) + (cs.y * local.z), local.y,
                                            (-cs.y * local.x) + (cs.x * local.z)} -
                                       local);
        local = {at.x, at.y, 0.0F};
        cs = cos_sin(turns.z);
        displacement = displacement + (Vec3{(cs.x * local.x) - (cs.y * local.y),
                                            (cs.y * local.x) + (cs.x * local.y), local.z} -
                                       local);
    }
    return emission.basis(displacement * (1.0F / delta));
}

// process_radial_displacement.
Vec3 radial_displacement(const ParticleProcess3D& p, const Dynamics& dynamics, float life,
                         std::uint32_t& seed, const Vec3& origin, const Affine& emission,
                         float delta) {
    if (delta < 0.001F) {
        return {};
    }
    const float multiplier = sample_curve(curve(p, ParticleParam::RadialVelocity), life, 1.0F);
    const Vec3 pivot = emission.apply(p.velocity_pivot);
    const Vec3 away = origin - pivot;
    Vec3 out;
    if (length(away) > 0.01F) {
        out = normalize(away) * (multiplier * dynamics.radial_velocity);
    } else {
        out = random_direction(p, seed, 360.0F) * dynamics.radial_velocity;
    }
    if (multiplier * dynamics.radial_velocity < 0.0F) {
        // Inward speed stops at the pivot instead of flickering past it.
        out = normalize(out) *
              std::min(std::abs(multiplier * dynamics.radial_velocity), length(away) / delta);
    }
    return out;
}

// The generated process(): one step of a live particle's life.
void process(Particle3D& particle, const ParticleProcess3D& p, const ParticleStep3D& step,
             std::uint32_t number, float delta, bool& active) {
    std::uint32_t seed = hash(number + 1U + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(p, seed, display, dynamics, physical);

    particle.custom.y += delta / step.lifetime;
    particle.custom.y = mix(particle.custom.y, 1.0F, step.interp_to_end);
    const float life = particle.custom.y / display.lifetime;
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }

    const Affine emission = Affine::of(step.emission);
    Vec3 origin = xyz(particle.origin);
    Vec3 controlled = orbit_displacement(p, dynamics, life, origin, emission, delta);
    controlled = controlled + radial_displacement(p, dynamics, life, seed, origin, emission, delta);
    if (!p.directional_velocity_curve.empty()) {
        Vec3 directional =
            sample3(p.directional_velocity_curve, life, 0.0F) * dynamics.directional_velocity;
        if (p.directional_velocity_global) {
            directional = emission.basis(directional);
        }
        controlled = controlled + directional;
    }

    physical.linear_accel *= sample_curve(curve(p, ParticleParam::LinearAccel), life, 1.0F);
    physical.radial_accel *= sample_curve(curve(p, ParticleParam::RadialAccel), life, 1.0F);
    physical.tangent_accel *= sample_curve(curve(p, ParticleParam::TangentialAccel), life, 1.0F);
    physical.damping *= sample_curve(curve(p, ParticleParam::Damping), life, 1.0F);

    Vec3 velocity = particle.velocity;
    Vec3 force = p.gravity;
    if (length(velocity) > 0.0F) {
        force = force + (normalize(velocity) * physical.linear_accel);
    }
    const Vec3 diff = origin - emission.origin;
    if (length(diff) > 0.0F) {
        force = force + (normalize(diff) * physical.radial_accel);
    }
    if (p.disable_z) {
        if (length(Vec3{diff.y, diff.x, 0.0F}) > 0.0F) {
            force = force + (normalize(Vec3{-diff.y, diff.x, 0.0F}) * physical.tangent_accel);
        }
        force.z = 0.0F;
    } else {
        // Godot's normalize of a zero vector makes NaNs, which the length test then skips.
        const Vec3 across = cross(normalize(diff), normalize(p.gravity));
        if (length(across) > 0.0F) {
            force = force + (normalize(across) * physical.tangent_accel);
        }
    }
    velocity = velocity + (force * delta);
    if (physical.damping > 0.0F) {
        float v = length(velocity);
        if (!p.damping_as_friction) {
            v -= physical.damping * delta;
        } else if (v > 0.001F) {
            // Godot's friction, for a particle of 0.05 kg.
            v -= v * v * physical.damping * 0.05F * delta;
        }
        velocity = v < 0.0F ? Vec3{} : normalize(velocity) * v;
    }

    // Turbulence before limiting.
    if (p.turbulence_enabled) {
        const float influence =
            sample_curve(curve(p, ParticleParam::TurbulenceInfluence), life, 1.0F);
        const Vec3 direction = normalize(noise_direction(p, step, origin));
        const float amount = std::clamp(dynamics.turbulence_influence * influence, 0.0F, 1.0F) *
                             (delta <= 0.0F ? 0.0F : 1.0F);
        const float boost = 1.0F + ((1.0F - amount) * 0.2F);
        velocity = particle_math::mix(velocity, direction * (length(velocity) * boost), amount);
        controlled =
            particle_math::mix(controlled, direction * (length(controlled) * boost), amount);
    }
    Vec3 final_velocity = controlled + velocity;
    if (!p.velocity_limit_curve.empty() && length(final_velocity) > 0.001F) {
        const float limit = std::abs(sample_curve(p.velocity_limit_curve, life, 0.0F));
        final_velocity =
            normalize(final_velocity) * std::min(std::abs(length(final_velocity)), limit);
    }
    if (p.disable_z) {
        final_velocity.z = 0.0F;
    }
    origin = origin + (final_velocity * delta);
    particle.velocity = velocity;

    display_at(p, display, life);

    float base_angle = dynamics.angle * sample_curve(curve(p, ParticleParam::Angle), life, 1.0F);
    base_angle += particle.custom.y * step.lifetime * dynamics.angular_velocity *
                  sample_curve(curve(p, ParticleParam::AngularVelocity), life, 1.0F);
    particle.custom.x = base_angle * kDegreesToRadians;
    particle.color = display.color;

    Vec3 x = xyz(particle.x);
    Vec3 y = xyz(particle.y);
    Vec3 z = xyz(particle.z);
    if (p.disable_z) {
        if (p.align_y) {
            y = normalize(length(final_velocity) > 0.0F ? final_velocity : y);
            x = normalize(cross(y, z));
        } else {
            const Vec2 cs = cos_sin(particle.custom.x);
            x = {cs.x, -cs.y, 0.0F};
            y = {cs.y, cs.x, 0.0F};
        }
        z = {0.0F, 0.0F, 1.0F};
        origin.z = 0.0F;
    } else {
        if (p.align_y) {
            y = normalize_or_else(length(final_velocity) > 0.0F ? final_velocity : y,
                                  {0.0F, 1.0F, 0.0F});
            if (y == normalize_or_else(x, {1.0F, 0.0F, 0.0F})) {
                x = normalize_or_else(cross(y, z), {1.0F, 0.0F, 0.0F});
                z = normalize_or_else(cross(x, y), {0.0F, 0.0F, 1.0F});
            } else {
                z = normalize_or_else(cross(x, y), {0.0F, 0.0F, 1.0F});
                x = normalize_or_else(cross(y, z), {1.0F, 0.0F, 0.0F});
            }
        } else {
            x = normalize_or_else(x, {1.0F, 0.0F, 0.0F});
            y = normalize_or_else(y, {0.0F, 1.0F, 0.0F});
            z = normalize_or_else(z, {0.0F, 0.0F, 1.0F});
        }
        if (p.rotate_y) {
            const Vec2 cs = cos_sin(particle.custom.x);
            x = {cs.x, 0.0F, -cs.y};
            y = {0.0F, 1.0F, 0.0F};
            z = {cs.y, 0.0F, cs.x};
        }
    }
    if (!p.scale_over_velocity_curve.empty()) {
        const float speed = length(final_velocity);
        const float at =
            speed > 0.001F
                ? std::clamp(speed / (p.scale_over_velocity_max - p.scale_over_velocity_min), 0.0F,
                             1.0F)
                : 0.0F;
        display.scale = times(display.scale, sample3(p.scale_over_velocity_curve, at, 0.0F));
    }
    // A scale of 0 keeps its sign as positive, at the smallest scale.
    const auto scaled = [](float s) {
        return (s < 0.0F ? -1.0F : 1.0F) * std::max(std::abs(s), 0.001F);
    };
    particle.x = with_w(x * scaled(display.scale.x));
    particle.y = with_w(y * scaled(display.scale.y));
    particle.z = with_w(z * scaled(display.scale.z));
    particle.origin = with_w(origin);
    particle.custom.z = display.animation_offset + (life * display.animation_speed);
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }
}

// One step of a particle, as particles.glsl runs it: restarts it in its turn while emitting,
// then moves it.
void step_particle(Particle3D& particle, std::uint32_t index, std::uint32_t total,
                   const ParticleProcess3D& p, const ParticleStep3D& step) {
    if (step.clear) {
        particle = Particle3D{};
    }
    particle.flags &= ~kParticleStarted;
    float delta = step.delta;
    bool restart = false;
    if (step.emitting) {
        const float phase_step = detail::particle_phase_step(total);
        float restart_phase = static_cast<float>(index) * phase_step;
        if (step.randomness > 0.0F) {
            std::uint32_t seed = step.cycle;
            if (restart_phase >= step.system_phase) {
                seed -= 1U;
            }
            seed *= total;
            seed += index;
            const float random = static_cast<float>(hash(seed) % 65536U) / 65536.0F;
            restart_phase += step.randomness * random * phase_step;
        }
        restart_phase *= 1.0F - step.explosiveness;
        if (step.system_phase > step.prev_system_phase) {
            if (restart_phase >= step.prev_system_phase && restart_phase < step.system_phase) {
                restart = true;
                if (step.fractional_delta) {
                    delta = (step.system_phase - restart_phase) * step.lifetime;
                }
            }
        } else if (step.delta > 0.0F) {
            if (restart_phase >= step.prev_system_phase) {
                restart = true;
                if (step.fractional_delta) {
                    delta = (1.0F - restart_phase + step.system_phase) * step.lifetime;
                }
            } else if (restart_phase < step.system_phase) {
                restart = true;
                if (step.fractional_delta) {
                    delta = (step.system_phase - restart_phase) * step.lifetime;
                }
            }
        }
        if (restart) {
            particle.flags =
                kParticleActive | kParticleStarted | (step.cycle << kParticleCycleShift);
        }
    }
    bool active = particle.active();
    const std::uint32_t number = ((particle.flags >> kParticleCycleShift) * total) + index;
    if (restart && active) {
        start(particle, p, step, number, active);
    }
    if (active) {
        process(particle, p, step, number, delta, active);
    }
    particle.flags &= ~kParticleActive;
    if (active) {
        particle.flags |= kParticleActive;
    }
}

} // namespace

void process_particles_3d(std::span<Particle3D> particles, const ParticleProcess3D& process,
                          std::span<const ParticleStep3D> steps) {
    const auto total = static_cast<std::uint32_t>(particles.size());
    for (const ParticleStep3D& step : steps) {
        for (std::uint32_t i = 0; i < total; ++i) {
            step_particle(particles[i], i, total, process, step);
        }
    }
}

std::uint32_t particle_draw_index_3d(std::uint32_t slot, std::uint32_t total, ParticleOrder3D order,
                                     std::uint32_t lifetime_split) {
    if (order != ParticleOrder3D::Lifetime && order != ParticleOrder3D::ReverseLifetime) {
        return slot;
    }
    const std::uint32_t split = std::min(lifetime_split, total - 1);
    const std::uint32_t limit = total - split;
    const std::uint32_t particle =
        order == ParticleOrder3D::ReverseLifetime ? total - slot - 1 : slot;
    return particle < limit ? split + particle : particle - limit;
}

void particle_mesh_vertices_3d(std::span<const Particle3D> particles,
                               std::span<const Vertex3D> source, const ParticleMeshDraw3D& draw,
                               std::span<Vertex3D> out) {
    const auto total = static_cast<std::uint32_t>(particles.size());
    const std::size_t corners = source.size();
    if (total == 0 || corners == 0 || out.size() < total * corners) {
        return;
    }
    // View depth: back to front along the camera's z, ties in slot order, as Godot's sort
    // buffer of every particle.
    std::vector<std::uint32_t> sorted;
    if (draw.order == ParticleOrder3D::ViewDepth) {
        sorted.resize(total);
        std::iota(sorted.begin(), sorted.end(), 0U);
        std::ranges::stable_sort(sorted, {}, [&](std::uint32_t i) {
            return dot(draw.sort_direction, xyz(particles[i].origin));
        });
    }
    const Affine emitter = Affine::of(draw.transform);
    const Affine camera = Affine::of(draw.camera);
    const Vec3 sort = draw.sort_direction;
    const Vec3 up = draw.align_up;
    for (std::uint32_t slot = 0; slot < total; ++slot) {
        const std::uint32_t index =
            sorted.empty() ? particle_draw_index_3d(slot, total, draw.order, draw.lifetime_split)
                           : sorted[slot];
        const Particle3D& particle = particles[index];
        Vertex3D* const first = out.data() + (static_cast<std::size_t>(slot) * corners);
        if (!particle.active()) {
            // Folded onto one point, as Godot's zero scale.
            for (std::size_t c = 0; c < corners; ++c) {
                first[c] = source[c];
                first[c].position = {0.0F, 0.0F, 0.0F};
            }
            continue;
        }
        Affine place{.x = xyz(particle.x),
                     .y = xyz(particle.y),
                     .z = xyz(particle.z),
                     .origin = xyz(particle.origin)};
        const Vec3 v = particle.velocity;
        switch (draw.align) {
        case ParticleAlign::Disabled:
            break;
        case ParticleAlign::ZBillboard: {
            const Affine local{.x = normalize(cross(up, sort)), .y = up, .z = sort, .origin = {}};
            place.x = local.basis(place.x);
            place.y = local.basis(place.y);
            place.z = local.basis(place.z);
            break;
        }
        case ParticleAlign::YToVelocity: {
            const float s = (length(place.x) + length(place.y) + length(place.z)) / 3.0F;
            place.y = normalize(length(v) > 0.0F ? v : place.y);
            place.x = normalize(cross(place.y, place.z));
            place.z = Vec3{0.0F, 0.0F, 1.0F} * s;
            place.x = place.x * s;
            place.y = place.y * s;
            break;
        }
        case ParticleAlign::ZBillboardYToVelocity: {
            Vec3 screen = v - (sort * dot(sort, v));
            if (length(screen) == 0.0F) {
                screen = up;
            }
            screen = normalize(screen);
            const Vec3 x = normalize(cross(screen, sort)) * length(place.x);
            const Vec3 y = screen * length(place.y);
            place.z = sort * length(place.z);
            place.x = x;
            place.y = y;
            break;
        }
        }
        place.origin = place.origin + (v * draw.remainder);
        const Affine model = emitter * place;
        // BaseMaterial3D's billboard: the camera's basis in place of the model's.
        Affine placed = model;
        if (draw.billboard != ParticleBillboard::Disabled) {
            if (draw.billboard == ParticleBillboard::Enabled) {
                placed.x = camera.x;
                placed.y = camera.y;
                placed.z = camera.z;
            } else if (draw.billboard == ParticleBillboard::FixedY) {
                placed.x = normalize(cross({0.0F, 1.0F, 0.0F}, camera.z));
                placed.y = {0.0F, 1.0F, 0.0F};
                placed.z = normalize(cross(camera.x, {0.0F, 1.0F, 0.0F}));
            } else {
                const Vec3 cx = normalize(camera.x);
                const Vec3 cy = normalize(camera.y);
                const Vec2 cs = cos_sin(particle.custom.x);
                placed.x = (cx * cs.x) - (cy * cs.y);
                placed.y = (cx * cs.y) + (cy * cs.x);
                placed.z = normalize(camera.z);
            }
            if (draw.keep_scale) {
                placed.x = placed.x * length(model.x);
                placed.y = placed.y * length(model.y);
                placed.z = placed.z * length(model.z);
            }
        }
        // Normals turn with the basis, as Godot's MODELVIEW_NORMAL_MATRIX: as it is for a
        // billboard, by its inverse's transpose otherwise.
        const Affine inverse = placed.inverse();
        const bool billboard = draw.billboard != ParticleBillboard::Disabled;
        const auto normal_of = [&](Vec3 n) {
            return billboard
                       ? normalize(placed.basis(n))
                       : normalize(
                             Vec3{(inverse.x.x * n.x) + (inverse.x.y * n.y) + (inverse.x.z * n.z),
                                  (inverse.y.x * n.x) + (inverse.y.y * n.y) + (inverse.y.z * n.z),
                                  (inverse.z.x * n.x) + (inverse.z.y * n.y) + (inverse.z.z * n.z)});
        };
        const std::array<float, 4> tint{
            std::clamp(particle.color.x, 0.0F, 1.0F), std::clamp(particle.color.y, 0.0F, 1.0F),
            std::clamp(particle.color.z, 0.0F, 1.0F), std::clamp(particle.color.w, 0.0F, 1.0F)};
        // The frame a Particles billboard shows, as Godot's particle animation.
        const float h = static_cast<float>(std::max(draw.h_frames, 1U));
        const float vf = static_cast<float>(std::max(draw.v_frames, 1U));
        float frame = 0.0F;
        if (draw.billboard == ParticleBillboard::Particles) {
            const float frames = h * vf;
            frame = std::floor(particle.custom.z * frames);
            if (!draw.anim_loop) {
                frame = std::clamp(frame, 0.0F, frames - 1.0F);
            } else {
                frame -= frames * std::floor(frame / frames);
            }
        }
        const float column = frame - (h * std::floor(frame / h));
        const float row = std::floor((frame + 0.5F) / h);
        for (std::size_t c = 0; c < corners; ++c) {
            const Vertex3D& from = source[c];
            Vertex3D& to = first[c];
            to = from;
            const Vec3 position =
                placed.apply({from.position[0], from.position[1], from.position[2]});
            to.position = {position.x, position.y, position.z};
            const Vec3 normal = normal_of({from.normal[0], from.normal[1], from.normal[2]});
            to.normal = {normal.x, normal.y, normal.z};
            const Vec3 tangent =
                normalize(placed.basis({from.tangent[0], from.tangent[1], from.tangent[2]}));
            to.tangent = {tangent.x, tangent.y, tangent.z, from.tangent[3]};
            float u = (from.uv[0] * draw.uv_scale[0]) + draw.uv_offset[0];
            float w = (from.uv[1] * draw.uv_scale[1]) + draw.uv_offset[1];
            if (draw.billboard == ParticleBillboard::Particles) {
                u = (u / h) + (column / h);
                w = (w / vf) + (row / vf);
            }
            to.uv = {u, w};
            for (std::size_t k = 0; k < 4; ++k) {
                to.rgba.at(k) = static_cast<std::uint8_t>(
                    std::lround(static_cast<float>(from.rgba.at(k)) * tint.at(k)));
            }
        }
    }
}

namespace detail {

Result<Particles3DId> ParticleStore3D::create(std::uint32_t amount) {
    if (auto checked = check_particle_amount_3d(amount); !checked) {
        return checked.error();
    }
    const std::uint32_t id = ++next_;
    systems_[id].assign(amount, Particle3D{});
    return static_cast<Particles3DId>(id);
}

void ParticleStore3D::destroy(Particles3DId particles) {
    systems_.erase(static_cast<std::uint32_t>(particles));
}

Result<void> ParticleStore3D::process(Particles3DId particles, const ParticleProcess3D& process,
                                      std::span<const ParticleStep3D> steps) {
    const auto found = systems_.find(static_cast<std::uint32_t>(particles));
    if (found == systems_.end()) {
        return make_error(ErrorCode::NotFound, "no such 3D particle system");
    }
    if (auto checked = check_particle_process_3d(process, steps); !checked) {
        return checked;
    }
    process_particles_3d(found->second, process, steps);
    return {};
}

Result<std::span<const Particle3D>> ParticleStore3D::particles(Particles3DId particles) const {
    const auto found = systems_.find(static_cast<std::uint32_t>(particles));
    if (found == systems_.end()) {
        return make_error(ErrorCode::NotFound, "no such 3D particle system");
    }
    return std::span<const Particle3D>(found->second);
}

} // namespace detail

} // namespace slash::render
