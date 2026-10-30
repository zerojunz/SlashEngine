// Runs steps on a 3D particle system, a thread a particle: Godot's particles shader and the code
// ParticleProcessMaterial generates for 3D. Its CPU twin is process_particles_3d in
// src/particles3d.cpp, which this follows line for line; a change to one goes in the other. The
// process material and steps are words laid out as src/particle_layout_3d.hpp says. The noise,
// random numbers and curves are particles_process.hlsl's, at this layout's places.

#include "particles_3d.hlsl"

cbuffer Params : UNIFORMS {
    uint amount;
    uint material_offset; // in words
    uint steps_offset;
    uint step_count;
    uint reset;       // clears every particle first
    float phase_step; // 1 / amount, divided on the CPU: Metal's fast division rounds otherwise
    uint pad0;
    uint pad1;
};

StructuredBuffer<uint4> data : READ_ONLY;
RWStructuredBuffer<Particle3D> particles : READ_WRITE;

static const float kPi = 3.14159;
static const float kDegreesToRadians = kPi / 180.0;

// Where the process material's parts are, in words (src/particle_layout_3d.hpp).
static const uint kParamMinAt = 12u;
static const uint kParamMaxAt = 16u;
static const uint kCurveCountsAt = 20u;
static const uint kGradientCountsAt = 28u;
static const uint kCurvesAt = 29u;
static const uint kCurveWords = 16u;
static const uint kGradientsAt = 509u;
static const uint kGradientWords = 40u;
static const uint kPointsAt = 589u;
static const uint kPointWords = 3u;
// A step's words.
static const uint kStepWords = 8u;
// The curves after the parameters': the velocity limit, alpha and emission curves, then the x, y
// and z of the scale, orbit velocity, directional velocity and scale over velocity curves.
static const uint kVelocityLimitCurve = 15u;
static const uint kAlphaCurve = 16u;
static const uint kEmissionCurve = 17u;
static const uint kScaleCurve = 18u;
static const uint kOrbitCurve = 21u;
static const uint kDirectionalCurve = 24u;
static const uint kScaleOverVelocityCurve = 27u;
// The flags in word 4's w.
static const uint kAlignY = 1u;
static const uint kRotateY = 2u;
static const uint kDisableZ = 4u;
static const uint kDampingAsFriction = 8u;
static const uint kTurbulence = 16u;
static const uint kDirectionalGlobal = 32u;
static const uint kAttractors = 64u;
static const uint kCollisionUseScale = 128u;
static const uint kCollisionRigid = 256u;
static const uint kCollisionHide = 512u;
// The attractor count's place in a step's flags, an attractor's words and AttractorShape3D.
static const uint kStepAttractorShift = 8u;
static const uint kAttractorWords = 5u;
static const uint kAttractorSphere = 0u;
static const uint kAttractorBox = 1u;
// The collider count's place in a step's flags, a collider's words and ColliderShape3D.
static const uint kStepColliderShift = 16u;
static const uint kColliderWords = 5u;
static const uint kColliderSphere = 0u;
static const uint kColliderBox = 1u;
static const uint kColliderSdf = 2u;

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

struct Step {
    Affine emission;
    float3 emitter_velocity;
    float system_phase;
    float prev_system_phase;
    float delta;
    float lifetime;
    float explosiveness;
    float randomness;
    float amount_ratio;
    float interp_to_end;
    float time;
    uint cycle;
    uint seed;
    bool emitting;
    bool fractional_delta;
    bool clear;
    uint attractor_count;
    uint attractors_at; // in words
    uint collider_count;
    uint colliders_at; // in words
    float collision_size;
};

// Where a particle touches the step's colliders: the way out of them and how deep it is.
struct Contact {
    bool collided;
    float3 normal;
    float depth;
};

struct Display {
    float3 scale;
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
    float directional_velocity;
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
    return word_uint(3u, 3u);
}

bool flag(uint bit) {
    return (word_uint(4u, 3u) & bit) != 0u;
}

float mix(float a, float b, float t) {
    return (a * (1.0 - t)) + (b * t);
}

float3 mix(float3 a, float3 b, float t) {
    return (a * (1.0 - t)) + (b * t);
}

// Godot's normalize_or_else: v at length 1, or fallback for exactly zero.
float3 unit_or_else(float3 v, float3 fallback) {
    return all(v == float3(0.0, 0.0, 0.0)) ? fallback : unit(v);
}

float sign_of(float v) {
    return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0);
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

// ---- Turbulence, as particles_process.hlsl makes it.

// 16 bits as a number from -1 to 1, exactly on every device.
float unit_of(uint bits) {
    return (float(bits & 0xFFFFu) * (1.0 / 32768.0)) - 1.0;
}

float4 grad(int4 cell, uint channel) {
    uint h = hash(uint(cell.x) + (channel * 0x632BE59Bu) + 0x9E3779B9u);
    h = hash(h + uint(cell.y));
    h = hash(h + uint(cell.z));
    h = hash(h + uint(cell.w));
    uint g = hash(h + 1u);
    return float4(unit_of(h), unit_of(h >> 16u), unit_of(g), unit_of(g >> 16u));
}

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
    float l = length_of(curl);
    return l > 0.0 ? curl / l : float3(0.0, 0.0, 0.0);
}

// Godot's get_noise_direction.
float3 noise_direction(Step step, float3 pos) {
    float4 turbulence = word(10u); // strength, frequency and speed_random after .x
    float4 speed = word(11u);
    float contrast = max(turbulence.y - 1.0, 0.0) * 70.0;
    precise float4 at = float4((pos.x * turbulence.z) + (step.time * speed.x),
                               (pos.y * turbulence.z) + (step.time * speed.y),
                               (pos.z * turbulence.z) + (step.time * speed.z),
                               0.0 + (step.time * turbulence.w));
    float3 direction = curl_3d(at, contrast);
    return mix(direction * 0.9, direction, turbulence.y - 9.0);
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

// ---- Parameters, curves and gradients.

float param_min(uint param) {
    return word(kParamMinAt + (param / 4u))[param % 4u];
}

float param_max(uint param) {
    return word(kParamMaxAt + (param / 4u))[param % 4u];
}

float random_param(uint param, inout uint seed) {
    return mix(param_min(param), param_max(param), rand_from_seed(seed));
}

uint curve_count(uint curve) {
    return word_uint(kCurveCountsAt + (curve / 4u), curve % 4u);
}

float2 curve_point(uint curve, uint i) {
    float4 pair = word(kCurvesAt + (curve * kCurveWords) + (i / 2u));
    return (i % 2u) == 0u ? pair.xy : pair.zw;
}

// The first point after offset, past the first: count when there is none. The loop runs to its
// end, as particles_process.hlsl's, for FXC.
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

// Whether one of the x, y and z curves from first has points.
bool has_curve3(uint first) {
    return curve_count(first) + curve_count(first + 1u) + curve_count(first + 2u) > 0u;
}

float3 sample3(uint first, float offset, float fallback) {
    return float3(sample_curve(first, offset, fallback), sample_curve(first + 1u, offset, fallback),
                  sample_curve(first + 2u, offset, fallback));
}

uint gradient_count(uint gradient) {
    return word_uint(kGradientCountsAt, gradient);
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

// ---- The generated shader.

bool point_shape() {
    return shape() == kShapePoints || shape() == kShapeDirectedPoints;
}

uint emission_point(Display display) {
    int count = int(point_count());
    return uint(max(min(count - 1, int(display.emission_position * float(count))), 0));
}

// calculate_initial_display_params, calculate_initial_dynamics_params and
// calculate_initial_physical_params.
void initial_params(inout uint seed, out Display display, out Dynamics dynamics,
                    out Physical physical) {
    float scale = random_param(kScale, seed);
    scale = sign_of(scale) * max(abs(scale), 0.001);
    display.scale = float3(scale, scale, scale);
    display.hue_rotation = kPi * 2.0 * random_param(kHueVariation, seed);
    display.animation_speed = random_param(kAnimSpeed, seed);
    display.animation_offset = random_param(kAnimOffset, seed);
    display.lifetime = 1.0 - (word(9u).x * rand_from_seed(seed));
    display.color = word(8u);
    display.emission_position = 0.0;
    if (gradient_count(1u) > 0u) {
        display.color = display.color * sample_gradient(1u, rand_from_seed(seed));
    }
    if (point_shape()) {
        display.emission_position = rand_from_seed(seed);
        if (point_count() > 0u) {
            display.color =
                display.color * word(kPointsAt + (emission_point(display) * kPointWords) + 2u);
        }
    }

    dynamics.angle = random_param(kAngle, seed);
    dynamics.angular_velocity = random_param(kAngularVelocity, seed);
    dynamics.initial_velocity = random_param(kInitialVelocity, seed);
    dynamics.directional_velocity = mix(word(9u).y, word(9u).z, rand_from_seed(seed));
    dynamics.radial_velocity = random_param(kRadialVelocity, seed);
    dynamics.orbit_velocity = random_param(kOrbitVelocity, seed);
    dynamics.turbulence_influence = 0.0;
    if (flag(kTurbulence)) {
        dynamics.turbulence_influence = random_param(kTurbulenceInfluence, seed);
    }

    physical.linear_accel = random_param(kLinearAccel, seed);
    physical.radial_accel = random_param(kRadialAccel, seed);
    physical.tangent_accel = random_param(kTangentialAccel, seed);
    physical.damping = random_param(kDamping, seed);
}

// process_display_param.
void display_at(inout Display display, float life) {
    if (has_curve3(kScaleCurve)) {
        display.scale = display.scale * sample3(kScaleCurve, life, 0.0);
    }
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

// get_random_direction_from_spread.
float3 random_direction(inout uint seed, float spread) {
    float3 direction = word(5u).xyz;
    float spread_rad = spread * kDegreesToRadians;
    if (flag(kDisableZ)) {
        float angle = rand_m1_p1(seed) * spread_rad;
        angle += direction.x != 0.0 ? atan2(direction.y, direction.x)
                                    : sign_of(direction.y) * (kPi / 2.0);
        return float3(cos(angle), sin(angle), 0.0);
    }
    float angle1 = rand_m1_p1(seed) * spread_rad;
    float angle2 = rand_m1_p1(seed) * spread_rad * (1.0 - word(6u).w);
    float3 xz = float3(sin(angle1), 0.0, cos(angle1));
    float3 yz = float3(0.0, sin(angle2), cos(angle2));
    yz.z = yz.z / max(0.0001, sqrt(abs(yz.z)));
    float3 spread_direction = float3(xz.x * yz.z, yz.y, xz.z * yz.z);
    float3 forward = length_of(direction) > 0.0 ? unit(direction) : float3(0.0, 0.0, 1.0);
    float3 binormal = cross(float3(0.0, 1.0, 0.0), forward);
    if (length_of(binormal) < 0.0001) {
        binormal = float3(0.0, 0.0, 1.0);
    }
    binormal = unit(binormal);
    float3 normal = cross(binormal, forward);
    return unit((binormal * spread_direction.x) + (normal * spread_direction.y) +
                (forward * spread_direction.z));
}

// calculate_initial_position.
float3 initial_position(Display display, inout uint seed) {
    float3 pos = float3(0.0, 0.0, 0.0);
    uint kind = shape();
    float4 radii = word(0u); // shape, sphere radius, ring radius, ring inner radius
    if (kind == kShapeSphere) {
        float s = (rand_from_seed(seed) * 2.0) - 1.0;
        float t = rand_from_seed(seed) * 2.0 * kPi;
        float along = rand_from_seed(seed);
        float radius = radii.y * sqrt(1.0 - (s * s));
        pos = float3(radius * cos(t), radius * sin(t), radii.y * s) * along;
    } else if (kind == kShapeSphereSurface) {
        float s = (rand_from_seed(seed) * 2.0) - 1.0;
        float t = rand_from_seed(seed) * 2.0 * kPi;
        float radius = radii.y * sqrt(1.0 - (s * s));
        pos = float3(radius * cos(t), radius * sin(t), radii.y * s);
    } else if (kind == kShapeBox) {
        float x = (rand_from_seed(seed) * 2.0) - 1.0;
        float y = (rand_from_seed(seed) * 2.0) - 1.0;
        float z = (rand_from_seed(seed) * 2.0) - 1.0;
        pos = float3(x, y, z) * word(3u).xyz;
    } else if (kind == kShapePoints || kind == kShapeDirectedPoints) {
        if (point_count() > 0u) {
            pos = word(kPointsAt + (emission_point(display) * kPointWords)).xyz;
        }
    } else if (kind == kShapeRing) {
        float radius = max(0.001, radii.z);
        float inner = radii.w;
        float height = word(1u).w;
        // The ring's top radius and skew, worked out on the CPU (particle_layout_3d.cpp).
        float4 ring = word(2u);
        float top = ring.w;
        float skew = word(9u).w;
        float y_pos = rand_from_seed(seed);
        y_pos = radius < top ? pow(y_pos, skew) : 1.0 - pow(y_pos, skew);
        float angle = rand_from_seed(seed) * 2.0 * kPi;
        float r = sqrt((rand_from_seed(seed) * ((radius * radius) - (inner * inner))) +
                       (inner * inner));
        r = mix(r, r * (top / radius), y_pos);
        float3 axis = word(4u).xyz; // made a unit vector on the CPU
        float3 ortho = abs(axis.x) == 1.0 && axis.y == 0.0 && axis.z == 0.0
                           ? cross(axis, float3(0.0, 1.0, 0.0))
                           : cross(axis, float3(1.0, 0.0, 0.0));
        ortho = unit(ortho);
        float s = sin(angle);
        float c = cos(angle);
        float oc = 1.0 - c;
        float3 c0 = float3(c + (axis.x * axis.x * oc), (axis.x * axis.y * oc) - (axis.z * s),
                           (axis.x * axis.z * oc) + (axis.y * s));
        float3 c1 = float3((axis.x * axis.y * oc) + (s * axis.z), c + (axis.y * axis.y * oc),
                           (axis.y * axis.z * oc) - (axis.x * s));
        float3 c2 = float3((axis.z * axis.x * oc) - (axis.y * s),
                           (axis.z * axis.y * oc) + (axis.x * s), c + (axis.z * axis.z * oc));
        ortho = unit((c0 * ortho.x) + (c1 * ortho.y) + (c2 * ortho.z));
        pos = (ortho * r) + (axis * ((y_pos * height) - (height / 2.0)));
    }
    return (pos * word(2u).xyz) + word(1u).xyz;
}

// The generated start().
void start(inout Particle3D particle, Step step, uint number, inout bool active) {
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
    particle.color = display.color;
    float3 origin = initial_position(display, seed);
    if (flag(kTurbulence)) {
        float displacement = random_param(kTurbulenceDisplacement, seed);
        origin = origin + (noise_direction(step, origin) * displacement);
    }
    particle.x = float4(step.emission.x, 0.0);
    particle.y = float4(step.emission.y, 0.0);
    particle.z = float4(step.emission.z, 0.0);
    origin = apply(step.emission, origin);
    float3 velocity = random_direction(seed, word(5u).w) * dynamics.initial_velocity;
    if (shape() == kShapeDirectedPoints && point_count() > 0u) {
        float4 normal4 = word(kPointsAt + (emission_point(display) * kPointWords) + 1u);
        float3 normal = normal4.xyz;
        if (flag(kDisableZ)) {
            float2 v = velocity.xy;
            velocity = float3((normal.x * v.x) + (normal.y * v.y),
                              (normal.y * v.x) - (normal.x * v.y), velocity.z);
        } else if (all(normal == float3(0.0, 0.0, 0.0))) {
            velocity = float3(0.0, 0.0, 0.0);
        } else {
            float3 v0 = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
            float3 tangent = unit(cross(v0, normal));
            float3 bitangent = unit(cross(tangent, normal));
            velocity = (tangent * velocity.x) + (bitangent * velocity.y) + (normal * velocity.z);
        }
    }
    velocity = basis(step.emission, velocity);
    velocity = velocity + (step.emitter_velocity * word(7u).w);
    if (flag(kDisableZ)) {
        velocity.z = 0.0;
        origin.z = 0.0;
    }
    particle.velocity = velocity;
    particle.origin = float4(origin, 0.0);
}

// process_orbit_displacement: 2D's with disable_z, and in 3D only with a curve, as Godot's.
float3 orbit_displacement(Dynamics dynamics, float life, float3 origin, Affine emission,
                          float delta) {
    bool curved = has_curve3(kOrbitCurve);
    if ((!curved && !flag(kDisableZ)) || abs(dynamics.orbit_velocity) < 0.01 || delta < 0.001) {
        return float3(0.0, 0.0, 0.0);
    }
    float3 displacement = float3(0.0, 0.0, 0.0);
    if (flag(kDisableZ)) {
        float amount = dynamics.orbit_velocity;
        if (curved) {
            amount *= sample_curve(kOrbitCurve + 2u, life, 0.0);
        }
        if (amount != 0.0) {
            float3 diff = origin - emission.origin;
            float turn = amount * kPi * 2.0 * delta;
            float c = cos(turn);
            float s = sin(turn);
            displacement = float3((c * diff.x) + (s * diff.y) - diff.x,
                                  (-s * diff.x) + (c * diff.y) - diff.y, 0.0);
        }
    } else {
        float3 turns =
            sample3(kOrbitCurve, life, 0.0) * (dynamics.orbit_velocity * kPi * 2.0 * delta);
        float3 at = apply(inverse(emission), origin) - word(6u).xyz;
        float3 local = float3(0.0, at.y, at.z);
        float c = cos(turns.x);
        float s = sin(turns.x);
        displacement =
            float3(local.x, (c * local.y) - (s * local.z), (s * local.y) + (c * local.z)) - local;
        local = float3(at.x, 0.0, at.z);
        c = cos(turns.y);
        s = sin(turns.y);
        displacement +=
            float3((c * local.x) + (s * local.z), local.y, (-s * local.x) + (c * local.z)) - local;
        local = float3(at.x, at.y, 0.0);
        c = cos(turns.z);
        s = sin(turns.z);
        displacement +=
            float3((c * local.x) - (s * local.y), (s * local.x) + (c * local.y), local.z) - local;
    }
    return basis(emission, displacement * (1.0 / delta));
}

// process_radial_displacement.
float3 radial_displacement(Dynamics dynamics, float life, inout uint seed, float3 origin,
                           Affine emission, float delta) {
    if (delta < 0.001) {
        return float3(0.0, 0.0, 0.0);
    }
    float multiplier = sample_curve(kRadialVelocity, life, 1.0);
    float3 pivot = apply(emission, word(6u).xyz);
    float3 away = origin - pivot;
    float3 out_;
    if (length_of(away) > 0.01) {
        out_ = unit(away) * (multiplier * dynamics.radial_velocity);
    } else {
        out_ = random_direction(seed, 360.0) * dynamics.radial_velocity;
    }
    if (multiplier * dynamics.radial_velocity < 0.0) {
        out_ = unit(out_) *
               min(abs(multiplier * dynamics.radial_velocity), length_of(away) / delta);
    }
    return out_;
}

// A scale of 0 keeps its sign as positive, at the smallest scale.
float scaled(float s) {
    return (s < 0.0 ? -1.0 : 1.0) * max(abs(s), 0.001);
}

// The generated process().
// Godot's safe_normalize: zero below a length of 0.001.
float3 safe_normalize(float3 v) {
    return length_of(v) < 0.001 ? float3(0.0, 0.0, 0.0) : unit(v);
}

// GLSL's pow for a base of 0 or more, 0 for 0.
float attractor_power(float base, float exponent) {
    return base <= 0.0 ? 0.0 : exp(exponent * log(base));
}

float3 field_texel(uint field, uint3 size, uint x, uint y, uint z) {
    return asfloat(data[field + 1u + (((z * size.y) + y) * size.x) + x]).xyz;
}

// A vector field's texel at uvw, filtered as a linear sampler that clamps to the edge filters a
// Texture3D's first level; the field is {width, height, depth, 0}, then a word a texel.
float3 sample_field(uint field, float3 uvw) {
    uint3 size = data[field].xyz;
    float3 texel = clamp((uvw * float3(size)) - 0.5, float3(0.0, 0.0, 0.0), float3(size - 1u));
    uint3 low = uint3(texel);
    uint3 high = min(low + 1u, size - 1u);
    float3 t = texel - float3(low);
    float3 near = mix(mix(field_texel(field, size, low.x, low.y, low.z),
                            field_texel(field, size, high.x, low.y, low.z), t.x),
                       mix(field_texel(field, size, low.x, high.y, low.z),
                            field_texel(field, size, high.x, high.y, low.z), t.x),
                       t.y);
    float3 far = mix(mix(field_texel(field, size, low.x, low.y, high.z),
                           field_texel(field, size, high.x, low.y, high.z), t.x),
                      mix(field_texel(field, size, low.x, high.y, high.z),
                           field_texel(field, size, high.x, high.y, high.z), t.x),
                      t.y);
    return mix(near, far, t.z);
}

// What a step's attractors push a particle at pos with, as particles.glsl's attractor loop (its
// mass is 1). The loop runs to its end: FXC miscompiles returns and breaks inside loops.
float3 attractor_force(Step step, float3 pos) {
    float3 force = float3(0.0, 0.0, 0.0);
    for (uint i = 0u; i < step.attractor_count; ++i) {
        uint at = step.attractors_at + (i * kAttractorWords);
        float4 a = asfloat(data[at]);
        float4 b = asfloat(data[at + 1u]);
        float4 c = asfloat(data[at + 2u]);
        Affine place;
        place.x = a.xyz;
        place.y = float3(a.w, b.xy);
        place.z = float3(b.zw, c.x);
        place.origin = c.yzw;
        float4 d = asfloat(data[at + 3u]);
        float3 extents = d.xyz;
        float strength = d.w;
        uint4 e = data[at + 4u];
        float attenuation = asfloat(e.z);
        float directionality = asfloat(e.w);

        float3 rel = pos - place.origin;
        float3 local = float3(dot(rel, place.x), dot(rel, place.y), dot(rel, place.z));
        float3 dir = float3(0.0, 0.0, 0.0);
        float amount = 0.0;
        bool inside = true;
        if (e.x == kAttractorSphere) {
            dir = safe_normalize(rel);
            float dist = length_of(local) / extents.x;
            inside = !(dist > 1.0);
            amount = max(0.0, 1.0 - dist);
        } else if (e.x == kAttractorBox) {
            dir = safe_normalize(rel);
            float dist = max(abs(local.x / extents.x),
                             max(abs(local.y / extents.y), abs(local.z / extents.z)));
            inside = !(dist > 1.0);
            amount = max(0.0, 1.0 - dist);
        } else {
            float3 uvw = ((local / extents) + 1.0) * 0.5;
            inside = e.y != 0u && !(uvw.x < 0.0 || uvw.y < 0.0 || uvw.z < 0.0 || uvw.x > 1.0 ||
                                    uvw.y > 1.0 || uvw.z > 1.0);
            if (inside) {
                float3 s = (sample_field(e.y, uvw) * -2.0) + float3(1.0, 1.0, 1.0);
                dir = basis(place, safe_normalize(s));
                amount = length_of(s);
            }
        }
        if (inside) {
            amount = attractor_power(amount, attenuation);
            dir = safe_normalize(mix(dir, place.z, directionality));
            force -= (dir * amount) * strength;
        }
    }
    return force;
}

// An SDF's distance from a half float's bits, made into a float's bits; subnormal halves read
// as 0, as on the CPU.
float sdf_value(uint half_bits) {
    uint exponent = (half_bits >> 10u) & 0x1Fu;
    uint sign_bit = (half_bits & 0x8000u) << 16u;
    uint mantissa = (half_bits & 0x3FFu) << 13u;
    if (exponent == 0u) {
        return 0.0;
    }
    return asfloat(sign_bit | (exponent == 31u ? 0x7F800000u : ((exponent + 112u) << 23u)) |
                   mantissa);
}

// An SDF's cell: {width, height, depth, 0}, then eight half floats a word, each word's two halves
// a lane, low bits first.
float sdf_cell(uint sdf, uint3 size, uint x, uint y, uint z) {
    uint index = (((z * size.y) + y) * size.x) + x;
    uint lane = data[sdf + 1u + (index >> 3u)][(index >> 1u) & 3u];
    return sdf_value((index & 1u) == 0u ? (lane & 0xFFFFu) : (lane >> 16u));
}

// An SDF's distance at uvw, filtered as sample_field filters a field's texels.
float sample_sdf(uint sdf, float3 uvw) {
    uint3 size = data[sdf].xyz;
    float3 texel = clamp((uvw * float3(size)) - 0.5, float3(0.0, 0.0, 0.0), float3(size - 1u));
    uint3 low = uint3(texel);
    uint3 high = min(low + 1u, size - 1u);
    float3 t = texel - float3(low);
    float near = mix(mix(sdf_cell(sdf, size, low.x, low.y, low.z),
                         sdf_cell(sdf, size, high.x, low.y, low.z), t.x),
                     mix(sdf_cell(sdf, size, low.x, high.y, low.z),
                         sdf_cell(sdf, size, high.x, high.y, low.z), t.x),
                     t.y);
    float far = mix(mix(sdf_cell(sdf, size, low.x, low.y, high.z),
                        sdf_cell(sdf, size, high.x, low.y, high.z), t.x),
                    mix(sdf_cell(sdf, size, low.x, high.y, high.z),
                        sdf_cell(sdf, size, high.x, high.y, high.z), t.x),
                    t.y);
    return mix(near, far, t.z);
}

// A height field's height: {width, height, 0, 0}, then four floats a word.
float height_texel(uint field, uint width, uint x, uint z) {
    uint index = (z * width) + x;
    return asfloat(data[field + 1u + (index >> 2u)][index & 3u]);
}

// A height field's height at uv, filtered as a linear sampler that clamps to the edge.
float sample_heights(uint field, float2 uv) {
    uint2 size = data[field].xy;
    float2 texel = clamp((uv * float2(size)) - 0.5, float2(0.0, 0.0), float2(size - 1u));
    uint2 low = uint2(texel);
    uint2 high = min(low + 1u, size - 1u);
    float2 t = texel - float2(low);
    return mix(mix(height_texel(field, size.x, low.x, low.y),
                   height_texel(field, size.x, high.x, low.y), t.x),
               mix(height_texel(field, size.x, low.x, high.y),
                   height_texel(field, size.x, high.x, high.y), t.x),
               t.y);
}

// GLSL's sign: 0 for 0.
float3 sign_of(float3 v) {
    return float3(v.x > 0.0 ? 1.0 : (v.x < 0.0 ? -1.0 : 0.0),
                  v.y > 0.0 ? 1.0 : (v.y < 0.0 ? -1.0 : 0.0),
                  v.z > 0.0 ? 1.0 : (v.z < 0.0 ? -1.0 : 0.0));
}

// How near a particle comes to a collider before it touches: collision_size, times the average
// of its axes' lengths with collision_use_scale, as Godot's.
float collision_size(Particle3D particle, Step step) {
    if (!flag(kCollisionUseScale)) {
        return step.collision_size;
    }
    float scale = length_of(particle.x.xyz) + length_of(particle.y.xyz) + length_of(particle.z.xyz);
    return step.collision_size * (scale * 0.33333333333);
}

// particles.glsl's collider loop: each sphere, box, SDF and height field the particle at pos of
// the given size touches, their ways out added up so it leaves them all. The loop runs to its end, as
// attractor_force's.
Contact collider_contact(Step step, float3 pos, float size) {
    // Particles just touching a collider count as touching it.
    const float kEpsilon = 0.001;
    Contact contact;
    contact.collided = false;
    contact.normal = float3(0.0, 0.0, 0.0);
    contact.depth = 0.0;
    for (uint i = 0u; i < step.collider_count; ++i) {
        uint at = step.colliders_at + (i * kColliderWords);
        float4 a = asfloat(data[at]);
        float4 b = asfloat(data[at + 1u]);
        float4 c = asfloat(data[at + 2u]);
        Affine place;
        place.x = a.xyz;
        place.y = float3(a.w, b.xy);
        place.z = float3(b.zw, c.x);
        place.origin = c.yzw;
        uint4 d = data[at + 3u];
        float3 ext = asfloat(d.xyz);
        uint4 e = data[at + 4u];

        float3 rel = pos - place.origin;
        float3 normal = float3(0.0, 0.0, 0.0);
        float depth = 0.0;
        bool touches = false;
        if (d.w == kColliderSphere) {
            float dist = length_of(rel) - (size + ext.x);
            if (dist <= kEpsilon) {
                touches = true;
                depth = -dist;
                normal = unit(rel);
            }
        } else if (d.w == kColliderSdf) {
            float3 local = float3(dot(rel, place.x), dot(rel, place.y), dot(rel, place.z));
            float3 abs_pos = abs(local);
            float extra = 0.0;
            if (abs_pos.x > ext.x || abs_pos.y > ext.y || abs_pos.z > ext.z) {
                // Outside the box: as far again as its nearest point.
                extra = length_of(abs_pos - min(abs_pos, ext));
            }
            if (!(extra > size) && e.x != 0u) {
                float3 uvw = ((local / ext) + 1.0) * 0.5;
                float dist = (sample_sdf(e.x, uvw) * asfloat(e.y)) + extra;
                if (dist <= size + kEpsilon) {
                    touches = true;
                    depth = size - dist;
                    float3 across = float3(
                        sample_sdf(e.x, uvw + float3(kEpsilon, 0.0, 0.0)) -
                            sample_sdf(e.x, uvw - float3(kEpsilon, 0.0, 0.0)),
                        sample_sdf(e.x, uvw + float3(0.0, kEpsilon, 0.0)) -
                            sample_sdf(e.x, uvw - float3(0.0, kEpsilon, 0.0)),
                        sample_sdf(e.x, uvw + float3(0.0, 0.0, kEpsilon)) -
                            sample_sdf(e.x, uvw - float3(0.0, 0.0, kEpsilon)));
                    normal = basis(place, unit(across));
                }
            }
        } else if (d.w != kColliderBox) {
            // A height field, whose normal and depth Godot leaves in its space, unturned.
            const float kDelta = 1.0 / 8192.0;
            float3 local = float3(dot(rel, place.x), dot(rel, place.y), dot(rel, place.z));
            float3 bottom = float3(local.x, local.y - size, local.z);
            if (!(abs(bottom.x) > ext.x || abs(bottom.y) > ext.y || abs(bottom.z) > ext.z) &&
                e.x != 0u) {
                float3 uvw = ((bottom / ext) + 1.0) * 0.5;
                float y = sample_heights(e.x, uvw.xz);
                if (y + kEpsilon >= uvw.y) {
                    float3 pos1 = ((float3(uvw.x, y, uvw.z) * 2.0) - 1.0) * ext;
                    float3 pos2 =
                        ((float3(uvw.x + kDelta, sample_heights(e.x, uvw.xz + float2(kDelta, 0.0)),
                                 uvw.z) *
                          2.0) -
                         1.0) *
                        ext;
                    float3 pos3 =
                        ((float3(uvw.x, sample_heights(e.x, uvw.xz + float2(0.0, kDelta)),
                                 uvw.z + kDelta) *
                          2.0) -
                         1.0) *
                        ext;
                    touches = true;
                    normal = unit(cross(pos1 - pos2, pos1 - pos3));
                    depth = dot(normal, pos1) - dot(normal, bottom);
                }
            }
        } else {
            float3 local = float3(dot(rel, place.x), dot(rel, place.y), dot(rel, place.z));
            float3 abs_pos = abs(local);
            float3 sgn = sign_of(local);
            if (abs_pos.x > ext.x || abs_pos.y > ext.y || abs_pos.z > ext.z) {
                // Outside the box: from its nearest point.
                float3 out_ = abs_pos - min(abs_pos, ext);
                depth = length_of(out_) - size;
                if (depth <= kEpsilon) {
                    touches = true;
                    normal = basis(place, unit(out_) * sgn);
                    depth = -depth;
                }
            } else {
                // Inside: out through the nearest face.
                float3 axis_len = ext - abs_pos;
                float3 axis = float3(0.0, 0.0, 1.0);
                if (axis_len.x < axis_len.y && axis_len.x < axis_len.z) {
                    axis = float3(1.0, 0.0, 0.0);
                } else if (axis_len.y < axis_len.x && axis_len.y < axis_len.z) {
                    axis = float3(0.0, 1.0, 0.0);
                }
                touches = true;
                depth = dot(axis * axis_len, float3(1.0, 1.0, 1.0)) + size;
                normal = basis(place, axis * sgn);
            }
        }
        if (touches) {
            if (!contact.collided) {
                contact.collided = true;
                contact.normal = normal;
                contact.depth = depth;
            } else {
                float3 sum = contact.normal * contact.depth;
                sum += normal * max(0.0, depth - dot(normal, sum));
                contact.normal = unit(sum);
                contact.depth = length_of(sum);
            }
        }
    }
    return contact;
}

void process(inout Particle3D particle, Step step, uint number, float delta, Contact contact,
             inout bool active) {
    uint seed = hash(number + 1u + step.seed);
    Display display;
    Dynamics dynamics;
    Physical physical;
    initial_params(seed, display, dynamics, physical);

    particle.custom.y += delta / step.lifetime;
    particle.custom.y = mix(particle.custom.y, 1.0, step.interp_to_end);
    float life = particle.custom.y / display.lifetime;
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }

    Affine emission = step.emission;
    float3 origin = particle.origin.xyz;
    float3 controlled = orbit_displacement(dynamics, life, origin, emission, delta);
    controlled += radial_displacement(dynamics, life, seed, origin, emission, delta);
    if (has_curve3(kDirectionalCurve)) {
        float3 directional = sample3(kDirectionalCurve, life, 0.0) * dynamics.directional_velocity;
        if (flag(kDirectionalGlobal)) {
            directional = basis(emission, directional);
        }
        controlled += directional;
    }

    physical.linear_accel *= sample_curve(kLinearAccel, life, 1.0);
    physical.radial_accel *= sample_curve(kRadialAccel, life, 1.0);
    physical.tangent_accel *= sample_curve(kTangentialAccel, life, 1.0);
    physical.damping *= sample_curve(kDamping, life, 1.0);

    float3 velocity = particle.velocity;
    float3 gravity = word(7u).xyz;
    float3 force = gravity;
    if (length_of(velocity) > 0.0) {
        force += unit(velocity) * physical.linear_accel;
    }
    float3 diff = origin - emission.origin;
    if (length_of(diff) > 0.0) {
        force += unit(diff) * physical.radial_accel;
    }
    if (flag(kDisableZ)) {
        if (length_of(float3(diff.y, diff.x, 0.0)) > 0.0) {
            force += unit(float3(-diff.y, diff.x, 0.0)) * physical.tangent_accel;
        }
    } else {
        // Godot's normalize of a zero vector makes NaNs, which the length test then skips.
        float3 across = cross(unit(diff), unit(gravity));
        if (length_of(across) > 0.0) {
            force += unit(across) * physical.tangent_accel;
        }
    }
    if (flag(kAttractors) && step.attractor_count > 0u) {
        force += attractor_force(step, origin);
    }
    if (flag(kDisableZ)) {
        force.z = 0.0;
    }
    velocity += force * delta;
    if (physical.damping > 0.0) {
        float v = length_of(velocity);
        if (!flag(kDampingAsFriction)) {
            v -= physical.damping * delta;
        } else if (v > 0.001) {
            v -= v * v * physical.damping * 0.05 * delta;
        }
        velocity = v < 0.0 ? float3(0.0, 0.0, 0.0) : unit(velocity) * v;
    }

    bool bounced = contact.collided && flag(kCollisionRigid);
    if (bounced) {
        float friction = asfloat(word_uint(kGradientCountsAt, 2u));
        float bounce = asfloat(word_uint(kGradientCountsAt, 3u));
        float response = dot(contact.normal, velocity);
        float bounces = abs(response) >= 2.0 / clamp(bounce + 1.0, 1.0, 2.0) ? 1.0 : 0.0;
        origin += contact.normal * contact.depth;
        velocity -= contact.normal * response;
        velocity = mix(velocity, float3(0.0, 0.0, 0.0), clamp(friction, 0.0, 1.0));
        velocity -= contact.normal * (response * (bounce * bounces));
    } else if (contact.collided && flag(kCollisionHide)) {
        active = false;
    }

    // Turbulence before limiting, but not in a bounce.
    if (flag(kTurbulence) && !bounced) {
        float influence = sample_curve(kTurbulenceInfluence, life, 1.0);
        float3 direction = unit(noise_direction(step, origin));
        float amount = clamp(dynamics.turbulence_influence * influence, 0.0, 1.0) *
                       (delta <= 0.0 ? 0.0 : 1.0);
        float boost = 1.0 + ((1.0 - amount) * 0.2);
        velocity = mix(velocity, direction * (length_of(velocity) * boost), amount);
        controlled = mix(controlled, direction * (length_of(controlled) * boost), amount);
    }
    float3 final_velocity = controlled + velocity;
    if (curve_count(kVelocityLimitCurve) > 0u && length_of(final_velocity) > 0.001) {
        float limit = abs(sample_curve(kVelocityLimitCurve, life, 0.0));
        final_velocity = unit(final_velocity) * min(abs(length_of(final_velocity)), limit);
    }
    if (flag(kDisableZ)) {
        final_velocity.z = 0.0;
    }
    origin += final_velocity * delta;
    particle.velocity = velocity;

    display_at(display, life);

    float base_angle = dynamics.angle * sample_curve(kAngle, life, 1.0);
    base_angle += particle.custom.y * step.lifetime * dynamics.angular_velocity *
                  sample_curve(kAngularVelocity, life, 1.0);
    particle.custom.x = base_angle * kDegreesToRadians;
    particle.color = display.color;

    float3 x = particle.x.xyz;
    float3 y = particle.y.xyz;
    float3 z = particle.z.xyz;
    if (flag(kDisableZ)) {
        if (flag(kAlignY)) {
            y = unit(length_of(final_velocity) > 0.0 ? final_velocity : y);
            x = unit(cross(y, z));
        } else {
            float c = cos(particle.custom.x);
            float s = sin(particle.custom.x);
            x = float3(c, -s, 0.0);
            y = float3(s, c, 0.0);
        }
        z = float3(0.0, 0.0, 1.0);
        origin.z = 0.0;
    } else {
        if (flag(kAlignY)) {
            y = unit_or_else(length_of(final_velocity) > 0.0 ? final_velocity : y,
                             float3(0.0, 1.0, 0.0));
            if (all(y == unit_or_else(x, float3(1.0, 0.0, 0.0)))) {
                x = unit_or_else(cross(y, z), float3(1.0, 0.0, 0.0));
                z = unit_or_else(cross(x, y), float3(0.0, 0.0, 1.0));
            } else {
                z = unit_or_else(cross(x, y), float3(0.0, 0.0, 1.0));
                x = unit_or_else(cross(y, z), float3(1.0, 0.0, 0.0));
            }
        } else {
            x = unit_or_else(x, float3(1.0, 0.0, 0.0));
            y = unit_or_else(y, float3(0.0, 1.0, 0.0));
            z = unit_or_else(z, float3(0.0, 0.0, 1.0));
        }
        if (flag(kRotateY)) {
            float c = cos(particle.custom.x);
            float s = sin(particle.custom.x);
            x = float3(c, 0.0, -s);
            y = float3(0.0, 1.0, 0.0);
            z = float3(s, 0.0, c);
        }
    }
    if (has_curve3(kScaleOverVelocityCurve)) {
        float speed = length_of(final_velocity);
        float range = word(10u).x; // scale_over_velocity_max - scale_over_velocity_min
        float at = speed > 0.001 ? clamp(speed / range, 0.0, 1.0) : 0.0;
        display.scale = display.scale * sample3(kScaleOverVelocityCurve, at, 0.0);
    }
    particle.x = float4(x * scaled(display.scale.x), 0.0);
    particle.y = float4(y * scaled(display.scale.y), 0.0);
    particle.z = float4(z * scaled(display.scale.z), 0.0);
    particle.origin = float4(origin, 0.0);
    particle.custom.z = display.animation_offset + (life * display.animation_speed);
    if (particle.custom.y > particle.custom.w) {
        active = false;
    }
}

Step read_step(uint at) {
    Step step;
    float4 a = asfloat(data[at]);
    float4 b = asfloat(data[at + 1u]);
    float4 c = asfloat(data[at + 2u]);
    step.emission.x = a.xyz;
    step.emission.y = float3(a.w, b.xy);
    step.emission.z = float3(b.zw, c.x);
    step.emission.origin = c.yzw;
    float4 d = asfloat(data[at + 3u]);
    step.emitter_velocity = d.xyz;
    step.system_phase = d.w;
    float4 e = asfloat(data[at + 4u]);
    step.prev_system_phase = e.x;
    step.delta = e.y;
    step.lifetime = e.z;
    step.explosiveness = e.w;
    float4 f = asfloat(data[at + 5u]);
    step.randomness = f.x;
    step.amount_ratio = f.y;
    step.interp_to_end = f.z;
    step.time = f.w;
    uint4 g = data[at + 6u];
    step.cycle = g.x;
    step.seed = g.y;
    step.emitting = (g.z & 1u) != 0u;
    step.fractional_delta = (g.z & 2u) != 0u;
    step.clear = (g.z & 4u) != 0u;
    step.attractor_count = (g.z >> kStepAttractorShift) & 0xFFu;
    step.attractors_at = g.w;
    step.collider_count = g.z >> kStepColliderShift;
    uint4 h = data[at + 7u];
    step.colliders_at = h.x;
    step.collision_size = asfloat(h.y);
    return step;
}

Particle3D empty_particle() {
    Particle3D particle;
    particle.x = float4(1.0, 0.0, 0.0, 0.0);
    particle.y = float4(0.0, 1.0, 0.0, 0.0);
    particle.z = float4(0.0, 0.0, 1.0, 0.0);
    particle.origin = float4(0.0, 0.0, 0.0, 0.0);
    particle.velocity = float3(0.0, 0.0, 0.0);
    particle.flags = 0u;
    particle.color = float4(1.0, 1.0, 1.0, 1.0);
    particle.custom = float4(0.0, 0.0, 0.0, 0.0);
    particle.userdata = float4(0.0, 0.0, 0.0, 0.0);
    return particle;
}

// One step of a particle: restarts it in its turn while emitting, then moves it.
void step_particle(inout Particle3D particle, uint index, Step step) {
    if (step.clear) {
        particle = empty_particle();
    }
    particle.flags &= ~kParticleStarted;
    float delta = step.delta;
    bool restart = false;
    if (step.emitting) {
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
            particle.flags =
                kParticleActive | kParticleStarted | (step.cycle << kParticleCycleShift);
        }
    }
    bool active = (particle.flags & kParticleActive) != 0u;
    uint number = ((particle.flags >> kParticleCycleShift) * amount) + index;
    if (restart && active) {
        start(particle, step, number, active);
    }
    Contact contact;
    contact.collided = false;
    contact.normal = float3(0.0, 0.0, 0.0);
    contact.depth = 0.0;
    if (active && (flag(kCollisionRigid) || flag(kCollisionHide)) && step.collider_count > 0u) {
        contact = collider_contact(step, particle.origin.xyz, collision_size(particle, step));
    }
    if (active) {
        process(particle, step, number, delta, contact, active);
    }
    particle.flags &= ~kParticleActive;
    if (active) {
        particle.flags |= kParticleActive;
    }
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= amount) {
        return;
    }
    Particle3D particle = particles[index];
    if (reset != 0u) {
        particle = empty_particle();
    }
    for (uint i = 0u; i < step_count; ++i) {
        Step step = read_step(steps_offset + (i * kStepWords));
        step_particle(particle, index, step);
    }
    particles[index] = particle;
}
