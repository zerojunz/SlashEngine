#include "slash/render/particles.hpp"

#include "slash/render/device.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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

float sign(float v) {
    if (v > 0.0F) {
        return 1.0F;
    }
    return v < 0.0F ? -1.0F : 0.0F;
}

Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
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

// The generated start(): a particle's place, velocity, color and life at its restart.
void start(Particle& particle, const ParticleProcess& p, const ParticleStep& step,
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
    particle.userdata.w = 0.0F;
    particle.color = display.color;
    particle.origin = initial_position(p, display, seed);
    // TRANSFORM = EMISSION_TRANSFORM * TRANSFORM, from an identity basis.
    particle.basis = {step.emission[0], step.emission[1], step.emission[2], step.emission[3]};
    particle.origin = transform_point(step.emission, particle.origin);
    Vec2 velocity = random_direction(p, seed, p.spread) * dynamics.initial_velocity;
    if (p.emission_shape == EmissionShape::DirectedPoints && from_points(p)) {
        const std::size_t point = emission_point(p, display);
        // As Godot's black default texture, a point with no normal doesn't move.
        const Vec2 normal = point < p.emission_normals.size() ? p.emission_normals[point] : Vec2{};
        velocity = {(velocity.x * normal.x) + (velocity.y * normal.y),
                    (velocity.x * normal.y) - (velocity.y * normal.x)};
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

// The generated process(): one step of a live particle's life.
void process(Particle& particle, const ParticleProcess& p, const ParticleStep& step,
             std::uint32_t number, float delta, bool& active) {
    std::uint32_t seed = hash(number + 1U + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(p, seed, display, dynamics, physical);

    const bool first_frame = particle.custom.y == 0.0F;
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

    Vec2 built{particle.userdata.x, particle.userdata.y};
    Vec2 force = p.gravity;
    if (length(built) > 0.0F) {
        force = force + (normalize(built) * physical.linear_accel);
    }
    const Vec2 diff = particle.origin - Vec2{step.emission[4], step.emission[5]};
    if (length(diff) > 0.0F) {
        force = force + (normalize(diff) * physical.radial_accel);
    }
    if (length(diff) > 0.0F) {
        force = force + (normalize(Vec2{-diff.y, diff.x}) * physical.tangent_accel);
    }
    built = built + (force * delta);
    if (physical.damping > 0.0F) {
        float v = length(built);
        if (!p.damping_as_friction) {
            v -= physical.damping * delta;
        } else if (v > 0.001F) {
            // Godot's friction, for a particle of 0.05 kg.
            v -= v * v * physical.damping * 0.05F * delta;
        }
        built = v < 0.0F ? Vec2{} : normalize(built) * v;
    }
    particle.userdata.x = built.x;
    particle.userdata.y = built.y;

    Vec2 velocity = controlled + built;
    if (!p.velocity_limit_curve.empty() && length(velocity) > 0.001F) {
        const float limit = std::abs(sample_curve(p.velocity_limit_curve, life, 0.0F));
        velocity = normalize(velocity) * std::min(std::abs(length(velocity)), limit);
    }
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
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }
}

void step_particle(Particle& particle, std::uint32_t index, std::uint32_t total,
                   const ParticleProcess& p, const ParticleStep& step) {
    if (step.clear) {
        particle = Particle{};
    }
    particle.flags &= ~kParticleStarted;
    float delta = step.delta;
    bool restart = false;
    if (step.emitting) {
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

std::uint8_t to_byte(float channel) {
    return static_cast<std::uint8_t>(std::floor((std::clamp(channel, 0.0F, 1.0F) * 255.0F) + 0.5F));
}

// GLSL's mod: x - y * floor(x / y).
float mod(float x, float y) {
    return x - (y * std::floor(x / y));
}

} // namespace

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
                       std::span<const ParticleStep> steps) {
    const auto total = static_cast<std::uint32_t>(particles.size());
    for (const ParticleStep& step : steps) {
        for (std::uint32_t index = 0; index < total; ++index) {
            step_particle(particles[index], index, total, process, step);
        }
    }
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
