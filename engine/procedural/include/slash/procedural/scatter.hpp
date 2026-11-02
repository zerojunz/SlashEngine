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

// Scatter rules: .scatter.json files that say what to place over terrains, meshes, tile maps and
// areas, and how thickly, as Unreal's PCG graphs and Godot's MultiMesh "Populate Surface" do.
// scatter.run places what they ask for as entities or as one MultiMeshInstance3D a layer:
//
//   {
//     "format": 1,
//     "seed": 7,
//     "layers": [
//       {"name": "trees", "mesh": "models/tree.model.json", "as": "multimesh", "on": ["ground"],
//        "density": 0.4, "spacing": 1.5, "slope": [0, 25], "rotate_random": 1,
//        "scale_random": 0.2, "noise": {"frequency": 0.08}, "noise_range": [0, 1]},
//       {"name": "rocks", "prefab": "props/rock.prefab.json", "on": ["ground"], "count": 12,
//        "spacing": 2, "avoid": ["trees"]}
//     ]
//   }
//
// Places are picked with a seeded random number generator, so the same rules over the same
// ground place the same things on every machine.
namespace slash::procedural {

inline constexpr std::string_view kScatterSuffix = ".scatter.json";

// One kind of thing scattered.
struct ScatterLayer {
    std::string name; // letters, digits, "_" and "-"
    // What each place gets: a model drawn there, or an entity extending a prefab. 2D layers
    // place prefabs.
    std::string mesh;
    std::string prefab;
    // "entities" (one an instance) or "multimesh" (one MultiMeshInstance3D drawing mesh at
    // every place; 3D only).
    std::string as = "entities";
    // The entities to scatter over, with those under them: in 3D their geometry as navigation
    // bakes read it (meshes, CSG, terrains, grid maps, static bodies' shapes); in 2D
    // TileMapLayers' cells. Empty: the area alone, in 3D on the ground plane y = 0.
    std::vector<std::string> on;
    // Only within [min x, min z, max x, max z] in 3D or [min x, min y, max x, max y] in 2D, in
    // world space.
    std::optional<std::array<float, 4>> area;
    // 2D: only cells whose legend key or tile is one of these.
    std::vector<std::string> tiles;
    // How many: a count, or a density per square unit (meter in 3D, pixel in 2D). Fewer are
    // placed when the rules leave too little room.
    std::optional<int> count;
    std::optional<float> density;
    // The least distance between two places of this layer and those of the layers it avoids.
    float spacing = 0.0F;
    std::vector<std::string> avoid; // earlier layers, by name
    // 3D: the ground's slope in degrees from flat, and the world height, each as [min, max].
    Vec2 slope{0.0F, 90.0F};
    std::optional<Vec2> height;
    // Only where this noise, sampled at (x, z) in 3D or (x, y) in 2D, is within noise_range.
    std::optional<NoiseSettings> noise;
    Vec2 noise_range{0.0F, 1.0F};
    // As Godot's Populate Surface: 3D places stand along the ground's normal when
    // align_to_normal is on, else upright; each turns about its up axis by up to rotate_random
    // and tilts by up to tilt_random half turns either way, and is scaled by scale plus up to
    // scale_random either way. offset moves it along its up axis (a negative one sinks it).
    bool align_to_normal = false;
    float rotate_random = 0.0F;
    float tilt_random = 0.0F;
    float scale = 1.0F;
    float scale_random = 0.0F;
    float offset = 0.0F;
};

// A .scatter.json file.
struct ScatterRules {
    std::string space = "3d"; // "3d" or "2d"
    std::uint32_t seed = 0;
    std::vector<ScatterLayer> layers;
};

// JSON Schema for .scatter.json files.
[[nodiscard]] json::Value scatter_schema();

// Every error in a .scatter.json document, each with its JSON Pointer under at: the schema's,
// then names used twice, layers that place nothing, avoid naming no earlier layer, and a 2D
// layer drawing a mesh.
[[nodiscard]] std::vector<json::ValidationError> validate_scatter(const json::Value& doc,
                                                                  const std::string& at = "");

// The rules a valid document holds, or its first error.
[[nodiscard]] Result<ScatterRules> parse_scatter(const json::Value& doc);

// The ground a layer scatters over: triangles in world space, in 2D with z = 0. Their winding
// doesn't matter: 3D normals are turned to face up.
struct ScatterTriangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
};

// A place picked: where, the Euler angles (radians, Godot's YXZ) or the 2D rotation in
// rotation.z, and the uniform scale.
struct ScatterPlace {
    Vec3 position;
    Vec3 rotation;
    float scale = 1.0F;
};

// How a layer went: what it placed and, when it placed fewer than asked, why the tries it threw
// away failed.
struct ScatterResult {
    std::vector<ScatterPlace> places;
    int wanted = 0;
    int tries = 0;
    int outside = 0; // outside the area
    int slope = 0;
    int height = 0;
    int noise = 0;
    int spacing = 0;
    float ground_area = 0.0F; // of the triangles scattered over
};

// Scatters a layer over the ground: picks a triangle by area, a point in it as Godot's
// Face3::get_random_point_inside does, keeps it if the layer's rules allow and turns and scales
// it as Godot's MultiMesh populate does, up to 30 tries a place wanted. avoid holds places of
// earlier layers to keep spacing from. seed is the rules' seed; each layer has its own stream
// by its index.
[[nodiscard]] ScatterResult scatter(const ScatterLayer& layer, std::size_t index,
                                    std::uint32_t seed, bool flat,
                                    std::span<const ScatterTriangle> ground,
                                    std::span<const Vec3> avoid);

} // namespace slash::procedural
