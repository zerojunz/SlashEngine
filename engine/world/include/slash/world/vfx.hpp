#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Visual effects as recipes, after Unreal's Niagara systems (Godot has none: its effects are
// scenes of particles, lights and sounds a script times). A .vfx.json file lays layers on one
// timeline: each layer is an entity of components (particles, ribbons and trails, mesh
// particles, flipbooks, decals, lights, sounds, screen flashes, camera shakes, anything a scene
// holds) that appears at its start, has its fields moved along curves while it lasts and goes
// at its end, particles first finishing what they emitted. Any value may be an expression over
// the recipe's parameters (size, color, intensity: whatever it names), which a VisualEffect3D or
// VisualEffect2D sets. Level of detail drops layers and thins particles with the camera's
// distance, and a budget caps what one effect may cost.
namespace slash::world {

inline constexpr std::string_view kVfxSuffix = ".vfx.json";
[[nodiscard]] bool is_vfx_path(std::string_view path);

// A key on a curve: a time in the layer's seconds and a value, which may be an expression.
struct VfxKey {
    float time = 0.0F;
    json::Value value;
};

// A field moved along keys while a layer lasts: "OmniLight3D/light_energy",
// "CPUParticles3D/scale_amount_max", "ColorRect/color/a".
struct VfxCurve {
    std::string path;
    std::vector<VfxKey> keys;
    std::string ease = "linear"; // "linear", "smooth" (smoothstep) or "step"
};

struct VfxLayer {
    std::string name;
    float start = 0.0F;    // seconds into the effect
    float duration = 0.0F; // seconds it lasts; 0 lasts to the effect's end
    // Where it sits in the effect's space: 3 numbers in 3D, 2 in 2D (each may be an expression),
    // and its turn (Euler radians in 3D, radians in 2D).
    json::Value offset;
    json::Value rotation;
    // Its entity's components by name, as a scene writes them; values may be expressions.
    json::Value components;
    std::vector<VfxCurve> curves;
    // The least level of detail it plays at, 0 to 1: a layer with 0.5 drops once the effect is
    // far enough for its detail to fall below a half.
    float detail = 0.0F;
    // Whether it stays where it appeared (a decal, a scorch) rather than moving with the effect.
    bool world = false;
};

// A level of detail: from distance (meters in 3D, pixels in 2D) from the current camera on, the
// effect plays at detail: layers needing more drop and particles' amounts are scaled by it.
struct VfxLod {
    float distance = 0.0F;
    float detail = 1.0F;
};

// What one effect may cost at full detail; 0 is no limit.
struct VfxBudget {
    int particles = 0; // particles alive at once, over every emitter
    int lights = 0;
    int sounds = 0;
    int decals = 0;
    int layers = 0; // layers playing at once
};

struct VfxRecipe {
    std::string name;
    std::string dimension = "3d"; // "3d" or "2d"
    float duration = 1.0F;        // seconds
    bool loop = false;
    // Parameters by name with their defaults: numbers, booleans, strings or arrays (colors); an
    // object once parsed.
    json::Value parameters;
    std::vector<VfxLayer> layers;
    std::vector<VfxLod> lod; // nearest first
    VfxBudget budget;
};

// JSON Schema for .vfx.json files.
[[nodiscard]] json::Value vfx_schema();
// The schema's errors, then each expression that doesn't parse, layers named twice, curves
// with keys out of order and levels of detail out of order.
[[nodiscard]] std::vector<json::ValidationError> validate_vfx(const json::Value& doc);
[[nodiscard]] Result<VfxRecipe> parse_vfx(const json::Value& doc);
// A recipe read through the world's assets (kept until the file changes) or file reader.
[[nodiscard]] Result<std::shared_ptr<const VfxRecipe>> load_vfx(const World& world,
                                                                std::string_view path);

// The recipe's parameters with an effect's own over them.
[[nodiscard]] json::Value vfx_parameters(const VfxRecipe& recipe, const json::Value& own);
// A value with every expression in it worked out: a string starting with '=' is an expression
// over the parameters, detail and any extra names; whole numbers come out as integers.
[[nodiscard]] Result<json::Value> vfx_value(const json::Value& value, const json::Value& parameters,
                                            float detail, const json::Value& extra = {});
// A layer's components for parameters at a level of detail: expressions worked out, and each
// particle emitter's amount scaled by detail (never below 1).
[[nodiscard]] Result<json::Value> vfx_components(const VfxLayer& layer,
                                                 const json::Value& parameters, float detail);
// A curve's value t seconds into its layer, which also reads t and life (t over the layer's
// length) as names.
[[nodiscard]] Result<json::Value> vfx_curve_value(const VfxCurve& curve, float t, float length,
                                                  const json::Value& parameters, float detail);
// How long a layer lasts in an effect.
[[nodiscard]] float vfx_layer_length(const VfxRecipe& recipe, const VfxLayer& layer);
// The level of detail at a distance from the camera.
[[nodiscard]] float vfx_detail(const VfxRecipe& recipe, float distance);

// What an effect costs at its busiest moment: particles alive (each emitter's amount), lights,
// sounds, decals and layers playing at once, over the times layers start.
struct VfxCost {
    int particles = 0;
    int lights = 0;
    int sounds = 0;
    int decals = 0;
    int layers = 0;
};
[[nodiscard]] Result<VfxCost> vfx_cost(const VfxRecipe& recipe, const json::Value& parameters,
                                       float detail);
// Where cost goes past the recipe's budget, as "particles: 900 of 500".
[[nodiscard]] std::vector<std::string> vfx_over_budget(const VfxRecipe& recipe,
                                                       const VfxCost& cost);

// Adds the "vfx" system, which plays every VisualEffect3D and VisualEffect2D and shakes the
// current cameras by CameraShakes. Emits "vfx_started", "vfx_finished" and "vfx_failed".
void add_vfx_system(World& world);
// The entity an effect's layer plays as, or null while it isn't playing.
[[nodiscard]] entt::entity vfx_layer_entity(const World& world, entt::entity effect,
                                            std::string_view layer);
// The current cameras' offsets that shakes add now: a Camera3D's h_offset and v_offset, and a
// Camera2D's offset.
struct CameraShakeOffsets {
    Vec2 camera_3d;
    Vec2 camera_2d;
};
[[nodiscard]] CameraShakeOffsets camera_shake_offsets(const World& world);

// The effects a tool's recipe plays, by the world event that plays them: a .creature.json,
// .vehicle.json, .character.json or .foliage.json file's "vfx", each event kind (such as
// "creature_attacked", "vehicle_hit", "killed" or "foliage_harvested") naming a .vfx.json
// recipe. A vehicle's "running" plays while its throttle is open.
using VfxHooks = std::map<std::string, std::string, std::less<>>;
// The JSON Schema of a recipe's "vfx".
[[nodiscard]] json::Value vfx_hooks_schema();
// A recipe's "vfx", or none.
[[nodiscard]] VfxHooks read_vfx_hooks(const json::Value& doc);
// The "vfx" of the recipe an entity is made from (its Creature3D or Creature2D, Vehicle3D or
// Vehicle2D, Character2D, or a MeshInstance3D drawing a .character.json file), or of a
// .foliage.json file; none when it has none or it doesn't load.
[[nodiscard]] VfxHooks vfx_hooks_of(const World& world, entt::entity entity);
[[nodiscard]] VfxHooks foliage_vfx_hooks(const World& world, std::string_view path);
// Plays a recipe once where something happened: a new entity <near>_<what> (numbered past
// taken names) with a VisualEffect3D at position, or a VisualEffect2D when flat, that frees
// itself when it has finished. Null when recipe is "" or no entity could be made.
entt::entity play_vfx(World& world, std::string_view recipe, std::string_view near,
                      std::string_view what, Vec3 position, bool flat);

} // namespace slash::world
