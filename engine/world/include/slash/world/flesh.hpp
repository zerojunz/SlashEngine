#pragma once

// A character's anatomy and flesh, after Unreal's Chaos Flesh (Godot has none): the bones under
// the skin (skull, spine, ribs, pelvis, limbs' long bones) and the muscles between them, each a
// spindle from its origin on one bone to its insertion on another, simulated as a tetrahedral
// (in 2D, triangular) body of stable Neo-Hookean flesh by XPBD (Macklin and Müller's constraint
// form), which keeps its volume, so a muscle that shortens bulges; contracts along its fibers as
// it is activated; is held to where the skeleton puts it by tethers that let it jiggle; and stays
// outside the bone it rests on. The skin follows the muscles under it. Both dimensions share it:
// slash/scene3d/flesh.hpp runs it on a character's model, add_flesh_2d_system here on its 2D
// cut-out. Built with -ffp-contract=off, so it steps the same everywhere.

#include "slash/core/math.hpp"
#include "slash/world/character.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

// The parts of a body that anatomy is placed on.
enum class BodyPart : std::uint8_t {
    Head,
    Jaw,
    Neck,
    Chest,
    Abdomen,
    Pelvis,
    Clavicle,
    UpperArm,
    LowerArm,
    Hand,
    UpperLeg,
    LowerLeg,
    Foot,
};
inline constexpr std::size_t kBodyParts = 13;
// Whether a part comes in a pair, left and right (front and back in 2D).
[[nodiscard]] bool is_sided(BodyPart part);
[[nodiscard]] std::string_view body_part_name(BodyPart part);

// A part's place at rest, in the space of the model or cut-out it is built in: from its head (the
// joint nearer the body's middle) to its tail, the way its front faces and the way out from the
// body's middle (for a part in the middle, the body's left), both square to it, its half depth
// front to back and half width across, and the bone that moves it.
struct PartFrame {
    Vec3 head;
    Vec3 tail;
    Vec3 front;
    Vec3 out;
    real_t depth = 0.0F;
    real_t width = 0.0F;
    std::string bone;
};

// A body's parts, each side's (a part in the middle is side 0's), in 3D or in 2D (a cut-out
// seen from the side: x forward, y up, z toward whoever looks, in meters). Sides are named
// "left" and "right" in 3D and "back" and "front" in 2D.
struct BodyFrames {
    int dimensions = 3;
    std::array<std::array<std::optional<PartFrame>, 2>, kBodyParts> parts{};
    std::array<std::string, 2> sides;

    [[nodiscard]] const PartFrame* find(BodyPart part, int side) const;
};
// A recipe's 3D model's parts, its bones named as slash/scene3d/character.hpp's skeleton.
[[nodiscard]] BodyFrames body_frames_3d(const CharacterMeasures& m);
// A recipe's 2D cut-out's parts, its bones named as character_cutout's.
[[nodiscard]] BodyFrames body_frames_2d(const CharacterMeasures& m);

// A bone of the skeleton under the skin, as tapered capsules (a sphere where a and b are the
// same) in the frames' space, moved by a bone of the rig.
struct AnatomyPiece {
    Vec3 a;
    Vec3 b;
    real_t radius = 0.0F;
};
struct AnatomyBone {
    std::string name; // its kind, then "_<side>" when it comes in a pair: "humerus_left"
    std::string kind; // "skull", "mandible", "humerus", ...
    std::string bone; // the rig's bone that moves it
    int side = -1;    // -1 for one in the middle
    std::vector<AnatomyPiece> pieces;
};

// A muscle at rest: rings along it from its origin to its insertion, each square to it, with
// two radii (u toward its front, v across). The first and last are its tendons, held to the
// bones.
struct MuscleRing {
    Vec3 center;
    Vec3 u;
    Vec3 v;
    real_t ru = 0.0F;
    real_t rv = 0.0F;
};
struct PartSide {
    BodyPart part = BodyPart::Chest;
    int side = -1;
    friend bool operator==(const PartSide&, const PartSide&) = default;
};
struct AnatomyMuscle {
    std::string name;   // its kind, then "_<side>" when it comes in a pair: "biceps_left"
    std::string kind;   // "biceps", "deltoid", ...
    std::string group;  // "arm", "shoulder", "chest", "back", "core", "hip", "thigh", "calf",
                        // "neck" or "jaw"
    std::string action; // what it does, in a few words
    int side = -1;
    PartSide origin;
    PartSide insertion;
    PartSide rests_on;
    std::string origin_bone;
    std::string insertion_bone;
    std::string rest_bone; // the bone it rests on, whose capsule it stays outside
    Vec3 bone_a;           // that capsule, at rest
    Vec3 bone_b;
    real_t bone_radius = 0.0F;
    std::vector<MuscleRing> rings;
};

struct Anatomy {
    int dimensions = 3;
    std::array<std::string, 2> sides;
    std::vector<AnatomyBone> bones;
    std::vector<AnatomyMuscle> muscles;
};

// Every muscle kind, in the order anatomies list them, and every bone kind.
[[nodiscard]] const std::vector<std::string_view>& muscle_kinds();
[[nodiscard]] const std::vector<std::string_view>& anatomy_bone_kinds();

// A recipe's bones and muscles on a body's frames, sized by its measures and its body's muscle
// and fat. In 2D, a pair of muscles both of whose ends are in the middle keeps only the near
// (front) one, as a side view shows one.
[[nodiscard]] Anatomy make_anatomy(const CharacterRecipe& recipe, const BodyFrames& frames);

// ---- Flesh ----

// What a muscle's flesh is made of: its points (rings of six around a center in 3D, three
// across in 2D), its elements (tetrahedra in 3D, triangles in 2D) and how it is held.
struct FleshMuscle {
    std::string name;
    std::string kind;
    std::string group;
    PartSide origin;
    PartSide insertion;
    PartSide rests_on;
    int origin_bone = -1; // into FleshBody::bones
    int insertion_bone = -1;
    int rest_bone = -1;
    Vec3 bone_a;
    Vec3 bone_b;
    real_t bone_radius = 0.0F;
    Vec3 origin_at; // the middles of its end rings, at rest
    Vec3 insertion_at;
    real_t rest_length = 0.0F;
    real_t rest_radius = 0.0F; // its middle ring's mean radius
    std::size_t first_point = 0;
    std::size_t rings = 0;
    std::size_t ring_points = 0; // the points of each ring, its center first
};

struct FleshPoint {
    Vec3 rest;
    Vec3 radial;         // from its muscle's axis, at rest
    real_t along = 0.0F; // 0 at its muscle's origin to 1 at its insertion
    // How much the origin's and the insertion's bones move it; the bone it rests on the rest.
    real_t to_origin = 0.0F;
    real_t to_insertion = 0.0F;
    real_t inverse_mass = 0.0F;
    int muscle = 0;
    bool pinned = false; // a tendon's: where the bones put it
};

struct FleshElement {
    std::array<std::uint32_t, 4> corners{}; // three in 2D
    std::array<real_t, 9> rest_inverse{};   // the inverse of its rest edges, column major
    real_t volume = 0.0F;                   // its area in 2D
    Vec3 fiber;                             // the muscle's way, in its rest space
    int muscle = 0;
};

struct FleshBody {
    int dimensions = 3;
    std::vector<std::string> bones; // the rig's bones that move it
    std::vector<FleshMuscle> muscles;
    std::vector<FleshPoint> points;
    std::vector<FleshElement> elements;
};
[[nodiscard]] FleshBody build_flesh(const Anatomy& anatomy);

// How a body's flesh moves, from a Flesh3D's or Flesh2D's fields.
struct FleshSettings {
    float stiffness = 0.5F;
    float damping = 0.5F;
    float jiggle = 0.5F;
    float activation = 0.0F;
    bool automatic = true;
    const std::map<std::string, float>* muscles = nullptr; // by name, kind or group
    int substeps = 4;
};
[[nodiscard]] FleshSettings flesh_settings(const Flesh3D& flesh);
[[nodiscard]] FleshSettings flesh_settings(const Flesh2D& flesh);

// Where flesh is as it steps: its points' places and velocities and each muscle's activation.
struct FleshState {
    std::vector<Vec3> positions;
    std::vector<Vec3> velocities;
    std::vector<Vec3> targets; // the shape the muscle takes between its ends, which it springs to
    std::vector<Vec3> skinned; // where its bones alone carry each point, as a skin's would
    std::vector<float> activations;
    bool started = false;
};

// One step: each of FleshBody::bones' moves from its rest (its pose times the inverse of its
// rest), the acceleration of the body's own space in that space (gravity and the root's
// motion's), and the seconds.
struct FleshStep {
    std::span<const Affine3D> moves;
    Vec3 acceleration;
    real_t seconds = 0.0F;
    FleshSettings settings;
};
// Steps flesh, putting every point where the skeleton puts it first if it hasn't started.
void step_flesh(const FleshBody& body, FleshState& state, const FleshStep& step);
// Puts flesh where the skeleton puts it, at rest.
void reset_flesh(const FleshBody& body, FleshState& state, std::span<const Affine3D> moves);

// A muscle as it is: its activation (0 to 1), its length over its rest's and how far its
// middle has bulged out (its mean radius over its rest's, less 1).
struct MuscleReport {
    std::string name;
    std::string kind;
    std::string group;
    float activation = 0.0F;
    float length = 1.0F;
    float bulge = 0.0F;
};
[[nodiscard]] std::vector<MuscleReport> muscle_reports(const FleshBody& body,
                                                       const FleshState& state);

// Skin bound to flesh: each point of the skin follows up to six points of the two muscles
// nearest it under its part, by inverse distance, less where fat lies thicker over them.
struct SkinPoint {
    Vec3 rest;
    PartSide part;
};
struct FleshBinding {
    std::vector<std::uint32_t> first; // into entries, one more than there are skin points
    std::vector<std::pair<std::uint32_t, float>> entries;
};
[[nodiscard]] FleshBinding bind_skin(const FleshBody& body, std::span<const SkinPoint> skin,
                                     float fat);
// How far each bound skin point is moved from where the skeleton alone puts it.
void skin_offsets(const FleshState& state, const FleshBinding& binding, std::vector<Vec3>& out);

// ---- 2D ----

// Adds the flesh_2d system (priority 957, after physics and ragdolls): each Character2D with an
// enabled Flesh2D steps its cut-out's muscles from where its bones are, and moves its parts'
// Polygon2D points by them as they are drawn (flesh_offsets_2d). Its view shows the muscles or
// the skeleton over the body as a child entity, <id>_anatomy.
void add_flesh_2d_system(World& world);
// How far flesh moves each point of an entity's Polygon2D, in its own space, or null where it
// moves none.
[[nodiscard]] const std::vector<Vec2>* flesh_offsets_2d(const World& world, entt::entity entity);
// A Character2D's flesh, or null where it has none yet.
struct FleshView {
    const FleshBody* body = nullptr;
    const FleshState* state = nullptr;
    const Anatomy* anatomy = nullptr;
};
[[nodiscard]] FleshView flesh_2d(const World& world, entt::entity entity);

} // namespace slash::world
