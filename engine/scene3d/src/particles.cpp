#include "slash/scene3d/particles.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

// Ported from Godot 4.6's scene/3d/cpu_particles_3d.cpp (_particles_process) and
// scene/resources/material.cpp (BaseMaterial3D's billboard and particle animation code), MIT
// licensed. Emission timing and random values follow CPUParticles2D's port
// (engine/canvas/src/particles.cpp), so the two behave alike tick for tick.
namespace slash::scene3d {
namespace {

using world::Affine3D;
using world::CPUParticles3D;
using world::GradientPoint;

constexpr float kTau = 2.0F * std::numbers::pi_v<float>;
constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0F;
constexpr float kCmpEpsilon = 0.00001F; // Godot's CMP_EPSILON
constexpr int kMaxAmount = 65536;

// The random values each particle takes when it is emitted.
enum Value : std::uint8_t {
    kLifetime,
    kPhase,
    kSpread,
    kSpread2,
    kSpeed,
    kAngle,
    kAngularVelocity,
    kOrbit,
    kLinear,
    kRadial,
    kTangential,
    kDamping,
    kScale,
    kHue,
    kAnimSpeed,
    kAnimOffset,
    kInitialColor,
    kShapeA,
    kShapeB,
    kShapeC,
    kPoint,
    kValueCount
};

// Scrambles bits, so neighboring inputs give unrelated outputs (a 32-bit hash finalizer).
std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7feb352dU;
    x ^= x >> 15U;
    x *= 0x846ca68bU;
    x ^= x >> 16U;
    return x;
}

// A random number from 0 up to 1: the same for the same seed, cycle, slot and value.
float random(std::uint32_t seed, std::uint32_t cycle, std::uint32_t slot, Value value) {
    const std::uint32_t h = mix(seed ^ mix(cycle + 0x9e3779b9U));
    const std::uint32_t r = mix(h ^ mix((slot * 0x85ebca6bU) + value));
    return static_cast<float>(r >> 8U) * (1.0F / 16777216.0F);
}

float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// v at length 1, as Godot's Vector3::normalized: the zero vector stays zero.
Vec3 normalized(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

// Godot's Basis::orthonormalize: Gram-Schmidt over the columns, x first.
void orthonormalize(Affine3D& basis) {
    basis.x = normalized(basis.x);
    basis.y = normalized(basis.y - (basis.x * dot(basis.x, basis.y)));
    basis.z =
        normalized(basis.z - (basis.x * dot(basis.x, basis.z)) - (basis.y * dot(basis.y, basis.z)));
}

// v turned by angle radians about a unit axis, as Godot's Vector3::rotate.
Vec3 rotated(Vec3 v, Vec3 axis, float angle) {
    const Vec2 cs = cos_sin(angle);
    return (v * cs.x) + (cross(axis, v) * cs.y) + (axis * (dot(axis, v) * (1.0F - cs.x)));
}

// x to the power p, for x from 0 to 1, the same bits on every platform.
float power(float x, float p) {
    return x > 0.0F ? slash::exp(p * slash::ln(x)) : 0.0F;
}

// A curve's points, sorted by offset.
std::vector<Vec2> sorted_curve(const std::vector<Vec2>& curve) {
    std::vector<Vec2> out = curve;
    std::ranges::stable_sort(out, [](Vec2 a, Vec2 b) { return a.x < b.x; });
    return out;
}

std::vector<GradientPoint> sorted_gradient(const std::vector<GradientPoint>& gradient) {
    std::vector<GradientPoint> out = gradient;
    std::ranges::stable_sort(
        out, [](const GradientPoint& a, const GradientPoint& b) { return a.offset < b.offset; });
    return out;
}

// A curve's value at t: its points joined by straight lines and level past its ends, or 1 if it
// has none.
float sample(std::span<const Vec2> curve, float t) {
    if (curve.empty()) {
        return 1.0F;
    }
    if (t <= curve.front().x) {
        return curve.front().y;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (t < curve[i].x) {
            const Vec2 a = curve[i - 1];
            const Vec2 b = curve[i];
            return lerp(a.y, b.y, (t - a.x) / (b.x - a.x));
        }
    }
    return curve.back().y;
}

Vec4 sample(std::span<const GradientPoint> gradient, float t) {
    if (gradient.empty()) {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    if (t <= gradient.front().offset) {
        return gradient.front().color;
    }
    for (std::size_t i = 1; i < gradient.size(); ++i) {
        if (t < gradient[i].offset) {
            const GradientPoint& a = gradient[i - 1];
            const GradientPoint& b = gradient[i];
            const float f = (t - a.offset) / (b.offset - a.offset);
            return {lerp(a.color.x, b.color.x, f), lerp(a.color.y, b.color.y, f),
                    lerp(a.color.z, b.color.z, f), lerp(a.color.w, b.color.w, f)};
        }
    }
    return gradient.back().color;
}

Vec4 multiply(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

// Turns a color's hue by an angle in radians, as Godot's particles do: a rotation about the
// gray axis in YIQ space, applied through its transpose (Basis::xform_inv).
Vec4 turn_hue(Vec4 color, float radians) {
    if (radians == 0.0F) {
        return color;
    }
    const Vec2 cs = cos_sin(radians);
    constexpr std::array<std::array<float, 3>, 3> kGray{
        {{0.299F, 0.587F, 0.114F}, {0.299F, 0.587F, 0.114F}, {0.299F, 0.587F, 0.114F}}};
    constexpr std::array<std::array<float, 3>, 3> kCos{
        {{0.701F, -0.587F, -0.114F}, {-0.299F, 0.413F, -0.114F}, {-0.300F, -0.588F, 0.886F}}};
    constexpr std::array<std::array<float, 3>, 3> kSin{
        {{0.168F, 0.330F, -0.497F}, {-0.328F, 0.035F, 0.292F}, {1.250F, -1.050F, -0.203F}}};
    const std::array<float, 3> rgb{color.x, color.y, color.z};
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            const float m =
                kGray.at(j).at(i) + (kCos.at(j).at(i) * cs.x) + (kSin.at(j).at(i) * cs.y);
            out.at(i) += m * rgb.at(j);
        }
    }
    return {out[0], out[1], out[2], color.w};
}

// A CPUParticles3D's settings readied for a tick: curves and gradients sorted.
struct Settings {
    explicit Settings(const CPUParticles3D& s)
        : component(&s), angular_velocity(sorted_curve(s.angular_velocity_curve)),
          orbit_velocity(sorted_curve(s.orbit_velocity_curve)),
          linear_accel(sorted_curve(s.linear_accel_curve)),
          radial_accel(sorted_curve(s.radial_accel_curve)),
          tangential_accel(sorted_curve(s.tangential_accel_curve)),
          damping(sorted_curve(s.damping_curve)), angle(sorted_curve(s.angle_curve)),
          scale_amount(sorted_curve(s.scale_amount_curve)), scale_x(sorted_curve(s.scale_curve_x)),
          scale_y(sorted_curve(s.scale_curve_y)), scale_z(sorted_curve(s.scale_curve_z)),
          hue_variation(sorted_curve(s.hue_variation_curve)),
          anim_speed(sorted_curve(s.anim_speed_curve)),
          anim_offset(sorted_curve(s.anim_offset_curve)), color_ramp(sorted_gradient(s.color_ramp)),
          color_initial_ramp(sorted_gradient(s.color_initial_ramp)),
          direction(normalized(s.direction)),
          amount(static_cast<std::size_t>(std::clamp(s.amount, 1, kMaxAmount))),
          lifetime(std::max(static_cast<double>(s.lifetime), 0.001)) {
        if (direction == Vec3{}) {
            direction = {0.0F, 0.0F, 1.0F};
        }
    }

    const CPUParticles3D* component;
    std::vector<Vec2> angular_velocity;
    std::vector<Vec2> orbit_velocity;
    std::vector<Vec2> linear_accel;
    std::vector<Vec2> radial_accel;
    std::vector<Vec2> tangential_accel;
    std::vector<Vec2> damping;
    std::vector<Vec2> angle;
    std::vector<Vec2> scale_amount;
    std::vector<Vec2> scale_x;
    std::vector<Vec2> scale_y;
    std::vector<Vec2> scale_z;
    std::vector<Vec2> hue_variation;
    std::vector<Vec2> anim_speed;
    std::vector<Vec2> anim_offset;
    std::vector<GradientPoint> color_ramp;
    std::vector<GradientPoint> color_initial_ramp;
    Vec3 direction;
    std::size_t amount;
    double lifetime;
};

struct Slot {
    Particle3D particle;
    bool alive = false;
    std::array<float, kValueCount> random{};
    // Its pick of color_initial_ramp times its point's color, Godot's start_color_rand and
    // base_color.
    Vec4 tint{1.0F, 1.0F, 1.0F, 1.0F};
};

// One CPUParticles3D's particles and where its cycle is.
struct Emitter {
    std::vector<Slot> slots;
    double time = 0.0; // seconds into the current cycle
    std::uint32_t cycle = 0;
    bool started = false; // preprocess has run
    bool was_emitting = false;
    bool done = false;    // a one_shot's cycle ended; it emits again once restarted
    bool running = false; // a one_shot emitted and hasn't reported finished
};

// The second each slot emits at in a cycle, as CPUParticles2D's: evenly over the cycle, moved
// later at random by up to a slot's share times randomness, and squeezed toward the cycle's start
// by explosiveness, as Godot's restart_phase.
double emit_time(const Settings& settings, std::uint32_t seed, std::uint32_t cycle,
                 std::uint32_t slot) {
    const CPUParticles3D& s = *settings.component;
    const auto amount = static_cast<float>(settings.amount);
    float phase = static_cast<float>(slot) / amount;
    if (s.randomness > 0.0F) {
        phase += std::clamp(s.randomness, 0.0F, 1.0F) * random(seed, cycle, slot, kPhase) / amount;
    }
    phase *= 1.0F - std::clamp(s.explosiveness, 0.0F, 1.0F);
    return static_cast<double>(phase) * settings.lifetime;
}

class Simulation {
public:
    Simulation(const Settings& settings, const Affine3D& transform, std::uint32_t seed)
        : settings_(settings), s_(*settings.component), transform_(transform), seed_(seed),
          origin_(s_.local_coords ? Vec3{} : transform.origin) {}

    // Moves the emitter on by seconds of game time. Returns whether a one_shot's cycle ended.
    bool step(Emitter& e, float seconds) const {
        if (e.slots.size() != settings_.amount) {
            // A new amount starts over, as in Godot.
            e.slots.assign(settings_.amount, Slot{});
            e.time = 0.0;
        }
        if (s_.emitting && !e.was_emitting && s_.one_shot) {
            // Emitting again starts a one_shot's cycle over, with new random values.
            e.time = 0.0;
            e.done = false;
            ++e.cycle;
        }
        e.was_emitting = s_.emitting;
        const bool emitting = s_.emitting && !e.done;
        for (Slot& slot : e.slots) {
            slot.particle.previous = slot.particle.transform.origin;
        }
        const double delta =
            static_cast<double>(seconds) * static_cast<double>(std::max(s_.speed_scale, 0.0F));
        if (delta <= 0.0) {
            return false;
        }
        const double lifetime = settings_.lifetime;
        const double before = e.time;
        double now = before + delta;
        const bool wrapped = now >= lifetime;
        if (wrapped) {
            now = std::fmod(now, lifetime);
        }
        e.time = now;
        const std::uint32_t old_cycle = e.cycle;
        if (wrapped) {
            ++e.cycle;
        }
        const bool emit_new = emitting && !(wrapped && s_.one_shot);
        for (std::size_t i = 0; i < e.slots.size(); ++i) {
            Slot& slot = e.slots[i];
            const auto index = static_cast<std::uint32_t>(i);
            // When this slot emits during the step, if it does: how long ago, and in which cycle.
            // A step covers from its start up to, but not including, its end.
            std::optional<std::pair<double, std::uint32_t>> emitted;
            if (emitting) {
                const double at = emit_time(settings_, seed_, old_cycle, index);
                if (at >= before && (wrapped || at < now)) {
                    emitted = {wrapped ? lifetime - at + now : now - at, old_cycle};
                }
            }
            if (wrapped && emit_new) {
                if (const double at = emit_time(settings_, seed_, e.cycle, index); at < now) {
                    emitted = {now - at, e.cycle};
                }
            }
            if (emitted) {
                emit(slot, index, emitted->second);
                e.running = e.running || s_.one_shot;
                advance(slot, static_cast<float>(emitted->first));
            } else if (slot.alive) {
                advance(slot, static_cast<float>(delta));
            }
            if (slot.alive && slot.particle.age >= slot.particle.lifetime) {
                slot.alive = false;
            }
        }
        if (wrapped && s_.one_shot && emitting) {
            e.done = true;
            return true;
        }
        return false;
    }

private:
    // The velocity a particle starts with before its emission shape and the entity turn it, as
    // Godot's spread: in the xy plane with disable_z, else a cone about direction flattened
    // toward the plane across its y.
    [[nodiscard]] Vec3 start_velocity(const Slot& slot) const {
        const auto r = [&](Value v) { return slot.random.at(v); };
        const float speed = lerp(s_.initial_velocity_min, s_.initial_velocity_max, r(kSpeed));
        if (s_.particle_flag_disable_z) {
            const float angle = slash::atan2(s_.direction.y, s_.direction.x) +
                                (((r(kSpread) * 2.0F) - 1.0F) * s_.spread * kRadiansPerDegree);
            const Vec2 cs = cos_sin(angle);
            return Vec3{cs.x, cs.y, 0.0F} * speed;
        }
        const Vec2 a1 = cos_sin(((r(kSpread) * 2.0F) - 1.0F) * s_.spread * kRadiansPerDegree);
        const Vec2 a2 = cos_sin(((r(kSpread2) * 2.0F) - 1.0F) * (1.0F - s_.flatness) * s_.spread *
                                kRadiansPerDegree);
        const Vec3 spread{a1.y * a2.x, a2.y, a1.x * a2.x};
        const Vec3 forward = settings_.direction;
        Vec3 binormal = cross({0.0F, 1.0F, 0.0F}, forward);
        if (dot(binormal, binormal) < 0.00000001F) {
            // direction is along y: z is the binormal.
            binormal = {0.0F, 0.0F, 1.0F};
        }
        binormal = normalized(binormal);
        const Vec3 normal = cross(binormal, forward);
        return ((binormal * spread.x) + (normal * spread.y) + (forward * spread.z)) * speed;
    }

    // Where a particle starts in the entity's space, as Godot's emission shapes; it may turn its
    // velocity and tint it.
    [[nodiscard]] Vec3 start_position(Slot& slot, Vec3& velocity) const {
        const auto r = [&](Value v) { return slot.random.at(v); };
        const std::string& shape = s_.emission_shape;
        if (shape == "sphere" || shape == "sphere_surface") {
            const float s = (2.0F * r(kShapeA)) - 1.0F;
            const Vec2 turn = cos_sin(kTau * r(kShapeB));
            const float radius = s_.emission_sphere_radius * std::sqrt(1.0F - (s * s));
            const Vec3 surface{radius * turn.x, radius * turn.y, s_.emission_sphere_radius * s};
            // As Godot, a point inside the sphere is a straight blend toward its surface.
            return shape == "sphere" ? surface * r(kShapeC) : surface;
        }
        if (shape == "box") {
            return {((r(kShapeA) * 2.0F) - 1.0F) * s_.emission_box_extents.x,
                    ((r(kShapeB) * 2.0F) - 1.0F) * s_.emission_box_extents.y,
                    ((r(kShapeC) * 2.0F) - 1.0F) * s_.emission_box_extents.z};
        }
        if ((shape == "points" || shape == "directed_points") && !s_.emission_points.empty()) {
            const std::size_t count = s_.emission_points.size();
            const std::size_t at = std::min(
                count - 1, static_cast<std::size_t>(r(kPoint) * static_cast<float>(count)));
            if (shape == "directed_points" && at < s_.emission_normals.size()) {
                const Vec3 normal = s_.emission_normals[at];
                if (s_.particle_flag_disable_z) {
                    // The normal is the velocity's x axis, as Godot's Transform2D.
                    velocity = Vec3{normal.x, normal.y, 0.0F} * velocity.x +
                               Vec3{normal.y, -normal.x, 0.0F} * velocity.y;
                } else {
                    // The normal is the velocity's z axis, in a frame of its own.
                    const Vec3 v0 = std::abs(normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F}
                                                                : Vec3{0.0F, 1.0F, 0.0F};
                    const Vec3 tangent = normalized(cross(v0, normal));
                    const Vec3 bitangent = normalized(cross(tangent, normal));
                    velocity =
                        (tangent * velocity.x) + (bitangent * velocity.y) + (normal * velocity.z);
                }
            }
            if (at < s_.emission_colors.size()) {
                slot.tint = multiply(slot.tint, s_.emission_colors[at]);
            }
            return s_.emission_points[at];
        }
        if (shape == "ring") {
            const float radius = std::max(0.001F, s_.emission_ring_radius);
            const float height = s_.emission_ring_height;
            // tan(90 degrees - cone angle): how far in the side leans per meter up.
            const Vec2 lean = cos_sin((90.0F - s_.emission_ring_cone_angle) * kRadiansPerDegree);
            const float top =
                lean.x != 0.0F ? std::max(radius - ((lean.y / lean.x) * height), 0.0F) : 0.0F;
            const float skew = std::max(std::min(radius, top) / std::max(radius, top), 0.5F);
            float y = r(kShapeA);
            y = radius < top ? power(y, skew) : 1.0F - power(y, skew);
            const float inner = s_.emission_ring_inner_radius * s_.emission_ring_inner_radius;
            float across = std::sqrt((r(kShapeC) * ((radius * radius) - inner)) + inner);
            across = lerp(across, across * (top / radius), y);
            const Vec3 axis = s_.emission_ring_axis == Vec3{} ? Vec3{0.0F, 0.0F, 1.0F}
                                                              : normalized(s_.emission_ring_axis);
            const bool along_x = std::abs(axis.x) == 1.0F && axis.y == 0.0F && axis.z == 0.0F;
            Vec3 ortho = normalized(along_x ? cross({0.0F, 1.0F, 0.0F}, axis)
                                            : cross({1.0F, 0.0F, 0.0F}, axis));
            ortho = normalized(rotated(ortho, axis, r(kShapeB) * kTau));
            return (ortho * across) + (axis * ((y * height) - (height / 2.0F)));
        }
        return {};
    }

    // Starts a slot's particle where the emission shape says, with its random values.
    void emit(Slot& slot, std::uint32_t index, std::uint32_t cycle) const {
        for (std::size_t v = 0; v < kValueCount; ++v) {
            slot.random.at(v) = random(seed_, cycle, index, static_cast<Value>(v));
        }
        Particle3D& p = slot.particle;
        p = Particle3D{};
        p.index = index;
        p.lifetime =
            static_cast<float>(settings_.lifetime) *
            (1.0F - (std::clamp(s_.lifetime_randomness, 0.0F, 1.0F) * slot.random.at(kLifetime)));
        slot.tint = sample(settings_.color_initial_ramp, slot.random.at(kInitialColor));
        Vec3 velocity = start_velocity(slot);
        const Vec3 position = start_position(slot, velocity);
        if (s_.local_coords) {
            p.transform.origin = position;
        } else {
            // As Godot, it starts with the entity's basis, which its flags may turn later.
            p.transform = transform_;
            p.transform.origin = transform_.apply(position);
            velocity = transform_.basis(velocity);
        }
        if (s_.particle_flag_disable_z) {
            velocity.z = 0.0F;
            p.transform.origin.z = 0.0F;
        }
        p.velocity = velocity;
        p.previous = p.transform.origin;
        slot.alive = true;
    }

    // Moves a particle on by seconds and works out its look at its new age, as Godot's
    // CPUParticles3D: forces change its velocity, orbiting turns it about the emitter, damping
    // slows it, its flags turn it, its scale sizes it, then it moves by its velocity.
    void advance(Slot& slot, float seconds) const {
        Particle3D& p = slot.particle;
        p.age += seconds;
        const float t = std::clamp(p.age / p.lifetime, 0.0F, 1.0F);
        // A parameter's value for this particle at its age.
        const auto value = [&](float min, float max, Value which, std::span<const Vec2> curve) {
            return lerp(min, max, slot.random.at(which)) * sample(curve, t);
        };
        const bool flat = s_.particle_flag_disable_z;
        Vec3 force = s_.gravity;
        Vec3 position = p.transform.origin;
        if (flat) {
            position.z = 0.0F;
        }
        const float linear =
            value(s_.linear_accel_min, s_.linear_accel_max, kLinear, settings_.linear_accel);
        force = force + (normalized(p.velocity) * linear);
        const Vec3 diff = position - origin_;
        const float radial =
            value(s_.radial_accel_min, s_.radial_accel_max, kRadial, settings_.radial_accel);
        force = force + (normalized(diff) * radial);
        const float tangential = value(s_.tangential_accel_min, s_.tangential_accel_max,
                                       kTangential, settings_.tangential_accel);
        if (flat) {
            force = force + (normalized({-diff.y, diff.x, 0.0F}) * tangential);
        } else {
            // Round the entity across gravity, as Godot's: none without gravity.
            force =
                force + (normalized(cross(normalized(diff), normalized(s_.gravity))) * tangential);
        }
        p.velocity = p.velocity + (force * seconds);
        if (flat) {
            const float orbit = value(s_.orbit_velocity_min, s_.orbit_velocity_max, kOrbit,
                                      settings_.orbit_velocity);
            if (orbit != 0.0F) {
                const Vec2 turn = cos_sin(-orbit * seconds * kTau);
                p.transform.origin.x += ((diff.x * turn.x) - (diff.y * turn.y)) - diff.x;
                p.transform.origin.y += ((diff.x * turn.y) + (diff.y * turn.x)) - diff.y;
            }
        }
        const float damping = value(s_.damping_min, s_.damping_max, kDamping, settings_.damping);
        if (damping > 0.0F) {
            const float speed = length(p.velocity) - (damping * seconds);
            p.velocity = speed > 0.0F ? normalized(p.velocity) * speed : Vec3{};
        }
        p.angle = (value(s_.angle_min, s_.angle_max, kAngle, settings_.angle) +
                   (p.age * value(s_.angular_velocity_min, s_.angular_velocity_max,
                                  kAngularVelocity, settings_.angular_velocity))) *
                  kRadiansPerDegree;
        p.frame =
            value(s_.anim_offset_min, s_.anim_offset_max, kAnimOffset, settings_.anim_offset) +
            (t * value(s_.anim_speed_min, s_.anim_speed_max, kAnimSpeed, settings_.anim_speed));

        // Its color: color times color_ramp, its hue turned, times its tint.
        const Vec4 color = multiply(s_.color, sample(settings_.color_ramp, t));
        p.color = multiply(turn_hue(color, kTau * value(s_.hue_variation_min, s_.hue_variation_max,
                                                        kHue, settings_.hue_variation)),
                           slot.tint);

        turn(p);
        Vec3 scale{1.0F, 1.0F, 1.0F};
        if (s_.split_scale) {
            scale = {sample(settings_.scale_x, t), sample(settings_.scale_y, t),
                     sample(settings_.scale_z, t)};
        } else {
            const float amount = sample(settings_.scale_amount, t);
            scale = {amount, amount, amount};
        }
        const float base = lerp(s_.scale_amount_min, s_.scale_amount_max, slot.random.at(kScale));
        scale = {std::max(scale.x * base, kCmpEpsilon), std::max(scale.y * base, kCmpEpsilon),
                 std::max(scale.z * base, kCmpEpsilon)};
        // As Godot's Basis::scale, which scales its rows: along the world's axes.
        for (Vec3* axis : {&p.transform.x, &p.transform.y, &p.transform.z}) {
            *axis = {axis->x * scale.x, axis->y * scale.y, axis->z * scale.z};
        }
        if (flat) {
            p.velocity.z = 0.0F;
            p.transform.origin.z = 0.0F;
        }
        p.transform.origin = p.transform.origin + (p.velocity * seconds);
    }

    // Turns a particle's basis as its flags say, then makes it orthonormal.
    void turn(Particle3D& p) const {
        Affine3D& b = p.transform;
        const bool moving = length(p.velocity) > 0.0F;
        if (s_.particle_flag_disable_z) {
            if (s_.particle_flag_align_y) {
                if (moving) {
                    b.y = normalized(p.velocity);
                }
                b.x = normalized(cross(b.y, b.z));
                b.z = {0.0F, 0.0F, 1.0F};
            } else {
                const Vec2 cs = cos_sin(p.angle);
                b.x = {cs.x, -cs.y, 0.0F};
                b.y = {cs.y, cs.x, 0.0F};
                b.z = {0.0F, 0.0F, 1.0F};
            }
        } else {
            if (s_.particle_flag_align_y) {
                // Its y axis along its velocity.
                b.y = moving ? normalized(p.velocity) : normalized(b.y);
                if (b.y == b.x) {
                    b.x = normalized(cross(b.y, b.z));
                    b.z = normalized(cross(b.x, b.y));
                } else {
                    b.z = normalized(cross(b.x, b.y));
                    b.x = normalized(cross(b.y, b.z));
                }
            } else {
                orthonormalize(b);
            }
            if (s_.particle_flag_rotate_y) {
                const Vec2 cs = cos_sin(p.angle);
                b.x = {cs.x, 0.0F, -cs.y};
                b.y = {0.0F, 1.0F, 0.0F};
                b.z = {cs.y, 0.0F, cs.x};
            }
        }
        orthonormalize(b);
    }

    const Settings& settings_;
    const CPUParticles3D& s_;
    Affine3D transform_;
    std::uint32_t seed_;
    Vec3 origin_; // what radial and tangential acceleration and orbits go round
};

bool alive(const Emitter& emitter) {
    return std::ranges::any_of(emitter.slots, [](const Slot& slot) { return slot.alive; });
}

// Ticks of tick_seconds that cover seconds.
std::int64_t ticks_in(double seconds, double tick_seconds) {
    return static_cast<std::int64_t>(std::ceil((std::max(seconds, 0.0) / tick_seconds) - 1e-9));
}

// Runs preprocess, the first time, and then ticks steps. Returns whether a one_shot's cycle
// ended.
bool run(Emitter& emitter, const CPUParticles3D& component, const Affine3D& transform,
         std::uint32_t seed, double tick_seconds, std::int64_t ticks) {
    const Settings settings(component);
    const Simulation simulation(settings, transform, seed);
    if (!emitter.started) {
        emitter.started = true;
        ticks += ticks_in(component.preprocess, tick_seconds);
    }
    bool ended = false;
    for (std::int64_t i = 0; i < ticks; ++i) {
        ended = simulation.step(emitter, static_cast<float>(tick_seconds)) || ended;
    }
    return ended;
}

std::vector<Particle3D> live(const Emitter& emitter, std::string_view draw_order) {
    std::vector<Particle3D> out;
    for (const Slot& slot : emitter.slots) {
        if (slot.alive) {
            out.push_back(slot.particle);
        }
    }
    if (draw_order == "lifetime") {
        // The oldest first, so the youngest draw last, as Godot's SortLifetime.
        std::ranges::stable_sort(
            out, [](const Particle3D& a, const Particle3D& b) { return a.age > b.age; });
    }
    return out;
}

// What an entity's emitter is kept under: its ID, which outlives the entity handles that
// restoring a copy of the world (a failed command, a dry run, undo) hands out anew.
std::string key_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id);
}

// Every emitter's particles, kept with the world it runs in.
struct ParticleSystem3D {
    std::map<std::string, Emitter, std::less<>> emitters;

    void run_all(world::World& world, double tick_seconds) {
        auto& ecs = world.ecs();
        std::set<std::string, std::less<>> seen;
        const world::Tree3D tree(world);
        struct Run {
            entt::entity entity;
            CPUParticles3D* component;
            Emitter* emitter;
            Affine3D transform;
            std::uint32_t seed;
            bool ended = false;
        };
        std::vector<Run> runs;
        for (const entt::entity entity : ecs.view<CPUParticles3D>()) {
            auto& component = ecs.get<CPUParticles3D>(entity);
            const std::string key = key_of(world, entity);
            seen.insert(key);
            // Emitted from where it is after its parents.
            runs.push_back({.entity = entity,
                            .component = &component,
                            .emitter = &emitters[key],
                            .transform = tree.global(entity),
                            .seed = particle_seed(world.id_of(entity), component.seed)});
        }
        // Each emitter steps alone, so they share out over the job system's threads.
        if (tick_seconds > 0.0) {
            world.pool().parallel_for(runs.size(), [&](std::size_t i) {
                Run& each = runs[i];
                each.ended =
                    run(*each.emitter, *each.component, each.transform, each.seed, tick_seconds, 1);
            });
        }
        for (const Run& each : runs) {
            if (each.ended) {
                // As Godot, a one_shot stops emitting when its cycle ends.
                each.component->emitting = false;
                each.emitter->was_emitting = false;
            }
            if (each.emitter->running && !each.component->emitting && !alive(*each.emitter)) {
                each.emitter->running = false;
                world.emit("finished", world.id_of(each.entity), {});
            }
        }
        std::erase_if(emitters, [&](const auto& entry) { return !seen.contains(entry.first); });
    }

    [[nodiscard]] world::MemoryPart memory() const {
        world::MemoryPart part{"cpu_particles_3d", 0, {}, {}};
        std::uint64_t slots = 0;
        std::uint64_t live = 0;
        for (const auto& [key, emitter] : emitters) {
            part.bytes += world::kMapNodeBytes<decltype(emitters)> + json::heap_bytes(key) +
                          (emitter.slots.capacity() * sizeof(Slot));
            slots += emitter.slots.size();
            live += static_cast<std::uint64_t>(
                std::ranges::count_if(emitter.slots, [](const Slot& slot) { return slot.alive; }));
        }
        part.counts = {{"emitters", emitters.size()}, {"particles", live}, {"slots", slots}};
        return part;
    }
};

} // namespace

std::uint32_t particle_seed(std::string_view id, int seed) {
    // FNV-1a over the ID's bytes, as canvas::particle_seed.
    std::uint32_t h = 2166136261U;
    for (const char c : id) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return mix(h ^ mix(static_cast<std::uint32_t>(seed)));
}

void add_particles_system(world::World& world) {
    if (world.ecs().ctx().contains<std::shared_ptr<ParticleSystem3D>>()) {
        return;
    }
    auto system = std::make_shared<ParticleSystem3D>();
    world.ecs().ctx().emplace<std::shared_ptr<ParticleSystem3D>>(system);
    world.add_reset([system](world::World& /*world*/) { system->emitters.clear(); });
    world.add_memory_source("cpu_particles_3d",
                            [system](const world::World& /*world*/) { return system->memory(); });
    world.add_system(
        "cpu_particles_3d",
        [system](world::World& w, double tick_seconds) { system->run_all(w, tick_seconds); },
        {.doc = "Emits, moves and ages each CPUParticles3D's particles.",
         .priority = 1100,
         .threaded = true,
         .reads = {"Transform3D"},
         .writes = {"CPUParticles3D"}});
}

std::vector<Particle3D> particles_3d(const world::World& world, entt::entity entity) {
    const auto* system = world.ecs().ctx().find<std::shared_ptr<ParticleSystem3D>>();
    const auto* component = world.ecs().try_get<CPUParticles3D>(entity);
    if (system == nullptr || component == nullptr) {
        return {};
    }
    const auto found = (*system)->emitters.find(key_of(world, entity));
    return found != (*system)->emitters.end() ? live(found->second, component->draw_order)
                                              : std::vector<Particle3D>{};
}

std::vector<Particle3D> simulate(const CPUParticles3D& settings, const Affine3D& transform,
                                 std::uint32_t seed, double seconds, double tick_seconds) {
    if (tick_seconds <= 0.0) {
        return {};
    }
    CPUParticles3D copy = settings;
    Emitter emitter;
    // Tick 0 runs only preprocess.
    for (std::int64_t i = 0, ticks = ticks_in(seconds, tick_seconds); i <= ticks; ++i) {
        if (run(emitter, copy, transform, seed, tick_seconds, i == 0 ? 0 : 1)) {
            copy.emitting = false;
            emitter.was_emitting = false;
        }
    }
    return live(emitter, copy.draw_order);
}

void sort_by_depth(std::vector<Particle3D>& particles, Vec3 camera_z) {
    std::ranges::stable_sort(particles, [camera_z](const Particle3D& a, const Particle3D& b) {
        return dot(a.transform.origin, camera_z) < dot(b.transform.origin, camera_z);
    });
}

Billboard billboard_of(std::string_view mode) {
    if (mode == "enabled") {
        return Billboard::Enabled;
    }
    if (mode == "fixed_y") {
        return Billboard::FixedY;
    }
    if (mode == "particles") {
        return Billboard::Particles;
    }
    return Billboard::Disabled;
}

Affine3D billboard(const Affine3D& model, const Affine3D& camera, Billboard mode, bool keep_scale,
                   float angle) {
    if (mode == Billboard::Disabled) {
        return model;
    }
    Affine3D out;
    out.origin = model.origin;
    if (mode == Billboard::Enabled) {
        // The camera's own basis, as MAIN_CAM_INV_VIEW_MATRIX's.
        out.x = camera.x;
        out.y = camera.y;
        out.z = camera.z;
    } else if (mode == Billboard::FixedY) {
        out.x = normalized(cross({0.0F, 1.0F, 0.0F}, camera.z));
        out.y = {0.0F, 1.0F, 0.0F};
        out.z = normalized(cross(camera.x, {0.0F, 1.0F, 0.0F}));
    } else {
        // The camera's axes at length 1, turned by the particle's angle about its z.
        const Vec3 x = normalized(camera.x);
        const Vec3 y = normalized(camera.y);
        const Vec2 cs = cos_sin(angle);
        out.x = (x * cs.x) - (y * cs.y);
        out.y = (x * cs.y) + (y * cs.x);
        out.z = normalized(camera.z);
    }
    if (keep_scale) {
        out.x = out.x * length(model.x);
        out.y = out.y * length(model.y);
        out.z = out.z * length(model.z);
    }
    return out;
}

Vec2 particle_frame_uv(Vec2 uv, float frame, int h_frames, int v_frames, bool loop) {
    const auto h = static_cast<float>(std::max(h_frames, 1));
    const auto v = static_cast<float>(std::max(v_frames, 1));
    const float total = h * v;
    float at = std::floor(frame * total);
    if (!loop) {
        at = std::clamp(at, 0.0F, total - 1.0F);
    } else {
        // GLSL's mod: x - y * floor(x / y), so frames before the first wrap round too.
        at -= total * std::floor(at / total);
    }
    const float column = at - (h * std::floor(at / h));
    return {(uv.x / h) + (column / h), (uv.y / v) + (std::floor((at + 0.5F) / h) / v)};
}

} // namespace slash::scene3d
