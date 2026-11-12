#pragma once

// Hair and fur, after Unreal's Groom and its Chaos-driven strand simulation (Godot has none). A
// .groom.json file says how strands grow (how many, how long, how thick, standing up or combed
// down, clumped and curled, their colors and the maps that vary them), how they draw (thin
// strands, cards or fur shells) and how they move. A HairInstance3D grows a groom over the model
// its entity draws, a Hair2D along a line in its own space; a few guide strands are simulated
// and the strands drawn follow them.
//
//   {
//     "format": 1,
//     "render": "strands",
//     "guides": 48, "density": 6000, "segments": 6,
//     "length": 0.12, "length_variation": 0.25, "lift": 0.4, "comb": [0, -1, 0.3],
//     "clumping": 0.4, "curl_radius": 0.004, "curl_turns": 2,
//     "root_color": [0.18, 0.11, 0.06, 1], "tip_color": [0.42, 0.28, 0.15, 1],
//     "simulation": {"stiffness": 0.4, "damping": 2, "drag_coefficient": 1}
//   }
//
// The simulation is the strands' own position-based dynamics, stepped by 2D and 3D physics in
// their step: gravity, the wind (WindSource3D and WindSource2D), physics fields, a pull back
// toward the groomed shape that weakens toward the tips, bending, follow-the-leader lengths and
// the spheres, capsules and boxes of the body it grows on (its CollisionShape3Ds and
// CollisionShape2Ds), as Chaos's hair solver keeps guides off a physics asset's capsules.

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

inline constexpr std::string_view kGroomSuffix = ".groom.json";

[[nodiscard]] bool is_groom_path(std::string_view path);

// One groom. Lengths are meters in 3D and pixels in 2D; a 2D file's defaults are in pixels.
struct Groom {
    std::string space = "3d";       // "3d" or "2d"
    std::string render = "strands"; // "strands", "cards" or "shells" (3D)
    int guides = 32;                // strands simulated; the rest follow the nearest
    // Strands drawn per square meter of the surface (3D) or per pixel of the root line (2D), up
    // to max_strands.
    float density = 2000.0F;
    int max_strands = 4096;
    int segments = 6; // points along a strand after its root
    float length = 0.1F;
    float length_variation = 0.2F; // the share a strand may be shorter, at random
    // Images over the surface's texture coordinates (3D) or along the root line (2D): red scales
    // the length (0 grows none there), red keeps a strand by its value (density), and the color
    // multiplies the strands' colors.
    std::string length_map;
    std::string density_map;
    std::string color_map;
    Vec4 root_color{0.2F, 0.13F, 0.08F, 1.0F};
    Vec4 tip_color{0.45F, 0.32F, 0.2F, 1.0F};
    float width = 0.002F; // a strand's width at its root, narrowing to tip_width
    float tip_width = 0.0005F;
    // How far strands stand out along the surface's normal (1) rather than turning toward comb
    // (0), a direction in the model's space (3D) or the entity's (2D).
    float lift = 1.0F;
    Vec3 comb{0.0F, -1.0F, 0.0F};
    float clumping = 0.3F;    // 0 to 1: how far strands gather toward their guide at the tip
    float curl_radius = 0.0F; // how far strands curl about their way
    float curl_turns = 0.0F;  // and how many times along their length
    std::uint32_t seed = 0;
    float card_width = 0.02F;  // cards: each guide draws a ribbon this wide
    std::string texture;       // cards: the ribbon's image, alpha cutting it out
    int shell_count = 16;      // shells: layers of fur over the surface
    float shell_tiling = 8.0F; // shells: times the fur's pattern repeats over texture coordinates
    float roughness = 0.45F;
    float anisotropy = 0.8F; // the highlight stretched across the strands, as hair shines
    float specular = 0.5F;
    // How the guides move.
    bool simulate = true;
    float stiffness = 0.5F;      // 0 to 1: the pull back toward the groomed shape at the root
    float tip_stiffness = 0.05F; // and at the tip
    float bend_stiffness = 0.2F; // 0 to 1: how hard each keeps the bends it was groomed with
    float damping = 2.0F;        // per second
    float gravity_scale = 1.0F;
    float drag_coefficient = 1.0F; // how hard the wind carries it, 0 to 1
    float point_radius = 0.002F;   // how far each point keeps from what it collides with
    int precision = 4;             // small steps per physics step

    friend bool operator==(const Groom&, const Groom&) = default;
};

// JSON Schema for .groom.json files.
[[nodiscard]] json::Value groom_schema();

// Every error in a .groom.json document: the schema's, then "shells" in 2D and a tip wider than
// its root.
[[nodiscard]] std::vector<json::ValidationError> validate_groom(const json::Value& doc);

// The groom a valid document holds, or its first error.
[[nodiscard]] Result<Groom> parse_groom(const json::Value& doc);

// The groom at a project path, read through the world's assets (else its file reader), with its
// version (0 without assets); kept until the file changes in worlds install_hair readied.
struct LoadedGroom {
    std::shared_ptr<const Groom> groom;
    std::uint64_t version = 0;
};
[[nodiscard]] Result<LoadedGroom> load_groom(const World& world, const std::string& path);
// Readies a world to keep parsed grooms; 2D and 3D physics and the canvas call it.
void install_hair(World& world);

// What a strand point keeps out of, in the world: a capsule from a to b (a sphere where they
// meet) or a box placed by place, half its size each way.
struct StrandCollider {
    bool box = false;
    Vec3 a;
    Vec3 b;
    real_t radius = 0.0F;
    Affine3D place;
    Vec3 half;
};

// Simulated strands: points strand after strand, each from its root.
struct Strands {
    int points = 0; // per strand, root included
    std::vector<Vec3> previous;
    std::vector<Vec3> current;
    std::vector<Vec3> velocity;
    std::vector<Vec3> targets; // where the groomed shape was at the last step
};

// What moves strands for one step.
struct StrandStep {
    float seconds = 0.0F;
    int steps = 1;
    Vec3 gravity;
    float stiffness = 0.5F;
    float tip_stiffness = 0.05F;
    float bend_stiffness = 0.2F;
    float damping = 0.0F;
    float drag_coefficient = 0.0F;
    real_t point_radius = 0.0F;
    // The wind's velocity and the fields' acceleration at a point; empty for none.
    std::function<Vec3(Vec3)> wind;
    std::function<Vec3(Vec3)> force;
    std::span<const StrandCollider> colliders;
};

// Steps strands toward their groomed shape, targets (each point where the groom puts it now, in
// the world), its roots carried there from where they were. Strands of another shape start at
// their targets, as do all of them when step.seconds is 0.
void step_strands(Strands& strands, int points, const std::vector<Vec3>& targets,
                  const StrandStep& step);

// The colliders of a body a groom grows on: every enabled sphere, capsule and box
// CollisionShape3D (circle, capsule and rectangle CollisionShape2D, at z 0) under root in its
// Transform3D (Transform2D) tree, the root's own included, except those of a CharacterBody3D
// (CharacterBody2D), whose capsule moves the whole character.
[[nodiscard]] std::vector<StrandCollider>
strand_colliders_3d(const World& world, const Tree3D& tree, entt::entity root);
class Tree2D;
[[nodiscard]] std::vector<StrandCollider>
strand_colliders_2d(const World& world, const Tree2D& tree, entt::entity root);

// The topmost entity above one in its tree, itself when it has no parent.
[[nodiscard]] entt::entity tree_root_3d(const Tree3D& tree, entt::entity entity);
[[nodiscard]] entt::entity tree_root_2d(const Tree2D& tree, entt::entity entity);

// Where physics put a groom's guide strands after its last step and the step before, in the
// world, and where each guide's root frame was (3D: the skin's place for its root, which the
// strands that follow it are carried by). 2D and 3D physics each give theirs; drawing and
// hair.get read them.
struct HairStrands {
    int points = 0;
    std::vector<Vec3> previous;
    std::vector<Vec3> current;
    std::vector<Affine3D> previous_frames;
    std::vector<Affine3D> frames;
};
using HairSource = std::function<const HairStrands*(const World&, entt::entity)>;
void set_hair_source_3d(World& world, HairSource source);
void set_hair_source_2d(World& world, HairSource source);
// The entity's simulated guides, or null when physics hasn't stepped them.
[[nodiscard]] const HairStrands* hair_strands_3d(const World& world, entt::entity entity);
[[nodiscard]] const HairStrands* hair_strands_2d(const World& world, entt::entity entity);

// A small, well mixed hash of a number, for seeding strands.
[[nodiscard]] std::uint32_t hair_hash(std::uint32_t value);
// A number in [0, 1) from a hash.
[[nodiscard]] float hair_unit(std::uint32_t hash);

// A 3D groom's growth kept by its key (scene3d's, which this module can't name), in worlds
// install_hair readied, so physics, drawing and commands grow it once; null when none is kept.
// Keeping one lets go of those nothing else holds.
[[nodiscard]] std::shared_ptr<const void> kept_hair_3d(const World& world, std::string_view key);
void keep_hair_3d(const World& world, std::string key, std::shared_ptr<const void> growth);

// 2D: a groom grown along a Hair2D's root line, in the entity's space.
struct HairGuide2D {
    real_t along = 0.0F; // its root's distance along the line
    Vec2 root;
    Vec2 normal;
    std::vector<Vec2> rest; // its points at rest, root first
};
struct HairStrand2D {
    real_t along = 0.0F;
    Vec2 root;
    std::uint32_t guide = 0;
    float length = 1.0F; // the share of its guide's length
    float phase = 0.0F;  // where its curl starts, radians
    float clump = 0.0F;  // how far it gathers toward its guide at the tip
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
};
struct HairGrowth2D {
    std::vector<HairGuide2D> guides;
    std::vector<HairStrand2D> strands;
};
// The guides and strands a groom grows along a line of points, red of the maps (images of
// width x height RGBA8 pixels, read along their middle row) varying them.
struct HairMap {
    const std::uint8_t* pixels = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    [[nodiscard]] Vec4 at(float u, float v) const;
    [[nodiscard]] bool empty() const noexcept { return pixels == nullptr || width == 0; }
};
[[nodiscard]] HairGrowth2D grow_hair_2d(const Groom& groom, std::span<const Vec2> line,
                                        const HairMap& length_map, const HairMap& density_map,
                                        const HairMap& color_map);

// An image a groom's maps name, as RGBA8 pixels, with its version: the canvas reads them
// through the world's assets (canvas::install sets the reader, as the world module can't decode
// images).
struct HairImage {
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t version = 0;
    [[nodiscard]] HairMap map() const {
        return {.pixels = pixels != nullptr ? pixels->data() : nullptr,
                .width = width,
                .height = height};
    }
};
using HairImageReader = std::function<Result<HairImage>(const std::string& path)>;
void set_hair_image_reader(World& world, HairImageReader reader);
// The image at a path; an error when it can't be read or the world has no reader. "" reads as
// an empty image, which varies nothing.
[[nodiscard]] Result<HairImage> read_hair_image(const World& world, const std::string& path);

// A Hair2D's groom and its growth along its root line, kept while the groom, the line and the
// maps stay the same (in worlds install_hair readied); key names them all.
struct HairGrown2D {
    std::shared_ptr<const Groom> groom;
    std::shared_ptr<const HairGrowth2D> growth;
    std::string key;
};
[[nodiscard]] Result<HairGrown2D> hair_growth_2d(const World& world, entt::entity entity);

// The points of a strand that follows a guide, guide's points (root first) sampled at
// share_of_guide of its length, moved by offset at the root, gathered toward the guide by clump
// at the tip and curled about it by curl_radius, curl_turns times along it from phase. side is a
// direction across the guide that the curl starts from; in a plane (flat) the curl stays in it.
void follow_guide(std::span<const Vec3> guide, Vec3 offset, float share_of_guide, float clump,
                  real_t curl_radius, float curl_turns, float phase, Vec3 side, bool flat,
                  std::vector<Vec3>& out);

} // namespace slash::world
