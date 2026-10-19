// Runs steps on a particle system, a thread a particle: Godot's particles shader and the code
// ParticleProcessMaterial generates for 2D. Its CPU twin is process_particles in
// src/particles.cpp, which this follows line for line; a change to one goes in the other. The
// process material and steps are words laid out as src/particle_layout.hpp says.

#include "particles.hlsl"

cbuffer Params : UNIFORMS {
    uint amount;
    uint material_offset; // in words
    uint steps_offset;
    uint step_count;
    uint reset; // clears every particle first
};

StructuredBuffer<uint4> data : READ_ONLY;
RWStructuredBuffer<Particle> particles : READ_WRITE;

static const float kPi = 3.14159;
static const float kDegreesToRadians = kPi / 180.0;

// Where the process material's parts are, in words (src/particle_layout.hpp).
static const uint kCurveWords = 16u;
static const uint kCurvesAt = 20u;
static const uint kGradientsAt = 276u;
static const uint kGradientWords = 40u;
static const uint kPointsAt = 356u;
// Curves after the parameters'.
static const uint kVelocityLimitCurve = 13u;
static const uint kAlphaCurve = 14u;
static const uint kEmissionCurve = 15u;

// EmissionShape and ParticleParam.
static const uint kShapePoint = 0u;
static const uint kShapeSphere = 1u;
static const uint kShapeSphereSurface = 2u;
static const uint kShapeBox = 3u;
static const uint kShapePoints = 4u;
static const uint kShapeDirectedPoints = 5u;
static const uint kShapeRing = 6u;
static const uint kInitialVelocity = 0u;
static const uint kAngularVelocity = 1u;
static const uint kOrbitVelocity = 2u;
static const uint kLinearAccel = 3u;
static const uint kRadialAccel = 4u;
static const uint kTangentialAccel = 5u;
static const uint kDamping = 6u;
static const uint kAngle = 7u;
static const uint kScale = 8u;
static const uint kHueVariation = 9u;
static const uint kAnimSpeed = 10u;
static const uint kAnimOffset = 11u;
static const uint kRadialVelocity = 12u;

struct Step {
    float4 emission_a; // emission[0..3]
    float2 emission_b; // emission[4..5]
    float2 emitter_velocity;
    float system_phase;
    float prev_system_phase;
    float delta;
    float lifetime;
    float explosiveness;
    float randomness;
    float amount_ratio;
    float interp_to_end;
    uint cycle;
    uint seed;
    bool emitting;
    bool fractional_delta;
    bool clear;
};

struct Display {
    float scale;
    float hue_rotation;
    float animation_speed;
    float animation_offset;
    float lifetime;
    float4 color;
    float emission_position;
};

struct Dynamics {
    float angle;
    float angular_velocity;
    float initial_velocity;
    float radial_velocity;
    float orbit_velocity;
};

struct Physical {
    float linear_accel;
    float radial_accel;
    float tangent_accel;
    float damping;
};

float4 word(uint at) {
    return asfloat(data[material_offset + at]);
}

uint word_uint(uint at, uint lane) {
    return data[material_offset + at][lane];
}

uint shape() {
    return word_uint(0u, 0u);
}

uint point_count() {
    return word_uint(2u, 2u);
}

float mix(float a, float b, float t) {
    return (a * (1.0 - t)) + (b * t);
}

// v made a unit vector, or zero for zero.
float2 safe_normalize(float2 v) {
    float l = sqrt(dot(v, v));
    return l > 0.0 ? v / l : float2(0.0, 0.0);
}

float2 transform_point(Step step, float2 p) {
    return float2((step.emission_a.x * p.x) + (step.emission_a.z * p.y) + step.emission_b.x,
                  (step.emission_a.y * p.x) + (step.emission_a.w * p.y) + step.emission_b.y);
}

float2 transform_vector(Step step, float2 v) {
    return float2((step.emission_a.x * v.x) + (step.emission_a.z * v.y),
                  (step.emission_a.y * v.x) + (step.emission_a.w * v.y));
}

uint hash(uint x) {
    x = ((x >> 16u) ^ x) * 73244475u;
    x = ((x >> 16u) ^ x) * 73244475u;
    x = (x >> 16u) ^ x;
    return x;
}

float rand_from_seed(inout uint seed) {
    int s = int(seed);
    if (s == 0) {
        s = 305420679;
    }
    int k = s / 127773;
    s = (16807 * (s - (k * 127773))) - (2836 * k);
    if (s < 0) {
        s += 2147483647;
    }
    seed = uint(s);
    return float(seed % 65536u) / 65535.0;
}

float rand_m1_p1(inout uint seed) {
    return (rand_from_seed(seed) * 2.0) - 1.0;
}

float4 rotate_hue(float4 color, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    float3 column0 = float3(0.299, 0.587, 0.114) + (float3(0.701, -0.587, -0.114) * c) +
                     (float3(0.168, 0.330, -0.497) * s);
    float3 column1 = float3(0.299, 0.587, 0.114) + (float3(-0.299, 0.413, -0.114) * c) +
                     (float3(-0.328, 0.035, 0.292) * s);
    float3 column2 = float3(0.299, 0.587, 0.114) + (float3(-0.300, -0.588, 0.886) * c) +
                     (float3(1.250, -1.050, -0.203) * s);
    float3 rgb = (column0 * color.r) + (column1 * color.g) + (column2 * color.b);
    return float4(rgb, color.a);
}

float param_min(uint param) {
    return word(7u + (param / 4u))[param % 4u];
}

float param_max(uint param) {
    return word(11u + (param / 4u))[param % 4u];
}

float random_param(uint param, inout uint seed) {
    return mix(param_min(param), param_max(param), rand_from_seed(seed));
}

uint curve_count(uint curve) {
    return word_uint(15u + (curve / 4u), curve % 4u);
}

float2 curve_point(uint curve, uint i) {
    float4 pair = word(kCurvesAt + (curve * kCurveWords) + (i / 2u));
    return (i % 2u) == 0u ? pair.xy : pair.zw;
}

float sample_curve(uint curve, float offset, float fallback) {
    uint count = curve_count(curve);
    if (count == 0u) {
        return fallback;
    }
    float2 previous = curve_point(curve, 0u);
    if (!(offset > previous.x)) {
        return previous.y;
    }
    for (uint i = 1u; i < count; ++i) {
        float2 next = curve_point(curve, i);
        if (offset < next.x) {
            float t = (offset - previous.x) / (next.x - previous.x);
            return previous.y + ((next.y - previous.y) * t);
        }
        previous = next;
    }
    return previous.y;
}

uint gradient_count(uint gradient) {
    return word_uint(19u, gradient);
}

float gradient_offset(uint gradient, uint i) {
    return word(kGradientsAt + (gradient * kGradientWords) + (i / 4u))[i % 4u];
}

float4 gradient_color(uint gradient, uint i) {
    return word(kGradientsAt + (gradient * kGradientWords) + 8u + i);
}

float4 sample_gradient(uint gradient, float offset) {
    uint count = gradient_count(gradient);
    if (count == 0u) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    float first = gradient_offset(gradient, 0u);
    if (!(offset > first)) {
        return gradient_color(gradient, 0u);
    }
    for (uint i = 1u; i < count; ++i) {
        float b = gradient_offset(gradient, i);
        if (offset < b) {
            float a = gradient_offset(gradient, i - 1u);
            float t = (offset - a) / (b - a);
            float4 x = gradient_color(gradient, i - 1u);
            float4 y = gradient_color(gradient, i);
            return x + ((y - x) * t);
        }
    }
    return gradient_color(gradient, count - 1u);
}

bool from_points() {
    return (shape() == kShapePoints || shape() == kShapeDirectedPoints) && point_count() > 0u;
}

uint emission_point(Display display) {
    int count = int(point_count());
    return uint(max(min(count - 1, int(display.emission_position * float(count))), 0));
}

void initial_params(inout uint seed, out Display display, out Dynamics dynamics,
                    out Physical physical) {
    display.scale = random_param(kScale, seed);
    display.scale = sign(display.scale) * max(abs(display.scale), 0.001);
    display.hue_rotation = kPi * 2.0 * random_param(kHueVariation, seed);
    display.animation_speed = random_param(kAnimSpeed, seed);
    display.animation_offset = random_param(kAnimOffset, seed);
    display.lifetime = 1.0 - (word(6u).x * rand_from_seed(seed));
    display.color = word(5u);
    display.emission_position = 0.0;
    if (gradient_count(1u) > 0u) {
        display.color = display.color * sample_gradient(1u, rand_from_seed(seed));
    }
    if (shape() == kShapePoints || shape() == kShapeDirectedPoints) {
        display.emission_position = rand_from_seed(seed);
        if (from_points()) {
            display.color = display.color * word(kPointsAt + (emission_point(display) * 2u) + 1u);
        }
    }

    dynamics.angle = random_param(kAngle, seed);
    dynamics.angular_velocity = random_param(kAngularVelocity, seed);
    dynamics.initial_velocity = random_param(kInitialVelocity, seed);
    rand_from_seed(seed); // directional velocity, which 2D doesn't use
    dynamics.radial_velocity = random_param(kRadialVelocity, seed);
    dynamics.orbit_velocity = random_param(kOrbitVelocity, seed);

    physical.linear_accel = random_param(kLinearAccel, seed);
    physical.radial_accel = random_param(kRadialAccel, seed);
    physical.tangent_accel = random_param(kTangentialAccel, seed);
    physical.damping = random_param(kDamping, seed);
}

void display_at(inout Display display, float life) {
    display.scale *= sample_curve(kScale, life, 1.0);
    display.hue_rotation *= sample_curve(kHueVariation, life, 1.0);
    display.animation_offset += sample_curve(kAnimOffset, life, 0.0);
    display.animation_speed *= sample_curve(kAnimSpeed, life, 1.0);
    if (gradient_count(0u) > 0u) {
        display.color = display.color * sample_gradient(0u, life);
    }
    display.color.a *= sample_curve(kAlphaCurve, life, 1.0);
    display.color = rotate_hue(display.color, display.hue_rotation);
    if (curve_count(kEmissionCurve) > 0u) {
        float bright = 1.0 + sample_curve(kEmissionCurve, life, 0.0);
        display.color = float4(display.color.rgb * bright, display.color.a);
    }
}

float2 random_direction(inout uint seed, float spread) {
    float4 direction = word(3u);
    float angle = rand_m1_p1(seed) * (spread * kDegreesToRadians);
    angle += direction.x != 0.0 ? atan2(direction.y, direction.x) : sign(direction.y) * (kPi / 2.0);
    return float2(cos(angle), sin(angle));
}

float2 initial_position(Display display, inout uint seed) {
    float2 pos = float2(0.0, 0.0);
    uint kind = shape();
    if (kind == kShapeSphere || kind == kShapeSphereSurface) {
        float s = (rand_from_seed(seed) * 2.0) - 1.0;
        float t = rand_from_seed(seed) * 2.0 * kPi;
        float radius = word(0u).y * sqrt(1.0 - (s * s));
        pos = float2(radius * cos(t), radius * sin(t));
        if (kind == kShapeSphere) {
            float along = rand_from_seed(seed);
            pos = float2(mix(0.0, pos.x, along), mix(0.0, pos.y, along));
        }
    } else if (kind == kShapeBox) {
        float x = (rand_from_seed(seed) * 2.0) - 1.0;
        float y = (rand_from_seed(seed) * 2.0) - 1.0;
        rand_from_seed(seed); // z
        float4 box = word(2u);
        pos = float2(x * box.x, y * box.y);
    } else if (kind == kShapePoints || kind == kShapeDirectedPoints) {
        if (from_points()) {
            pos = word(kPointsAt + (emission_point(display) * 2u)).xy;
        }
    } else if (kind == kShapeRing) {
        float radius = max(0.001, word(0u).z);
        float inner = word(0u).w;
        rand_from_seed(seed); // the height on the ring, which 2D drops
        float angle = rand_from_seed(seed) * 2.0 * kPi;
        float r = sqrt((rand_from_seed(seed) * ((radius * radius) - (inner * inner))) +
                       (inner * inner));
        pos = float2(sin(angle) * r, cos(angle) * r);
    }
    float4 offset_scale = word(1u);
    return float2((pos.x * offset_scale.z) + offset_scale.x,
                  (pos.y * offset_scale.w) + offset_scale.y);
}

void start(inout Particle particle, Step step, uint number, inout bool active) {
    uint seed = hash(number + 1u + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(seed, display, dynamics, physical);
    display_at(display, 0.0);
    if (rand_from_seed(seed) > step.amount_ratio) {
        active = false;
    }
    particle.custom = float4(dynamics.angle, 0.0, 0.0, display.lifetime);
    particle.userdata.w = 0.0;
    particle.color = display.color;
    float2 origin = initial_position(display, seed);
    particle.basis = step.emission_a;
    origin = transform_point(step, origin);
    float2 velocity = random_direction(seed, word(3u).z) * dynamics.initial_velocity;
    if (shape() == kShapeDirectedPoints && from_points()) {
        float2 normal = word(kPointsAt + (emission_point(display) * 2u)).zw;
        velocity = float2((velocity.x * normal.x) + (velocity.y * normal.y),
                          (velocity.x * normal.y) - (velocity.y * normal.x));
    }
    velocity = transform_vector(step, velocity);
    velocity = velocity + (step.emitter_velocity * word(3u).w);
    particle.origin_velocity.xy = origin;
    particle.userdata.xyz = float3(velocity, 0.0);
}

float2 orbit_displacement(Dynamics dynamics, float life, Particle particle, Step step,
                          float delta) {
    if (abs(dynamics.orbit_velocity) < 0.01 || delta < 0.001) {
        return float2(0.0, 0.0);
    }
    float amount = dynamics.orbit_velocity * sample_curve(kOrbitVelocity, life, 1.0);
    float2 displacement = float2(0.0, 0.0);
    if (amount != 0.0) {
        float2 diff = particle.origin_velocity.xy - step.emission_b;
        float turn = amount * kPi * 2.0 * delta;
        float c = cos(turn);
        float s = sin(turn);
        float2 turned = float2((c * diff.x) + (s * diff.y), (-s * diff.x) + (c * diff.y));
        displacement = turned - diff;
    }
    return transform_vector(step, displacement / delta);
}

float2 radial_displacement(Dynamics dynamics, float life, inout uint seed, Particle particle,
                           Step step, float delta) {
    if (delta < 0.001) {
        return float2(0.0, 0.0);
    }
    float multiplier = sample_curve(kRadialVelocity, life, 1.0);
    float4 pivot_gravity = word(4u);
    float2 pivot = transform_point(step, pivot_gravity.xy);
    float2 away = particle.origin_velocity.xy - pivot;
    float2 result;
    if (length(away) > 0.01) {
        result = safe_normalize(away) * multiplier * dynamics.radial_velocity;
    } else {
        result = random_direction(seed, 360.0) * dynamics.radial_velocity;
    }
    if (multiplier * dynamics.radial_velocity < 0.0) {
        result = safe_normalize(result) *
                 min(abs(multiplier * dynamics.radial_velocity), length(away) / delta);
    }
    return result;
}

void process(inout Particle particle, Step step, uint number, float delta, inout bool active) {
    uint seed = hash(number + 1u + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(seed, display, dynamics, physical);

    bool first_frame = particle.custom.y == 0.0;
    particle.custom.y += delta / step.lifetime;
    particle.custom.y = mix(particle.custom.y, 1.0, step.interp_to_end);
    float life = particle.custom.y / display.lifetime;
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }

    float2 controlled = orbit_displacement(dynamics, life, particle, step, delta);
    controlled = controlled + radial_displacement(dynamics, life, seed, particle, step, delta);

    physical.linear_accel *= sample_curve(kLinearAccel, life, 1.0);
    physical.radial_accel *= sample_curve(kRadialAccel, life, 1.0);
    physical.tangent_accel *= sample_curve(kTangentialAccel, life, 1.0);
    physical.damping *= sample_curve(kDamping, life, 1.0);

    float2 built = particle.userdata.xy;
    float2 force = word(4u).zw;
    if (length(built) > 0.0) {
        force = force + (safe_normalize(built) * physical.linear_accel);
    }
    float2 diff = particle.origin_velocity.xy - step.emission_b;
    if (length(diff) > 0.0) {
        force = force + (safe_normalize(diff) * physical.radial_accel);
    }
    if (length(diff) > 0.0) {
        force = force + (safe_normalize(float2(-diff.y, diff.x)) * physical.tangent_accel);
    }
    built = built + (force * delta);
    if (physical.damping > 0.0) {
        float v = length(built);
        if (word_uint(6u, 2u) == 0u) {
            v -= physical.damping * delta;
        } else if (v > 0.001) {
            v -= v * v * physical.damping * 0.05 * delta;
        }
        built = v < 0.0 ? float2(0.0, 0.0) : safe_normalize(built) * v;
    }
    particle.userdata.xy = built;

    float2 velocity = controlled + built;
    if (curve_count(kVelocityLimitCurve) > 0u && length(velocity) > 0.001) {
        float limit = abs(sample_curve(kVelocityLimitCurve, life, 0.0));
        velocity = safe_normalize(velocity) * min(abs(length(velocity)), limit);
    }
    particle.origin_velocity.zw = velocity;
    particle.origin_velocity.xy = particle.origin_velocity.xy + (velocity * delta);

    display_at(display, life);

    float base_angle = dynamics.angle * sample_curve(kAngle, life, 1.0);
    if (curve_count(kAngularVelocity) > 0u) {
        particle.userdata.w +=
            sample_curve(kAngularVelocity, life, 1.0) * delta * dynamics.angular_velocity;
        particle.custom.x = (base_angle * kDegreesToRadians) + particle.userdata.w;
    } else {
        base_angle += particle.custom.y * step.lifetime * dynamics.angular_velocity;
        particle.custom.x = base_angle * kDegreesToRadians;
    }
    particle.color = display.color;

    float2 x_axis;
    float2 y_axis;
    if (word_uint(6u, 1u) != 0u) {
        y_axis = length(velocity) > 0.0 ? safe_normalize(velocity)
                                        : safe_normalize(particle.basis.zw);
        x_axis = safe_normalize(float2(y_axis.y, -y_axis.x));
    } else {
        float c = cos(particle.custom.x);
        float s = sin(particle.custom.x);
        x_axis = float2(c, -s);
        y_axis = float2(s, c);
    }
    float scale = max(abs(display.scale), 0.001);
    if (first_frame && display.scale < 0.0) {
        scale = -scale;
    }
    particle.basis = float4(x_axis * scale, y_axis * scale);
    particle.custom.z = display.animation_offset + (life * display.animation_speed);
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }
}

Particle empty_particle() {
    Particle particle;
    particle.basis = float4(1.0, 0.0, 0.0, 1.0);
    particle.origin_velocity = float4(0.0, 0.0, 0.0, 0.0);
    particle.color = float4(1.0, 1.0, 1.0, 1.0);
    particle.custom = float4(0.0, 0.0, 0.0, 0.0);
    particle.userdata = float4(0.0, 0.0, 0.0, 0.0);
    particle.flags = uint4(0u, 0u, 0u, 0u);
    return particle;
}

void step_particle(inout Particle particle, uint index, uint total, Step step) {
    if (step.clear) {
        particle = empty_particle();
    }
    particle.flags.x &= ~kParticleStarted;
    float delta = step.delta;
    bool restart = false;
    if (step.emitting) {
        float restart_phase = float(index) / float(total);
        if (step.randomness > 0.0) {
            uint seed = step.cycle;
            if (restart_phase >= step.system_phase) {
                seed -= 1u;
            }
            seed *= total;
            seed += index;
            float random = float(hash(seed) % 65536u) / 65536.0;
            restart_phase += step.randomness * random * 1.0 / float(total);
        }
        restart_phase *= 1.0 - step.explosiveness;
        if (step.system_phase > step.prev_system_phase) {
            if (restart_phase >= step.prev_system_phase && restart_phase < step.system_phase) {
                restart = true;
                if (step.fractional_delta) {
                    delta = (step.system_phase - restart_phase) * step.lifetime;
                }
            }
        } else if (step.delta > 0.0) {
            if (restart_phase >= step.prev_system_phase) {
                restart = true;
                if (step.fractional_delta) {
                    delta = (1.0 - restart_phase + step.system_phase) * step.lifetime;
                }
            } else if (restart_phase < step.system_phase) {
                restart = true;
                if (step.fractional_delta) {
                    delta = (step.system_phase - restart_phase) * step.lifetime;
                }
            }
        }
        if (restart) {
            particle.flags.x =
                kParticleActive | kParticleStarted | (step.cycle << kParticleCycleShift);
        }
    }
    bool active = (particle.flags.x & kParticleActive) != 0u;
    uint number = ((particle.flags.x >> kParticleCycleShift) * total) + index;
    if (restart && active) {
        start(particle, step, number, active);
    }
    if (active) {
        process(particle, step, number, delta, active);
    }
    particle.flags.x &= ~kParticleActive;
    if (active) {
        particle.flags.x |= kParticleActive;
    }
}

Step read_step(uint at) {
    Step step;
    step.emission_a = asfloat(data[at]);
    float4 b = asfloat(data[at + 1u]);
    step.emission_b = b.xy;
    step.emitter_velocity = b.zw;
    float4 phases = asfloat(data[at + 2u]);
    step.system_phase = phases.x;
    step.prev_system_phase = phases.y;
    step.delta = phases.z;
    step.lifetime = phases.w;
    float4 mixes = asfloat(data[at + 3u]);
    step.explosiveness = mixes.x;
    step.randomness = mixes.y;
    step.amount_ratio = mixes.z;
    step.interp_to_end = mixes.w;
    uint4 counts = data[at + 4u];
    step.cycle = counts.x;
    step.seed = counts.y;
    step.emitting = (counts.z & 1u) != 0u;
    step.fractional_delta = (counts.z & 2u) != 0u;
    step.clear = (counts.z & 4u) != 0u;
    return step;
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= amount) {
        return;
    }
    Particle particle = particles[index];
    if (reset != 0u) {
        particle = empty_particle();
    }
    for (uint i = 0u; i < step_count; ++i) {
        step_particle(particle, index, amount, read_step(steps_offset + (i * 5u)));
    }
    particles[index] = particle;
}
