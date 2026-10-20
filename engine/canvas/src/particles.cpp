#include "slash/canvas/particles.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/world/tree2d.hpp"

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

namespace slash::canvas {
namespace {

using world::CPUParticles2D;
using world::GradientPoint;

constexpr float kTau = 2.0F * std::numbers::pi_v<float>;
constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0F;
constexpr int kMaxAmount = 65536;

// The random values each particle takes when it is emitted.
enum Value : std::uint8_t {
    kLifetime,
    kPhase,
    kSpread,
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

float length(Vec2 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

Vec2 normalized(Vec2 v) {
    const float l = length(v);
    return l > 0.0F ? Vec2{v.x / l, v.y / l} : Vec2{};
}

// The angle of a vector in radians, from -pi to pi, as std::atan2 but computed the same way on
// every platform: a reduction to at most tan(pi/12), then a series, in double.
float angle_of(Vec2 v) {
    const double x = std::abs(static_cast<double>(v.x));
    const double y = std::abs(static_cast<double>(v.y));
    if (x == 0.0 && y == 0.0) {
        return 0.0F;
    }
    double a = std::min(x, y) / std::max(x, y); // 0 to 1
    double base = 0.0;
    constexpr double kTanPiOver12 = 0.2679491924311227;
    if (a > kTanPiOver12) {
        // atan(a) = pi/6 + atan((a * sqrt(3) - 1) / (a + sqrt(3)))
        a = ((a * std::numbers::sqrt3) - 1.0) / (a + std::numbers::sqrt3);
        base = std::numbers::pi / 6.0;
    }
    // atan(a) = a - a^3/3 + a^5/5 - ..., to a^21: good to 1e-14 for |a| <= tan(pi/12).
    const double a2 = a * a;
    double sum = 0.0;
    for (int n = 21; n >= 1; n -= 2) {
        sum = ((((n / 2) % 2 == 0) ? 1.0 : -1.0) / n) + (a2 * sum);
    }
    double angle = base + (a * sum);
    if (y > x) {
        angle = (std::numbers::pi / 2.0) - angle;
    }
    if (v.x < 0.0F) {
        angle = std::numbers::pi - angle;
    }
    return static_cast<float>(v.y < 0.0F ? -angle : angle);
}

// v turned by the angle whose {cos, sin} is turn.
Vec2 rotate(Vec2 v, Vec2 turn) {
    return {(v.x * turn.x) - (v.y * turn.y), (v.x * turn.y) + (v.y * turn.x)};
}

// A point or vector from the entity's space into the world's: scaled, skewed, turned, then
// moved.
Vec2 to_world(const world::Transform2D& transform, Vec2 local, bool is_point) {
    Vec2 out = rotate({local.x * transform.scale.x, local.y * transform.scale.y},
                      cos_sin(transform.rotation));
    if (transform.skew != 0.0F) {
        // The y axis leans by the skew: x along the rotation, y along rotation + skew.
        const Vec2 turn = cos_sin(transform.rotation);
        const Vec2 lean = cos_sin(transform.rotation + transform.skew);
        const float x = local.x * transform.scale.x;
        const float y = local.y * transform.scale.y;
        out = {(x * turn.x) - (y * lean.y), (x * turn.y) + (y * lean.x)};
    }
    return is_point ? out + transform.position : out;
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
// gray axis in YIQ space.
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

// A CPUParticles2D's settings readied for a tick: curves and gradients sorted.
struct Settings {
    explicit Settings(const CPUParticles2D& s)
        : component(&s), angular_velocity(sorted_curve(s.angular_velocity_curve)),
          orbit_velocity(sorted_curve(s.orbit_velocity_curve)),
          linear_accel(sorted_curve(s.linear_accel_curve)),
          radial_accel(sorted_curve(s.radial_accel_curve)),
          tangential_accel(sorted_curve(s.tangential_accel_curve)),
          damping(sorted_curve(s.damping_curve)), angle(sorted_curve(s.angle_curve)),
          scale_amount(sorted_curve(s.scale_amount_curve)),
          hue_variation(sorted_curve(s.hue_variation_curve)),
          anim_speed(sorted_curve(s.anim_speed_curve)),
          anim_offset(sorted_curve(s.anim_offset_curve)), color_ramp(sorted_gradient(s.color_ramp)),
          color_initial_ramp(sorted_gradient(s.color_initial_ramp)),
          direction(normalized(s.direction)),
          amount(static_cast<std::size_t>(std::clamp(s.amount, 1, kMaxAmount))),
          lifetime(std::max(static_cast<double>(s.lifetime), 0.001)) {
        if (direction == Vec2{}) {
            direction = {1.0F, 0.0F};
        }
    }

    const CPUParticles2D* component;
    std::vector<Vec2> angular_velocity;
    std::vector<Vec2> orbit_velocity;
    std::vector<Vec2> linear_accel;
    std::vector<Vec2> radial_accel;
    std::vector<Vec2> tangential_accel;
    std::vector<Vec2> damping;
    std::vector<Vec2> angle;
    std::vector<Vec2> scale_amount;
    std::vector<Vec2> hue_variation;
    std::vector<Vec2> anim_speed;
    std::vector<Vec2> anim_offset;
    std::vector<GradientPoint> color_ramp;
    std::vector<GradientPoint> color_initial_ramp;
    Vec2 direction;
    std::size_t amount;
    double lifetime;
};

struct Slot {
    Particle particle;
    bool alive = false;
    std::array<float, kValueCount> random{};
    Vec4 tint{1.0F, 1.0F, 1.0F, 1.0F}; // its pick of color_initial_ramp times its point's color
};

// One CPUParticles2D's particles and where its cycle is.
struct Emitter {
    std::vector<Slot> slots;
    double time = 0.0; // seconds into the current cycle
    std::uint32_t cycle = 0;
    bool started = false; // preprocess has run
    bool was_emitting = false;
    bool done = false;    // a one_shot's cycle ended; it emits again once restarted
    bool running = false; // a one_shot emitted and hasn't reported finished
};

// The second each slot emits at in a cycle, as Godot spreads them: evenly over the cycle,
// moved later at random by up to a slot's share times randomness, and squeezed toward the
// cycle's start by explosiveness.
double emit_time(const Settings& settings, std::uint32_t seed, std::uint32_t cycle,
                 std::uint32_t slot) {
    const CPUParticles2D& s = *settings.component;
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
    Simulation(const Settings& settings, const world::Transform2D& transform, std::uint32_t seed)
        : settings_(settings), s_(*settings.component), transform_(transform), seed_(seed),
          origin_(s_.local_coords ? Vec2{} : transform.position) {}

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
            slot.particle.previous = slot.particle.position;
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
            // As Godot, a step covers from its start up to, but not including, its end.
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
    // Starts a slot's particle where the emission shape says, with its random values.
    void emit(Slot& slot, std::uint32_t index, std::uint32_t cycle) const {
        for (std::size_t v = 0; v < kValueCount; ++v) {
            slot.random.at(v) = random(seed_, cycle, index, static_cast<Value>(v));
        }
        const auto r = [&](Value v) { return slot.random.at(v); };
        Particle& p = slot.particle;
        p = Particle{};
        p.index = index;
        p.lifetime = static_cast<float>(settings_.lifetime) *
                     (1.0F - (std::clamp(s_.lifetime_randomness, 0.0F, 1.0F) * r(kLifetime)));
        const float turn = s_.spread * kRadiansPerDegree * ((r(kSpread) * 2.0F) - 1.0F);
        Vec2 velocity = rotate(settings_.direction, cos_sin(turn)) *
                        lerp(s_.initial_velocity_min, s_.initial_velocity_max, r(kSpeed));
        Vec2 position;
        Vec4 tint = sample(settings_.color_initial_ramp, r(kInitialColor));
        const std::string& shape = s_.emission_shape;
        if (shape == "sphere" || shape == "sphere_surface") {
            const float radius = shape == "sphere"
                                     ? s_.emission_sphere_radius * std::sqrt(r(kShapeB))
                                     : s_.emission_sphere_radius;
            position = cos_sin(kTau * r(kShapeA)) * radius;
        } else if (shape == "rectangle") {
            position = {((r(kShapeA) * 2.0F) - 1.0F) * s_.emission_rect_extents.x,
                        ((r(kShapeB) * 2.0F) - 1.0F) * s_.emission_rect_extents.y};
        } else if (shape == "ring") {
            const float outer = s_.emission_ring_radius * s_.emission_ring_radius;
            const float inner = s_.emission_ring_inner_radius * s_.emission_ring_inner_radius;
            position = cos_sin(kTau * r(kShapeA)) *
                       std::sqrt(std::max((r(kShapeB) * (outer - inner)) + inner, 0.0F));
        } else if ((shape == "points" || shape == "directed_points") &&
                   !s_.emission_points.empty()) {
            const std::size_t count = s_.emission_points.size();
            const std::size_t at = std::min(
                count - 1, static_cast<std::size_t>(r(kPoint) * static_cast<float>(count)));
            position = s_.emission_points[at];
            if (shape == "directed_points" && at < s_.emission_normals.size()) {
                // As Godot: the normal is the velocity's x axis.
                const Vec2 normal = normalized(s_.emission_normals[at]);
                velocity = (normal * velocity.x) + (Vec2{normal.y, -normal.x} * velocity.y);
            }
            if (at < s_.emission_colors.size()) {
                tint = multiply(tint, s_.emission_colors[at]);
            }
        }
        if (!s_.local_coords) {
            position = to_world(transform_, position, true);
            velocity = to_world(transform_, velocity, false);
        }
        p.position = position;
        p.previous = position;
        p.velocity = velocity;
        slot.tint = tint;
        slot.alive = true;
    }

    // Moves a particle on by seconds and works out its look at its new age, as Godot's
    // CPUParticles2D: forces change its velocity, orbiting turns it about the emitter, damping
    // slows it, then it moves by its velocity.
    void advance(Slot& slot, float seconds) const {
        Particle& p = slot.particle;
        p.age += seconds;
        const float t = std::clamp(p.age / p.lifetime, 0.0F, 1.0F);
        // A parameter's value for this particle at its age.
        const auto value = [&](float min, float max, Value which, std::span<const Vec2> curve) {
            return lerp(min, max, slot.random.at(which)) * sample(curve, t);
        };
        Vec2 force = s_.gravity;
        const float linear =
            value(s_.linear_accel_min, s_.linear_accel_max, kLinear, settings_.linear_accel);
        if (linear != 0.0F) {
            force = force + (normalized(p.velocity) * linear);
        }
        const Vec2 offset = p.position - origin_;
        const float radial =
            value(s_.radial_accel_min, s_.radial_accel_max, kRadial, settings_.radial_accel);
        if (radial != 0.0F) {
            force = force + (normalized(offset) * radial);
        }
        const float tangential = value(s_.tangential_accel_min, s_.tangential_accel_max,
                                       kTangential, settings_.tangential_accel);
        if (tangential != 0.0F) {
            force = force + (normalized({-offset.y, offset.x}) * tangential);
        }
        p.velocity = p.velocity + (force * seconds);
        const float orbit =
            value(s_.orbit_velocity_min, s_.orbit_velocity_max, kOrbit, settings_.orbit_velocity);
        if (orbit != 0.0F) {
            p.position = origin_ + rotate(offset, cos_sin(-orbit * seconds * kTau));
        }
        const float damping = value(s_.damping_min, s_.damping_max, kDamping, settings_.damping);
        if (damping > 0.0F) {
            const float speed = length(p.velocity) - (damping * seconds);
            p.velocity = speed > 0.0F ? normalized(p.velocity) * speed : Vec2{};
        }
        const float degrees = value(s_.angle_min, s_.angle_max, kAngle, settings_.angle) +
                              (p.age * value(s_.angular_velocity_min, s_.angular_velocity_max,
                                             kAngularVelocity, settings_.angular_velocity));
        if (s_.particle_flag_align_y) {
            // Its y axis along its velocity; standing still, it keeps the way it faced.
            const Vec2 y_axis =
                p.velocity == Vec2{} ? Vec2{-p.axis.y, p.axis.x} : normalized(p.velocity);
            p.axis = {y_axis.y, -y_axis.x};
            p.rotation = angle_of(p.axis);
        } else {
            p.rotation = degrees * kRadiansPerDegree;
            p.axis = cos_sin(p.rotation);
        }
        p.frame =
            value(s_.anim_offset_min, s_.anim_offset_max, kAnimOffset, settings_.anim_offset) +
            (t * value(s_.anim_speed_min, s_.anim_speed_max, kAnimSpeed, settings_.anim_speed));
        p.position = p.position + (p.velocity * seconds);
        p.scale = value(s_.scale_amount_min, s_.scale_amount_max, kScale, settings_.scale_amount);
        const Vec4 color = multiply(multiply(s_.color, sample(settings_.color_ramp, t)), slot.tint);
        p.color = turn_hue(color, kTau * value(s_.hue_variation_min, s_.hue_variation_max, kHue,
                                               settings_.hue_variation));
    }

    const Settings& settings_;
    const CPUParticles2D& s_;
    world::Transform2D transform_;
    std::uint32_t seed_;
    Vec2 origin_; // what radial and tangential acceleration and orbits go round
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
bool run(Emitter& emitter, const CPUParticles2D& component, const world::Transform2D& transform,
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

std::vector<Particle> live(const Emitter& emitter, std::string_view draw_order) {
    std::vector<Particle> out;
    for (const Slot& slot : emitter.slots) {
        if (slot.alive) {
            out.push_back(slot.particle);
        }
    }
    if (draw_order == "lifetime") {
        // The oldest first, so the youngest draw on top.
        std::ranges::stable_sort(
            out, [](const Particle& a, const Particle& b) { return a.age > b.age; });
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
struct ParticleSystem {
    std::map<std::string, Emitter, std::less<>> emitters;

    void run_all(world::World& world, double tick_seconds) {
        auto& ecs = world.ecs();
        std::set<std::string, std::less<>> seen;
        const world::Tree2D tree(world);
        struct Run {
            entt::entity entity;
            CPUParticles2D* component;
            Emitter* emitter;
            world::Transform2D transform;
            std::uint32_t seed;
            bool ended = false;
        };
        std::vector<Run> runs;
        for (const entt::entity entity : ecs.view<CPUParticles2D>()) {
            auto& component = ecs.get<CPUParticles2D>(entity);
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
};

} // namespace

Vec2 world_position(const CPUParticles2D& settings, const world::Transform2D& transform,
                    Vec2 position) {
    return settings.local_coords ? to_world(transform, position, true) : position;
}

std::uint32_t particle_seed(std::string_view id, int seed) {
    // FNV-1a over the ID's bytes.
    std::uint32_t h = 2166136261U;
    for (const char c : id) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return mix(h ^ mix(static_cast<std::uint32_t>(seed)));
}

void add_particles_system(world::World& world) {
    if (world.ecs().ctx().contains<std::shared_ptr<ParticleSystem>>()) {
        return;
    }
    auto system = std::make_shared<ParticleSystem>();
    world.ecs().ctx().emplace<std::shared_ptr<ParticleSystem>>(system);
    world.add_reset([system](world::World& /*world*/) { system->emitters.clear(); });
    world.add_system(
        "cpu_particles_2d",
        [system](world::World& w, double tick_seconds) { system->run_all(w, tick_seconds); },
        {.doc = "Emits, moves and ages each CPUParticles2D's particles.",
         .priority = 1100,
         .threaded = true,
         .reads = {"Transform2D"},
         .writes = {"CPUParticles2D"}});
}

std::vector<Particle> particles(const world::World& world, entt::entity entity) {
    const auto* system = world.ecs().ctx().find<std::shared_ptr<ParticleSystem>>();
    const auto* component = world.ecs().try_get<CPUParticles2D>(entity);
    if (system == nullptr || component == nullptr) {
        return {};
    }
    const auto found = (*system)->emitters.find(key_of(world, entity));
    return found != (*system)->emitters.end() ? live(found->second, component->draw_order)
                                              : std::vector<Particle>{};
}

std::vector<Particle> simulate(const CPUParticles2D& settings, const world::Transform2D& transform,
                               std::uint32_t seed, double seconds, double tick_seconds) {
    if (tick_seconds <= 0.0) {
        return {};
    }
    CPUParticles2D copy = settings;
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

} // namespace slash::canvas
