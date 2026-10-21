#include "slash/render/particles.hpp"

#include "slash/render/device.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

// Godot's particles shader (servers/rendering/renderer_rd/shaders/particles.glsl) and the code
// ParticleProcessMaterial::_update_shader generates for 2D, step for step, on the CPU. The GPU
// runs the same in shaders/particles_process.hlsl and particles_draw.hlsl; a change to one goes
// in the other.
namespace slash::render {
namespace {

// The value of pi Godot's generated shader uses.
constexpr float kPi = 3.14159F; // NOLINT(modernize-use-std-numbers): Godot's, not std's
constexpr float kDegreesToRadians = kPi / 180.0F;

Vec2 operator/(Vec2 v, float s) {
    return {v.x / s, v.y / s};
}

float dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

float length(Vec2 v) {
    return std::sqrt(dot(v, v));
}

// v made a unit vector, or zero for zero (GLSL's normalize gives NaN there).
Vec2 normalize(Vec2 v) {
    const float l = length(v);
    return l > 0.0F ? v / l : Vec2{};
}

// GLSL's mix: a * (1 - t) + b * t.
float mix(float a, float b, float t) {
    return (a * (1.0F - t)) + (b * t);
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalize(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

Vec3 mix(Vec3 a, Vec3 b, float t) {
    return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)};
}

float sign(float v) {
    if (v > 0.0F) {
        return 1.0F;
    }
    return v < 0.0F ? -1.0F : 0.0F;
}

Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

// GLSL's mod: x - y * floor(x / y).
float mod(float x, float y) {
    return x - (y * std::floor(x / y));
}

// A 2D affine transform as ParticleStep::emission holds one, applied to a point or a vector.
Vec2 transform_point(const std::array<float, 6>& t, Vec2 p) {
    return {(t[0] * p.x) + (t[2] * p.y) + t[4], (t[1] * p.x) + (t[3] * p.y) + t[5]};
}

Vec2 transform_vector(const std::array<float, 6>& t, Vec2 v) {
    return {(t[0] * v.x) + (t[2] * v.y), (t[1] * v.x) + (t[3] * v.y)};
}

// Godot's hash, which improves a seed.
std::uint32_t hash(std::uint32_t x) {
    x = ((x >> 16U) ^ x) * 73244475U;
    x = ((x >> 16U) ^ x) * 73244475U;
    x = (x >> 16U) ^ x;
    return x;
}

// Godot's rand_from_seed: Park and Miller's generator, a number from 0 to 1.
float rand_from_seed(std::uint32_t& seed) {
    auto s = static_cast<std::int32_t>(seed);
    if (s == 0) {
        s = 305420679;
    }
    const std::int32_t k = s / 127773;
    s = (16807 * (s - (k * 127773))) - (2836 * k);
    if (s < 0) {
        s += 2147483647;
    }
    seed = static_cast<std::uint32_t>(s);
    return static_cast<float>(seed % 65536U) / 65535.0F;
}

float rand_m1_p1(std::uint32_t& seed) {
    return (rand_from_seed(seed) * 2.0F) - 1.0F;
}

// ---- Turbulence: the noise ParticleProcessMaterial generates for it.

using Coord = std::array<float, 4>;

float dot4(const Coord& a, const Coord& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
}

// 16 bits as a number from -1 to 1, exactly on every device.
float unit_of(std::uint32_t bits) {
    return (static_cast<float>(bits & 0xFFFFU) * (1.0F / 32768.0F)) - 1.0F;
}

// Godot's grad: a lattice point's gradient. Godot hashes the point with fractions of float
// products, which round differently on different GPUs; this hashes its integer coordinates, and
// channel tells apart the three noises curl_3d samples, which Godot moves far apart instead.
Coord grad(const std::array<std::int32_t, 4>& cell, std::uint32_t channel) {
    std::uint32_t h =
        hash(static_cast<std::uint32_t>(cell[0]) + (channel * 0x632BE59BU) + 0x9E3779B9U);
    h = hash(h + static_cast<std::uint32_t>(cell[1]));
    h = hash(h + static_cast<std::uint32_t>(cell[2]));
    h = hash(h + static_cast<std::uint32_t>(cell[3]));
    const std::uint32_t g = hash(h + 1U);
    return {unit_of(h), unit_of(h >> 16U), unit_of(g), unit_of(g >> 16U)};
}

// Godot's noise: 4D gradient noise, its domain turned to improve the look of slices.
float noise(Coord coord, std::uint32_t channel) {
    constexpr float kSixth = -0.1666667F;
    const float shift = dot4(coord, {kSixth, kSixth, kSixth, -0.5F});
    coord = {coord[0] + shift, coord[1] + shift, coord[2] + shift,
             dot4(coord, {0.5F, 0.5F, 0.5F, 0.5F})};
    std::array<std::int32_t, 4> base{};
    Coord delta{};
    Coord fade{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float floored = std::floor(coord.at(i));
        base.at(i) = static_cast<std::int32_t>(floored);
        const float d = coord.at(i) - floored;
        delta.at(i) = d;
        fade.at(i) = d * d * d * (10.0F + (d * (-15.0F + (d * 6.0F))));
    }
    // Each corner's dot with its gradient, weighted by the fade along each axis: Godot's mixes
    // over w, z, y and x as one sum, which the compute shader runs as a loop rather than as
    // sixteen copies of the hash.
    float result = 0.0F;
    for (std::uint32_t corner = 0; corner < 16; ++corner) {
        std::array<std::int32_t, 4> cell = base;
        Coord offset = delta;
        float weight = 1.0F;
        for (std::size_t axis = 0; axis < 4; ++axis) {
            if ((corner & (1U << axis)) != 0) {
                cell.at(axis) += 1;
                offset.at(axis) -= 1.0F;
                weight *= fade.at(axis);
            } else {
                weight *= 1.0F - fade.at(axis);
            }
        }
        result += weight * dot4(offset, grad(cell, channel));
    }
    return result;
}

// Godot's curl_3d: the curl of three noises, from differences epsilon either side. Godot samples
// each noise at all six places; the curl uses two at each, taken here in the order its sums take
// them: term i adds to component i / 2, plus for even i and minus for odd.
Vec3 curl_3d(const Coord& p, float c) {
    const float epsilon = 0.001F + c;
    std::array<float, 3> curl{};
    for (std::uint32_t term = 0; term < 6; ++term) {
        const std::uint32_t component = term / 2;
        const std::uint32_t odd = term % 2;
        const std::size_t axis = (component + 1 + odd) % 3;
        const std::uint32_t channel = (component + 2 - odd) % 3;
        Coord ahead = p;
        Coord behind = p;
        ahead.at(axis) += epsilon;
        behind.at(axis) -= epsilon;
        const float difference = noise(ahead, channel) - noise(behind, channel);
        curl.at(component) += odd != 0 ? -difference : difference;
    }
    return normalize(Vec3{curl[0], curl[1], curl[2]});
}

// Godot's get_noise_direction, at a point on the screen.
Vec3 noise_direction(const ParticleProcess& p, const ParticleStep& step, Vec2 pos) {
    const float contrast = std::max(p.turbulence_noise_strength - 1.0F, 0.0F) * 70.0F;
    const float frequency = turbulence_frequency(p.turbulence_noise_scale);
    const Coord at{(pos.x * frequency) + (step.time * p.turbulence_noise_speed.x),
                   (pos.y * frequency) + (step.time * p.turbulence_noise_speed.y),
                   (0.0F * frequency) + (step.time * p.turbulence_noise_speed.z),
                   0.0F + (step.time * p.turbulence_noise_speed_random)};
    const Vec3 direction = curl_3d(at, contrast);
    return mix(direction * 0.9F, direction, p.turbulence_noise_strength - 9.0F);
}

// Godot's rotate_hue: a turn of the color about the gray axis.
Vec4 rotate_hue(Vec4 color, float angle) {
    const Vec2 turn = cos_sin(angle);
    const float c = turn.x;
    const float s = turn.y;
    // Columns of the matrix (each [r, g, b] of a column; alpha passes through).
    constexpr std::array<std::array<float, 3>, 3> kBase{
        {{0.299F, 0.587F, 0.114F}, {0.299F, 0.587F, 0.114F}, {0.299F, 0.587F, 0.114F}}};
    constexpr std::array<std::array<float, 3>, 3> kCos{
        {{0.701F, -0.587F, -0.114F}, {-0.299F, 0.413F, -0.114F}, {-0.300F, -0.588F, 0.886F}}};
    constexpr std::array<std::array<float, 3>, 3> kSin{
        {{0.168F, 0.330F, -0.497F}, {-0.328F, 0.035F, 0.292F}, {1.250F, -1.050F, -0.203F}}};
    const std::array<float, 3> in{color.x, color.y, color.z};
    std::array<float, 3> out{};
    for (std::size_t row = 0; row < 3; ++row) {
        float sum = 0.0F;
        for (std::size_t column = 0; column < 3; ++column) {
            const float m = kBase[column][row] + (kCos[column][row] * c) + (kSin[column][row] * s);
            sum += m * in[column];
        }
        out[row] = sum;
    }
    return {out[0], out[1], out[2], color.w};
}

float param_min(const ParticleProcess& p, ParticleParam param) {
    return p.param_min[static_cast<std::size_t>(param)];
}

float param_max(const ParticleProcess& p, ParticleParam param) {
    return p.param_max[static_cast<std::size_t>(param)];
}

float random_param(const ParticleProcess& p, ParticleParam param, std::uint32_t& seed) {
    return mix(param_min(p, param), param_max(p, param), rand_from_seed(seed));
}

const ParticleCurve& curve(const ParticleProcess& p, ParticleParam param) {
    return p.param_curve[static_cast<std::size_t>(param)];
}

// Godot's DisplayParameters, DynamicsParameters and PhysicalParameters.
struct Display {
    float scale = 1.0F;
    float hue_rotation = 0.0F;
    float animation_speed = 0.0F;
    float animation_offset = 0.0F;
    float lifetime = 1.0F;
    Vec4 color;
    float emission_position = 0.0F; // where in emission_points, from 0 to 1
};

struct Dynamics {
    float angle = 0.0F;
    float angular_velocity = 0.0F;
    float initial_velocity = 0.0F;
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

// The emission point a display's emission_position picks.
std::size_t emission_point(const ParticleProcess& p, const Display& display) {
    const auto count = static_cast<int>(p.emission_points.size());
    return static_cast<std::size_t>(
        std::max(std::min(count - 1,
                          static_cast<int>(display.emission_position * static_cast<float>(count))),
                 0));
}

bool from_points(const ParticleProcess& p) {
    return (p.emission_shape == EmissionShape::Points ||
            p.emission_shape == EmissionShape::DirectedPoints) &&
           !p.emission_points.empty();
}

// calculate_initial_display_params, calculate_initial_dynamics_params and
// calculate_initial_physical_params, in that order, from the particle's seed.
void initial_params(const ParticleProcess& p, std::uint32_t& seed, Display& display,
                    Dynamics& dynamics, Physical& physical) {
    display.scale = random_param(p, ParticleParam::Scale, seed);
    display.scale = sign(display.scale) * std::max(std::abs(display.scale), 0.001F);
    display.hue_rotation = kPi * 2.0F * random_param(p, ParticleParam::HueVariation, seed);
    display.animation_speed = random_param(p, ParticleParam::AnimSpeed, seed);
    display.animation_offset = random_param(p, ParticleParam::AnimOffset, seed);
    display.lifetime = 1.0F - (p.lifetime_randomness * rand_from_seed(seed));
    display.color = p.color;
    if (!p.color_initial_ramp.empty()) {
        display.color =
            times(display.color, sample_gradient(p.color_initial_ramp, rand_from_seed(seed)));
    }
    if (p.emission_shape == EmissionShape::Points ||
        p.emission_shape == EmissionShape::DirectedPoints) {
        display.emission_position = rand_from_seed(seed);
        if (from_points(p) && !p.emission_colors.empty()) {
            const std::size_t point = emission_point(p, display);
            if (point < p.emission_colors.size()) {
                display.color = times(display.color, p.emission_colors[point]);
            }
        }
    }

    dynamics.angle = random_param(p, ParticleParam::Angle, seed);
    dynamics.angular_velocity = random_param(p, ParticleParam::AngularVelocity, seed);
    dynamics.initial_velocity = random_param(p, ParticleParam::InitialVelocity, seed);
    (void)rand_from_seed(seed); // directional velocity, which 2D doesn't use
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

// process_display_param: the display parameters at a point in the particle's life.
void display_at(const ParticleProcess& p, Display& display, float life) {
    display.scale *= sample_curve(curve(p, ParticleParam::Scale), life, 1.0F);
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

// get_random_direction_from_spread for 2D.
Vec2 random_direction(const ParticleProcess& p, std::uint32_t& seed, float spread) {
    float angle = rand_m1_p1(seed) * (spread * kDegreesToRadians);
    angle += p.direction.x != 0.0F ? atan2(p.direction.y, p.direction.x)
                                   : sign(p.direction.y) * (kPi / 2.0F);
    return cos_sin(angle);
}

// calculate_initial_position for 2D.
Vec2 initial_position(const ParticleProcess& p, const Display& display, std::uint32_t& seed) {
    Vec2 pos;
    switch (p.emission_shape) {
    case EmissionShape::Point:
        break;
    case EmissionShape::Sphere:
    case EmissionShape::SphereSurface: {
        const float s = (rand_from_seed(seed) * 2.0F) - 1.0F;
        const float t = rand_from_seed(seed) * 2.0F * kPi;
        const float radius = p.emission_sphere_radius * std::sqrt(1.0F - (s * s));
        const Vec2 turn = cos_sin(t);
        pos = {radius * turn.x, radius * turn.y};
        if (p.emission_shape == EmissionShape::Sphere) {
            const float along = rand_from_seed(seed);
            pos = {mix(0.0F, pos.x, along), mix(0.0F, pos.y, along)};
        }
        break;
    }
    case EmissionShape::Box: {
        const float x = (rand_from_seed(seed) * 2.0F) - 1.0F;
        const float y = (rand_from_seed(seed) * 2.0F) - 1.0F;
        (void)rand_from_seed(seed); // z
        pos = {x * p.emission_box_extents.x, y * p.emission_box_extents.y};
        break;
    }
    case EmissionShape::Points:
    case EmissionShape::DirectedPoints:
        if (from_points(p)) {
            pos = p.emission_points[emission_point(p, display)];
        }
        break;
    case EmissionShape::Ring: {
        // Godot's ring about the z axis, one pixel high and its cone square, as 2D leaves it.
        const float radius = std::max(0.001F, p.emission_ring_radius);
        const float inner = p.emission_ring_inner_radius;
        (void)rand_from_seed(seed); // the height on the ring, which 2D drops
        const float angle = rand_from_seed(seed) * 2.0F * kPi;
        const float r = std::sqrt((rand_from_seed(seed) * ((radius * radius) - (inner * inner))) +
                                  (inner * inner));
        const Vec2 turn = cos_sin(angle);
        pos = {turn.y * r, turn.x * r};
        break;
    }
    }
    return {(pos.x * p.emission_shape_scale.x) + p.emission_shape_offset.x,
            (pos.y * p.emission_shape_scale.y) + p.emission_shape_offset.y};
}

// What a particle starts in the next system of a chain in a step, as Godot's ParticleEmission:
// count particles where it is, turned and scaled as it is, and moving as it moves when kept.
struct Emitted {
    std::uint32_t count = 0;
    bool keep_velocity = false;
    Vec2 origin;
    Vec2 velocity;
    Vec4 basis;
};

// What a sub-emitter takes in a step from the particles the link before started: count of them,
// the first of each particle's at its offset, numbered on from base, where its emission cycle's
// count stood. An even cycle takes its slots from the first and an odd one from the last.
struct Intake {
    std::span<const Emitted> emitted;
    std::span<const std::uint32_t> offsets;
    std::uint32_t count = 0;
    std::uint32_t base = 0;
    bool odd = false;

    // The emission a slot of a system of total takes, if any: the one numbered as the slot is
    // in the cycle's order, which is new particles' turn. A slot whose particle still lives
    // takes it all the same, so a particle depends only on the steps since it started.
    [[nodiscard]] const Emitted* take(std::uint32_t index, std::uint32_t total) const {
        const std::uint32_t turn = odd ? total - 1U - index : index;
        const std::uint32_t rank = (turn + total - base) % total;
        if (rank >= count) {
            return nullptr;
        }
        // The last particle whose first emission is at or before it.
        const auto found = std::upper_bound(offsets.begin(), offsets.end(), rank);
        return &emitted[static_cast<std::size_t>(found - offsets.begin()) - 1];
    }
};

// The particles a sub-emitter takes in a step, as particles_emit.hlsl works them out: each
// particle's first emission's place, adding at most the system's size each and stopping there,
// and the count its first particle keeps of those its emission cycle took, which starts over
// with each cycle.
Intake take_emissions(std::span<const Emitted> emitted, std::vector<std::uint32_t>& offsets,
                      std::span<Particle> particles, const ParticleStep& step) {
    const auto size = static_cast<std::uint32_t>(particles.size());
    offsets.resize(emitted.size());
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < emitted.size(); ++i) {
        offsets[i] = sum;
        sum = std::min(sum + std::min(emitted[i].count, size), size);
    }
    Particle& first = particles.front();
    const std::uint32_t tag = step.cycle + 1U;
    const std::uint32_t base = step.clear || first.spare[1] != tag ? 0U : first.spare[0];
    first.spare[0] = (base + sum) % size;
    first.spare[1] = tag;
    return {.emitted = emitted,
            .offsets = offsets,
            .count = sum,
            .base = base,
            .odd = (step.cycle % 2U) != 0U};
}

// The generated start(): a particle's place, velocity, color and life at its restart; for a
// sub-emitter's, where the particle that started it is, as Godot's does without RESTART_POSITION
// and RESTART_ROT_SCALE, and moving as it moves without RESTART_VELOCITY.
void start(Particle& particle, const ParticleProcess& p, const ParticleStep& step,
           std::uint32_t number, const Emitted* from, bool& active) {
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
    particle.userdata.w = 0.0F;
    particle.color = display.color;
    if (from != nullptr) {
        particle.origin = from->origin;
        particle.basis = from->basis;
    } else {
        particle.origin = initial_position(p, display, seed);
        if (p.turbulence_enabled) {
            const float displacement = random_param(p, ParticleParam::TurbulenceDisplacement, seed);
            const Vec3 direction = noise_direction(p, step, particle.origin);
            particle.origin = particle.origin + (Vec2{direction.x, direction.y} * displacement);
        }
        // TRANSFORM = EMISSION_TRANSFORM * TRANSFORM, from an identity basis.
        particle.basis = {step.emission[0], step.emission[1], step.emission[2], step.emission[3]};
        particle.origin = transform_point(step.emission, particle.origin);
    }
    Vec2 velocity;
    if (from != nullptr && from->keep_velocity) {
        velocity = from->velocity;
    } else {
        velocity = random_direction(p, seed, p.spread) * dynamics.initial_velocity;
        if (p.emission_shape == EmissionShape::DirectedPoints && from_points(p)) {
            const std::size_t point = emission_point(p, display);
            // As Godot's black default texture, a point with no normal doesn't move.
            const Vec2 normal =
                point < p.emission_normals.size() ? p.emission_normals[point] : Vec2{};
            velocity = {(velocity.x * normal.x) + (velocity.y * normal.y),
                        (velocity.x * normal.y) - (velocity.y * normal.x)};
        }
    }
    velocity = transform_vector(step.emission, velocity);
    velocity = velocity + (step.emitter_velocity * p.inherit_velocity_ratio);
    particle.userdata.x = velocity.x;
    particle.userdata.y = velocity.y;
    particle.userdata.z = 0.0F;
}

// process_orbit_displacement for 2D.
Vec2 orbit_displacement(const ParticleProcess& p, const Dynamics& dynamics, float life,
                        const Particle& particle, const ParticleStep& step, float delta) {
    if (std::abs(dynamics.orbit_velocity) < 0.01F || delta < 0.001F) {
        return {};
    }
    const float amount =
        dynamics.orbit_velocity * sample_curve(curve(p, ParticleParam::OrbitVelocity), life, 1.0F);
    Vec2 displacement;
    if (amount != 0.0F) {
        const Vec2 diff = particle.origin - Vec2{step.emission[4], step.emission[5]};
        const Vec2 turn = cos_sin(amount * kPi * 2.0F * delta);
        const Vec2 turned{(turn.x * diff.x) + (turn.y * diff.y),
                          (-turn.y * diff.x) + (turn.x * diff.y)};
        displacement = turned - diff;
    }
    return transform_vector(step.emission, displacement / delta);
}

// process_radial_displacement.
Vec2 radial_displacement(const ParticleProcess& p, const Dynamics& dynamics, float life,
                         std::uint32_t& seed, const Particle& particle, const ParticleStep& step,
                         float delta) {
    if (delta < 0.001F) {
        return {};
    }
    const float multiplier = sample_curve(curve(p, ParticleParam::RadialVelocity), life, 1.0F);
    const Vec2 pivot = transform_point(step.emission, p.velocity_pivot);
    const Vec2 away = particle.origin - pivot;
    Vec2 out;
    if (length(away) > 0.01F) {
        out = normalize(away) * multiplier * dynamics.radial_velocity;
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

// The seconds between a Constant sub-emitter's emissions, as Godot passes its shader.
float sub_emitter_interval(const ParticleProcess& p) {
    return static_cast<float>(1.0 / static_cast<double>(p.sub_emitter_frequency));
}

// How many particles a particle starts in the next system of a chain in a step, as the code
// Godot generates for each sub_emitter_mode, with its age (CUSTOM.y) before the step and after.
std::uint32_t emission_count(const ParticleProcess& p, const ParticleStep& step, float delta,
                             float before, const Particle& particle, bool collided) {
    switch (p.sub_emitter_mode) {
    case SubEmitterMode::Disabled:
        break;
    case SubEmitterMode::Constant: {
        // Godot's CUSTOM.y * LIFETIME - DELTA, its age before the step in seconds, taken from
        // the age kept when nothing ends the step early: the same in exact arithmetic, and
        // exactly 0 in a particle's first step, where Godot's rounds either way.
        const float interval = sub_emitter_interval(p);
        const float from = step.interp_to_end == 0.0F ? before * step.lifetime
                                                      : (particle.custom.y * step.lifetime) - delta;
        const float rem = interval - mod(from, interval);
        return delta >= rem ? 1U : 0U;
    }
    case SubEmitterMode::AtEnd:
        return (particle.custom.y / particle.custom.w * step.lifetime) > step.lifetime - delta
                   ? p.sub_emitter_amount_at_end
                   : 0U;
    case SubEmitterMode::AtCollision:
        return collided ? p.sub_emitter_amount_at_collision : 0U;
    case SubEmitterMode::AtStart:
        return before == 0.0F ? p.sub_emitter_amount_at_start : 0U;
    }
    return 0U;
}

// The generated process(): one step of a live particle's life, after it touched a shape or not,
// noting in out what it starts in the next system of a chain.
void process(Particle& particle, const ParticleProcess& p, const ParticleStep& step,
             std::uint32_t number, float delta, const ParticleContact& contact, Emitted* out,
             bool& active) {
    std::uint32_t seed = hash(number + 1U + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(p, seed, display, dynamics, physical);

    const float before = particle.custom.y;
    const bool first_frame = before == 0.0F;
    particle.custom.y += delta / step.lifetime;
    particle.custom.y = mix(particle.custom.y, 1.0F, step.interp_to_end);
    const float life = particle.custom.y / display.lifetime;
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }

    Vec2 controlled = orbit_displacement(p, dynamics, life, particle, step, delta);
    controlled = controlled + radial_displacement(p, dynamics, life, seed, particle, step, delta);

    physical.linear_accel *= sample_curve(curve(p, ParticleParam::LinearAccel), life, 1.0F);
    physical.radial_accel *= sample_curve(curve(p, ParticleParam::RadialAccel), life, 1.0F);
    physical.tangent_accel *= sample_curve(curve(p, ParticleParam::TangentialAccel), life, 1.0F);
    physical.damping *= sample_curve(curve(p, ParticleParam::Damping), life, 1.0F);

    Vec3 built{particle.userdata.x, particle.userdata.y, particle.userdata.z};
    Vec2 force = p.gravity;
    if (length(built) > 0.0F) {
        const Vec3 along = normalize(built);
        force = force + (Vec2{along.x, along.y} * physical.linear_accel);
    }
    const Vec2 diff = particle.origin - Vec2{step.emission[4], step.emission[5]};
    if (length(diff) > 0.0F) {
        force = force + (normalize(diff) * physical.radial_accel);
    }
    if (length(diff) > 0.0F) {
        force = force + (normalize(Vec2{-diff.y, diff.x}) * physical.tangent_accel);
    }
    built = built + Vec3{force.x * delta, force.y * delta, 0.0F};
    if (physical.damping > 0.0F) {
        float v = length(built);
        if (!p.damping_as_friction) {
            v -= physical.damping * delta;
        } else if (v > 0.001F) {
            // Godot's friction, for a particle of 0.05 kg.
            v -= v * v * physical.damping * 0.05F * delta;
        }
        built = v < 0.0F ? Vec3{} : normalize(built) * v;
    }

    // A rigid particle is pushed out of the shape, loses its speed into it and bounces back.
    const bool collided = contact.collided && p.collision_mode != ParticleCollision::Disabled;
    if (collided && p.collision_mode == ParticleCollision::Rigid) {
        const Vec3 normal{contact.normal.x, contact.normal.y, 0.0F};
        const float response = dot(normal, built);
        const float bounces =
            std::abs(response) >= 2.0F / std::clamp(p.collision_bounce + 1.0F, 1.0F, 2.0F) ? 1.0F
                                                                                           : 0.0F;
        particle.origin = particle.origin + (contact.normal * contact.depth);
        built = built - (normal * response);
        built = mix(built, Vec3{}, std::clamp(p.collision_friction, 0.0F, 1.0F));
        built = built - (normal * (response * (p.collision_bounce * bounces)));
    } else if (collided && p.collision_mode == ParticleCollision::HideOnContact) {
        active = false;
    }

    // Turbulence before limiting, but not for a particle bouncing off a shape.
    Vec3 controlled3{controlled.x, controlled.y, 0.0F};
    const bool bounced = collided && p.collision_mode == ParticleCollision::Rigid;
    if (p.turbulence_enabled && !bounced) {
        const float influence =
            sample_curve(curve(p, ParticleParam::TurbulenceInfluence), life, 1.0F);
        const Vec3 direction = normalize(noise_direction(p, step, particle.origin));
        const float amount = std::clamp(dynamics.turbulence_influence * influence, 0.0F, 1.0F) *
                             (delta <= 0.0F ? 0.0F : 1.0F);
        const float boost = 1.0F + ((1.0F - amount) * 0.2F);
        built = mix(built, direction * (length(built) * boost), amount);
        controlled3 = mix(controlled3, direction * (length(controlled3) * boost), amount);
    }
    particle.userdata.x = built.x;
    particle.userdata.y = built.y;
    particle.userdata.z = built.z;

    Vec3 final_velocity = controlled3 + built;
    if (!p.velocity_limit_curve.empty() && length(final_velocity) > 0.001F) {
        const float limit = std::abs(sample_curve(p.velocity_limit_curve, life, 0.0F));
        final_velocity =
            normalize(final_velocity) * std::min(std::abs(length(final_velocity)), limit);
    }
    const Vec2 velocity{final_velocity.x, final_velocity.y};
    particle.velocity = velocity;
    particle.origin = particle.origin + (velocity * delta);

    display_at(p, display, life);

    float base_angle = dynamics.angle * sample_curve(curve(p, ParticleParam::Angle), life, 1.0F);
    if (!curve(p, ParticleParam::AngularVelocity).empty()) {
        particle.userdata.w += sample_curve(curve(p, ParticleParam::AngularVelocity), life, 1.0F) *
                               delta * dynamics.angular_velocity;
        particle.custom.x = (base_angle * kDegreesToRadians) + particle.userdata.w;
    } else {
        base_angle += particle.custom.y * step.lifetime * dynamics.angular_velocity;
        particle.custom.x = base_angle * kDegreesToRadians;
    }
    particle.color = display.color;

    Vec2 x_axis;
    Vec2 y_axis;
    if (p.align_y) {
        y_axis = length(velocity) > 0.0F ? normalize(velocity)
                                         : normalize(Vec2{particle.basis.z, particle.basis.w});
        x_axis = normalize(Vec2{y_axis.y, -y_axis.x});
    } else {
        const Vec2 turn = cos_sin(particle.custom.x);
        x_axis = {turn.x, -turn.y};
        y_axis = {turn.y, turn.x};
    }
    float scale = std::max(std::abs(display.scale), 0.001F);
    if (first_frame && display.scale < 0.0F) {
        scale = -scale;
    }
    particle.basis = {x_axis.x * scale, x_axis.y * scale, y_axis.x * scale, y_axis.y * scale};
    particle.custom.z = display.animation_offset + (life * display.animation_speed);
    if (out != nullptr) {
        // emit_subparticle(TRANSFORM, VELOCITY, ...) with Godot's emission flags.
        out->count = emission_count(p, step, delta, before, particle, collided);
        out->keep_velocity = p.sub_emitter_keep_velocity;
        out->origin = particle.origin;
        out->velocity = velocity;
        out->basis = particle.basis;
    }
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }
}

// How near a particle comes to a shape before it touches: collision_size, times the average of
// its axes' lengths with collision_use_scale, as Godot's. Its z axis is 1 when it has just started
// and as long as the others after.
float collision_size(const Particle& particle, const ParticleProcess& p, const ParticleStep& step,
                     bool started) {
    if (!p.collision_use_scale) {
        return step.collision_size;
    }
    const float x = length(Vec2{particle.basis.x, particle.basis.y});
    const float y = length(Vec2{particle.basis.z, particle.basis.w});
    const float z = started ? 1.0F : (x + y) * 0.5F;
    return step.collision_size * ((x + y + z) * 0.33333333333F);
}

// Godot's 2D collision: the particle and its size taken to the shapes, and the way out and the
// depth brought back.
ParticleContact shape_contact(Vec2 origin, float size, const ParticleStep& step,
                              std::span<const ParticleShape> shapes) {
    const std::array<float, 6>& to = step.to_shapes;
    const Vec2 at{(to[0] * origin.x) + (to[2] * origin.y) + to[4],
                  (to[1] * origin.x) + (to[3] * origin.y) + to[5]};
    ParticleContact contact = particle_contact(at, size * length(Vec2{to[2], to[3]}), shapes);
    if (contact.collided) {
        const std::array<float, 4>& from = step.from_shapes;
        const Vec2 out = contact.normal * contact.depth;
        const Vec2 back{(from[0] * out.x) + (from[2] * out.y),
                        (from[1] * out.x) + (from[3] * out.y)};
        contact.depth = length(back);
        contact.normal = contact.depth > 0.0F ? back / contact.depth : Vec2{0.0F, -1.0F};
    }
    return contact;
}

// One step of a particle: a sub-emitter's takes what intake gives its slot, others restart in
// their turn while emitting; out notes what it starts in the next system of a chain.
void step_particle(Particle& particle, std::uint32_t index, std::uint32_t total,
                   const ParticleProcess& p, const ParticleStep& step,
                   std::span<const ParticleShape> shapes, const Intake* intake, Emitted* out) {
    if (step.clear) {
        // The count a sub-emitter keeps in its spare words stays: it was set for this step.
        const std::array<std::uint32_t, 3> spare = particle.spare;
        particle = Particle{};
        particle.spare = spare;
    }
    if (out != nullptr) {
        out->count = 0;
    }
    particle.flags &= ~kParticleStarted;
    float delta = step.delta;
    bool restart = false;
    const Emitted* from = nullptr;
    if (intake != nullptr) {
        // Godot's sub_emitter_mode, which takes the place of emitting; no fractional delta.
        from = intake->take(index, total);
        if (from != nullptr) {
            restart = true;
            particle.flags =
                kParticleActive | kParticleStarted | (step.cycle << kParticleCycleShift);
        }
    } else if (step.emitting) {
        float restart_phase = static_cast<float>(index) / static_cast<float>(total);
        if (step.randomness > 0.0F) {
            std::uint32_t seed = step.cycle;
            if (restart_phase >= step.system_phase) {
                seed -= 1U;
            }
            seed *= total;
            seed += index;
            const float random = static_cast<float>(hash(seed) % 65536U) / 65536.0F;
            restart_phase += step.randomness * random * 1.0F / static_cast<float>(total);
        }
        restart_phase *= 1.0F - step.explosiveness;
        if (step.system_phase > step.prev_system_phase) {
            // >= the previous phase, so particles emit in the first step that runs them.
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
        start(particle, p, step, number, from, active);
    }
    // Where it is against the step's shapes before it moves, as Godot's particles storage works
    // out against its signed distance field.
    ParticleContact contact;
    if (active && p.collision_mode != ParticleCollision::Disabled && step.shape_count > 0 &&
        static_cast<std::size_t>(step.shape_first) + step.shape_count <= shapes.size()) {
        contact = shape_contact(particle.origin, collision_size(particle, p, step, restart), step,
                                shapes.subspan(step.shape_first, step.shape_count));
    }
    if (active) {
        process(particle, p, step, number, delta, contact, out, active);
    }
    particle.flags &= ~kParticleActive;
    if (active) {
        particle.flags |= kParticleActive;
    }
}

std::uint8_t to_byte(float channel) {
    return static_cast<std::uint8_t>(std::floor((std::clamp(channel, 0.0F, 1.0F) * 255.0F) + 0.5F));
}

} // namespace

float turbulence_frequency(float noise_scale) {
    // Godot's slider runs from 0 to 10: a frequency of -4 at 0, 0 at 10.
    constexpr float kAtZero = 4.0F;
    const float rescale = kAtZero / std::sqrt(std::sqrt(10.0F));
    return (std::sqrt(std::sqrt(std::max(noise_scale, 0.0F))) * rescale) - kAtZero;
}

float sample_curve(const ParticleCurve& curve, float offset, float fallback) {
    if (curve.empty()) {
        return fallback;
    }
    if (!(offset > curve.front().x)) {
        return curve.front().y;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (offset < curve[i].x) {
            const Vec2 a = curve[i - 1];
            const Vec2 b = curve[i];
            const float t = (offset - a.x) / (b.x - a.x);
            return a.y + ((b.y - a.y) * t);
        }
    }
    return curve.back().y;
}

Vec4 sample_gradient(const ParticleGradient& gradient, float offset) {
    if (gradient.empty()) {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    if (!(offset > gradient.front().offset)) {
        return gradient.front().color;
    }
    for (std::size_t i = 1; i < gradient.size(); ++i) {
        if (offset < gradient[i].offset) {
            const GradientStop& a = gradient[i - 1];
            const GradientStop& b = gradient[i];
            const float t = (offset - a.offset) / (b.offset - a.offset);
            const auto blend = [t](float x, float y) { return x + ((y - x) * t); };
            return {blend(a.color.x, b.color.x), blend(a.color.y, b.color.y),
                    blend(a.color.z, b.color.z), blend(a.color.w, b.color.w)};
        }
    }
    return gradient.back().color;
}

void process_particles(std::span<Particle> particles, const ParticleProcess& process,
                       std::span<const ParticleStep> steps, std::span<const ParticleShape> shapes) {
    const ParticleChainLink link{
        .particles = particles, .process = &process, .steps = steps, .shapes = shapes};
    process_particle_chain({&link, 1});
}

void process_particle_chain(std::span<const ParticleChainLink> links) {
    if (links.empty()) {
        return;
    }
    std::size_t steps = links.front().steps.size();
    for (const ParticleChainLink& link : links) {
        steps = std::min(steps, link.steps.size());
    }
    // What each link's particles start in the next, one record a particle, as the GPU keeps it
    // in the link's emission buffer.
    std::vector<std::vector<Emitted>> emitted(links.size() - 1);
    for (std::size_t k = 0; k + 1 < links.size(); ++k) {
        emitted[k].resize(links[k].particles.size());
    }
    const ParticleProcess still;
    std::vector<std::uint32_t> offsets;
    for (std::size_t i = 0; i < steps; ++i) {
        for (std::size_t k = 0; k < links.size(); ++k) {
            const ParticleChainLink& link = links[k];
            const ParticleStep& step = link.steps[i];
            const auto total = static_cast<std::uint32_t>(link.particles.size());
            std::optional<Intake> intake;
            if (k > 0 && total > 0) {
                intake = take_emissions(emitted[k - 1], offsets, link.particles, step);
            }
            Emitted* out = k + 1 < links.size() ? emitted[k].data() : nullptr;
            const ParticleProcess& process = link.process != nullptr ? *link.process : still;
            for (std::uint32_t index = 0; index < total; ++index) {
                step_particle(link.particles[index], index, total, process, step, link.shapes,
                              intake ? &*intake : nullptr, out != nullptr ? out + index : nullptr);
            }
        }
    }
}

ParticleContact particle_contact(Vec2 point, float size, std::span<const ParticleShape> shapes) {
    // The nearest point on any edge, and whether the point is inside a closed shape by the
    // even-odd rule. Godot samples a distance field drawn from the shapes; this is the distance
    // it approximates, worked out exactly so every device gets the same.
    float nearest_squared = std::numeric_limits<float>::max();
    Vec2 from_nearest; // the way from the nearest point on an edge to the point
    Vec2 along;
    bool inside = false;
    for (const ParticleShape& shape : shapes) {
        const std::size_t count = shape.points.size();
        const bool solid = shape.closed && count > 2;
        // A closed outline's last point joins its first; a line's doesn't.
        std::size_t edges = count;
        if (!solid) {
            edges = count > 0 ? count - 1 : 0;
        }
        bool odd = false;
        for (std::size_t i = 0; i < edges; ++i) {
            const Vec2 a = shape.points[i];
            const Vec2 b = shape.points[(i + 1) % count];
            const Vec2 ab = b - a;
            const Vec2 ap = point - a;
            const float length_squared = dot(ab, ab);
            const float t = length_squared > 0.0F ? dot(ap, ab) / length_squared : 0.0F;
            // Past an end, the way from it; beside the edge, straight out from it, worked out
            // from the cross product so a point a hair from a long edge still gets its normal.
            Vec2 away = ap;
            if (t >= 1.0F) {
                away = point - b;
            } else if (t > 0.0F) {
                const float cross = (ab.x * ap.y) - (ab.y * ap.x);
                away = Vec2{-ab.y, ab.x} * (cross / length_squared);
            }
            const float distance_squared = dot(away, away);
            if (distance_squared < nearest_squared) {
                nearest_squared = distance_squared;
                from_nearest = away;
                along = ab;
            }
            if (solid && ((a.y > point.y) != (b.y > point.y)) &&
                point.x < a.x + ((point.y - a.y) * (b.x - a.x) / (b.y - a.y))) {
                odd = !odd;
            }
        }
        inside = inside || odd;
    }
    ParticleContact contact;
    if (nearest_squared == std::numeric_limits<float>::max()) {
        return contact;
    }
    const float distance = std::sqrt(nearest_squared);
    const float signed_distance = inside ? -distance : distance;
    if (signed_distance - size >= 0.0F) {
        return contact;
    }
    contact.collided = true;
    contact.depth = size - signed_distance;
    if (distance > 0.0F) {
        contact.normal = (inside ? from_nearest * -1.0F : from_nearest) / distance;
    } else {
        // On an edge: out across it, to the left of the way it runs.
        contact.normal =
            length(along) > 0.0F ? normalize(Vec2{along.y, -along.x}) : Vec2{0.0F, -1.0F};
    }
    return contact;
}

std::uint32_t particle_lifetime_split(std::uint32_t amount, double phase) {
    if (amount == 0) {
        return 0;
    }
    const auto oldest = static_cast<std::int64_t>(static_cast<double>(amount) * phase);
    return static_cast<std::uint32_t>(
        (std::min(oldest, static_cast<std::int64_t>(amount) - 1) + 1) % amount);
}

void particle_vertices(std::span<const Particle> particles, const ParticleDraw& draw,
                       std::vector<Vertex2D>& out) {
    const auto amount = static_cast<std::uint32_t>(particles.size());
    out.reserve(out.size() + (std::size_t{amount} * 6));
    const auto h = static_cast<float>(std::max(draw.hframes, 1U));
    const auto v = static_cast<float>(std::max(draw.vframes, 1U));
    const float total = h * v;
    const Vec2 half = draw.size * 0.5F;
    for (std::uint32_t slot = 0; slot < amount; ++slot) {
        std::uint32_t index = slot;
        if (draw.order != ParticleOrder::Index) {
            const std::uint32_t limit = amount - std::min(draw.lifetime_split, amount);
            if (draw.order == ParticleOrder::ReverseLifetime) {
                index = amount - index - 1;
            }
            index = index < limit ? draw.lifetime_split + index : index - limit;
        }
        const Particle& particle = particles[index];
        if (!particle.active()) {
            out.insert(out.end(), 6,
                       Vertex2D{.x = 0.0F, .y = 0.0F, .u = 0.0F, .v = 0.0F, .rgba = {0, 0, 0, 0}});
            continue;
        }
        float frame = std::floor(particle.custom.z * total);
        frame = draw.anim_loop ? mod(frame, total) : std::clamp(frame, 0.0F, total - 1.0F);
        const float u0 = mod(frame, h) / h;
        const float v0 = std::floor((frame + 0.5F) / h) / v;
        const float u1 = u0 + (1.0F / h);
        const float v1 = v0 + (1.0F / v);
        const Vec4 color = times(particle.color, draw.modulate);
        const std::array<std::uint8_t, 4> rgba{to_byte(color.x), to_byte(color.y), to_byte(color.z),
                                               to_byte(color.w)};
        const Vec2 at = particle.origin + (particle.velocity * draw.remainder);
        const Vec2 x_axis{particle.basis.x, particle.basis.y};
        const Vec2 y_axis{particle.basis.z, particle.basis.w};
        const auto corner = [&](float sx, float sy, float u, float tv) {
            const Vec2 point = transform_point(draw.transform, at + (x_axis * (sx * half.x)) +
                                                                   (y_axis * (sy * half.y)));
            return Vertex2D{.x = point.x, .y = point.y, .u = u, .v = tv, .rgba = rgba};
        };
        const Vertex2D top_left = corner(-1.0F, -1.0F, u0, v0);
        const Vertex2D top_right = corner(1.0F, -1.0F, u1, v0);
        const Vertex2D bottom_left = corner(-1.0F, 1.0F, u0, v1);
        const Vertex2D bottom_right = corner(1.0F, 1.0F, u1, v1);
        out.insert(out.end(),
                   {top_left, top_right, bottom_left, top_right, bottom_right, bottom_left});
    }
}

} // namespace slash::render
