#pragma once

// Faces as recipes, after Unreal's MetaHuman Creator (Godot has none): a .face.json file, or a
// character recipe's "face", says a face by named sculpt controls (each -1 to 1, 0 for an
// average adult's), carves at its landmarks, blends of other faces and a style, and sculpt_head
// shapes a head from it that 3D models (slash/scene3d/character.hpp) and 2D cut-outs
// (character_cutout's side-on profile, face_profile) both draw.

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

inline constexpr std::string_view kFaceSuffix = ".face.json";

// Whether a path names a .face.json file.
[[nodiscard]] bool is_face_path(std::string_view path);

// A sculpt control: its name ("nose.length") and what 1 does (-1 does the opposite).
struct FaceControl {
    std::string_view name;
    std::string_view doc;
};
// Every control, in the order CharacterFace::controls holds them.
[[nodiscard]] const std::vector<FaceControl>& face_controls();
[[nodiscard]] std::optional<std::size_t> face_control_index(std::string_view name);

// The landmarks carves are placed at, and that sculpt_head reports where they ended up.
[[nodiscard]] const std::vector<std::string_view>& face_landmark_names();

// A carve into the sculpt: a "dent" or "bump" round a point, or a "crease" or "ridge" along a
// line through it, at a landmark moved by offset (across toward the face's left and up, in
// half the head's width and height), radius and length in the same units, depth from -1 to 1
// (1 is 5% of the head's depth, outward for bumps and ridges), angle in degrees from across,
// and mirror to carve the other side too.
struct FaceCarve {
    std::string kind = "dent";
    std::string at = "cheek_left";
    Vec2 offset;
    float radius = 0.06F;
    float depth = 0.5F;
    float length = 0.0F;
    float angle = 0.0F;
    bool mirror = false;
};

// The eyebrows, drawn on the skin: their color (alpha 0 takes the hair's, darkened), how thick
// (0 for none) and their shape.
struct FaceBrows {
    Vec4 color{0.0F, 0.0F, 0.0F, 0.0F};
    float thickness = 0.5F;
    std::string shape = "soft"; // "soft", "straight", "arched" or "angled"
};

struct CharacterFace {
    Vec4 eyes{0.3F, 0.22F, 0.12F, 1.0F};
    float eye_size = 1.0F;
    float eye_spacing = 1.0F;
    float nose = 1.0F;
    float ears = 1.0F;
    Vec4 lips{0.7F, 0.42F, 0.38F, 1.0F};
    std::string beard = "none"; // "none", "stubble", "short" or "long"
    // How it is sculpted: "realistic", "stylized", "toon", "painterly" or "low_poly".
    std::string style = "realistic";
    std::vector<float> controls; // by face_controls(); missing ones are 0
    std::vector<FaceCarve> carves;
    FaceBrows brows;
    float asymmetry = 0.0F; // 0 to 1: how far its sides differ, as the seed picks
    std::uint32_t seed = 0;

    // A control's value, 0 when unset.
    [[nodiscard]] float control(std::size_t index) const;
    [[nodiscard]] float control(std::string_view name) const;
};

// The JSON Schema of .face.json files, and of a character recipe's "face" (the same fields,
// with "from" naming a .face.json file they are a merge patch over).
[[nodiscard]] json::Value face_schema();
[[nodiscard]] json::Value inline_face_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_face(const json::Value& doc);

// The face a .face.json file's JSON, or a character's inline "face", makes: its "extends" (or
// "from") chain merged as JSON Merge Patch, then its "blend" presets' controls mixed by weight
// under its own, read with read_file.
[[nodiscard]] Result<CharacterFace> parse_face(const json::Value& doc, const FileReader& read_file);

// A head's place and size in meters: its center and half sizes across (x), up (y) and front to
// back (z), facing +z, with +x the face's left.
struct HeadShape {
    Vec3 center;
    Vec3 radii{0.1F, 0.12F, 0.11F};
};

// Where a face's eyes are and how big (their centers, in meters), and the height of the line
// its lips close on, which a rig's eye bones and jaw follow.
struct FaceEyes {
    Vec3 left;
    Vec3 right;
    real_t radius = 0.0F;
};
[[nodiscard]] FaceEyes face_eyes(const CharacterFace& face, const HeadShape& head);
[[nodiscard]] real_t face_mouth(const CharacterFace& face, const HeadShape& head);

// What a sculpted head's triangle is: skin, lips, eyebrows, the inside of the mouth, or skin
// hidden behind an eyeball.
enum class FaceArea : std::uint8_t { Skin, Lips, Brows, Mouth, Hidden };

struct FaceLandmark {
    std::string_view name;
    Vec3 position; // meters
    Vec3 normal;
};

// A sculpted head: a grid of rings from the bottom up and segments round from the back, its
// vertices denser over the face, each with the direction on the unit sphere it was sculpted
// from (directions), and a triangle list with each triangle's area and whether a beard grows
// on it. Low poly heads are flat shaded: each triangle has corners of its own.
struct SculptedHead {
    std::size_t rings = 0;
    std::size_t segments = 0;
    std::vector<Vec3> positions; // meters
    std::vector<Vec3> normals;
    std::vector<Vec3> directions;
    std::vector<Vec2> uvs;
    std::vector<std::uint32_t> indices;
    std::vector<FaceArea> areas;       // a triangle's
    std::vector<std::uint8_t> bearded; // a triangle's: 1 where a beard grows
    std::vector<FaceLandmark> landmarks;
    FaceEyes eyes;
    real_t mouth = 0.0F;
    bool flat = false;
};
[[nodiscard]] SculptedHead sculpt_head(const CharacterFace& face, const HeadShape& head);

// The head's outline seen from its left side, facing +x (its front, +z, along x and up along
// y), in meters from the head's center: from under the chin up the face, over the crown and
// down the back. front_count is how many of its points are the face's, chin to crown.
struct FaceProfile {
    std::vector<Vec2> points;
    std::size_t front_count = 0;
    std::size_t mouth_index = 0; // the point where the lips meet
    Vec2 eye;                    // the near eye's center
    real_t eye_radius = 0.0F;
    Vec2 brow; // the brow's middle above the near eye, on the skin
    Vec2 ear;
};
[[nodiscard]] FaceProfile face_profile(const CharacterFace& face, const HeadShape& head);

} // namespace slash::world
