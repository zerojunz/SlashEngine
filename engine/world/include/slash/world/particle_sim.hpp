#pragma once

#include "slash/core/expression.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Simulation stages for particles, after Unreal's Niagara (Godot has none), shared by 2D and 3D:
// neighbors found by sorting along a Morton curve (no grid), flocking, curl noise, drag, speed
// limits, collision through a query the caller gives, attributes worked out by Expressions and
// events. 2D particles are 3D ones at z 0. Built with -ffp-contract=off and with no libm but
// square roots, so a simulation is the same bits everywhere.
namespace slash::world {

// The most attributes a particle carries.
inline constexpr std::size_t kMaxParticleAttributes = 8;

// The Morton code of a cell: its coordinates (each kept to 21 bits) interleaved, x lowest.
[[nodiscard]] std::uint64_t morton_code(std::int32_t x, std::int32_t y, std::int32_t z);

// Points sorted along a Morton curve by the cell of side radius each is in, so the points of a
// cell lie together and the points near one are found by looking up its cell and the cells
// round it (27 in 3D, 9 when flat) with binary searches: no grid is kept. The fluids share it.
class NeighborIndex {
public:
    // Sorts points by their cells; points with flat ignore z.
    void build(std::span<const Vec3f> points, float radius, bool flat);

    // Calls visit(index) for each point in the cells round p, cell by cell in Morton order and
    // in the order the points were given within a cell; the caller measures the distance.
    template <typename Visit>
    void around(Vec3f p, Visit&& visit) const {
        const std::array<std::int32_t, 3> c = cell(p);
        const std::int32_t z_low = flat_ ? 0 : -1;
        const std::int32_t z_high = flat_ ? 0 : 1;
        for (std::int32_t dz = z_low; dz <= z_high; ++dz) {
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const auto [first, last] = range(morton_code(c[0] + dx, c[1] + dy, c[2] + dz));
                    for (std::size_t i = first; i < last; ++i) {
                        visit(sorted_[i].second);
                    }
                }
            }
        }
    }

    // The points' indices along the curve.
    [[nodiscard]] std::vector<std::uint32_t> order() const;
    [[nodiscard]] std::size_t size() const { return sorted_.size(); }

private:
    [[nodiscard]] std::array<std::int32_t, 3> cell(Vec3f p) const;
    [[nodiscard]] std::pair<std::size_t, std::size_t> range(std::uint64_t code) const;

    std::vector<std::pair<std::uint64_t, std::uint32_t>> sorted_;
    float inverse_ = 1.0F;
    bool flat_ = false;
};

// Curl noise at p moving along by time: the curl of three smooth noises of whole-number
// lattices (one in 2D, whose curl lies in the plane), so it swirls without gathering or
// spreading. Each axis from about -1 to 1.
[[nodiscard]] Vec3f curl_noise(Vec3f p, float time, std::uint32_t seed, bool flat);

// One particle as the stages see it, in the space its emitter keeps it in.
struct SimParticle {
    Vec3f position;
    Vec3f velocity;
    float age = 0.0F;
    float lifetime = 1.0F;
    float random = 0.0F; // from 0 to 1, fixed for its life
    std::uint32_t index = 0;
    bool alive = true;
    bool born = false; // it started this tick
    bool collided = false;
    int neighbors = 0;
    std::array<float, kMaxParticleAttributes> attributes{};
};

// Where a particle touches what it collides with: the way out and how deep it is.
struct SimContact {
    bool collided = false;
    Vec3f normal;
    float depth = 0.0F;
};

// What a tick's stages read from the world.
struct SimInputs {
    float delta = 0.0F;
    double time = 0.0;
    std::uint32_t seed = 0;
    bool flat = false;
    // Where a particle of a radius at a place touches the scene; none when empty.
    std::function<SimContact(Vec3f position, float radius)> contact;
    // An extra push at a place, an acceleration (attractors); none when empty.
    std::function<Vec3f(Vec3f position)> force;
    // A place in the world, for location events; the place as it is when empty.
    std::function<Vec3f(Vec3f position)> to_world;
};

// Something that happened to a particle that an event rule answers.
struct SimEvent {
    std::size_t rule = 0;
    Vec3f position; // in the particles' space
    Vec3f velocity;
    std::uint32_t particle = 0;
};

// A ParticleSimulation's settings made ready to run: its attributes' Expressions parsed.
struct ParticleStages {
    ParticleSimulation settings;
    std::vector<Expression> attributes;
    int size_attribute = -1;
    int alpha_attribute = -1;
    int kill_attribute = -1;
};

// Parses a simulation's attributes, refusing more than kMaxParticleAttributes, a name used
// twice or a value that doesn't parse, with the JSON Pointer under the component of the field.
[[nodiscard]] Result<ParticleStages> compile_stages(const ParticleSimulation& settings);

// Runs the stages over particles for one tick, after their emitter moved them: new ones take
// their attributes; neighbors push and pull; curl noise, the extra force and drag change their
// velocity and speed limits hold it, the change moving them this tick too; they collide; the
// attributes worked out each tick are; and the collision and location events that happen are
// appended to events, in particle order, a particle's rules in the order given. Particles not alive
// are left as they are. failed is set to the first error an attribute's Expression gives.
void run_stages(const ParticleStages& stages, std::span<SimParticle> particles,
                const SimInputs& inputs, std::vector<SimEvent>& events, std::string& failed);

// Adds the events the death of particle fires, as run_stages adds collision and location ones;
// the caller knows when particles die.
void add_death_events(const ParticleStages& stages, const SimParticle& particle,
                      const SimInputs& inputs, std::vector<SimEvent>& events);

// The value of an attribute named name of a particle, or fallback when the stages have none.
[[nodiscard]] float attribute_or(const ParticleStages& stages, const SimParticle& particle,
                                 int attribute, float fallback);

// A random number from 0 to 1 from a seed and two counters, the same everywhere.
[[nodiscard]] float sim_random(std::uint32_t seed, std::uint32_t a, std::uint32_t b);

// A direction from two random numbers, evenly over a sphere, or a circle when flat.
[[nodiscard]] Vec3f sim_direction(float u, float v, bool flat);

} // namespace slash::world
