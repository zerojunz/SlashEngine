#pragma once

// Characters as recipes, after Unreal's MetaHuman Creator and Mutable (Godot has none): a
// .character.json file says a person's body, face, hair, clothing and props, which
// slash/scene3d/character.hpp builds into a rigged model and character_cutout here into a 2D
// cut-out of Polygon2D parts. Both dimensions read the same recipe.

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/face.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

inline constexpr std::string_view kCharacterSuffix = ".character.json";

// Whether a path names a .character.json file.
[[nodiscard]] bool is_character_path(std::string_view path);

// The body's build: its height in meters and its proportions, each 1 for an average adult's.
struct CharacterBody {
    float height = 1.75F;
    float build = 0.5F; // 0 slight to 1 heavy
    float head = 1.0F;
    float neck = 1.0F;
    float shoulders = 1.0F;
    float hips = 1.0F;
    float arms = 1.0F;
    float legs = 1.0F;
    Vec4 skin{0.87F, 0.68F, 0.55F, 1.0F};
    float muscle = 0.5F; // 0 to 1: the size of the muscles (slash/world/flesh.hpp)
    float fat = 0.3F;    // 0 to 1: how much fat lies over them
};

struct CharacterHair {
    std::string style = "short"; // "none", "short", "long", "ponytail", "bun" or "mohawk"
    Vec4 color{0.18F, 0.12F, 0.08F, 1.0F};
    float length = 0.5F; // 0 to 1, for long hair and ponytails
    std::string groom;   // a .groom.json file grown on the "scalp" section in its place
};

// Something worn: its kind says what it covers, length how far down (0 to 1) and loose how far
// it stands off the body (0 to 1).
struct Garment {
    std::string kind;
    Vec4 color{0.5F, 0.5F, 0.5F, 1.0F};
    std::string material = "cloth"; // "cloth", "leather", "metal", "silk" or "fur"
    float length = 0.5F;
    float loose = 0.0F;
    bool cloth = false; // capes and robes: simulated as Cloth3D on the garment's section
};

// A model held or worn at one of the character's points.
struct CharacterProp {
    std::string model;
    std::string point; // "right_hand", "left_hand", "back", "hip_left", "hip_right" or "head_top"
    Vec3 position;
    Vec3 rotation; // radians, Godot's YXZ
    float scale = 1.0F;
};

struct CharacterRecipe {
    std::string name;
    std::string role = "npc"; // "player" or "npc"
    std::string faction;      // a storyboard faction's ID, for rules that group by it
    CharacterBody body;
    CharacterFace face;
    CharacterHair hair;
    std::vector<Garment> clothing;
    std::vector<CharacterProp> props;
};

// The JSON Schema of .character.json files.
[[nodiscard]] json::Value character_schema();

// Every problem with a recipe's JSON, after its schema: unknown garments' combinations (two
// garments of a kind that covers the same place), props at the same point.
[[nodiscard]] std::vector<json::ValidationError> validate_character(const json::Value& doc);

// The recipe a .character.json file's JSON makes, its "extends" chain read with read_file (each a
// merge patch over the file it extends, as JSON Merge Patch).
[[nodiscard]] Result<CharacterRecipe> parse_character(const json::Value& source,
                                                      const FileReader& read_file);
// The JSON a file's "extends" chain merges to, without "extends".
[[nodiscard]] Result<json::Value> resolve_character(const json::Value& doc, const FileReader& read);

// Every garment kind, in the schema's order.
[[nodiscard]] const std::vector<std::string_view>& garment_kinds();

// Where a recipe's body is, in meters with y up and the feet at 0, which its 3D model and 2D
// cut-out are both built by: joints' heights, bones' lengths, half widths across the body (x),
// depths front to back (z) and limbs' radii.
struct CharacterMeasures {
    float scale = 1.0F; // meters per unit of an average adult's proportions
    float ankle = 0.0F, knee = 0.0F, hip = 0.0F, waist = 0.0F, chest = 0.0F, shoulder = 0.0F;
    float head_base = 0.0F, top = 0.0F, head_height = 0.0F, head_radius = 0.0F;
    float upper_arm = 0.0F, lower_arm = 0.0F, hand = 0.0F;
    float upper_leg = 0.0F, lower_leg = 0.0F, foot_length = 0.0F;
    float shoulder_half_width = 0.0F, chest_half_width = 0.0F, waist_half_width = 0.0F;
    float hip_half_width = 0.0F, leg_offset = 0.0F, chest_depth = 0.0F, hip_depth = 0.0F;
    float arm_radius = 0.0F, forearm_radius = 0.0F, thigh_radius = 0.0F, calf_radius = 0.0F;
    float neck_radius = 0.0F;
};
[[nodiscard]] CharacterMeasures character_measures(const CharacterRecipe& recipe);
// Where a character's head is (slash/world/face.hpp sculpts it there): an ellipsoid above the
// neck, as wide as head_radius and as tall as head_height.
[[nodiscard]] HeadShape character_head(const CharacterMeasures& measures);

// What a garment covers: a region of the body and, along limbs, skirts and capes, the share of
// its length from the top (from) to how far down (to).
enum class Region : std::uint8_t {
    Torso,
    UpperArms,
    LowerArms,
    Hands,
    Pelvis,
    UpperLegs,
    LowerLegs,
    Feet,
    Skirt,
    Cape,
    Belt,
    Plate,
    Shoulders,
    Hat,
    Hood,
    Helmet,
    Scarf,
};
struct Covering {
    Region region = Region::Torso;
    float from = 0.0F;
    float to = 1.0F;
};
[[nodiscard]] std::vector<Covering> garment_coverage(const Garment& garment);
// How far a recipe's garment stands off the skin, in meters: more when loose, and each later
// garment over the ones before.
[[nodiscard]] float garment_offset(const CharacterRecipe& recipe, std::size_t index);

// A recipe's parts as a 2D cut-out, seen from the side facing +x, in meters with y up and the
// feet at 0: a tree of bones (a parent before its children), each with the polygons drawn on it,
// in the order they draw. The far arm and leg are "<bone>_back", behind the body; the near ones
// "<bone>_front".
struct CutoutShape {
    std::vector<Vec2> points; // in the bone's space
    Vec4 color;
};
struct CutoutBone {
    std::string name; // "pelvis", "chest", "head", "upper_arm_l", ...
    int parent = -1;
    Vec2 position; // in its parent's space
    int z_index = 0;
    std::vector<CutoutShape> shapes;
};
[[nodiscard]] std::vector<CutoutBone> character_cutout(const CharacterRecipe& recipe);

// The recipe a Character2D names, read through the world's assets (kept by version).
[[nodiscard]] Result<std::shared_ptr<const CharacterRecipe>> load_character(const World& world,
                                                                            std::string_view path);

// Adds the character_2d system (priority 880): each Character2D keeps a child entity per
// cut-out bone, <id>_<bone>, with its Transform2D (made where the cut-out puts it, then left to
// animations and ragdolls) and a Polygon2D of its shapes in pixels.
void add_character_system(World& world);

// Colors as Oklab, the perceptual space character checks measure differences in, and the
// distance between two, times 100 (about 1 is the least step people see).
[[nodiscard]] Vec3 oklab(Vec4 rgb);
[[nodiscard]] float color_difference(Vec4 a, Vec4 b);

} // namespace slash::world
