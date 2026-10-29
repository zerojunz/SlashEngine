#pragma once

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Particles in 3D, as Godot's CPUParticles3D: each CPUParticles3D entity emits, moves and ages its
// particles every tick on the CPU, with the same results on every platform, and scene3d::Renderer
// draws the entity's mesh at each. As CPUParticles2D's, they are kept beside the components by
// entity ID, so scenes, snapshots and undo leave them as they are.
namespace slash::scene3d {

// One live particle.
struct Particle3D {
    // Its basis and position, in meters: in the world, or in the entity's space with
    // local_coords. As Godot's, the basis is turned by the flags and scaled by its scale.
    world::Affine3D transform;
    Vec3 previous;      // where it was before the last tick, for drawing between ticks
    Vec3 velocity;      // meters per second
    float angle = 0.0F; // radians: how far a "particles" billboard turns it, Godot's custom.x
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    float age = 0.0F;      // seconds since it was emitted
    float lifetime = 1.0F; // seconds it lives
    // Where it is in its material's frames, as Godot's custom.z: 0 is the first frame and 1 is
    // one past the last.
    float frame = 0.0F;
    std::uint32_t index = 0; // its emission slot, from 0 to amount - 1
};

// Adds the "cpu_particles_3d" system, which runs every CPUParticles3D each tick after physics
// (priority 1100) and emits "finished" when a one_shot's last particle dies.
void add_particles_system(world::World& world);

// The seed an entity's particles use, from its ID and its CPUParticles3D's seed.
[[nodiscard]] std::uint32_t particle_seed(std::string_view id, int seed);

// The live particles of an entity's CPUParticles3D after the last tick, in its draw order
// ("view_depth" draws as "index" here, since it needs a camera); none if it has none or the world
// has no particles system.
[[nodiscard]] std::vector<Particle3D> particles_3d(const world::World& world, entt::entity entity);

// Runs a CPUParticles3D's settings from the start for seconds, in ticks of tick_seconds, from a
// transform that stays put, apart from any world, and returns the live particles in draw order:
// what the system would have after that long.
[[nodiscard]] std::vector<Particle3D> simulate(const world::CPUParticles3D& settings,
                                               const world::Affine3D& transform, std::uint32_t seed,
                                               double seconds, double tick_seconds);

// The particles in the order a camera looking down camera_z (its basis's z axis, pointing back
// at it) draws them with draw_order "view_depth": the farthest first, as Godot's SortAxis.
void sort_by_depth(std::vector<Particle3D>& particles, Vec3 camera_z);

// How a mesh is turned to face the camera, as a StandardMaterial3D's billboard_mode.
enum class Billboard : std::uint8_t { Disabled, Enabled, FixedY, Particles };

// The billboard mode a StandardMaterial3D names; Disabled for any other.
[[nodiscard]] Billboard billboard_of(std::string_view mode);

// Where a mesh placed by model goes when its material is a billboard, as Godot's BaseMaterial3D
// vertex code: its basis replaced by the camera's (or, FixedY, one turned about y toward it),
// turned by angle about its z with Particles, then scaled by model's own axes' lengths with
// keep_scale. model as it is with Disabled.
[[nodiscard]] world::Affine3D billboard(const world::Affine3D& model, const world::Affine3D& camera,
                                        Billboard mode, bool keep_scale, float angle);

// A texture coordinate of a particle's mesh moved to the frame it shows, as Godot's particle
// animation: frame is the particle's (0 to 1 over the frames), h_frames by v_frames the
// texture's, and loop starts again after the last.
[[nodiscard]] Vec2 particle_frame_uv(Vec2 uv, float frame, int h_frames, int v_frames, bool loop);

} // namespace slash::scene3d
