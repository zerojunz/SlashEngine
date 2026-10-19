#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/particles.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// GPU particles, as Godot's GPUParticles2D. Each tick the "gpu_particles_2d" system works out
// each emitter's steps as Godot's GPUParticles2D node and particles storage do (its emission
// cycle, fixed_fps steps, preprocess, one_shot timing) and keeps them; a renderer runs the steps
// it hasn't run on its device (render::RenderDevice::process_particles) and draws the particles
// there, so a new renderer, or one that missed frames, catches up from the steps kept. The steps
// are the same on every machine, and so are the particles they make on every device. Particles
// are not part of the world's components, so scenes, snapshots and undo leave them as they are.
//
// .process_material.json files are Godot's ParticleProcessMaterial in 2D, with its property
// names; fields left out keep Godot's defaults, and gravity is [0, 98] as a 2D one's:
//
//   {
//     "format": 1,
//     "emission_shape": "sphere",
//     "emission_sphere_radius": 6,
//     "direction": [0, -1],
//     "spread": 20,
//     "initial_velocity_min": 60,
//     "initial_velocity_max": 90,
//     "scale_curve": [[0, 1], [1, 0]],
//     "color_ramp": [{"offset": 0, "color": [1, 0.8, 0.2, 1]}, {"offset": 1, "color": [1, 0.2, 0,
//     0]}]
//   }
namespace slash::canvas {

inline constexpr std::string_view kProcessMaterialSuffix = ".process_material.json";

// JSON Schema for .process_material.json files.
[[nodiscard]] json::Value process_material_schema();

// Every error in a .process_material.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_process_material(const json::Value& doc);

// The process a .process_material.json document describes, or its first error.
[[nodiscard]] Result<render::ParticleProcess> parse_process_material(const json::Value& doc);

// The most steps an emitter keeps.
inline constexpr std::size_t kMaxParticleSteps = 4096;

// An emitter's steps, kept so a renderer can run those it hasn't: enough of the last ones to
// make every particle that may be alive (more than one lifetime of them), since the last restart.
struct ParticleSteps {
    // Changes when the emitter starts over as another (a new entity, a world reset): a renderer
    // seeing a new serial makes the system again.
    std::uint64_t serial = 0;
    std::uint32_t amount = 1;
    // The number of steps[0] among every step the emitter has made: a renderer that has run up
    // to before it runs the steps kept from a new system, the first one clearing it.
    std::uint64_t first = 0;
    std::span<const render::ParticleStep> steps;
    // Seconds of particle time not yet stepped, for drawing them between steps (fixed_fps).
    float remainder = 0.0F;
};

// Adds the "gpu_particles_2d" system, which steps every GPUParticles2D's emission each tick after
// the CPU particles (priority 1110) and emits "finished" when a one_shot's cycle has had time to
// end, as Godot's: lifetime times 2 - explosiveness after it started.
void add_gpu_particles_system(world::World& world);

// The steps an entity's GPUParticles2D has kept; none if it has no GPUParticles2D, hasn't been
// stepped or the world has no gpu_particles_2d system.
[[nodiscard]] std::optional<ParticleSteps> particle_steps(const world::World& world,
                                                          entt::entity entity);

// The seed an entity's GPUParticles2D steps with: seed itself with use_fixed_seed, otherwise
// particle_seed of the ID and seed, varied by each one_shot restart.
[[nodiscard]] std::uint32_t gpu_particle_seed(const world::GPUParticles2D& settings,
                                              std::string_view id, std::uint32_t restarts);

// How a GPUParticles2D's draw_order draws.
[[nodiscard]] render::ParticleOrder particle_order(std::string_view draw_order);

// Runs steps on a system of amount particles from the start on the CPU, as every device does.
[[nodiscard]] std::vector<render::Particle> run_steps(std::uint32_t amount,
                                                      const render::ParticleProcess& process,
                                                      std::span<const render::ParticleStep> steps);

// The particles an entity's GPUParticles2D has after the last tick, run on the CPU from the
// steps kept with its process material, in draw order, live ones only; none if it has none.
// Fails if its process material doesn't load.
[[nodiscard]] Result<std::vector<render::Particle>>
gpu_particles(const world::World& world, entt::entity entity, asset::Assets& assets);

// Runs a GPUParticles2D's settings from the start for seconds, in ticks of tick_seconds, from a
// transform that stays put, apart from any world, and returns the live particles in draw order:
// what it would have after that long.
[[nodiscard]] std::vector<render::Particle> simulate(const world::GPUParticles2D& settings,
                                                     const render::ParticleProcess& process,
                                                     const world::Transform2D& transform,
                                                     std::string_view id, double seconds,
                                                     double tick_seconds);

// The live particles of a system, in the order a draw_order draws them, as the device draws them
// for phase (render::particle_lifetime_split).
[[nodiscard]] std::vector<render::Particle>
live_particles(std::span<const render::Particle> particles, render::ParticleOrder order,
               double phase);

} // namespace slash::canvas
