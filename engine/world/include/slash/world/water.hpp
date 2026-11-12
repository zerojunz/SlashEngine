#pragma once

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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Oceans, lakes and rivers, after Unreal's Water plugin (Godot has none): a surface of Gerstner
// waves (GPU Gems' sum of trochoids, each crest sharpened by moving the water toward it) with the
// ripples and wakes things leave rings of, foam where crests fold (the surface's Jacobian), where
// rings are steep and along the shore, a river's current along its Path3D, and the water's own
// velocity, which floats and drags bodies in physics3d and physics2d. Heights are found under a
// point by fixed-point inversion of the waves' sideways motion, so a query asks for the height
// at a place, not of a moving bit of water. Built with -ffp-contract=off, with no libm but square
// roots (slash::cos_sin and slash::exp), so the water is the same bits everywhere. 2D water is
// seen from the side, its waves along x, in pixels with y down.
namespace slash::world {

inline constexpr std::string_view kWaterSuffix = ".water.json";
[[nodiscard]] bool is_water_path(std::string_view path);

// The most waves a recipe holds, and the most ripples one body of water keeps.
inline constexpr std::size_t kMaxWaterWaves = 16;
inline constexpr std::size_t kMaxWaterRipples = 64;

// One Gerstner wave: direction (x, z; x alone in 2D), wavelength and amplitude in meters (pixels
// in 2D), steepness from 0 (round swells) to 1 (crests as sharp as they get without looping),
// speed (0 for deep water's, sqrt(g wavelength / 2 pi)) and phase in radians.
struct WaterWave {
    Vec2f direction{1.0F, 0.0F};
    float wavelength = 8.0F;
    float amplitude = 0.2F;
    float steepness = 0.5F;
    float speed = 0.0F;
    float phase = 0.0F;
};

// What a .water.json file says (slash-cli schema water).
struct WaterRecipe {
    enum class Kind : std::uint8_t { Lake, Ocean, River };
    std::string name;
    Kind kind = Kind::Lake;
    bool flat = false; // space "2d"
    std::vector<WaterWave> waves;
    // Physics.
    float density = 1000.0F; // kg/m^3, or kg/px^2 in 2D (default 0.001)
    float buoyancy = 1.0F;   // scales the lift
    float drag = 1.0F;       // the share of a body's speed through the water lost a second
    float angular_drag = 1.0F;
    float heave_damping = 8.0F; // the share of a wet point's speed along gravity lost a second
    Vec2f current;              // lakes and oceans: the water's drift (x, z), carrying waves along
    float flow = 0.0F;          // rivers: the current along the path (along x in 2D)
    // Ripples and wakes.
    float ripple_wavelength = 0.5F; // pixels in 2D (default 30)
    float ripple_speed = 0.0F;      // 0 for the speed of waves that long
    float ripple_decay = 1.0F;      // the share of a ripple's height lost a second, as e^-decay t
    float splash = 1.0F;            // scales the ripples bodies and liquids make
    // Look.
    Vec4 shallow{0.18F, 0.55F, 0.6F, 0.55F};
    Vec4 deep{0.02F, 0.12F, 0.25F, 0.92F};
    Vec4 foam{0.95F, 0.97F, 1.0F, 1.0F};
    float clarity = 4.0F;     // how deep the water is where it shows the deep color (px in 2D: 200)
    float crest_foam = 0.85F; // crests foam where the surface's Jacobian falls below this
    float shore_foam = 0.6F;  // the depth the shore foams to (px in 2D: 12)
    float roughness = 0.04F;
    float spacing = 0.5F; // the drawn mesh's cells (px in 2D: 8)
    float view = 120.0F;  // oceans: how far round the camera the surface draws
};

// Validates a .water.json document (its own fields; extends is followed as it loads).
[[nodiscard]] json::Value water_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_water(const json::Value& doc);
// The recipe a document makes, its defaults by kind and space filled in and the waves its wind
// raises worked out.
[[nodiscard]] Result<WaterRecipe> parse_water(const json::Value& doc);
// A recipe read through the world's assets, following extends and with overrides (a merge
// patch) over it; kept while the files stay the same.
[[nodiscard]] Result<std::shared_ptr<const WaterRecipe>>
load_water(const World& world, std::string_view path, const json::Value& overrides = {});

// The gravity water waves under: 9.8 m/s^2, or 980 px/s^2 in 2D.
[[nodiscard]] float water_gravity(const WaterRecipe& recipe);
// A wave's angular frequency: k times its speed, or deep water's sqrt(g k).
[[nodiscard]] float water_wave_frequency(const WaterRecipe& recipe, const WaterWave& wave);

// A ring a body or a drop left on the water, where (x, z; x and 0 in 2D), when (water_time) and
// how high it started (meters, pixels in 2D).
struct WaterRipple {
    Vec2f at;
    double born = 0.0;
    float strength = 0.0F;
};

// One body of water as it is placed in the world now.
struct WaterBody {
    entt::entity entity{entt::null};
    std::string id;
    std::shared_ptr<const WaterRecipe> recipe;
    bool flat = false;
    // 3D: the entity's global transform, its surface at rest on its x-z plane at y 0; 2D: its
    // Transform2D's place, the surface at rest along y = position.y.
    Affine3D place;
    Affine3D inverse;
    Vec2f half;          // the area's half size in its space (x, z; x and depth in 2D)
    bool bounded = true; // oceans aren't: the area only bounds where the shore is measured
    // Rivers: the middle of the river in world space every spacing along its path, with each
    // point's distance along it.
    std::vector<Vec3f> river;
    std::vector<float> river_along;
    float width = 0.0F;
    // The ground's height (y; px down in 2D) over the area every ground_cell, row by row (one row
    // in 2D), NaN where nothing is below; empty until measured (water_bodies with measure).
    std::vector<float> ground;
    int ground_columns = 0;
    int ground_rows = 0;
    float ground_cell = 0.0F;
    std::vector<WaterRipple> ripples;
    // Drawing.
    bool visible = true;
    std::uint32_t layers = 1;
    int z_index = 0;
    bool z_as_relative = true;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    int light_mask = 1;
};

// What the water is at a place: whether it is over the water's area, the surface's height there
// (y up in 3D; y in pixels, down, in 2D), the way out of the water (up: (0, 1, 0) at rest in
// 3D, (0, -1, 0) in 2D), the water's own velocity at the surface (its waves' motion and its
// current), foam from 0 to 1 and how deep it is to the ground (infinity where not measured).
// position is the point on the surface.
struct WaterSample {
    bool inside = false;
    float height = 0.0F;
    Vec3f position;
    Vec3f normal{0.0F, 1.0F, 0.0F};
    Vec3f velocity;
    float foam = 0.0F;
    float depth = std::numeric_limits<float>::infinity();
};

// The water under or over point (x, z in 3D; x in 2D), time seconds into water_time. Without
// ripples, only its waves, current and shores: what floats bodies, as rings are too small and
// quick to push them (and a body's own splash would pump it).
[[nodiscard]] WaterSample sample_water(const WaterBody& body, Vec3f point, double time,
                                       bool ripples = true);
// The bit of the surface at rest at (x, z) (x in 2D; a river's at its rest height) at time:
// position is where Gerstner's displacement and the ripples move it, as drawing moves a mesh's
// corners, with its normal, velocity, foam and depth there.
[[nodiscard]] WaterSample displace_water(const WaterBody& body, Vec3f rest, double time);
// The color water shows at a depth (deep's when infinite), with foam over it.
[[nodiscard]] Vec4 water_color(const WaterRecipe& recipe, float depth, float foam);

// The water a world holds, each Water3D (flat false) or Water2D placed by its tree, in entity
// order. measure measures the ground under bodies whose ground isn't kept for their placing yet,
// through the host physics set; without it a body whose ground isn't kept has none. The water
// system measures; physics, drawing and queries read.
[[nodiscard]] std::vector<WaterBody> water_bodies(World& world, bool flat, bool measure = false);
// The same for drawing, which changes nothing: no warnings, and only ground already measured.
[[nodiscard]] std::vector<WaterBody> water_bodies(const World& world, bool flat);
// The body of water entity is, when it is one.
[[nodiscard]] std::optional<WaterBody> water_body(World& world, entt::entity entity,
                                                  bool measure = false);

// What water asks of physics: the ground's height below from (y up in 3D; y down in 2D, from
// from down to from + reach), skipping bodies that move. physics3d and physics2d set them.
struct WaterHost {
    std::function<std::optional<float>(Vec3f from, float reach)> ground;
};
void set_water_host_3d(World& world, WaterHost host);
void set_water_host_2d(World& world, WaterHost host);

// The water's clock: seconds the water system has run, plus interpolation of the last tick for
// drawing between ticks.
[[nodiscard]] double water_time(const World& world, float interpolation = 0.0F);

// Leaves a ring on water entity at (x, z in 3D; x in 2D), strength meters (pixels) high; rings
// started close by in the last tenth of a second add to each other. Gameplay code's splashes.
void add_water_ripple(World& world, entt::entity water, Vec3f at, float strength);

// Adds the water system (priority 959, after physics and fluids): counts water_time, measures
// shores, takes in liquid particles that fall into it, rings where bodies enter, leave and move
// through it and emits water_entered and water_exited. canvas::install calls it.
void install_water(World& world);

} // namespace slash::world
