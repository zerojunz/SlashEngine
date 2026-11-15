#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/procedural/noise.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Procedural graphs: .pcg.json files of steps over points, after Unreal's PCG graphs, that
// pcg.run follows to fill a world with what every other tool makes. Samplers make points (over
// the ground, on a grid, along a path, at entities or storyboard places), filters and modifiers
// keep, thin, move and turn them (noise, slope, height, distance, density, spacing, difference),
// and spawners place things at them: prefabs, meshes, multimeshes, foliage instances, components
// (lights, wind, physics fields, fluids, effects, destructibles), any placing command (creatures,
// characters, vehicles, water) and routes through them.
//
//   {
//     "format": 1, "seed": 7,
//     "nodes": [
//       {"name": "ground", "op": "surface", "on": ["valley"], "density": 0.2, "slope": [0, 30]},
//       {"name": "meadow", "op": "filter", "from": ["ground"],
//        "noise": {"frequency": 0.05}, "noise_range": [0, 1]},
//       {"name": "grass", "op": "foliage", "from": ["meadow"], "type":
//       "plants/grass.foliage.json"},
//       {"name": "deer", "op": "command", "from": ["meadow"], "keep": 0.02,
//        "command": "creature.place",
//        "params": {"recipe": "fauna/deer.creature.json", "id": "$id", "at": "$local",
//                   "yaw": "$yaw", "parent": "$parent"}}
//     ]
//   }
//
// Each node's points come from the nodes it names in from, which come before it. Random choices
// hash the graph's seed, the node's index and the point's own seed, so a graph over the same world
// makes the same points on every machine.
namespace slash::procedural {

inline constexpr std::string_view kPcgSuffix = ".pcg.json";

// A point: where, its turn (radians, Godot's YXZ; the 2D rotation in z), its uniform scale, its
// density (0 to 1, which filters read and noise sets), the ground's normal under it and a seed
// of its own that its random choices hash.
struct PcgPoint {
    Vec3 position;
    Vec3 rotation;
    float scale = 1.0F;
    float density = 1.0F;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    std::uint32_t seed = 0;
};

// A step: its name, op and the nodes it reads, and the whole node for the op's own fields.
struct PcgNode {
    std::string name;
    std::string op;
    std::vector<std::string> from;
    json::Value fields;
};

// What a graph may make: the most points any node holds and the most entities it spawns.
struct PcgBudget {
    std::optional<int> points;
    std::optional<int> entities;
};

// A .pcg.json file.
struct PcgGraph {
    std::string space = "3d"; // "3d" or "2d"
    std::uint32_t seed = 0;
    PcgBudget budget;
    std::vector<PcgNode> nodes;
};

// The ops, by kind.
[[nodiscard]] bool pcg_is_sampler(std::string_view op);
[[nodiscard]] bool pcg_is_spawner(std::string_view op);

// JSON Schema for .pcg.json files.
[[nodiscard]] json::Value pcg_schema();

// Every error in a .pcg.json document, each with its JSON Pointer under at: the schema's, then
// names that aren't IDs or are used twice, from naming no earlier node, samplers given from and
// other ops given none, fields the op doesn't take, ranges out of order and noise settings.
[[nodiscard]] std::vector<json::ValidationError> validate_pcg(const json::Value& doc,
                                                              const std::string& at = "");

// The graph a valid document holds, or its first error.
[[nodiscard]] Result<PcgGraph> parse_pcg(const json::Value& doc);

// A number hashed from others: the seeds of points and of their random choices.
[[nodiscard]] std::uint32_t pcg_hash(std::uint32_t a, std::uint32_t b, std::uint32_t c = 0);
// From 0 up to 1, hashed from a seed and a salt.
[[nodiscard]] float pcg_unit(std::uint32_t seed, std::uint32_t salt);

// Points on a grid over [min x, min z, max x, max z] (3D, at y 0) or [min x, min y, max x,
// max y] (2D), spacing apart, each moved by up to jitter times spacing either way.
[[nodiscard]] std::vector<PcgPoint> pcg_grid(const std::array<float, 4>& area, float spacing,
                                             float jitter, std::uint32_t seed, bool flat);

// What a filter keeps; each is left out when not given.
struct PcgFilter {
    std::optional<Vec2> slope;  // degrees from flat (3D)
    std::optional<Vec2> height; // world y (3D) or y (2D)
    std::optional<NoiseSettings> noise;
    Vec2 noise_range{0.0F, 1.0F};
    std::optional<Vec2> density; // the points' own density
    std::optional<float> keep;   // the share kept at random
    std::optional<int> limit;    // the most kept, the first in order
    std::optional<std::array<float, 4>> area;
};
// The points a filter keeps, in order. salt makes keep's choices the node's own.
[[nodiscard]] std::vector<PcgPoint> pcg_filter(std::span<const PcgPoint> points,
                                               const PcgFilter& filter, std::uint32_t salt,
                                               bool flat);

// Sets each point's density from noise at it (mapped from range to 0..1 and clamped), times
// what it had when multiply is on.
void pcg_density(std::vector<PcgPoint>& points, const NoiseSettings& noise, Vec2 range,
                 bool multiply, bool flat);

// Keeps the points no closer than spacing to one kept before, densest first (ties in order),
// then puts them back in their order.
[[nodiscard]] std::vector<PcgPoint> pcg_prune(std::span<const PcgPoint> points, float spacing,
                                              bool flat);

// The points no closer than distance to any of others, measured across the ground (x and z in
// 3D) as every distance between points is.
[[nodiscard]] std::vector<PcgPoint> pcg_difference(std::span<const PcgPoint> points,
                                                   std::span<const Vec3> others, float distance,
                                                   bool flat);

// The points within [min, max] of the nearest of targets (none when there are no targets).
[[nodiscard]] std::vector<PcgPoint> pcg_near(std::span<const PcgPoint> points,
                                             std::span<const Vec3> targets, Vec2 range, bool flat);

// How a transform moves the points: offset in the world, then along each point's up
// (offset_up), turned by rotation and up to rotate_random half turns about its up axis (and
// tilt_random about the others), scaled by scale plus up to scale_random either way, and in 3D
// stood along the ground's normal with align_to_normal.
struct PcgTransform {
    Vec3 offset;
    float offset_up = 0.0F;
    Vec3 rotation;
    float rotate_random = 0.0F;
    float tilt_random = 0.0F;
    float scale = 1.0F;
    float scale_random = 0.0F;
    bool align_to_normal = false;
};
void pcg_transform(std::vector<PcgPoint>& points, const PcgTransform& transform, std::uint32_t salt,
                   bool flat);

// The points in an order that visits them as a route: from the first, each time the nearest one
// not yet visited (in 3D through the air, so routes take hills into account).
[[nodiscard]] std::vector<PcgPoint> pcg_route_order(std::span<const PcgPoint> points, bool flat);

// The area a set of points covers: [min x, min z, max x, max z] (3D) or [min x, min y, max x,
// max y] (2D); zero for none.
[[nodiscard]] std::array<float, 4> pcg_bounds(std::span<const PcgPoint> points, bool flat);

} // namespace slash::procedural
