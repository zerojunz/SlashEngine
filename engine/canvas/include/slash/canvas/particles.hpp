#pragma once

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

// Particles, as Godot's CPUParticles2D: each CPUParticles2D entity emits, moves and ages its
// particles every tick on the CPU, with the same results on every platform, and CanvasRenderer
// draws them. Particles are not part of the world's components, so scenes, snapshots and undo
// leave them as they are, as Godot doesn't save them either.
namespace slash::canvas {

// One live particle.
struct Particle {
    Vec2 position;         // pixels: in the world, or from the entity with local_coords
    Vec2 previous;         // where it was before the last tick, for drawing between ticks
    Vec2 velocity;         // pixels per second
    Vec2 axis{1.0F, 0.0F}; // its x axis, a unit vector: its rotation
    float rotation = 0.0F; // radians clockwise, as axis says
    float scale = 1.0F;
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    float age = 0.0F;      // seconds since it was emitted
    float lifetime = 1.0F; // seconds it lives
    // Where it is in the texture's frames, as Godot's particle animation: 0 is the first frame
    // and 1 is one past the last.
    float frame = 0.0F;
    std::uint32_t index = 0; // its emission slot, from 0 to amount - 1
};

// Adds the "cpu_particles_2d" system, which runs every CPUParticles2D each tick after physics
// (priority 1100) and emits "finished" when a one_shot's last particle dies.
void add_particles_system(world::World& world);

// The seed an entity's particles use, from its ID and its CPUParticles2D's seed.
[[nodiscard]] std::uint32_t particle_seed(std::string_view id, int seed);

// The live particles of an entity's CPUParticles2D after the last tick, in draw order; none if
// it has none or the world has no particles system.
[[nodiscard]] std::vector<Particle> particles(const world::World& world, entt::entity entity);

// Runs a CPUParticles2D's settings from the start for seconds, in ticks of tick_seconds, from a
// transform that stays put, apart from any world, and returns the live particles in draw order:
// what the system would have after that long.
[[nodiscard]] std::vector<Particle> simulate(const world::CPUParticles2D& settings,
                                             const world::Transform2D& transform,
                                             std::uint32_t seed, double seconds,
                                             double tick_seconds);

// Where a particle's position is in the world: with local_coords, placed there by the entity's
// transform; otherwise as it is.
[[nodiscard]] Vec2 world_position(const world::CPUParticles2D& settings,
                                  const world::Transform2D& transform, Vec2 position);

// The sine and cosine of an angle in radians, computed the same way on every platform, as
// {cos, sin}. Particles use it instead of libm, whose results differ between platforms.
[[nodiscard]] Vec2 cos_sin(float radians);

} // namespace slash::canvas
