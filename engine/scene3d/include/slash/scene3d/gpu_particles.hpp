#pragma once

#include "slash/core/result.hpp"
#include "slash/render/particles3d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// GPU particles in 3D, as Godot's GPUParticles3D: the gpu_particles_3d system works out each
// emitter's steps (render::ParticleStep3D) every tick as Godot's node and particles storage do,
// and keeps those that can still have made a live particle; renderers run the steps their device
// hasn't (RenderDevice::process_particles_3d) and draw each draw pass's mesh at every particle
// (fill_particle_mesh). As GPUParticles2D's, the steps are kept beside the components by entity ID,
// so scenes, snapshots and undo leave them as they are.
namespace slash::scene3d {

// The most steps an emitter keeps.
inline constexpr std::size_t kMaxParticleSteps3D = 4096;

// An emitter's steps, kept so a renderer can run those it hasn't: enough of the last ones to make
// every particle that may be alive (more than one lifetime of them), since the last restart.
struct ParticleSteps3D {
    // Changes when the emitter starts over as another (a new entity, a world reset): a renderer
    // seeing a new serial makes the system again.
    std::uint64_t serial = 0;
    std::uint32_t amount = 1;
    // The number of steps[0] among every step the emitter has made: a renderer that has run up to
    // before it runs the steps kept from a new system, the first one clearing it.
    std::uint64_t first = 0;
    std::span<const render::ParticleStep3D> steps;
    // Seconds of particle time not yet stepped, for drawing them between steps (fixed_fps).
    float remainder = 0.0F;
};

// Adds the "gpu_particles_3d" system, which steps every GPUParticles3D's emission each tick after
// the CPU particles (priority 1110) and emits "finished" when a one_shot's cycle has had time to
// end, as Godot's: lifetime times 2 - explosiveness after it started.
void add_gpu_particles_system(world::World& world);

// The steps an entity's GPUParticles3D has kept; none if it has no GPUParticles3D, hasn't been
// stepped or the world has no gpu_particles_3d system.
[[nodiscard]] std::optional<ParticleSteps3D> particle_steps_3d(const world::World& world,
                                                               entt::entity entity);

// The seed an entity's GPUParticles3D steps with: seed itself with use_fixed_seed, otherwise
// particle_seed of the ID and seed, varied by each one_shot restart.
[[nodiscard]] std::uint32_t gpu_particle_seed(const world::GPUParticles3D& settings,
                                              std::string_view id, std::uint32_t restarts);

// How a GPUParticles3D's draw_order and transform_align draw.
[[nodiscard]] render::ParticleOrder3D particle_order_3d(std::string_view draw_order);
[[nodiscard]] render::ParticleAlign particle_align(std::string_view transform_align);

// Runs steps on a system of amount particles from the start on the CPU, as every device does.
[[nodiscard]] std::vector<render::Particle3D>
run_steps_3d(std::uint32_t amount, const render::ParticleProcess3D& process,
             std::span<const render::ParticleStep3D> steps);

// The live particles of a system in the order it draws them for a system phase, as
// particle_mesh_vertices_3d places them; "view_depth" draws as "index" here, since it needs a
// camera.
[[nodiscard]] std::vector<render::Particle3D>
live_particles_3d(std::span<const render::Particle3D> particles, render::ParticleOrder3D order,
                  float phase);

// The particles an entity's GPUParticles3D has after the last tick, run on the CPU from the steps
// kept with process, in draw order, live ones only; none if it has none.
[[nodiscard]] std::vector<render::Particle3D>
gpu_particles_3d(const world::World& world, entt::entity entity,
                 const render::ParticleProcess3D& process);

// Runs a GPUParticles3D's settings from the start for seconds, in ticks of tick_seconds, from a
// transform that stays put, apart from any world, and returns the live particles in draw order:
// what the system would have after that long.
[[nodiscard]] std::vector<render::Particle3D> simulate_gpu(const world::GPUParticles3D& settings,
                                                           const world::Affine3D& transform,
                                                           const render::ParticleProcess3D& process,
                                                           std::string_view id, double seconds,
                                                           double tick_seconds);

// A transform as 3D particle steps take one.
[[nodiscard]] render::ParticleTransform particle_transform(const world::Affine3D& affine);

// The process materials GPUParticles3D's process_material names, loaded by project path: a reload
// gives new ones.
using ParticleProcessSource =
    std::function<Result<std::shared_ptr<const render::ParticleProcess3D>>(
        const std::string& path)>;

} // namespace slash::scene3d
