#pragma once

#include "slash/core/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// GPU particles' device side, as Godot's particles storage and ParticleProcessMaterial in 2D. A
// particle system is a buffer of particles that steps run: each step restarts the particles whose
// turn in the emission cycle has come and moves every live one, by the process material's rules.
// It draws as one quad per particle. The GPU device runs both in compute shaders
// (shaders/particles_process.hlsl and particles_draw.hlsl); process_particles and
// particle_vertices are their CPU twins, which the other devices run, the same on every platform.
namespace slash::render {

struct Vertex2D; // device.hpp

// A particle system on a device (RenderDevice::create_particles).
// NOLINTNEXTLINE(performance-enum-size): ids count up, past 255
enum class ParticlesId : std::uint32_t { None = 0 };

// The most particles a system holds, as Godot's GPUParticles2D.
inline constexpr std::uint32_t kMaxParticles = 1000000;
// The most points a curve or a gradient of a ParticleProcess has.
inline constexpr std::size_t kMaxParticleCurvePoints = 32;

// Where particles start, as Godot's ParticleProcessMaterial::EmissionShape.
enum class EmissionShape : std::uint8_t {
    Point,
    Sphere,         // inside a circle of emission_sphere_radius (a sphere seen from the front)
    SphereSurface,  // on its edge
    Box,            // inside emission_box_extents either side of the center
    Points,         // at one of emission_points, picked at random
    DirectedPoints, // the same, moving along emission_normals
    Ring,           // between emission_ring_inner_radius and emission_ring_radius
};

// The parameters that take a random value between their min and max when a particle starts and
// change by their curve over its life, as Godot's ParticleProcessMaterial::Parameter.
enum class ParticleParam : std::uint8_t {
    InitialVelocity, // pixels per second along the spread direction; no curve
    AngularVelocity, // degrees per second
    OrbitVelocity,   // turns per second round the emitter
    LinearAccel,     // pixels per second squared along the velocity
    RadialAccel,     // away from the emitter
    TangentialAccel, // across that
    Damping,         // pixels per second squared against the velocity
    Angle,           // degrees
    Scale,
    HueVariation, // turns of the color's hue, from -1 to 1
    AnimSpeed,    // sprite sheet frames per lifetime
    AnimOffset,   // where in the sheet it starts, from 0 to 1; its curve adds instead
    RadialVelocity,
    TurbulenceInfluence,    // how far turbulence turns its velocity, from 0 to 1
    TurbulenceDisplacement, // pixels turbulence moves it at the start; no curve
    Count
};
inline constexpr std::size_t kParticleParams = static_cast<std::size_t>(ParticleParam::Count);

// What a particle does when it touches a shape, as Godot's ParticleProcessMaterial::CollisionMode.
enum class ParticleCollision : std::uint8_t {
    Disabled,
    Rigid,         // bounces off by collision_bounce and slides with collision_friction
    HideOnContact, // dies
};

// A curve over a particle's life: points [offset, value], offsets from 0 to 1 in order, joined by
// straight lines and flat past the ends. No points is no curve.
using ParticleCurve = std::vector<Vec2>;

struct GradientStop {
    float offset = 0.0F;
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};

    bool operator==(const GradientStop&) const = default;
};
// Colors at offsets from 0 to 1, in order, blended in between. No stops is no gradient.
using ParticleGradient = std::vector<GradientStop>;

// How particles start and move: Godot's ParticleProcessMaterial with particle_flag_disable_z, as
// its generated shader runs it. Angles are in degrees.
struct ParticleProcess {
    EmissionShape emission_shape = EmissionShape::Point;
    Vec2 emission_shape_offset;
    Vec2 emission_shape_scale{1.0F, 1.0F};
    float emission_sphere_radius = 1.0F;
    Vec2 emission_box_extents{1.0F, 1.0F};
    std::vector<Vec2> emission_points;  // Points and DirectedPoints
    std::vector<Vec2> emission_normals; // DirectedPoints: unit vectors, one per point
    std::vector<Vec4> emission_colors;  // none, or one per point, multiplying color
    float emission_ring_radius = 1.0F;
    float emission_ring_inner_radius = 0.0F;
    Vec2 direction{1.0F, 0.0F};
    float spread = 45.0F;
    float inherit_velocity_ratio = 0.0F; // of the emitter's velocity, added at the start
    Vec2 velocity_pivot;                 // what radial velocity pushes away from
    Vec2 gravity{0.0F, 98.0F};
    // By ParticleParam. Scale is 1 and TurbulenceInfluence 0.1, the rest 0.
    std::array<float, kParticleParams> param_min{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.1F, 0};
    std::array<float, kParticleParams> param_max{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.1F, 0};
    // Each multiplies its parameter over the particle's life; InitialVelocity's is unused.
    std::array<ParticleCurve, kParticleParams> param_curve;
    ParticleCurve velocity_limit_curve; // the fastest a particle moves
    ParticleCurve alpha_curve;          // multiplies alpha
    ParticleCurve emission_curve;       // brightens: color times 1 plus it
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    ParticleGradient color_ramp;         // over the particle's life
    ParticleGradient color_initial_ramp; // a random place on it, picked at the start
    float lifetime_randomness = 0.0F;    // shortens lifetimes by up to this fraction
    bool align_y = false;                // the particle's y axis along its velocity
    bool damping_as_friction = false;    // damping grows with the speed squared
    // Turbulence, as Godot's: curl noise that turns each particle's velocity by its
    // TurbulenceInfluence and moves it by its TurbulenceDisplacement at the start. Godot's
    // noise, with its lattice gradients from an integer hash so every device makes the same.
    bool turbulence_enabled = false;
    float turbulence_noise_strength = 1.0F;     // how sharply the noise turns, 0 to 20
    float turbulence_noise_scale = 9.0F;        // Godot's slider: higher is larger swirls
    Vec3 turbulence_noise_speed;                // how the noise moves per second of particle time
    float turbulence_noise_speed_random = 0.2F; // how fast the noise changes as it moves
    // Collision with the steps' shapes (ParticleShape), as Godot's with LightOccluder2D nodes.
    ParticleCollision collision_mode = ParticleCollision::Disabled;
    float collision_friction = 0.0F;  // of the speed along the surface lost, 0 to 1
    float collision_bounce = 0.0F;    // of the speed into the surface sent back, 0 to 1
    bool collision_use_scale = false; // the particle's scale multiplies its collision size

    bool operator==(const ParticleProcess&) const = default;
};

// A shape particles collide with, as a LightOccluder2D's outline in the signed distance field
// Godot's particles collide with: a closed one is solid, an open one a line.
struct ParticleShape {
    std::vector<Vec2> points; // pixels
    bool closed = true;

    bool operator==(const ParticleShape&) const = default;
};
// The most points the shapes one step collides with have between them.
inline constexpr std::size_t kMaxParticleShapePoints = 1024;

// One step of a system, as Godot's ParticlesFrameParams with the process call's push constants.
// Steps run in order; each restarts the particles whose turn comes between prev_system_phase and
// system_phase and moves every live particle on by delta.
struct ParticleStep {
    // Where new particles start: x' = [0] x + [2] y + [4], y' = [1] x + [3] y + [5]; identity
    // for particles in the emitter's own coordinates.
    std::array<float, 6> emission{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    Vec2 emitter_velocity;          // pixels per second
    float system_phase = 0.0F;      // where the emission cycle is after the step, from 0 to 1
    float prev_system_phase = 0.0F; // before it
    float delta = 0.0F;             // seconds
    float lifetime = 1.0F;          // seconds a particle lives; one emission cycle
    float explosiveness = 0.0F;
    float randomness = 0.0F;
    float amount_ratio = 1.0F;
    float interp_to_end = 0.0F;
    // Seconds of particle time since the system started over after the step, back to 0 every
    // hour, as Godot's TIME rolls over: what turbulence moves with.
    float time = 0.0F;
    // The shapes particles collide with during the step: shape_count of the shapes the steps run
    // with, from shape_first. A particle collides where it comes within collision_size pixels
    // of one, as Godot's collision_base_size, measured where the shapes are: to_shapes takes
    // particle coordinates there, as emission does, and from_shapes is the inverse of its first
    // four, x' = [0] x + [2] y, y' = [1] x + [3] y, which bring the way out back.
    std::uint32_t shape_first = 0;
    std::uint32_t shape_count = 0;
    float collision_size = 1.0F;
    std::array<float, 6> to_shapes{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    std::array<float, 4> from_shapes{1.0F, 0.0F, 0.0F, 1.0F};
    std::uint32_t cycle = 0; // emission cycles before this one
    std::uint32_t seed = 0;
    bool emitting = true;
    bool fractional_delta = true; // a restarted particle moves for the part of delta since its turn
    bool clear = false;           // resets every particle first

    bool operator==(const ParticleStep&) const = default;
};

// A particle's flags, as Godot's.
inline constexpr std::uint32_t kParticleActive = 1U;
inline constexpr std::uint32_t kParticleStarted = 2U;
// The emission cycle it started in is in the flags' top 16 bits.
inline constexpr std::uint32_t kParticleCycleShift = 16U;

// One particle, laid out as the GPU keeps it (96 bytes).
struct Particle {
    Vec4 basis{1.0F, 0.0F, 0.0F, 1.0F}; // its x axis [x, y] then its y axis [z, w], scaled
    Vec2 origin;                        // pixels
    Vec2 velocity;                      // pixels per second
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    // Godot's CUSTOM: its angle in radians, its age as a fraction of the system's lifetime, its
    // sprite sheet frame from 0 to 1, and its own lifetime as a fraction of the system's.
    Vec4 custom;
    // Godot's USERDATA1: the velocity its forces built up [x, y, z] (turbulence turns it out of
    // the screen's plane, where it keeps pulling on its speed), and the angle its angular
    // velocity built up.
    Vec4 userdata;
    std::uint32_t flags = 0;
    std::array<std::uint32_t, 3> padding{};

    [[nodiscard]] bool active() const { return (flags & kParticleActive) != 0; }
};
static_assert(sizeof(Particle) == 96);

// The order particles draw in, as Godot's GPUParticles2D::DrawOrder.
enum class ParticleOrder : std::uint8_t {
    Index,           // by slot
    Lifetime,        // oldest first
    ReverseLifetime, // youngest first
};

// How a system draws: one quad per particle, size wide and tall around it, turned and scaled by
// its basis, colored by its color times modulate and showing the sprite sheet frame its custom
// says, as Godot's GPUParticles2D with a CanvasItemMaterial's particle animation. Each particle
// takes six vertices in draw order, two triangles; a particle not live has no area.
struct ParticleDraw {
    ParticleOrder order = ParticleOrder::Index;
    std::uint32_t lifetime_split = 0; // particle_lifetime_split for the system's phase
    float remainder = 0.0F;           // seconds each particle is moved on along its velocity
    Vec2 size{1.0F, 1.0F};            // pixels
    std::uint32_t hframes = 1;
    std::uint32_t vframes = 1;
    bool anim_loop = false;
    // Moves the quads, as ParticleStep::emission: the emitter's transform for particles in its
    // coordinates.
    std::array<float, 6> transform{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};

    bool operator==(const ParticleDraw&) const = default;
};

// Runs steps on particles, a system of particles.size(), as the GPU does; the steps name the
// shapes they collide with by their place in shapes.
void process_particles(std::span<Particle> particles, const ParticleProcess& process,
                       std::span<const ParticleStep> steps,
                       std::span<const ParticleShape> shapes = {});

// Where a point is against shapes, as particles collide, all in the shapes' coordinates: the
// depth it is inside them by, less size, and the way out, along which moving it by depth leaves
// it size away. No collision when the point is further than size from every shape.
struct ParticleContact {
    bool collided = false;
    Vec2 normal;
    float depth = 0.0F;
};
[[nodiscard]] ParticleContact particle_contact(Vec2 point, float size,
                                               std::span<const ParticleShape> shapes);

// Appends particles' quads to out, as the GPU draws them: six vertices each, in draw order.
void particle_vertices(std::span<const Particle> particles, const ParticleDraw& draw,
                       std::vector<Vertex2D>& out);

// The slot the oldest particle is in, for drawing in lifetime order, when the emission cycle is at
// phase, as Godot's particles storage works it out.
[[nodiscard]] std::uint32_t particle_lifetime_split(std::uint32_t amount, double phase);

// The noise frequency a turbulence_noise_scale gives, as Godot's set_turbulence_noise_scale
// works it out, with square roots for its fourth roots so every machine gets the same.
[[nodiscard]] float turbulence_frequency(float noise_scale);

// The value of a curve at an offset, or fallback when it has no points.
[[nodiscard]] float sample_curve(const ParticleCurve& curve, float offset, float fallback);
// The color of a gradient at an offset, or white when it has no stops.
[[nodiscard]] Vec4 sample_gradient(const ParticleGradient& gradient, float offset);

} // namespace slash::render
