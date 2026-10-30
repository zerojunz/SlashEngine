#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/particles.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

// GPU particles in 3D, as Godot's GPUParticles3D with a ParticleProcessMaterial: a system is a
// buffer of particles that steps run, each restarting the particles whose turn in the emission
// cycle has come and moving every live one by the process material's rules, as Godot's
// particles shader and the code ParticleProcessMaterial::_update_shader generates for 3D do.
// A system draws as a mesh at each particle, as Godot's particles copy shader places a draw
// pass's instances: RenderDevice::fill_particle_mesh fills a mesh with a copy of the draw pass's
// vertices per particle, turned, scaled and colored by it.
//
// process_particles_3d and particle_mesh_vertices_3d are the CPU twins every device runs, the same
// bits on every platform.
namespace slash::render {

// How many sides a curve of a process material has: Godot's CurveTexture gives each of x, y and
// z the same value, and its CurveXYZTexture a curve each. None of the three is no curve.
struct ParticleCurve3 {
    std::array<ParticleCurve, 3> axes;

    [[nodiscard]] bool empty() const {
        return axes[0].empty() && axes[1].empty() && axes[2].empty();
    }
    bool operator==(const ParticleCurve3&) const = default;
};

// How particles start and move in 3D: Godot's ParticleProcessMaterial, as its generated shader
// runs it without particle_flag_disable_z (and with it, flattened onto the xy plane). Angles
// are in degrees and lengths in meters.
struct ParticleProcess3D {
    EmissionShape emission_shape = EmissionShape::Point;
    Vec3 emission_shape_offset;
    Vec3 emission_shape_scale{1.0F, 1.0F, 1.0F};
    float emission_sphere_radius = 1.0F;
    Vec3 emission_box_extents{1.0F, 1.0F, 1.0F};
    std::vector<Vec3> emission_points;  // Points and DirectedPoints
    std::vector<Vec3> emission_normals; // DirectedPoints: one per point
    std::vector<Vec4> emission_colors;  // none, or one per point, multiplying color
    // Ring: about emission_ring_axis, emission_ring_height tall, its sides at
    // emission_ring_cone_angle degrees (90 is a cylinder).
    Vec3 emission_ring_axis{0.0F, 0.0F, 1.0F};
    float emission_ring_height = 1.0F;
    float emission_ring_radius = 1.0F;
    float emission_ring_inner_radius = 0.0F;
    float emission_ring_cone_angle = 90.0F;
    Vec3 direction{1.0F, 0.0F, 0.0F};
    float spread = 45.0F;
    float flatness = 0.0F; // 1 keeps the spread in the plane of direction and x
    float inherit_velocity_ratio = 0.0F;
    Vec3 velocity_pivot;
    Vec3 gravity{0.0F, -9.8F, 0.0F};
    // By ParticleParam, as ParticleProcess's; Scale's and OrbitVelocity's curves are in
    // scale_curve and orbit_velocity_curve instead.
    std::array<float, kParticleParams> param_min{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.1F, 0};
    std::array<float, kParticleParams> param_max{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.1F, 0};
    std::array<ParticleCurve, kParticleParams> param_curve;
    // Scales each axis over the particle's life.
    ParticleCurve3 scale_curve;
    // Turns per second about each axis through velocity_pivot, times orbit_velocity; as Godot's,
    // particles orbit in 3D only with a curve (and a single curve turns them about all three).
    ParticleCurve3 orbit_velocity_curve;
    // Meters per second along each axis over the particle's life, times a directional_velocity
    // picked between min and max; in the emitter's space with directional_velocity_global.
    float directional_velocity_min = 1.0F;
    float directional_velocity_max = 1.0F;
    ParticleCurve3 directional_velocity_curve;
    bool directional_velocity_global = false;
    // Scales each axis by the particle's speed: its curve's offset is the speed over
    // scale_over_velocity_max - scale_over_velocity_min, in meters per second, as Godot's.
    float scale_over_velocity_min = 0.0F;
    float scale_over_velocity_max = 0.0F;
    ParticleCurve3 scale_over_velocity_curve;
    ParticleCurve velocity_limit_curve;
    ParticleCurve alpha_curve;
    ParticleCurve emission_curve;
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    ParticleGradient color_ramp;
    ParticleGradient color_initial_ramp;
    float lifetime_randomness = 0.0F;
    bool align_y = false;  // the particle's y axis along its velocity
    bool rotate_y = false; // its angle turns it about y
    bool disable_z = false;
    bool damping_as_friction = false;
    bool turbulence_enabled = false;
    float turbulence_noise_strength = 1.0F;
    float turbulence_noise_scale = 9.0F;
    Vec3 turbulence_noise_speed;
    float turbulence_noise_speed_random = 0.2F;
    // Attractors push the particles (Godot's attractor_interaction_enabled).
    bool attractor_interaction = true;

    bool operator==(const ParticleProcess3D&) const = default;
};

// An affine transform as 3D particles take one: its x, y and z axes, then its origin.
using ParticleTransform = std::array<float, 12>;
inline constexpr ParticleTransform kParticleIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

// A vector field's texels, as a GPUParticlesAttractorVectorField3D's Texture3D holds them: width by
// height by depth, x fastest, then y, then z, each from 0 to 1 (RGBA8 texels over 255).
struct ParticleField3D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::vector<Vec4> texels;

    bool operator==(const ParticleField3D&) const = default;
};

// The shapes of Godot's GPUParticlesAttractor3D.
enum class AttractorShape3D : std::uint8_t { Sphere, Box, VectorField };

// An attractor a step's particles feel, as Godot's particles storage hands one to its particles
// shader: its transform in the particles' space with its basis made orthonormal, the half size
// of its box (or its radius, in x, y and z) scaled by its transform, and its settings. A vector
// field's field is shared by every step that uses it.
struct ParticleAttractor3D {
    AttractorShape3D shape = AttractorShape3D::Sphere;
    ParticleTransform transform = kParticleIdentity;
    Vec3 extents{1.0F, 1.0F, 1.0F};
    float strength = 1.0F;
    float attenuation = 1.0F;
    float directionality = 0.0F;
    std::shared_ptr<const ParticleField3D> field; // VectorField only

    bool operator==(const ParticleAttractor3D&) const = default;
};

// The most attractors one step takes, as Godot's MAX_ATTRACTORS.
inline constexpr std::size_t kMaxParticleAttractors3D = 32;
// The most texels a vector field holds.
inline constexpr std::size_t kMaxParticleFieldTexels3D = std::size_t{1} << 18U;

// One step of a 3D system, as ParticleStep is of a 2D one: each restarts the particles whose turn
// comes between prev_system_phase and system_phase and moves every live particle on by delta.
struct ParticleStep3D {
    // Where new particles start: the emitter's transform, or the identity for particles in its
    // own space (local_coords).
    ParticleTransform emission = kParticleIdentity;
    Vec3 emitter_velocity; // meters per second
    float system_phase = 0.0F;
    float prev_system_phase = 0.0F;
    float delta = 0.0F;
    float lifetime = 1.0F;
    float explosiveness = 0.0F;
    float randomness = 0.0F;
    float amount_ratio = 1.0F;
    float interp_to_end = 0.0F;
    float time = 0.0F;
    std::uint32_t cycle = 0;
    std::uint32_t seed = 0;
    bool emitting = true;
    bool fractional_delta = true;
    bool clear = false;
    // What pushes the particles in this step (Godot's FrameParams attractors).
    std::vector<ParticleAttractor3D> attractors;

    bool operator==(const ParticleStep3D&) const = default;
};

// One 3D particle, laid out as the GPU keeps it (128 bytes), as Godot's ParticleData: its
// transform's axes (scaled) and origin, each a Vec4 whose w is 0, the velocity its forces built
// up (Godot's VELOCITY, which disable_velocity leaves out of its motion), its flags, its color
// and Godot's CUSTOM: its angle in radians, its age as a fraction of the system's lifetime, its
// frame from 0 to 1 and its own lifetime as a fraction of the system's.
struct Particle3D {
    Vec4 x{1.0F, 0.0F, 0.0F, 0.0F};
    Vec4 y{0.0F, 1.0F, 0.0F, 0.0F};
    Vec4 z{0.0F, 0.0F, 1.0F, 0.0F};
    Vec4 origin;
    Vec3 velocity;
    std::uint32_t flags = 0;
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    Vec4 custom;
    Vec4 userdata; // spare

    [[nodiscard]] bool active() const { return (flags & kParticleActive) != 0; }
};
static_assert(sizeof(Particle3D) == 128);

// Runs steps on a 3D system of particles.size() particles, as the GPU does.
void process_particles_3d(std::span<Particle3D> particles, const ParticleProcess3D& process,
                          std::span<const ParticleStep3D> steps);

// The order particles draw in, as Godot's GPUParticles3D::DrawOrder.
enum class ParticleOrder3D : std::uint8_t {
    Index,
    Lifetime,        // oldest first
    ReverseLifetime, // youngest first
    ViewDepth,       // farthest from the camera first
};

// How each particle's transform is turned before its mesh is placed, as Godot's
// GPUParticles3D::TransformAlign.
enum class ParticleAlign : std::uint8_t {
    Disabled,
    ZBillboard,            // its z axis toward the camera, its y up the screen
    YToVelocity,           // its y axis along its velocity
    ZBillboardYToVelocity, // both: y along its velocity as the camera sees it
};

// How a draw pass's material faces the camera, as a StandardMaterial3D's billboard_mode.
enum class ParticleBillboard : std::uint8_t { Disabled, Enabled, FixedY, Particles };

// How a system's particles place a draw pass's mesh (fill_particle_mesh), as Godot's particles
// copy shader and StandardMaterial3D's vertex code do: each particle in draw order takes as many
// vertices as the pass has, placed by its transform (turned as align says and moved on along its
// velocity by remainder seconds), then by transform, then faced to the camera as billboard
// says, colored by its color, its texture coordinates showing its frame of
// h_frames by v_frames with Particles. A particle not live has its vertices folded onto one
// point, so it draws nothing.
struct ParticleMeshDraw3D {
    ParticleOrder3D order = ParticleOrder3D::Index;
    std::uint32_t lifetime_split = 0; // particle_lifetime_split for the system's phase
    float remainder = 0.0F;
    ParticleAlign align = ParticleAlign::Disabled;
    // The emitter's transform for particles in its space (local_coords), else the identity.
    ParticleTransform transform = kParticleIdentity;
    // The camera's z axis (pointing back at it) and y axis: the z in the particles' own space,
    // as Godot's sort_direction, and the y as it is, as its align_up.
    Vec3 sort_direction{0.0F, 0.0F, 1.0F};
    Vec3 align_up{0.0F, 1.0F, 0.0F};
    // Where the camera is, for billboards.
    ParticleTransform camera = kParticleIdentity;
    ParticleBillboard billboard = ParticleBillboard::Disabled;
    bool keep_scale = false;
    std::uint32_t h_frames = 1;
    std::uint32_t v_frames = 1;
    bool anim_loop = false;
    // The material's uv1_scale and uv1_offset, applied before the frame moves them.
    std::array<float, 2> uv_scale{1.0F, 1.0F};
    std::array<float, 2> uv_offset{0.0F, 0.0F};
    // The box the mesh is drawn in, which orders it among blended draws and culls its shadows:
    // the emitter's visibility box, in the mesh's space, as Godot's visibility_aabb. A box whose
    // low is above its high, as by default, is the box round the vertices made.
    std::array<float, 3> box_low{1.0F, 1.0F, 1.0F};
    std::array<float, 3> box_high{0.0F, 0.0F, 0.0F};

    [[nodiscard]] bool has_box() const {
        return box_low[0] <= box_high[0] && box_low[1] <= box_high[1] && box_low[2] <= box_high[2];
    }
    bool operator==(const ParticleMeshDraw3D&) const = default;
};

// The particle draw slot slot of total shows, as Godot's particles copy shader picks it: slot
// itself in Index order (and ViewDepth, whose sort comes after), or along the emission cycle from
// lifetime_split (particle_lifetime_split for the system's phase).
[[nodiscard]] std::uint32_t particle_draw_index_3d(std::uint32_t slot, std::uint32_t total,
                                                   ParticleOrder3D order,
                                                   std::uint32_t lifetime_split);

// Fills out (particles.size() times source.size() vertices) with source placed at each particle,
// as the GPU does.
void particle_mesh_vertices_3d(std::span<const Particle3D> particles,
                               std::span<const Vertex3D> source, const ParticleMeshDraw3D& draw,
                               std::span<Vertex3D> out);

// A 3D particle system on a device (RenderDevice::create_particles_3d).
// NOLINTNEXTLINE(performance-enum-size): ids count up, past 255
enum class Particles3DId : std::uint32_t { None = 0 };

// The most particles a 3D system holds, as Godot's GPUParticles3D.
inline constexpr std::uint32_t kMaxParticles3D = 1000000;

namespace detail {

// 3D systems kept and run on the CPU: what RenderDevice does with them unless a device runs
// them itself.
class ParticleStore3D {
public:
    [[nodiscard]] Result<Particles3DId> create(std::uint32_t amount);
    void destroy(Particles3DId particles);
    [[nodiscard]] Result<void> process(Particles3DId particles, const ParticleProcess3D& process,
                                       std::span<const ParticleStep3D> steps);
    [[nodiscard]] Result<std::span<const Particle3D>> particles(Particles3DId particles) const;

private:
    std::map<std::uint32_t, std::vector<Particle3D>> systems_;
    std::uint32_t next_ = 0;
};

} // namespace detail

} // namespace slash::render
