// Runs steps on a particle system, a thread a particle: Godot's particles shader and the code
// ParticleProcessMaterial generates for 2D. Its CPU twin is process_particle_chain in
// src/particles.cpp, which this follows line for line; a change to one goes in the other. The
// process material and steps are words laid out as src/particle_layout.hpp says.
//
// In a chain of sub-emitters it runs one step of one system: what the particles start in the
// next system goes to target, and a sub-emitter takes what the system before started from
// source, after particles_emit.hlsl has numbered it.
//
// A system with trails keeps its trails' copies after its particles; the thread that runs a
// particle runs its copies after it, each step, each running the step its place in the trail
// puts it behind. The steps start with history steps the particles ran already.
//
// A process material's particle graphs run as the material shader runs its programs
// (material_ops.hlsl): the start graph after start(), the process graph in process().

#include "particles.hlsl"
#include "material_ops.hlsl"

// Two buffers read and two written, so Metal's indexes follow on from the uniforms'.
#ifdef SLASH_MSL
#define READ_ONLY_DATA register(t1)
#define READ_ONLY_SOURCE register(t2)
#define READ_WRITE_PARTICLES register(u3)
#define READ_WRITE_TARGET register(u4)
#else
#define READ_ONLY_DATA register(t0, space0)
#define READ_ONLY_SOURCE register(t1, space0)
#define READ_WRITE_PARTICLES register(u0, space1)
#define READ_WRITE_TARGET register(u1, space1)
#endif

cbuffer Params : UNIFORMS {
    uint amount;
    uint material_offset; // in words
    uint steps_offset;
    uint step_count;
    uint reset;   // clears every particle first
    uint chain;   // kChainTakes and kChainEmits
    uint trail;   // particles a trail
    uint history; // steps before the first to run
    float phase_step; // 1 / amount, divided on the CPU: Metal's fast division rounds otherwise
    uint pad0;
    uint pad1;
    uint pad2;
};

StructuredBuffer<uint4> data : READ_ONLY_DATA;
// The emissions of the system before, for a sub-emitter, and of this system, for the next.
StructuredBuffer<uint4> source : READ_ONLY_SOURCE;
RWStructuredBuffer<Particle> particles : READ_WRITE_PARTICLES;
RWStructuredBuffer<uint4> target : READ_WRITE_TARGET;

static const float kPi = 3.14159;
static const float kDegreesToRadians = kPi / 180.0;

// Where the process material's parts are, in words (src/particle_layout.hpp).
static const uint kCurveWords = 16u;
static const uint kCollisionAt = 23u;
static const uint kSubEmitterAt = 24u;
static const uint kCurvesAt = 25u;
static const uint kGradientsAt = 313u;
static const uint kGradientWords = 40u;
static const uint kPointsAt = 393u;
// The words before a process material's graphs' programs.
static const uint kGraphHeaderWords = 2u;
// A step's words.
static const uint kStepWords = 9u;
// Curves after the parameters'.
static const uint kVelocityLimitCurve = 15u;
static const uint kAlphaCurve = 16u;
static const uint kEmissionCurve = 17u;

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
static const uint kTurbulenceInfluence = 13u;
static const uint kTurbulenceDisplacement = 14u;
// ParticleCollision.
static const uint kCollisionRigid = 1u;
static const uint kCollisionHideOnContact = 2u;
// SubEmitterMode.
static const uint kSubEmitterConstant = 1u;
static const uint kSubEmitterAtEnd = 2u;
static const uint kSubEmitterAtCollision = 3u;
static const uint kSubEmitterAtStart = 4u;
// What a chain's step does: a sub-emitter takes from source, a system before another emits to
// target. An emission buffer is a header {base, count, odd, particles}, then three words a
// particle of the system that wrote it: {count, offset, keep_velocity, 0}, {origin, velocity}
// and its basis (src/particle_layout.hpp).
static const uint kChainTakes = 1u;
static const uint kChainEmits = 2u;
static const uint kEmissionWords = 3u;

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
    float time;
    uint shapes; // where its first shape's words are
    uint shape_count;
    float collision_size;
    float4 to_shapes_a; // to_shapes[0..3]
    float2 to_shapes_b; // to_shapes[4..5]
    float4 from_shapes;
    uint cycle;
    uint seed;
    uint trail_steps;
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
    float turbulence_influence;
};

struct Physical {
    float linear_accel;
    float radial_accel;
    float tangent_accel;
    float damping;
};

// ParticleContact.
struct Contact {
    bool collided;
    float2 normal;
    float depth;
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

// GLSL's mod: x - y * floor(x / y).
float mod(float x, float y) {
    return x - (y * floor(x / y));
}

float2 mix(float2 a, float2 b, float t) {
    return (a * (1.0 - t)) + (b * t);
}

float3 mix(float3 a, float3 b, float t) {
    return (a * (1.0 - t)) + (b * t);
}

float4 mix(float4 a, float4 b, float t) {
    return (a * (1.0 - t)) + (b * t);
}

// v made a unit vector, or zero for zero.
float2 safe_normalize(float2 v) {
    float l = sqrt(dot(v, v));
    return l > 0.0 ? v / l : float2(0.0, 0.0);
}

float3 safe_normalize(float3 v) {
    float l = sqrt(dot(v, v));
    return l > 0.0 ? v / l : float3(0.0, 0.0, 0.0);
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

// ---- Particle graphs.

// The parts of a particle a graph changes (ParticleProgram::outputs).
static const uint kOutPosition = 1u;
static const uint kOutVelocity = 2u;
static const uint kOutColor = 4u;
static const uint kOutAngle = 8u;
static const uint kOutScale = 16u;
static const uint kOutFrame = 32u;
static const uint kOutActive = 64u;

static float4 graph_registers[32];

// Where the graphs are, in words from the process material's start; 0 for none.
uint graphs_at() {
    return word_uint(2u, 3u);
}

// The start graph's outputs (lane 0) and the process graph's (lane 1).
uint graph_outputs(uint lane) {
    return graphs_at() == 0u ? 0u : word_uint(graphs_at() + 1u, lane);
}

// The four random numbers the CPU twin's graph_random makes.
float4 graph_random(uint number, uint seed) {
    uint base = hash(number + 1u + seed) ^ 0x68E31DA4u;
    return float4(float(hash(base) >> 8u), float(hash(base + 1u) >> 8u),
                  float(hash(base + 2u) >> 8u), float(hash(base + 3u) >> 8u)) *
           (1.0 / 16777216.0);
}

void fill_graph(Step step, uint index, uint number) {
    graph_registers[4] = float4(step.time, float(index), float(number), step.amount_ratio);
    graph_registers[5] = graph_random(number, step.seed);
    graph_registers[6] = float4(step.emission_b, step.emitter_velocity);
}

// An operand: a register, or a constant of the graph whose constants start at the word constants.
float4 graph_fetch(uint operand, uint constants) {
    uint index = operand & 0xFFu;
    float4 value;
    if (index < 32u) {
        value = graph_registers[index];
    } else {
        value = asfloat(data[constants + ((index - 32u) & 31u)]);
    }
    uint pick = operand >> 8u;
    return float4(value[pick & 3u], value[(pick >> 2u) & 3u], value[(pick >> 4u) & 3u],
                  value[(pick >> 6u) & 3u]);
}

// Runs the graph whose count instructions are at the word code, its constants after them, on
// registers 0 to 7, leaving what it changes in the three after.
void run_graph(uint code, uint count) {
    graph_registers[8] = graph_registers[0];
    graph_registers[9] = graph_registers[1];
    graph_registers[10] = graph_registers[2];
    uint constants = code + count;
    [loop] for (uint i = 0u; i < count; ++i) {
        uint4 instruction = data[code + i];
        float4 o = material_op(instruction.x & 0xFFu, (instruction.x >> 16u) & 0xFFu,
                               graph_fetch(instruction.y, constants),
                               graph_fetch(instruction.z, constants),
                               graph_fetch(instruction.w, constants));
        graph_registers[(instruction.x >> 8u) & 31u] = o;
    }
}

// ---- Turbulence: the noise ParticleProcessMaterial generates for it. The CPU twin says how its
// gradients differ from Godot's.

bool turbulence_enabled() {
    return word_uint(21u, 3u) != 0u;
}

// 16 bits as a number from -1 to 1, exactly on every device.
float unit_of(uint bits) {
    return (float(bits & 0xFFFFu) * (1.0 / 32768.0)) - 1.0;
}

// Godot's grad, from the lattice point's integer coordinates.
float4 grad(int4 cell, uint channel) {
    uint h = hash(uint(cell.x) + (channel * 0x632BE59Bu) + 0x9E3779B9u);
    h = hash(h + uint(cell.y));
    h = hash(h + uint(cell.z));
    h = hash(h + uint(cell.w));
    uint g = hash(h + 1u);
    return float4(unit_of(h), unit_of(h >> 16u), unit_of(g), unit_of(g >> 16u));
}

// Godot's noise: 4D gradient noise, its domain turned to improve the look of slices. The
// corners go in a loop, so the hash isn't copied sixteen times over.
float noise(float4 coord, uint channel) {
    float shift = dot(coord, float4(-0.1666667, -0.1666667, -0.1666667, -0.5));
    coord = float4(coord.xyz + shift, dot(coord, float4(0.5, 0.5, 0.5, 0.5)));
    float4 floored = floor(coord);
    int4 base = int4(floored);
    float4 delta = coord - floored;
    float4 fade = delta * delta * delta * (10.0 + (delta * (-15.0 + (delta * 6.0))));
    float result = 0.0;
    for (uint corner = 0u; corner < 16u; ++corner) {
        float4 at = float4(uint4(corner, corner >> 1u, corner >> 2u, corner >> 3u) & 1u);
        float4 weights = ((1.0 - at) * (1.0 - fade)) + (at * fade);
        float weight = weights.x * weights.y * weights.z * weights.w;
        result += weight * dot(delta - at, grad(base + int4(at), channel));
    }
    return result;
}

// Godot's curl_3d: the curl of three noises, from differences epsilon either side, the terms as
// the CPU twin takes them.
float3 curl_3d(float4 p, float c) {
    float epsilon = 0.001 + c;
    float3 curl = float3(0.0, 0.0, 0.0);
    for (uint term = 0u; term < 6u; ++term) {
        uint component = term / 2u;
        uint odd = term % 2u;
        uint axis = (component + 1u + odd) % 3u;
        uint channel = (component + 2u - odd) % 3u;
        float4 apart =
            float4(float(axis == 0u), float(axis == 1u), float(axis == 2u), 0.0) * epsilon;
        float difference = noise(p + apart, channel) - noise(p - apart, channel);
        float3 into =
            float3(float(component == 0u), float(component == 1u), float(component == 2u));
        curl += into * (odd != 0u ? -difference : difference);
    }
    return safe_normalize(curl);
}

// Godot's get_noise_direction, at a point on the screen.
float3 noise_direction(Step step, float2 pos) {
    float4 turbulence = word(21u); // strength, frequency, speed_random
    float4 speed = word(22u);
    float contrast = max(turbulence.x - 1.0, 0.0) * 70.0;
    // precise: no fused multiply-adds, so the place is the CPU's to the bit and the close
    // differences the curl takes don't magnify a rounding.
    precise float4 at = float4((pos.x * turbulence.y) + (step.time * speed.x),
                       (pos.y * turbulence.y) + (step.time * speed.y),
                       (0.0 * turbulence.y) + (step.time * speed.z),
                       0.0 + (step.time * turbulence.z));
    float3 direction = curl_3d(at, contrast);
    return mix(direction * 0.9, direction, turbulence.x - 9.0);
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

// The first point after offset, past the first: count when there is none. The loop runs to its
// end and the samplers interpolate after it: with returns inside the loop, FXC's code for
// Direct3D 12 gave some particles NaN colors.
uint curve_segment(uint curve, uint count, float offset) {
    uint found = count;
    for (uint i = count - 1u; i > 0u; --i) {
        if (offset < curve_point(curve, i).x) {
            found = i;
        }
    }
    return found;
}

float sample_curve(uint curve, float offset, float fallback) {
    uint count = curve_count(curve);
    if (count == 0u) {
        return fallback;
    }
    float2 first = curve_point(curve, 0u);
    uint next = curve_segment(curve, count, offset);
    float value = curve_point(curve, count - 1u).y;
    if (!(offset > first.x)) {
        value = first.y;
    } else if (next < count) {
        float2 a = curve_point(curve, next - 1u);
        float2 b = curve_point(curve, next);
        float t = (offset - a.x) / (b.x - a.x);
        value = a.y + ((b.y - a.y) * t);
    }
    return value;
}

uint gradient_count(uint gradient) {
    return word_uint(20u, gradient);
}

float gradient_offset(uint gradient, uint i) {
    float4 offsets = word(kGradientsAt + (gradient * kGradientWords) + (i / 4u));
    uint lane = i % 4u;
    return lane == 0u ? offsets.x : (lane == 1u ? offsets.y : (lane == 2u ? offsets.z : offsets.w));
}

float4 gradient_color(uint gradient, uint i) {
    return word(kGradientsAt + (gradient * kGradientWords) + 8u + i);
}

float4 sample_gradient(uint gradient, float offset) {
    uint count = gradient_count(gradient);
    if (count == 0u) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    // The first stop after offset, past the first, as curve_segment finds a curve's.
    uint next = count;
    for (uint i = count - 1u; i > 0u; --i) {
        if (offset < gradient_offset(gradient, i)) {
            next = i;
        }
    }
    float4 color = gradient_color(gradient, count - 1u);
    if (!(offset > gradient_offset(gradient, 0u))) {
        color = gradient_color(gradient, 0u);
    } else if (next < count) {
        float a = gradient_offset(gradient, next - 1u);
        float b = gradient_offset(gradient, next);
        float t = (offset - a) / (b - a);
        float4 x = gradient_color(gradient, next - 1u);
        float4 y = gradient_color(gradient, next);
        color = x + ((y - x) * t);
    }
    return color;
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
    dynamics.turbulence_influence = 0.0;
    if (turbulence_enabled()) {
        dynamics.turbulence_influence = random_param(kTurbulenceInfluence, seed);
    }

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

// A sub-emitter's particle starts where the particle that started it is, whose words in source
// start at from.
void start(inout Particle particle, Step step, uint index, uint number, bool taken, uint from,
           inout bool active) {
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
    float2 origin;
    if (taken) {
        origin = asfloat(source[from + 1u]).xy;
        particle.basis = asfloat(source[from + 2u]);
    } else {
        origin = initial_position(display, seed);
        if (turbulence_enabled()) {
            float displacement = random_param(kTurbulenceDisplacement, seed);
            float3 direction = noise_direction(step, origin);
            origin = origin + (direction.xy * displacement);
        }
        particle.basis = step.emission_a;
        origin = transform_point(step, origin);
    }
    float2 velocity;
    if (taken && source[from].z != 0u) {
        velocity = asfloat(source[from + 1u]).zw;
    } else {
        velocity = random_direction(seed, word(3u).z) * dynamics.initial_velocity;
        if (shape() == kShapeDirectedPoints && from_points()) {
            float2 normal = word(kPointsAt + (emission_point(display) * 2u)).zw;
            velocity = float2((velocity.x * normal.x) + (velocity.y * normal.y),
                              (velocity.x * normal.y) - (velocity.y * normal.x));
        }
    }
    velocity = transform_vector(step, velocity);
    velocity = velocity + (step.emitter_velocity * word(3u).w);
    particle.origin_velocity.xy = origin;
    particle.userdata.xyz = float3(velocity, 0.0);

    uint outputs = graph_outputs(0u);
    if (outputs != 0u) {
        uint4 counts = data[material_offset + graphs_at()];
        graph_registers[0] = float4(origin, velocity);
        graph_registers[1] = particle.color;
        graph_registers[2] = float4(0.0, 0.0, 0.0, 1.0);
        graph_registers[3] = float4(0.0, 0.0, particle.custom.w * step.lifetime, 0.0);
        graph_registers[7] = float4(0.0, 0.0, 0.0, 0.0);
        fill_graph(step, index, number);
        run_graph(material_offset + graphs_at() + kGraphHeaderWords, counts.x);
        if ((outputs & kOutPosition) != 0u) {
            particle.origin_velocity.xy = graph_registers[8].xy;
        }
        if ((outputs & kOutVelocity) != 0u) {
            particle.userdata.xy = graph_registers[8].zw;
        }
        if ((outputs & kOutActive) != 0u && !(graph_registers[10].w >= 0.5)) {
            active = false;
        }
    }
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

// ---- Collision: particle_contact against the step's shapes.

uint collision_mode() {
    return word_uint(kCollisionAt, 0u);
}

// A shape's point: a header word, then two points a word.
float2 shape_point(uint at, uint i) {
    float4 pair = asfloat(data[at + 1u + (i >> 1u)]);
    return (i & 1u) != 0u ? pair.zw : pair.xy;
}

Contact particle_contact(float2 p, float size, Step step) {
    float nearest_squared = asfloat(0x7f7fffffu); // the largest float
    float2 from_nearest = float2(0.0, 0.0);
    float2 along = float2(0.0, 0.0);
    bool inside = false;
    uint at = step.shapes;
    for (uint s = 0u; s < step.shape_count; ++s) {
        uint4 header = data[at];
        uint count = header.x;
        bool solid = header.y != 0u && count > 2u;
        uint edges = solid ? count : (count > 0u ? count - 1u : 0u);
        bool odd = false;
        for (uint i = 0u; i < edges; ++i) {
            float2 a = shape_point(at, i);
            float2 b = shape_point(at, (i + 1u) % count);
            float2 ab = b - a;
            float2 ap = p - a;
            float length_squared = dot(ab, ab);
            float t = length_squared > 0.0 ? dot(ap, ab) / length_squared : 0.0;
            float2 away = ap;
            if (t >= 1.0) {
                away = p - b;
            } else if (t > 0.0) {
                float cross_ab = (ab.x * ap.y) - (ab.y * ap.x);
                away = float2(-ab.y, ab.x) * (cross_ab / length_squared);
            }
            float distance_squared = dot(away, away);
            if (distance_squared < nearest_squared) {
                nearest_squared = distance_squared;
                from_nearest = away;
                along = ab;
            }
            if (solid && ((a.y > p.y) != (b.y > p.y)) &&
                p.x < a.x + (((p.y - a.y) * (b.x - a.x)) / (b.y - a.y))) {
                odd = !odd;
            }
        }
        inside = inside || odd;
        at += 1u + ((count + 1u) / 2u);
    }
    Contact contact;
    contact.collided = false;
    contact.normal = float2(0.0, 0.0);
    contact.depth = 0.0;
    float distance = sqrt(nearest_squared);
    float signed_distance = inside ? -distance : distance;
    if (nearest_squared != asfloat(0x7f7fffffu) && signed_distance - size < 0.0) {
        contact.collided = true;
        contact.depth = size - signed_distance;
        if (distance > 0.0) {
            contact.normal = (inside ? -from_nearest : from_nearest) / distance;
        } else {
            contact.normal = length(along) > 0.0 ? safe_normalize(float2(along.y, -along.x))
                                                 : float2(0.0, -1.0);
        }
    }
    return contact;
}

// shape_contact in the CPU twin.
Contact shape_contact(float2 origin, float size, Step step) {
    float2 at = float2((step.to_shapes_a.x * origin.x) + (step.to_shapes_a.z * origin.y) +
                           step.to_shapes_b.x,
                       (step.to_shapes_a.y * origin.x) + (step.to_shapes_a.w * origin.y) +
                           step.to_shapes_b.y);
    Contact contact = particle_contact(at, size * length(step.to_shapes_a.zw), step);
    if (contact.collided) {
        float2 away = contact.normal * contact.depth;
        float2 back = float2((step.from_shapes.x * away.x) + (step.from_shapes.z * away.y),
                             (step.from_shapes.y * away.x) + (step.from_shapes.w * away.y));
        contact.depth = length(back);
        contact.normal = contact.depth > 0.0 ? back / contact.depth : float2(0.0, -1.0);
    }
    return contact;
}

// collision_size in the CPU twin.
float collision_size(Particle particle, Step step, bool started) {
    if (word_uint(kCollisionAt, 3u) == 0u) {
        return step.collision_size;
    }
    float x = length(particle.basis.xy);
    float y = length(particle.basis.zw);
    float z = started ? 1.0 : (x + y) * 0.5;
    return step.collision_size * ((x + y + z) * 0.33333333333);
}

// How many particles a particle starts in the next system of a chain in a step, as the code
// Godot generates for each sub_emitter_mode, with its age before the step and after.
uint emission_count(Step step, float delta, float before, Particle particle, bool collided) {
    uint4 sub = data[material_offset + kSubEmitterAt];
    if (sub.x == kSubEmitterConstant) {
        float interval = asfloat(sub.y);
        float from = step.interp_to_end == 0.0 ? before * step.lifetime
                                               : (particle.custom.y * step.lifetime) - delta;
        float rem = interval - mod(from, interval);
        return delta >= rem ? 1u : 0u;
    }
    if (sub.x == kSubEmitterAtEnd) {
        return (particle.custom.y / particle.custom.w * step.lifetime) > step.lifetime - delta
                   ? sub.z
                   : 0u;
    }
    if (sub.x == kSubEmitterAtCollision) {
        return collided ? sub.z : 0u;
    }
    if (sub.x == kSubEmitterAtStart) {
        return before == 0.0 ? sub.z : 0u;
    }
    return 0u;
}

void process(inout Particle particle, Step step, uint index, uint number, float delta,
             Contact contact, inout uint emitted, inout bool active) {
    uint seed = hash(number + 1u + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(seed, display, dynamics, physical);

    float before = particle.custom.y;
    bool first_frame = before == 0.0;
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

    float3 built = particle.userdata.xyz;
    float2 force = word(4u).zw;
    if (length(built) > 0.0) {
        force = force + (safe_normalize(built).xy * physical.linear_accel);
    }
    float2 diff = particle.origin_velocity.xy - step.emission_b;
    if (length(diff) > 0.0) {
        force = force + (safe_normalize(diff) * physical.radial_accel);
    }
    if (length(diff) > 0.0) {
        force = force + (safe_normalize(float2(-diff.y, diff.x)) * physical.tangent_accel);
    }
    built = built + float3(force * delta, 0.0);
    if (physical.damping > 0.0) {
        float v = length(built);
        if (word_uint(6u, 2u) == 0u) {
            v -= physical.damping * delta;
        } else if (v > 0.001) {
            v -= v * v * physical.damping * 0.05 * delta;
        }
        built = v < 0.0 ? float3(0.0, 0.0, 0.0) : safe_normalize(built) * v;
    }

    // A rigid particle is pushed out of the shape, loses its speed into it and bounces back.
    float4 collision = word(kCollisionAt);
    bool collided = contact.collided && collision_mode() != 0u;
    if (collided && collision_mode() == kCollisionRigid) {
        float3 normal = float3(contact.normal, 0.0);
        float response = dot(normal, built);
        float bounces = abs(response) >= 2.0 / clamp(collision.z + 1.0, 1.0, 2.0) ? 1.0 : 0.0;
        particle.origin_velocity.xy =
            particle.origin_velocity.xy + (contact.normal * contact.depth);
        built = built - (normal * response);
        built = mix(built, float3(0.0, 0.0, 0.0), clamp(collision.y, 0.0, 1.0));
        built = built - (normal * (response * (collision.z * bounces)));
    } else if (collided && collision_mode() == kCollisionHideOnContact) {
        active = false;
    }

    // Turbulence before limiting, but not for a particle bouncing off a shape.
    float3 controlled3 = float3(controlled, 0.0);
    if (turbulence_enabled() && !(collided && collision_mode() == kCollisionRigid)) {
        float influence = sample_curve(kTurbulenceInfluence, life, 1.0);
        float3 direction = safe_normalize(noise_direction(step, particle.origin_velocity.xy));
        float amount = clamp(dynamics.turbulence_influence * influence, 0.0, 1.0) *
                       (delta <= 0.0 ? 0.0 : 1.0);
        float boost = 1.0 + ((1.0 - amount) * 0.2);
        built = mix(built, direction * (length(built) * boost), amount);
        controlled3 = mix(controlled3, direction * (length(controlled3) * boost), amount);
    }
    particle.userdata.xyz = built;

    float3 final_velocity = controlled3 + built;
    if (curve_count(kVelocityLimitCurve) > 0u && length(final_velocity) > 0.001) {
        float limit = abs(sample_curve(kVelocityLimitCurve, life, 0.0));
        final_velocity =
            safe_normalize(final_velocity) * min(abs(length(final_velocity)), limit);
    }
    float2 velocity = final_velocity.xy;

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
    float scale = max(abs(display.scale), 0.001);
    if (first_frame && display.scale < 0.0) {
        scale = -scale;
    }
    particle.custom.z = display.animation_offset + (life * display.animation_speed);

    // The graph sees the step worked out, and the particle where it was.
    uint outputs = graph_outputs(1u);
    bool placed = false;
    if (outputs != 0u) {
        uint4 counts = data[material_offset + graphs_at()];
        graph_registers[0] = float4(particle.origin_velocity.xy, velocity);
        graph_registers[1] = particle.color;
        graph_registers[2] = float4(particle.custom.x, scale, particle.custom.z, 1.0);
        graph_registers[3] = float4(particle.custom.y * step.lifetime, life,
                                    particle.custom.w * step.lifetime, delta);
        graph_registers[7] = float4(contact.normal, contact.depth, collided ? 1.0 : 0.0);
        fill_graph(step, index, number);
        run_graph(material_offset + graphs_at() + kGraphHeaderWords + counts.x + counts.y,
                  counts.z);
        float4 motion = graph_registers[8];
        float4 look = graph_registers[10];
        if ((outputs & kOutVelocity) != 0u) {
            // The forces carry the change on.
            particle.userdata.xy = particle.userdata.xy + (motion.zw - velocity);
            velocity = motion.zw;
        }
        if ((outputs & kOutPosition) != 0u) {
            particle.origin_velocity.xy = motion.xy;
            placed = true;
        }
        if ((outputs & kOutColor) != 0u) {
            particle.color = graph_registers[9];
        }
        if ((outputs & kOutAngle) != 0u) {
            particle.custom.x = look.x;
        }
        if ((outputs & kOutScale) != 0u) {
            float size = max(0.001, abs(look.y));
            scale = look.y < 0.0 ? -size : size;
        }
        if ((outputs & kOutFrame) != 0u) {
            particle.custom.z = look.z;
        }
        if ((outputs & kOutActive) != 0u && !(look.w >= 0.5)) {
            active = false;
        }
    }
    particle.origin_velocity.zw = velocity;
    if (!placed) {
        particle.origin_velocity.xy = particle.origin_velocity.xy + (velocity * delta);
    }

    float2 x_axis;
    float2 y_axis;
    if (word_uint(6u, 1u) != 0u && (outputs & kOutAngle) == 0u) {
        y_axis = length(velocity) > 0.0 ? safe_normalize(velocity)
                                        : safe_normalize(particle.basis.zw);
        x_axis = safe_normalize(float2(y_axis.y, -y_axis.x));
    } else {
        float c = cos(particle.custom.x);
        float s = sin(particle.custom.x);
        x_axis = float2(c, -s);
        y_axis = float2(s, c);
    }
    particle.basis = float4(x_axis * scale, y_axis * scale);
    if ((chain & kChainEmits) != 0u) {
        emitted = emission_count(step, delta, before, particle, collided);
    }
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

// The particle of the system before whose emissions include the one numbered rank: the last whose
// first is at or before it.
uint emitter_of(uint rank) {
    uint low = 0u;
    uint high = source[0].w;
    while (high - low > 1u) {
        uint mid = (low + high) / 2u;
        if (source[1u + (mid * kEmissionWords)].y <= rank) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
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
    step.time = asfloat(data[at + 4u].w);
    uint4 counts = data[at + 4u];
    step.cycle = counts.x;
    step.seed = counts.y;
    step.emitting = (counts.z & 1u) != 0u;
    step.fractional_delta = (counts.z & 2u) != 0u;
    step.clear = (counts.z & 4u) != 0u;
    uint4 shapes = data[at + 5u];
    step.shapes = shapes.x;
    step.shape_count = shapes.y;
    step.collision_size = asfloat(shapes.z);
    step.trail_steps = shapes.w;
    step.to_shapes_a = asfloat(data[at + 6u]);
    step.to_shapes_b = asfloat(data[at + 7u]).xy;
    step.from_shapes = asfloat(data[at + 8u]);
    return step;
}

// One step of the particle (j 0), or of copy j of its trail after the particle (head) ran the
// step at: step_particle and step_copy in the CPU twin. They share one body so the shader holds
// one copy of process(), which compilers inline at every call: with two, Direct3D 12 lost its
// device running it on CI.
void step_unit(inout Particle unit, Particle head, uint j, uint index, uint at, Step step) {
    if (step.clear) {
        // The count a sub-emitter keeps in a particle's spare words stays: it was set for this
        // step.
        uint3 spare = unit.flags.yzw;
        unit = empty_particle();
        if (j == 0u) {
            unit.flags.yzw = spare;
        }
    }
    Step run = step;
    float delta = step.delta;
    bool restart = false;
    bool active = true;
    uint number = 0u;
    if (j == 0u) {
        unit.flags.x &= ~kParticleStarted;
        bool taken = false;
        uint from = 0u;
        if ((chain & kChainTakes) != 0u) {
            // The emission numbered as this slot is in the cycle's order, taken even by a live
            // one.
            uint4 header = source[0];
            uint turn = header.z != 0u ? amount - 1u - index : index;
            uint rank = (turn + amount - header.x) % amount;
            if (rank < header.y) {
                taken = true;
                from = 1u + (emitter_of(rank) * kEmissionWords);
                restart = true;
                unit.flags.x =
                    kParticleActive | kParticleStarted | (step.cycle << kParticleCycleShift);
            }
        } else if (step.emitting) {
            // A product, and precise: Metal's fast math divides and fuses operations off the
            // CPU twin's rounding, which moves a particle that starts at a step's edge.
            precise float restart_phase = float(index) * phase_step;
            if (step.randomness > 0.0) {
                uint seed = step.cycle;
                if (restart_phase >= step.system_phase) {
                    seed -= 1u;
                }
                seed *= amount;
                seed += index;
                float random = float(hash(seed) % 65536u) * (1.0 / 65536.0);
                restart_phase += step.randomness * random * phase_step;
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
                unit.flags.x =
                    kParticleActive | kParticleStarted | (step.cycle << kParticleCycleShift);
            }
        }
        active = (unit.flags.x & kParticleActive) != 0u;
        number = ((unit.flags.x >> kParticleCycleShift) * amount) + index;
        if (restart && active) {
            start(unit, step, index, number, taken, from, active);
        }
    } else {
        uint cycle_bits = head.flags.x & (0xFFFFFFFFu << kParticleCycleShift);
        uint lag = (j * step.trail_steps) / trail;
        bool copied = false;
        if ((head.flags.x & kParticleStarted) != 0u) {
            unit = head;
            unit.flags = uint4(kParticleTrailed | cycle_bits, lag, 0u, 0u);
            copied = true;
        }
        if ((unit.flags.x & kParticleTrailed) != 0u) {
            if (!copied && unit.flags.y > 0u) {
                unit.flags.y -= 1u;
            }
            if (unit.flags.y == 0u) {
                unit.flags.x =
                    kParticleActive | (unit.flags.x & (0xFFFFFFFFu << kParticleCycleShift));
            }
            return;
        }
        if ((unit.flags.x & kParticleActive) == 0u || lag > at) {
            return;
        }
        run = read_step(steps_offset + ((at - lag) * kStepWords));
        delta = run.delta;
        number = ((unit.flags.x >> kParticleCycleShift) * amount) + index;
    }
    Contact contact;
    contact.collided = false;
    contact.normal = float2(0.0, 0.0);
    contact.depth = 0.0;
    if (active && collision_mode() != 0u && run.shape_count > 0u) {
        contact = shape_contact(unit.origin_velocity.xy, collision_size(unit, run, restart), run);
    }
    uint emitted = 0u;
    if (active) {
        process(unit, run, index, number, delta, contact, emitted, active);
    }
    unit.flags.x &= ~kParticleActive;
    if (active) {
        unit.flags.x |= kParticleActive;
    }
    if (j == 0u && (chain & kChainEmits) != 0u) {
        // emit_subparticle(TRANSFORM, VELOCITY, ...) with Godot's emission flags.
        uint target_at = 1u + (index * kEmissionWords);
        target[target_at] = uint4(emitted, 0u, data[material_offset + kSubEmitterAt].w, 0u);
        target[target_at + 1u] = asuint(unit.origin_velocity);
        target[target_at + 2u] = asuint(unit.basis);
    }
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= amount) {
        return;
    }
    Particle particle = particles[index];
    uint copies = amount + (index * (trail - 1u));
    if (reset != 0u) {
        particle = empty_particle();
        for (uint c = 1u; c < trail; ++c) {
            particles[copies + c - 1u] = empty_particle();
        }
    }
    for (uint i = 0u; i < step_count; ++i) {
        uint at = history + i;
        Step step = read_step(steps_offset + (at * kStepWords));
        // The particle, then its trail's copies, which take it up where it starts.
        for (uint j = 0u; j < trail; ++j) {
            Particle unit = particle;
            if (j > 0u) {
                unit = particles[copies + j - 1u];
            }
            step_unit(unit, particle, j, index, at, step);
            if (j == 0u) {
                particle = unit;
            } else {
                particles[copies + j - 1u] = unit;
            }
        }
    }
    particles[index] = particle;
}
