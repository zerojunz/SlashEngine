#pragma once

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Fluids and smoke with no grids, after Niagara's fluids and Unreal's water tools (Godot has
// none), built on particles: liquids are position-based fluids (Macklin and Müller's density
// constraint, with XSPH viscosity, cohesion for surface tension and vorticity confinement), gases
// are particles carrying temperature, smoke density and fuel that rise by buoyancy, cool, burn
// and swirl around vortex particles. Neighbors are found along a Morton curve (NeighborIndex),
// never in a grid. 2D fluids are 3D ones at z 0. Built with -ffp-contract=off and with no libm
// but square roots, and the work split over the job pool by particle, so a fluid steps the same
// bits on any number of threads and every platform.
namespace slash::world {

inline constexpr std::string_view kFluidSuffix = ".fluid.json";
[[nodiscard]] bool is_fluid_path(std::string_view path);

// The most particles one fluid may hold.
inline constexpr std::size_t kMaxFluidParticles = 65536;

// What a .fluid.json file says (slash-cli schema fluid).
struct FluidRecipe {
    std::string name;
    bool gas = false;             // kind "gas": smoke, steam and fire
    bool flat = false;            // space "2d"
    float radius = 0.05F;         // particle radius: meters, or pixels in 2D (default 4)
    float rest_density = 1000.0F; // kg/m^3 (a liquid's), or kg/m^2 in 2D
    int iterations = 3;
    int substeps = 1;
    float relaxation = 0.2F; // the density constraint's softness
    float viscosity = 0.02F; // XSPH: the share of neighbors' relative velocity taken
    float surface_tension = 0.2F;
    float vorticity = 0.05F; // vorticity confinement
    float gravity_scale = 1.0F;
    float drag = 0.0F;     // the share of velocity lost a second
    float lifetime = 0.0F; // seconds a particle lives; 0 for ever
    std::size_t max_particles = 4096;
    float bounce = 0.0F;
    float friction = 0.1F;
    // Gases.
    float buoyancy = 1.5F; // lift as a share of gravity, by the rise over absolute heat
    float ambient_temperature = 20.0F;
    float cooling = 0.6F;         // the share of the difference from ambient lost a second
    float diffusion = 0.3F;       // the share of neighbors' temperature and smoke taken a second
    float dissipation = 0.15F;    // smoke density lost a second
    float ignition = 250.0F;      // fuel burns above this temperature
    float burn_rate = 1.5F;       // fuel burned a second
    float heat = 600.0F;          // degrees a unit of fuel burned adds
    float smoke_yield = 1.0F;     // smoke density a unit of fuel burned adds
    float vortex_share = 0.1F;    // the share of particles that are vortex particles
    float vortex_strength = 0.5F; // how hard a vortex particle swirls its neighbors
    float spread = 0.05F;         // gases: how hard crowded particles push apart
    // Look.
    Vec4 color{0.2F, 0.45F, 0.85F, 0.85F};
    Vec4 foam{1.0F, 1.0F, 1.0F, 1.0F};
    Vec4 smoke{0.35F, 0.35F, 0.38F, 0.6F};
    Vec4 fire{1.0F, 0.55F, 0.15F, 1.0F};
    // Budget: the most particles and neighbor visits a tick (deterministic work, not time) a
    // fluid may take; fluid.check reports what goes past.
    std::size_t budget_particles = 0;
    std::size_t budget_work = 0; // neighbor visits a tick
};

// Validates a .fluid.json document (its own fields; extends is followed as it loads).
[[nodiscard]] json::Value fluid_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_fluid(const json::Value& doc);
// The recipe a document makes, its defaults by kind and space filled in.
[[nodiscard]] Result<FluidRecipe> parse_fluid(const json::Value& doc);
// A recipe read through the world's assets, following extends and with overrides (a merge
// patch) over it; kept while the files stay the same.
[[nodiscard]] Result<std::shared_ptr<const FluidRecipe>>
load_fluid(const World& world, std::string_view path, const json::Value& overrides = {});

// A fluid's particles, kept beside the components by entity ID.
struct FluidParticles {
    std::vector<Vec3f> position;
    std::vector<Vec3f> velocity;
    std::vector<float> age;
    std::vector<float> temperature; // gases
    std::vector<float> smoke;       // gases: smoke density, 0 to about 1
    std::vector<float> fuel;        // gases
    std::vector<float> density;     // the last step's density over rest (liquids)
    std::vector<std::uint32_t> id;  // a number each new particle takes, for vortices and order
    std::uint32_t next_id = 0;
    double emitted = 0.0; // particles owed to emitters, carried between ticks
    bool filled = false;
    std::size_t contacts = 0; // particles touching a collider in the last step
    std::size_t work = 0;     // neighbor visits in the last tick

    [[nodiscard]] std::size_t size() const { return position.size(); }
    void add(Vec3f p, Vec3f v, float temperature, float smoke, float fuel);
    void remove_if(const std::function<bool(std::size_t)>& doomed);
};

// What a collider says about a particle near it: where on its surface and which way out
// (normal), how deep the particle is in (depth, the particle's radius included), how fast the
// surface moves there and the body (null for a wall with no body).
struct FluidContact {
    Vec3f normal;
    float depth = 0.0F;
    Vec3f velocity;
    entt::entity body{entt::null};
};

// The physics a fluid steps in: collide gives each point's contact (nullopt where it touches
// nothing) with radius the particles', and push hands a body the impulse the fluid gave it at a
// point, and gravity gives the project's gravity. physics3d and physics2d set them
// (set_fluid_host_3d and _2d); without them fluids collide only with their bounds and fall at
// 9.8 m/s^2 (980 px/s^2 in 2D).
struct FluidHost {
    std::function<void(std::span<const Vec3f> points, float radius,
                       std::vector<std::optional<FluidContact>>& out)>
        collide;
    std::function<void(entt::entity body, Vec3f impulse, Vec3f at)> push;
    std::function<Vec3f()> gravity;
};
void set_fluid_host_3d(World& world, FluidHost host);
void set_fluid_host_2d(World& world, FluidHost host);

// What one step of a fluid takes, beside its recipe.
struct FluidStepInputs {
    float seconds = 1.0F / 60.0F;
    Vec3f gravity{0.0F, -9.8F, 0.0F};
    // A box the particles stay in (its world placement and half size), when the fluid has one.
    std::optional<Affine3D> bounds;
    Vec3f bounds_half;
    const FluidHost* host = nullptr;
    // Extra acceleration at a point (wind and physics fields), when given.
    std::function<Vec3f(Vec3f)> acceleration;
    jobs::Pool* pool = nullptr;
};

// Steps a fluid's particles by inputs.seconds (in the recipe's substeps).
void step_fluid(const FluidRecipe& recipe, FluidParticles& particles,
                const FluidStepInputs& inputs);

// The mass of one particle: rest density times the volume (area in 2D) each stands for.
[[nodiscard]] float fluid_particle_mass(const FluidRecipe& recipe);
// The volume (area in 2D) a particle stands for: its spacing cubed (squared).
[[nodiscard]] float fluid_particle_volume(const FluidRecipe& recipe);

// The color particle i draws with: a liquid's color, whiter as foam the faster it goes; a gas's
// smoke by its density, glowing toward the fire color above ignition. Alpha 0 draws nothing.
[[nodiscard]] Vec4 fluid_color(const FluidRecipe& recipe, const FluidParticles& particles,
                               std::size_t i);
// How wide a particle draws: a liquid's a little past its spacing, a gas's puffs three times it.
[[nodiscard]] float fluid_draw_size(const FluidRecipe& recipe);

// The particles of a Fluid3D or Fluid2D, in worlds install_fluids readied; null when none.
[[nodiscard]] const FluidParticles* fluid_particles(const World& world, entt::entity entity);
[[nodiscard]] FluidParticles* fluid_particles_mut(World& world, entt::entity entity);

// Adds the fluids system (priority 958, after physics): fills, emits into, steps and drains
// each Fluid3D and Fluid2D. canvas::install calls it.
void install_fluids(World& world);

} // namespace slash::world
