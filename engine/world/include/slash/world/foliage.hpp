#pragma once

// Foliage types, after Unreal's Foliage tool (Godot has none): a .foliage.json file says what one
// kind of plant or rock is (grass, flowers, shrubs, bushes, trees, moss, rocks, pebbles), how
// thickly a brush paints it, how it bends in the wind and parts for characters, whether it blocks
// them, can be climbed or harvested, and how far it is drawn. Foliage3D and Foliage2D components
// hold the instances painted of each type.
//
//   {
//     "format": 1,
//     "kind": "grass",
//     "mesh": "plants/grass.model.json",
//     "density": 6, "spacing": 0.15, "scale": [0.7, 1.2], "slope": [0, 35],
//     "cull_distance": [30, 40],
//     "wind": {"bend": 0.06, "sway": 0.3},
//     "interaction": {"radius": 0.6, "bend": 1.0}
//   }

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

inline constexpr std::string_view kFoliageSuffix = ".foliage.json";

[[nodiscard]] bool is_foliage_path(std::string_view path);

// The shape an instance blocks with, in its own space (scaled with it): 3D "capsule", "sphere",
// "cylinder" or "box"; 2D "circle", "rectangle" or "capsule"; "none" blocks nothing.
struct FoliageCollision {
    std::string shape = "none";
    float radius = 0.25F;
    float height = 2.0F;         // capsule and cylinder, end to end, standing up
    Vec3 size{1.0F, 1.0F, 1.0F}; // box and rectangle
    Vec3 offset; // where the shape's middle is; a capsule's default stands on the ground

    friend bool operator==(const FoliageCollision&, const FoliageCollision&) = default;
};

// One kind of plant or rock.
struct FoliageType {
    std::string space = "3d"; // "3d" or "2d"
    // "grass", "flower", "shrub", "bush", "tree", "moss", "rock" or "pebble".
    std::string kind = "grass";
    std::string mesh;        // 3D: a model file drawn at each instance
    std::string texture;     // 2D: an image drawn at each instance
    Vec2 size{16.0F, 16.0F}; // 2D: pixels across and up at scale 1
    Vec2 origin{0.5F, 1.0F}; // 2D: the point of the image on the ground, as shares of size
    // Instances a brush paints per square meter (3D) or per square pixel (2D) at full strength.
    float density = 1.0F;
    float spacing = 0.0F;         // the least distance between two instances of this type
    Vec2 scale{1.0F, 1.0F};       // [min, max]
    bool random_yaw = true;       // 3D: each turns about its up axis at random
    bool random_flip = true;      // 2D: each is mirrored or not at random
    bool align_to_normal = false; // 3D: stands along the ground's normal, not upright
    float tilt = 0.0F;            // 3D: radians each leans at random, either way
    Vec2 slope{0.0F, 90.0F};      // 3D: degrees from flat the ground may be, [min, max]
    std::optional<Vec2> height;   // 3D: world heights it grows between
    float sink = 0.0F;            // how far into the ground it stands
    // 3D: meters from the camera at which it starts to fade and is gone; [0, 0] never fades.
    Vec2 cull_distance{0.0F, 0.0F};
    // 3D: meters past which it draws as an impostor, a picture of it facing the camera; 0 never.
    float impostor_distance = 0.0F;
    std::string impostor; // the impostor's image; "" draws the model's last LOD instead
    std::string cast_shadow = "on";
    // Bending in the wind: radians it leans per meter a second (3D) or 100 pixels a second (2D)
    // of wind, up to max; sway rocks it about that by this share at frequency times a second.
    float wind_bend = 0.0F;
    float sway = 0.0F;
    float sway_frequency = 0.5F;
    float max_bend = 0.8F;
    // Parting for characters: bodies within radius push it over by up to bend radians, and it
    // springs back with stiffness (per second squared) and damping (per second).
    float interaction_radius = 0.0F;
    float interaction_bend = 0.8F;
    float stiffness = 40.0F;
    float damping = 8.0F;
    FoliageCollision collision;
    bool climbable = false;
    std::string harvest_item; // what harvesting one gives; "" can't be harvested
    int harvest_amount = 1;

    friend bool operator==(const FoliageType&, const FoliageType&) = default;
};

// JSON Schema for .foliage.json files.
[[nodiscard]] json::Value foliage_schema();

// Every error in a .foliage.json document: the schema's, then a 3D type without a mesh, a 2D one
// without a texture, ranges whose minimum is above their maximum and shapes of the other space.
[[nodiscard]] std::vector<json::ValidationError> validate_foliage(const json::Value& doc);

// The type a valid document holds, or its first error.
[[nodiscard]] Result<FoliageType> parse_foliage(const json::Value& doc);

} // namespace slash::world
