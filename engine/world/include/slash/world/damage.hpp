#pragma once

// How things take damage and break, after Unreal's Chaos Destruction (Godot has none): a
// .damage.json profile names a material (wood splinters, glass shatters, stone crumbles), its
// health and what resists which kind of damage, the damage states that change its looks, how it
// breaks (a fracture pattern, levels of pieces that break again) and whether its pieces stand
// linked, as Chaos's connection graph, until the weight through a link is more than it holds.
// Destructible3D and Destructible2D name a profile; physics3d and physics2d break them, and
// scene3d cuts meshes by the cuts here (fracture_sites).

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

inline constexpr std::string_view kDamageSuffix = ".damage.json";

// Whether a path names a .damage.json file.
[[nodiscard]] bool is_damage_path(std::string_view path);

// A damage state: below a share of its health, the body looks as this says.
struct DamageState {
    float below = 0.5F;                // of its full health
    bool tinted = false;               // whether tint is set
    Vec4 tint{1.0F, 1.0F, 1.0F, 1.0F}; // multiplies its StandardMaterial3D's albedo (2D: modulate)
    std::string material; // a material file its MeshInstance3D draws with in place of its own
    std::string mesh;     // a model file it draws in place of its own (2D: a texture)
};

// How it breaks.
struct FractureSettings {
    std::string pattern = "voronoi"; // a FractureCut's pattern, or "none" (it never breaks)
    int pieces = 8;
    std::uint32_t seed = 0;
    int levels = 1;        // how many times pieces break; 1: its pieces don't break again
    int level_pieces = 4;  // cells a piece breaks into when it breaks again
    float fine = 0.0F;     // meters (2D: pixels) around the hit the fine pieces reach; 0: a quarter
    float min_size = 0.0F; // meters (2D: pixels) across under which a piece breaks no more
};

// What its pieces do once loose.
struct DebrisSettings {
    float lifetime = 0.0F; // seconds before loose pieces go; 0 keeps them
    float push = 0.0F;     // meters (2D: pixels) a second they fly from the hit
};

struct DamageProfile {
    std::string name;
    // "wood", "glass", "metal", "stone", "concrete", "brick", "ceramic", "ice", "plastic",
    // "flesh" or "cloth", whose defaults fill what the file leaves out
    std::string material = "stone";
    float health = 100.0F;
    // What each kind of damage is multiplied by: "blunt", "pierce", "cut", "explosive", "fire"
    // and "impact" (what a collision does); a kind not named is 1.
    std::map<std::string, float> resist;
    // A collision does damage past threshold kilogram-meters per second (2D: kilogram-pixels),
    // damage health for each one past it.
    float impact_threshold = 0.0F;
    float impact_damage = 1.0F;
    std::vector<DamageState> states; // highest below first
    FractureSettings fracture;
    // Kilograms a cubic meter (2D: a square meter of 64 pixels a side) its pieces weigh.
    float density = 2000.0F;
    bool connected = false; // its pieces stand linked until a link gives way
    // Newtons a square meter of a link holds (2D: per pixel of edge, a newton per 64 pixels), and
    // how many times more it holds what stands on it.
    float strength = 100000.0F;
    float compression = 10.0F;
    // Which pieces are held in place: "bottom" (those at its bottom), "ends" (those at both ends
    // of its longest level axis, as a bridge's), "all" or "none".
    std::string anchors = "bottom";
    DebrisSettings debris;
};

// The kinds of damage profiles name.
[[nodiscard]] const std::vector<std::string>& damage_kinds();
// The materials profiles name, and what each starts from (a profile's JSON without format).
[[nodiscard]] const std::vector<std::string>& damage_materials();
[[nodiscard]] json::Value damage_material_defaults(std::string_view material);

// The JSON Schema of .damage.json files, and every error of one (its kinds and anchors
// spelled as they must be, with "did you mean").
[[nodiscard]] json::Value damage_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_damage(const json::Value& doc);

// A profile's JSON with what it extends merged under it, as JSON Merge Patch.
[[nodiscard]] Result<json::Value> resolve_damage(const json::Value& doc,
                                                 const FileReader& read_file);
// The profile a resolved file's JSON makes, over its material's defaults.
[[nodiscard]] Result<DamageProfile> parse_damage(const json::Value& resolved);

// The profile a Destructible3D or Destructible2D names, read through the world's assets (so it
// reloads), with its overrides merged over it. Fails, saying why, for one that can't be read.
[[nodiscard]] Result<std::shared_ptr<const DamageProfile>>
load_damage(const World& world, std::string_view path, const json::Value& overrides);

// The health damage of a kind takes away, by the profile's resistance.
[[nodiscard]] float damage_taken(const DamageProfile& profile, float amount, std::string_view kind);
// The damage state (counted from 1; 0 for none) a body at health is in.
[[nodiscard]] int damage_state(const DamageProfile& profile, float health);

// The points a cut's cells grow around in a box (low to high), the same on every machine; flat
// keeps them at low.z, for 2D. For "clusters", groups says which piece each point's cell joins;
// for every other pattern each point is a piece of its own, and groups is empty.
struct FractureSites {
    std::vector<std::array<double, 3>> points;
    std::vector<int> groups;
};
[[nodiscard]] FractureSites fracture_sites(const FractureCut& cut, Vec3 low, Vec3 high, bool flat);

// The cut a profile makes of what it is breaking, hit at a point (in the cut thing's space) at
// a level (0 for the whole body).
[[nodiscard]] FractureCut damage_cut(const DamageProfile& profile, Vec3 at, int level,
                                     std::uint32_t seed);

// What the load through each link of a standing group of pieces works out to, as Chaos's
// connection graph sends a cluster's strain to its anchors: each piece's weight goes along the
// links of a tree grown out from the anchored pieces, nearest first, down to an anchor. A link
// whose load is more than strength times its area (times compression for a piece standing on
// the one below it) gives way, the worst first, and the load is worked out again, until none
// does; what no link joins to an anchor then falls.
struct StressPiece {
    Vec3 center;         // in the world (2D: x, y and 0, with y down)
    float weight = 0.0F; // newtons
    bool anchored = false;
    std::vector<std::pair<std::size_t, float>> links; // other piece, area
};
struct StressResult {
    std::vector<std::pair<std::size_t, std::size_t>> broken; // links that gave way, lower first
    std::vector<bool> falls;                                 // pieces no link joins to an anchor
    std::vector<float> loads; // the most load any link of each piece held, in newtons
};
[[nodiscard]] StressResult settle_stress(const std::vector<StressPiece>& pieces, float strength,
                                         float compression, Vec3 up);

} // namespace slash::world
