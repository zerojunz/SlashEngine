#include "slash/scene3d/character.hpp"

#include "anatomy.hpp"
#include "rig_builder.hpp"

#include "slash/core/json.hpp"
#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Characters are built of Godot's primitive meshes, each placed by a transform that turns and
// stretches it over the recipe's measures (world::character_measures), so the same recipe gives
// the same model, bit for bit, everywhere: the library builds with -ffp-contract=off and angles
// go through slash::cos_sin.
namespace slash::scene3d {
namespace {

using world::Affine3D;
using world::CharacterMeasures;
using world::CharacterRecipe;
using world::Covering;
using world::Garment;
using world::Region;
using namespace rig; // NOLINT(google-build-using-namespace)

// How a garment's material shades: its roughness and metallic.
std::pair<float, float> surface_of(std::string_view material) {
    if (material == "leather") {
        return {0.55F, 0.0F};
    }
    if (material == "metal") {
        return {0.35F, 1.0F};
    }
    if (material == "silk") {
        return {0.3F, 0.0F};
    }
    if (material == "fur") {
        return {1.0F, 0.0F};
    }
    return {0.9F, 0.0F};
}

// ---- The rig ----

constexpr std::array<std::string_view, 2> kSides{"Left", "Right"};
constexpr std::array<std::string_view, 4> kFingers{"Index", "Middle", "Ring", "Little"};
constexpr std::array<std::string_view, 3> kPhalanges{"Proximal", "Intermediate", "Distal"};
constexpr std::array<std::string_view, 3> kThumb{"ThumbMetacarpal", "ThumbProximal", "ThumbDistal"};

real_t side_sign(std::string_view side) {
    return side == "Left" ? 1.0F : -1.0F;
}

// Smoothstep from from to to, which may be either way round.
real_t ramp(real_t from, real_t to, real_t x) {
    const real_t t = std::clamp<real_t>((x - from) / (to - from), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

// The turn that takes direction a onto direction b, the shorter way round.
Affine3D arc(Vec3 a, Vec3 b) {
    a = normalized(a);
    b = normalized(b);
    const real_t c = dot(a, b);
    Affine3D out;
    if (c < -0.9999F) {
        // Half a turn about an axis square to a.
        const Vec3 axis = normalized(std::abs(a.x) < 0.9F ? cross(a, {1.0F, 0.0F, 0.0F})
                                                          : cross(a, {0.0F, 1.0F, 0.0F}));
        out.x = (axis * (2.0F * axis.x)) - Vec3{1.0F, 0.0F, 0.0F};
        out.y = (axis * (2.0F * axis.y)) - Vec3{0.0F, 1.0F, 0.0F};
        out.z = (axis * (2.0F * axis.z)) - Vec3{0.0F, 0.0F, 1.0F};
        return out;
    }
    const Vec3 v = cross(a, b);
    const real_t k = 1.0F / (1.0F + c);
    const auto turn = [&](Vec3 u) { return (u * c) + cross(v, u) + (v * (dot(v, u) * k)); };
    out.x = turn({1.0F, 0.0F, 0.0F});
    out.y = turn({0.0F, 1.0F, 0.0F});
    out.z = turn({0.0F, 0.0F, 1.0F});
    return out;
}

// A turn of angle radians about axis, as a quaternion.
Quat turn_about(Vec3 axis, float angle) {
    const Vec3 a = normalized(axis);
    const Vec2f cs = cos_sin(angle * 0.5F);
    return {a.x * cs.y, a.y * cs.y, a.z * cs.y, cs.x};
}

// How a hand lies: the wrist, the way the hand points (along), the way its thumb side faces
// (front) and the way its palm faces (palm, toward the body's middle).
struct HandFrame {
    Vec3 wrist;
    Vec3 along;
    Vec3 front;
    Vec3 palm;
};

HandFrame hand_frame(Vec3 lower, Vec3 wrist, real_t side) {
    HandFrame f;
    f.wrist = wrist;
    f.along = normalized(wrist - lower);
    f.front = normalized(Vec3{0.0F, 0.0F, 1.0F} - (f.along * f.along.z));
    const Vec3 n = cross(f.along, f.front);
    f.palm = n.x * side < 0.0F ? n : n * -1.0F;
    return f;
}

// Each finger's knuckle along the palm and across it, its lean toward the thumb, its length and
// its thickness, as shares of the hand's.
struct FingerShape {
    real_t along;
    real_t across;
    real_t spread;
    real_t length;
    real_t radius;
};
constexpr std::array<FingerShape, 4> kFingerShapes{{
    {0.97F, 0.72F, 0.06F, 0.92F, 0.21F},
    {1.0F, 0.24F, 0.0F, 1.0F, 0.22F},
    {0.97F, -0.24F, -0.05F, 0.95F, 0.21F},
    {0.9F, -0.68F, -0.12F, 0.76F, 0.18F},
}};
// Each phalanx's share of its finger's length, and of the thumb's.
constexpr std::array<real_t, 3> kPhalanxShares{0.45F, 0.30F, 0.25F};
constexpr std::array<real_t, 3> kThumbShares{0.38F, 0.34F, 0.28F};
constexpr real_t kThumbRadius = 0.26F;

real_t palm_length(const CharacterMeasures& m) {
    return m.hand * 0.45F;
}

real_t finger_length(const CharacterMeasures& m) {
    return m.hand * 0.55F;
}

real_t palm_half_width(const CharacterMeasures& m) {
    return m.forearm_radius * 1.1F;
}

// The skeleton a recipe's measures place: Godot's humanoid profile's bones in its order, each
// turned as the profile's is but pointing where the body's next bone is (its tail), then a
// twist bone along each upper and lower arm and leg, which skin near the shoulder, wrist, hip
// and ankle follows, as MetaHuman's and Mixamo's rigs have.
std::vector<Bone> skeleton(const CharacterMeasures& m, const world::CharacterFace& face) {
    const std::span<const ProfileBone> profile = humanoid_profile();
    std::vector<Bone> bones(profile.size());
    std::vector<bool> aimed(profile.size(), false);
    const auto bone = [&](std::string_view name) -> Bone& {
        return bones.at(static_cast<std::size_t>(find_profile_bone(name)));
    };
    const auto aim = [&](std::string_view name, Vec3 tail) {
        bone(name).tail = tail;
        aimed.at(static_cast<std::size_t>(find_profile_bone(name))) = true;
    };
    for (std::size_t i = 0; i < profile.size(); ++i) {
        bones[i].name = std::string(profile[i].name);
        bones[i].parent = find_profile_bone(profile[i].parent);
    }
    const real_t base = m.head_base;
    const real_t hr = m.head_radius;
    const real_t hh = m.head_height;
    bone("Hips").head = {0.0F, m.hip, 0.0F};
    bone("Spine").head = {0.0F, m.waist, 0.0F};
    bone("Chest").head = {0.0F, m.chest, 0.0F};
    bone("UpperChest").head = {0.0F, m.chest + ((m.shoulder - m.chest) * 0.5F), 0.0F};
    bone("Neck").head = {0.0F, m.shoulder, 0.0F};
    bone("Head").head = {0.0F, base, 0.0F};
    bone("Jaw").head = {0.0F, base + (hh * 0.42F), hr * 0.1F};
    const real_t eye = hr * 0.16F * face.eye_size;
    // Arms hang 12 degrees out from straight down.
    const Vec2 out = cos_sin(0.2094395F);
    for (const std::string_view side_name : kSides) {
        const real_t side = side_sign(side_name);
        const std::string name(side_name);
        bone(name + "Eye").head = {side * hr * 0.38F * face.eye_spacing, base + (hh * 0.56F),
                                   hr - (eye * 0.5F)};
        const Vec3 down{side * out.y, -out.x, 0.0F};
        const Vec3 upper{side * m.shoulder_half_width, m.shoulder - (m.arm_radius * 0.5F), 0.0F};
        const Vec3 lower = upper + (down * m.upper_arm);
        const Vec3 wrist = lower + (down * m.lower_arm);
        bone(name + "Shoulder").head = {side * m.neck_radius, m.shoulder, 0.0F};
        bone(name + "UpperArm").head = upper;
        bone(name + "LowerArm").head = lower;
        bone(name + "Hand").head = wrist;
        // The fingers from their knuckles, each phalanx's head where the last one ends.
        const HandFrame f = hand_frame(lower, wrist, side);
        const real_t pl = palm_length(m);
        const real_t w = palm_half_width(m);
        for (std::size_t i = 0; i < kFingers.size(); ++i) {
            const FingerShape& shape = kFingerShapes[i];
            const Vec3 dir = normalized(f.along + (f.front * shape.spread));
            const real_t length = finger_length(m) * shape.length;
            Vec3 at = wrist + (f.along * (pl * shape.along)) + (f.front * (w * shape.across));
            for (std::size_t j = 0; j < kPhalanges.size(); ++j) {
                const std::string phalanx =
                    name + std::string(kFingers[i]) + std::string(kPhalanges[j]);
                bone(phalanx).head = at;
                at = at + (dir * (length * kPhalanxShares[j]));
                aim(phalanx, at);
            }
        }
        const Vec3 thumb_dir = normalized((f.along * 0.7F) + (f.front * 0.62F) + (f.palm * 0.25F));
        const real_t thumb = m.hand * 0.52F;
        Vec3 at =
            wrist + (f.along * (pl * 0.12F)) + (f.front * (w * 0.55F)) + (f.palm * (w * 0.15F));
        for (std::size_t j = 0; j < kThumb.size(); ++j) {
            const std::string phalanx = name + std::string(kThumb[j]);
            bone(phalanx).head = at;
            at = at + (thumb_dir * (thumb * kThumbShares[j]));
            aim(phalanx, at);
        }
        bone(name + "UpperLeg").head = {side * m.leg_offset, m.hip, 0.0F};
        bone(name + "LowerLeg").head = {side * m.leg_offset, m.knee, 0.0F};
        bone(name + "Foot").head = {side * m.leg_offset, m.ankle, 0.0F};
        bone(name + "Toes").head = {side * m.leg_offset, 0.0F, m.foot_length * 0.6F};
        aim(name + "Toes", {side * m.leg_offset, 0.0F, m.foot_length * 0.95F});
    }

    // Where the rest point, as the profile's tail directions say: a named child, the middle of
    // the children, or nowhere (turned as the parent was).
    for (std::size_t i = 0; i < profile.size(); ++i) {
        if (aimed[i]) {
            continue;
        }
        if (profile[i].tail_direction == TailDirection::SpecificChild) {
            bones[i].tail = bone(profile[i].tail).head;
            aimed[i] = true;
        } else if (profile[i].tail_direction == TailDirection::AverageChildren) {
            Vec3 sum;
            int count = 0;
            for (std::size_t c = 0; c < profile.size(); ++c) {
                if (bones[c].parent == static_cast<int>(i)) {
                    sum = sum + bones[c].head;
                    ++count;
                }
            }
            if (count > 0) {
                bones[i].tail = sum * (1.0F / static_cast<real_t>(count));
                aimed[i] = true;
            }
        }
    }
    // Each bone turned as the profile's, then onto its tail; one that points nowhere takes its
    // parent's turn away from the profile's.
    std::vector<Affine3D> reference(profile.size());
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const std::array<float, 9>& b = profile[i].basis;
        Affine3D local;
        local.x = {b[0], b[3], b[6]};
        local.y = {b[1], b[4], b[7]};
        local.z = {b[2], b[5], b[8]};
        reference[i] = bones[i].parent >= 0
                           ? reference[static_cast<std::size_t>(bones[i].parent)] * local
                           : local;
        const Vec3 span = bones[i].tail - bones[i].head;
        if (aimed[i] && length(span) > 0.000001F) {
            bones[i].turn = arc(reference[i].y, span) * reference[i];
        } else if (bones[i].parent >= 0) {
            const auto p = static_cast<std::size_t>(bones[i].parent);
            bones[i].turn = (bones[p].turn * reference[p].inverse()) * reference[i];
            bones[i].tail = bones[i].head;
        } else {
            bones[i].turn = reference[i];
            bones[i].tail = bones[i].head;
        }
    }
    // The twist bones, turned as their parents.
    for (const std::string_view side_name : kSides) {
        const std::string name(side_name);
        const auto twist = [&](const std::string& limb, const std::string& next, real_t share) {
            const Bone& parent = bone(name + limb);
            const Vec3 head = lerp(parent.head, bone(name + next).head, share);
            bones.push_back({.name = name + limb + "Twist",
                             .parent = find_profile_bone(name + limb),
                             .head = head,
                             .turn = parent.turn,
                             .tail = parent.tail});
        };
        twist("UpperArm", "LowerArm", 0.3F);
        twist("LowerArm", "Hand", 0.65F);
        twist("UpperLeg", "LowerLeg", 0.3F);
        twist("LowerLeg", "Foot", 0.65F);
    }
    return bones;
}

std::size_t bone_index(const std::vector<Bone>& bones, std::string_view name) {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) {
            return i;
        }
    }
    return 0;
}

// The parts of a body: its shapes, which skin and clothing both use, the clothing a little out
// from the skin by grow. Skin bends smoothly where bones meet: each limb is weighted to its bone,
// half to the next at the joint, and in part to its twist bone.
struct Body {
    const CharacterMeasures& m;
    const std::vector<Bone>& bones;
    Builder& out;

    [[nodiscard]] std::size_t bone(std::string_view name) const { return bone_index(bones, name); }
    [[nodiscard]] Vec3 head_of(std::string_view name) const { return bones[bone(name)].head; }

    // A round-ended tube along a stretch of a limb, from and to as shares of it.
    void limb(Vec3 a, Vec3 b, float radius, float from, float to, int material,
              const Weigh& weigh) const {
        const Vec3 start = lerp(a, b, from);
        const Vec3 end = lerp(a, b, to);
        const real_t span = length(end - start);
        out.add(capsule(radius, static_cast<float>(span)),
                placed(facing(end - start), {1, 1, 1}, lerp(start, end, 0.5F)), material, weigh);
    }

    void limb(Vec3 a, Vec3 b, float radius, float from, float to, int material,
              std::size_t joint) const {
        const VertexWeights weights = one_joint(joint);
        limb(a, b, radius, from, to, material, [weights](Vec3) { return weights; });
    }

    // A limb's weights from a to b: its bone (self), the bone before it easing in up to half at
    // a and the one after it up to half at b, and its twist bone's share, most near a for an
    // upper limb and near b for a lower one.
    [[nodiscard]] Weigh along(Vec3 a, Vec3 b, std::string_view before, std::string_view self,
                              std::string_view twist, bool upper, std::string_view after) const {
        const std::size_t first = bone(before);
        const std::size_t own = bone(self);
        const std::size_t turned = twist.empty() ? own : bone(twist);
        const std::size_t next = bone(after);
        const Vec3 span = b - a;
        const real_t span2 = std::max<real_t>(dot(span, span), 0.000001F);
        return [=](Vec3 p) {
            const real_t at = dot(p - a, span) / span2;
            const real_t share = upper ? 1.0F - ramp(0.1F, 0.75F, at) : ramp(0.25F, 0.9F, at);
            VertexWeights weights = blend_joints(own, turned, static_cast<float>(share));
            const real_t into_first = std::clamp<real_t>(1.0F - (at / 0.15F), 0.0F, 1.0F);
            const real_t into_next = std::clamp<real_t>((at - 0.85F) / 0.15F, 0.0F, 1.0F);
            weights = mix_weights(weights, one_joint(first), static_cast<float>(0.5F * into_first));
            return mix_weights(weights, one_joint(next), static_cast<float>(0.5F * into_next));
        };
    }

    // The trunk's weights by height: the hips, spine, chest and upper chest, each most at the
    // middle of its stretch and blended between.
    [[nodiscard]] VertexWeights trunk_at(real_t y) const {
        const real_t upper = head_of("UpperChest").y;
        const std::array<std::pair<real_t, std::size_t>, 4> centers{{
            {(m.hip + m.waist) * 0.5F, bone("Hips")},
            {(m.waist + m.chest) * 0.5F, bone("Spine")},
            {(m.chest + upper) * 0.5F, bone("Chest")},
            {(upper + m.shoulder) * 0.5F, bone("UpperChest")},
        }};
        if (y <= centers[0].first) {
            return one_joint(centers[0].second);
        }
        for (std::size_t i = 0; i + 1 < centers.size(); ++i) {
            if (y <= centers[i + 1].first) {
                return blend_joints(
                    centers[i].second, centers[i + 1].second,
                    static_cast<float>(ramp(centers[i].first, centers[i + 1].first, y)));
            }
        }
        return one_joint(centers.back().second);
    }

    [[nodiscard]] Weigh trunk() const {
        return [this](Vec3 p) { return trunk_at(p.y); };
    }

    void torso(float grow, int material) const {
        const Vec3 center{0.0F, ((m.waist + m.shoulder) * 0.5F) - (0.02F * m.scale), 0.0F};
        const Vec3 radii{m.chest_half_width + grow,
                         ((m.shoulder - m.waist) * 0.5F) + (0.03F * m.scale) + grow,
                         m.chest_depth + grow};
        out.add(unit_sphere(), placed(radii, center), material, trunk());
        // The shoulders, across from one arm to the other: the upper chest in the middle, then
        // each collarbone, then half the arm where it starts.
        const Vec3 left = head_of("LeftUpperArm");
        const Vec3 right = head_of("RightUpperArm");
        const std::size_t chest = bone("UpperChest");
        const std::array<std::size_t, 2> shoulders{bone("LeftShoulder"), bone("RightShoulder")};
        const std::array<std::size_t, 2> arms{bone("LeftUpperArmTwist"),
                                              bone("RightUpperArmTwist")};
        const real_t inner = m.neck_radius;
        const real_t outer = m.shoulder_half_width;
        out.add(capsule((m.arm_radius * 1.1F) + grow, static_cast<float>(length(left - right))),
                placed(facing(left - right), {1, 1, 1}, lerp(left, right, 0.5F)), material,
                [=](Vec3 p) {
                    const std::size_t s = p.x >= 0.0F ? 0 : 1;
                    const real_t x = std::abs(p.x);
                    const VertexWeights weights = blend_joints(
                        chest, shoulders.at(s), static_cast<float>(ramp(inner, outer, x)));
                    return mix_weights(
                        weights, one_joint(arms.at(s)),
                        static_cast<float>(0.5F * ramp(outer * 0.85F, outer * 1.15F, x)));
                });
    }

    void pelvis(float grow, int material) const {
        const Vec3 center{0.0F, (m.hip + m.waist) * 0.5F, 0.0F};
        const Vec3 radii{m.hip_half_width + grow,
                         ((m.waist - m.hip) * 0.5F) + (0.06F * m.scale) + grow, m.hip_depth + grow};
        out.add(unit_sphere(), placed(radii, center), material, trunk());
    }

    // A hand: its palm, then each finger's and the thumb's phalanges on their bones.
    void hand(std::string_view side_name, float grow, int material) const {
        const std::string side(side_name);
        const HandFrame f =
            hand_frame(head_of(side + "LowerArm"), head_of(side + "Hand"), side_sign(side));
        const real_t pl = palm_length(m);
        const real_t w = palm_half_width(m);
        Affine3D turn;
        turn.x = f.front;
        turn.y = f.along;
        turn.z = cross(f.front, f.along);
        out.add(unit_sphere(),
                placed(turn, {w + grow, (pl * 0.58F) + grow, (w * 0.5F) + grow},
                       f.wrist + (f.along * (pl * 0.5F))),
                material, bone(side + "Hand"));
        const auto phalanx = [&](const std::string& name, real_t radius) {
            const Bone& b = bones[bone(name)];
            limb(b.head, b.tail, static_cast<float>(radius) + grow, 0.0F, 1.0F, material,
                 bone(name));
        };
        for (std::size_t i = 0; i < kFingers.size(); ++i) {
            for (std::size_t j = 0; j < kPhalanges.size(); ++j) {
                const real_t taper = j == 2 ? 0.9F : 1.0F;
                phalanx(side + std::string(kFingers[i]) + std::string(kPhalanges[j]),
                        w * kFingerShapes[i].radius * taper);
            }
        }
        for (std::size_t j = 0; j < kThumb.size(); ++j) {
            phalanx(side + std::string(kThumb[j]), w * kThumbRadius * (j == 2 ? 0.9F : 1.0F));
        }
    }

    void arms(Region region, float grow, float from, float to, int material) const {
        for (const std::string_view side_name : kSides) {
            const std::string side(side_name);
            const Vec3 upper = head_of(side + "UpperArm");
            const Vec3 lower = head_of(side + "LowerArm");
            const Vec3 hand_at = head_of(side + "Hand");
            if (region == Region::UpperArms) {
                limb(upper, lower, m.arm_radius + grow, from, to, material,
                     along(upper, lower, side + "Shoulder", side + "UpperArm",
                           side + "UpperArmTwist", true, side + "LowerArm"));
            } else if (region == Region::LowerArms) {
                limb(lower, hand_at, m.forearm_radius + grow, from, to, material,
                     along(lower, hand_at, side + "UpperArm", side + "LowerArm",
                           side + "LowerArmTwist", false, side + "Hand"));
            } else {
                hand(side, grow, material);
            }
        }
    }

    void legs(Region region, float grow, float from, float to, int material) const {
        for (const std::string_view side_name : kSides) {
            const std::string side(side_name);
            const Vec3 hip = head_of(side + "UpperLeg");
            const Vec3 knee = head_of(side + "LowerLeg");
            const Vec3 ankle = head_of(side + "Foot");
            if (region == Region::UpperLegs) {
                limb(hip, knee, m.thigh_radius + grow, from, to, material,
                     along(hip, knee, "Hips", side + "UpperLeg", side + "UpperLegTwist", true,
                           side + "LowerLeg"));
            } else if (region == Region::LowerLegs) {
                // Ended high enough that its round end stays above the floor.
                const float radius = m.calf_radius + grow;
                const Vec3 end{ankle.x, std::max<real_t>(ankle.y, radius), ankle.z};
                limb(knee, end, radius, from, to, material,
                     along(knee, ankle, side + "UpperLeg", side + "LowerLeg",
                           side + "LowerLegTwist", false, side + "Foot"));
            } else {
                // Standing on the floor, however thick the sole; its front bends with the toes.
                const real_t half = (m.ankle * 0.6F) + grow;
                const Vec3 center{ankle.x, half, m.foot_length * 0.3F};
                const std::size_t foot = bone(side + "Foot");
                const std::size_t toes = bone(side + "Toes");
                const real_t start = m.foot_length * 0.45F;
                const real_t end = m.foot_length * 0.7F;
                out.add(
                    unit_sphere(),
                    placed({(m.calf_radius * 1.05F) + grow, half, (m.foot_length * 0.6F) + grow},
                           center),
                    material, [=](Vec3 p) {
                        return blend_joints(foot, toes, static_cast<float>(ramp(start, end, p.z)));
                    });
            }
        }
    }

    // Where the mouth closes, and how much of the head below it moves with the jaw.
    [[nodiscard]] real_t mouth() const { return m.head_base + (m.head_height * 0.27F); }
    [[nodiscard]] Weigh jaw_weights() const {
        const std::size_t head = bone("Head");
        const std::size_t jaw = bone("Jaw");
        const real_t line = mouth();
        const real_t hh = m.head_height;
        const real_t hr = m.head_radius;
        return [=](Vec3 p) {
            const real_t below = 1.0F - ramp(line - (hh * 0.02F), line + (hh * 0.01F), p.y);
            const real_t front = ramp(0.0F, hr * 0.5F, p.z);
            return blend_joints(head, jaw, static_cast<float>(below * front));
        };
    }
};

void add_face(const CharacterRecipe& recipe, const Body& body, int skin) {
    const CharacterMeasures& m = body.m;
    Builder& out = body.out;
    const std::size_t head = body.bone("Head");
    const std::size_t jaw = body.bone("Jaw");
    const real_t hr = m.head_radius;
    const real_t hh = m.head_height;
    const real_t base = m.head_base;
    // The neck and head, its chin moving with the jaw.
    const Vec3 neck_low{0.0F, m.shoulder - (0.03F * m.scale), 0.0F};
    const Vec3 neck_high{0.0F, base + (0.02F * m.scale), 0.0F};
    body.limb(neck_low, neck_high, m.neck_radius, 0.0F, 1.0F, skin,
              body.along(neck_low, neck_high, "UpperChest", "Neck", "", false, "Head"));
    out.add(unit_sphere(), placed({hr, hh * 0.5F, hr * 1.12F}, {0.0F, base + (hh * 0.5F), 0.0F}),
            skin, body.jaw_weights());
    const world::CharacterFace& face = recipe.face;
    // Eyes that turn: a white ball, the iris and the pupil on each eye's bone.
    const int white = out.material("eye_white", {0.92F, 0.9F, 0.86F, 1.0F}, 0.15F, 0.0F);
    const int iris = out.material("eyes", face.eyes, 0.2F, 0.0F);
    const int pupil = out.material("pupil", {0.02F, 0.02F, 0.02F, 1.0F}, 0.1F, 0.0F);
    const real_t eye = hr * 0.16F * face.eye_size;
    for (const std::string_view side : kSides) {
        const std::size_t ball = body.bone(std::string(side) + "Eye");
        const Vec3 at = body.bones[ball].head;
        out.add(unit_sphere(), placed(Vec3{1, 1, 1} * eye, at), white, ball);
        out.add(unit_sphere(),
                placed({eye * 0.55F, eye * 0.55F, eye * 0.18F}, at + Vec3{0.0F, 0.0F, eye * 0.88F}),
                iris, ball);
        out.add(unit_sphere(),
                placed({eye * 0.24F, eye * 0.24F, eye * 0.12F}, at + Vec3{0.0F, 0.0F, eye * 0.98F}),
                pupil, ball);
        out.add(unit_sphere(),
                placed(Vec3{0.15F, 0.32F, 0.22F} * (hr * face.ears),
                       {side_sign(side) * hr * 0.98F, base + (hh * 0.5F), 0.0F}),
                skin, head);
    }
    out.add(
        unit_sphere(),
        placed(Vec3{0.13F, 0.2F, 0.16F} * (hr * face.nose), {0.0F, base + (hh * 0.42F), hr * 1.1F}),
        skin, head);
    // The lips, the upper on the head and the lower on the jaw, and inside the mouth its dark,
    // the teeth and the tongue, which show as the jaw opens.
    const int lips = out.material("lips", face.lips, 0.5F, 0.0F);
    const real_t line = body.mouth();
    out.add(unit_sphere(),
            placed(Vec3{0.32F, 0.045F, 0.1F} * hr, {0.0F, line + (hr * 0.04F), hr * 1.03F}), lips,
            head);
    out.add(unit_sphere(),
            placed(Vec3{0.3F, 0.05F, 0.1F} * hr, {0.0F, line - (hr * 0.045F), hr * 1.02F}), lips,
            jaw);
    const int inside = out.material("mouth", {0.25F, 0.08F, 0.08F, 1.0F}, 0.7F, 0.0F);
    out.add(unit_sphere(), placed({hr * 0.25F, hh * 0.08F, hr * 0.25F}, {0.0F, line, hr * 0.78F}),
            inside, [=](Vec3 p) {
                return blend_joints(
                    head, jaw,
                    static_cast<float>(1.0F - ramp(line - (hh * 0.01F), line + (hh * 0.01F), p.y)));
            });
    const int teeth = out.material("teeth", {0.92F, 0.9F, 0.82F, 1.0F}, 0.3F, 0.0F);
    out.add(unit_sphere(),
            placed({hr * 0.22F, hh * 0.03F, hr * 0.12F}, {0.0F, line + (hh * 0.015F), hr * 0.86F}),
            teeth, head);
    out.add(unit_sphere(),
            placed({hr * 0.2F, hh * 0.03F, hr * 0.12F}, {0.0F, line - (hh * 0.015F), hr * 0.84F}),
            teeth, jaw);
    const int tongue = out.material("tongue", {0.75F, 0.35F, 0.38F, 1.0F}, 0.5F, 0.0F);
    out.add(unit_sphere(),
            placed({hr * 0.15F, hh * 0.025F, hr * 0.25F}, {0.0F, line - (hh * 0.03F), hr * 0.7F}),
            tongue, jaw);
    if (face.beard != "none") {
        const int hair = out.material("hair", recipe.hair.color, 0.8F, 0.0F);
        const real_t grow = face.beard == "stubble" ? 0.01F : 0.06F;
        out.add(unit_sphere(),
                placed({hr * (1.0F + grow), hh * (0.16F + grow), hr * (1.0F + grow)},
                       {0.0F, base + (hh * 0.12F), hr * 0.15F}),
                hair, body.jaw_weights());
        if (face.beard == "long") {
            body.limb({0.0F, base + (hh * 0.1F), hr * 0.9F},
                      {0.0F, m.shoulder - (0.08F * m.scale), m.chest_depth + (0.02F * m.scale)},
                      static_cast<float>(hr * 0.35F), 0.0F, 1.0F, hair, jaw);
        }
    }
}

void add_hair(const CharacterRecipe& recipe, const Body& body) {
    const world::CharacterHair& hair = recipe.hair;
    if (hair.style == "none" && hair.groom.empty()) {
        return;
    }
    const CharacterMeasures& m = body.m;
    Builder& out = body.out;
    const std::size_t head = body.bone("Head");
    const real_t hr = m.head_radius;
    const real_t hh = m.head_height;
    const real_t base = m.head_base;
    // A groom grows on a scalp of its own, which draws in the hair's color under it.
    const bool groomed = !hair.groom.empty();
    const int material = groomed ? out.material("scalp", hair.color, 0.9F, 0.0F)
                                 : out.material("hair", hair.color, 0.8F, 0.0F);
    if (groomed || hair.style != "mohawk") {
        out.add(
            unit_sphere(true),
            placed({hr * 1.07F, hh * 0.45F, hr * 1.16F}, {0.0F, base + (hh * 0.62F), -hr * 0.04F}),
            material, head);
        out.add(
            unit_sphere(),
            placed({hr * 1.02F, hh * 0.38F, hr * 0.85F}, {0.0F, base + (hh * 0.55F), -hr * 0.35F}),
            material, head);
    }
    if (groomed) {
        return;
    }
    const real_t long_hair = hair.length * 0.5F * m.scale;
    if (hair.style == "long") {
        out.add(unit_sphere(),
                placed({hr * 1.05F, (long_hair * 0.5F) + (hh * 0.2F), hr * 0.5F},
                       {0.0F, base + (hh * 0.5F) - (long_hair * 0.5F), -hr * 0.55F}),
                material, head);
    } else if (hair.style == "ponytail") {
        body.limb({0.0F, base + (hh * 0.7F), -hr * 1.1F},
                  {0.0F, base + (hh * 0.7F) - ((0.1F * m.scale) + long_hair), -hr * 1.3F},
                  static_cast<float>(hr * 0.22F), 0.0F, 1.0F, material, head);
    } else if (hair.style == "bun") {
        out.add(unit_sphere(),
                placed(Vec3{1, 1, 1} * (hr * 0.42F), {0.0F, base + (hh * 0.9F), -hr * 0.9F}),
                material, head);
    } else if (hair.style == "mohawk") {
        out.add(unit_sphere(),
                placed({hr * 0.18F, (hh * 0.32F) + (hair.length * 0.1F * m.scale), hr * 1.0F},
                       {0.0F, base + (hh * 0.85F), -hr * 0.1F}),
                material, head);
    }
}

void add_garment(const CharacterRecipe& recipe, std::size_t index, const Body& body) {
    const Garment& garment = recipe.clothing[index];
    const CharacterMeasures& m = body.m;
    Builder& out = body.out;
    const float grow = world::garment_offset(recipe, index);
    const auto [roughness, metallic] = surface_of(garment.material);
    for (const Covering& cover : world::garment_coverage(garment)) {
        const bool free = cover.region == Region::Skirt || cover.region == Region::Cape;
        const std::string name = garment.cloth && !free ? garment.kind + "_fitted" : garment.kind;
        const int material = out.material(name, garment.color, roughness, metallic, free);
        switch (cover.region) {
        case Region::Torso:
            body.torso(grow, material);
            break;
        case Region::Plate:
            body.torso(grow + (0.01F * m.scale), material);
            break;
        case Region::Pelvis:
            body.pelvis(grow, material);
            break;
        case Region::UpperArms:
        case Region::LowerArms:
        case Region::Hands:
            body.arms(cover.region, grow, cover.from, cover.to, material);
            break;
        case Region::UpperLegs:
        case Region::LowerLegs:
        case Region::Feet:
            body.legs(cover.region, grow, cover.from, cover.to, material);
            break;
        case Region::Skirt: {
            const real_t height = cover.to * (m.waist - (0.02F * m.scale));
            const float flare = 1.3F + (0.5F * garment.loose);
            out.add(open_tube(flare),
                    placed({m.hip_half_width + grow, height, (m.hip_depth * 1.3F) + grow},
                           {0.0F, m.waist - (height * 0.5F), 0.0F}),
                    material, body.bone("Hips"));
            break;
        }
        case Region::Cape: {
            const real_t height = cover.to * (m.shoulder - (0.05F * m.scale));
            world::PlaneMesh plane;
            plane.size = {1.0F, 1.0F};
            plane.subdivide_width = 6;
            plane.subdivide_depth = 12;
            plane.orientation = "z";
            // Turned half a turn, so it faces back from behind the shoulders.
            Affine3D back;
            back.x = {-1.0F, 0.0F, 0.0F};
            back.z = {0.0F, 0.0F, -1.0F};
            out.add(plane_mesh(plane),
                    placed(back, {m.shoulder_half_width * 2.4F, height, 1.0F},
                           {0.0F, m.shoulder - (height * 0.5F),
                            -(m.chest_depth + grow + (0.03F * m.scale))}),
                    material, body.bone("UpperChest"));
            break;
        }
        case Region::Belt:
            out.add(open_tube(),
                    placed({(m.hip_half_width * 0.95F) + grow, 0.035F * m.scale,
                            (m.hip_depth * 1.05F) + grow},
                           {0.0F, m.waist, 0.0F}),
                    material, body.bone("Hips"));
            break;
        case Region::Shoulders:
            for (const std::string side : {"Left", "Right"}) {
                out.add(unit_sphere(true),
                        placed(Vec3{1, 1, 1} * ((m.arm_radius * 1.7F) + grow),
                               body.head_of(side + "UpperArm")),
                        material, body.bone(side + "UpperArm"));
            }
            break;
        case Region::Hat: {
            const real_t hr = m.head_radius;
            const real_t brim = m.head_base + (m.head_height * 0.82F);
            const real_t crown = m.head_height * (0.3F + (0.5F * garment.length));
            out.add(disc_cylinder(),
                    placed({(hr * 1.7F) + grow, 0.012F * m.scale, (hr * 1.7F) + grow},
                           {0.0F, brim, 0.0F}),
                    material, body.bone("Head"));
            out.add(disc_cylinder(1.1F),
                    placed({(hr * 0.95F) + grow, crown, (hr * 1.05F) + grow},
                           {0.0F, brim + (crown * 0.5F), 0.0F}),
                    material, body.bone("Head"));
            break;
        }
        case Region::Hood: {
            // A dome open to the front, its pole behind the head.
            Affine3D open;
            open.y = {0.0F, 0.0F, -1.0F};
            open.z = {0.0F, 1.0F, 0.0F};
            const real_t hr = m.head_radius;
            out.add(
                unit_sphere(true),
                placed(open,
                       {(hr * 1.2F) + grow, (hr * 1.35F) + grow, (m.head_height * 0.62F) + grow},
                       {0.0F, m.head_base + (m.head_height * 0.55F), hr * 0.2F}),
                material, body.bone("Head"));
            out.add(open_tube(1.2F),
                    placed(Vec3{1, 1, 1} * ((m.neck_radius * 2.2F) + grow),
                           {0.0F, (m.shoulder + m.head_base) * 0.5F, 0.0F}),
                    material, body.bone("Neck"));
            break;
        }
        case Region::Helmet:
            out.add(unit_sphere(true),
                    placed({(m.head_radius * 1.15F) + grow, (m.head_height * 0.6F) + grow,
                            (m.head_radius * 1.25F) + grow},
                           {0.0F, m.head_base + (m.head_height * 0.5F), 0.0F}),
                    material, body.bone("Head"));
            break;
        case Region::Scarf: {
            world::TorusMesh torus;
            torus.inner_radius = 0.6F;
            torus.outer_radius = 1.0F;
            torus.rings = 16;
            torus.ring_segments = 8;
            const real_t size = (m.neck_radius * 1.9F) + grow;
            out.add(torus_mesh(torus),
                    placed({size, size, size}, {0.0F, m.shoulder + (0.02F * m.scale), 0.0F}),
                    material, body.bone("Neck"));
            body.limb({m.neck_radius * 0.3F, m.shoulder, m.neck_radius * 1.5F},
                      {m.neck_radius * 0.5F, m.chest - (0.05F * m.scale),
                       m.chest_depth + grow + (0.01F * m.scale)},
                      m.neck_radius * 0.5F, 0.0F, garment.length, material,
                      body.bone("UpperChest"));
            break;
        }
        }
    }
}

// Where each of a character's points is: the bone it moves with and its place at rest.
struct Point {
    std::string_view name;
    std::string_view bone;
    Vec3 at;
};

std::vector<Point> points(const CharacterMeasures& m, const std::vector<Bone>& bones) {
    const auto hand_point = [&](std::string_view side) {
        const std::string prefix(side);
        const Vec3 lower = bones[bone_index(bones, prefix + "LowerArm")].head;
        const Vec3 hand = bones[bone_index(bones, prefix + "Hand")].head;
        return hand + (normalized(hand - lower) * (m.hand * 0.45F));
    };
    return {
        {"right_hand", "RightHand", hand_point("Right")},
        {"left_hand", "LeftHand", hand_point("Left")},
        {"back",
         "UpperChest",
         {0.0F, (m.chest + m.shoulder) * 0.5F, -(m.chest_depth + (0.02F * m.scale))}},
        {"hip_left",
         "Hips",
         {m.hip_half_width + (0.01F * m.scale), (m.hip + m.waist) * 0.5F, 0.0F}},
        {"hip_right",
         "Hips",
         {-(m.hip_half_width + (0.01F * m.scale)), (m.hip + m.waist) * 0.5F, 0.0F}},
        {"head_top", "Head", {0.0F, m.top, 0.0F}},
    };
}

// A prop's model at its point, its surfaces moved by the point's bone.
Result<void> add_prop(const world::CharacterProp& prop, const Point& point, std::size_t joint,
                      Builder& out, const ModelReader& read) {
    auto bytes = read(prop.model);
    if (!bytes) {
        return make_error(ErrorCode::NotFound, "the prop \"" + prop.model +
                                                   "\" can't be read: " + bytes.error().message);
    }
    auto loaded = load_model(bytes.value(), prop.model, read);
    if (!loaded) {
        return make_error(ErrorCode::InvalidArgument,
                          "the prop \"" + prop.model +
                              "\" doesn't load: " + loaded.error().message);
    }
    const Model& model = loaded.value();
    const auto image_base = static_cast<int>(out.model.images.size());
    out.model.images.insert(out.model.images.end(), model.images.begin(), model.images.end());
    std::vector<int> materials;
    for (const ModelMaterial& material : model.materials) {
        ModelMaterial copy = material;
        for (int* image : {&copy.albedo_image, &copy.metallic_image, &copy.roughness_image,
                           &copy.normal_image, &copy.emission_image}) {
            if (*image >= 0) {
                *image += image_base;
            }
        }
        // Each prop's materials are its own, named after the prop where another has the name.
        for (int n = 2; std::ranges::any_of(
                 out.model.materials,
                 [&](const ModelMaterial& other) { return other.name == copy.name; });
             ++n) {
            copy.name = material.name + "_" + std::to_string(n);
        }
        out.model.materials.push_back(std::move(copy));
        materials.push_back(static_cast<int>(out.model.materials.size()) - 1);
    }
    world::Transform3D local;
    local.position = prop.position;
    local.rotation = prop.rotation;
    local.scale = Vec3{1, 1, 1} * prop.scale;
    const Affine3D at = moved(point.at) * world::affine(local);
    for (const ModelPart& part : model.parts) {
        for (const ModelSurface& surface : model.meshes.at(part.mesh).surfaces) {
            const MeshData data{surface.vertices, surface.indices};
            const int material =
                surface.material >= 0
                    ? materials.at(static_cast<std::size_t>(surface.material))
                    : out.material("prop_default", {1.0F, 1.0F, 1.0F, 1.0F}, 1.0F, 0.0F);
            out.add(data, at * part.transform, material, joint);
        }
    }
    return {};
}

// The hand poses and the open jaw a character's model plays as its own animations, each one key:
// every finger's, the thumb's and the jaw's bends in degrees, turned about the way each curls
// toward the palm (or opens down).
struct HandPose {
    std::string_view name;
    std::array<std::array<float, 3>, 5> degrees; // the thumb, then the fingers
};
constexpr std::array<HandPose, 4> kHandPoses{{
    {"relaxed", {{{5, 10, 10}, {15, 25, 15}, {15, 25, 15}, {15, 25, 15}, {20, 30, 20}}}},
    {"fist", {{{25, 35, 45}, {85, 100, 70}, {85, 100, 70}, {85, 100, 70}, {85, 100, 70}}}},
    {"grip", {{{20, 25, 20}, {55, 65, 45}, {55, 65, 45}, {55, 65, 45}, {55, 65, 45}}}},
    {"point", {{{25, 35, 45}, {0, 0, 0}, {85, 100, 70}, {85, 100, 70}, {85, 100, 70}}}},
}};
constexpr float kJawOpenDegrees = 22.0F;
constexpr float kRadians = 0.017453292519943295F;

// A key turning a bone by turn in the model's space about its head: its rest in its parent, then
// the turn brought into its own frame.
NodeTrack bend_track(const Model& model, const std::vector<Bone>& bones, std::size_t index,
                     const Quat& turn) {
    const Quat frame = decompose(bones[index].turn).rotation;
    const Quat rotation = normalized(
        multiply(model.nodes[index].rotation, multiply(inverse(frame), multiply(turn, frame))));
    NodeTrack track;
    track.node = index;
    track.property = NodeProperty::Rotation;
    track.times = {0.0F};
    track.values = {{static_cast<float>(rotation.x), static_cast<float>(rotation.y),
                     static_cast<float>(rotation.z), static_cast<float>(rotation.w)}};
    return track;
}

void add_clips(Model& model, const std::vector<Bone>& bones) {
    const auto clip = [](std::string name) {
        ModelAnimation animation;
        animation.name = std::move(name);
        animation.length = 1.0 / 30.0;
        return animation;
    };
    for (const std::string_view side : kSides) {
        const std::string prefix(side);
        const auto at = [&](std::string_view name) {
            return bone_index(bones, prefix + std::string(name));
        };
        const HandFrame f =
            hand_frame(bones[at("LowerArm")].head, bones[at("Hand")].head, side_sign(side));
        const Vec3 curl = cross(f.along, f.palm);
        const Vec3 thumb_dir =
            bones[at("ThumbMetacarpal")].tail - bones[at("ThumbMetacarpal")].head;
        const Vec3 thumb_curl = cross(thumb_dir, f.palm);
        for (const HandPose& pose : kHandPoses) {
            ModelAnimation animation = clip(std::string(pose.name) + "_" +
                                            (side == "Left" ? std::string("left") : "right"));
            for (std::size_t j = 0; j < kThumb.size(); ++j) {
                animation.tracks.push_back(
                    bend_track(model, bones, at(kThumb[j]),
                               turn_about(thumb_curl, pose.degrees[0][j] * kRadians)));
            }
            for (std::size_t i = 0; i < kFingers.size(); ++i) {
                for (std::size_t j = 0; j < kPhalanges.size(); ++j) {
                    animation.tracks.push_back(bend_track(
                        model, bones, at(std::string(kFingers[i]) + std::string(kPhalanges[j])),
                        turn_about(curl, pose.degrees[i + 1][j] * kRadians)));
                }
            }
            model.animations.push_back(std::move(animation));
        }
    }
    ModelAnimation jaw = clip("jaw_open");
    jaw.tracks.push_back(bend_track(model, bones, bone_index(bones, "Jaw"),
                                    turn_about({1.0F, 0.0F, 0.0F}, kJawOpenDegrees * kRadians)));
    model.animations.push_back(std::move(jaw));
}

void fill_triangle(Silhouette& out, std::array<Vec2, 3> corners) {
    const auto edge = [](Vec2 a, Vec2 b, Vec2 p) {
        return ((b.x - a.x) * (p.y - a.y)) - ((b.y - a.y) * (p.x - a.x));
    };
    const real_t area = edge(corners[0], corners[1], corners[2]);
    if (area == 0.0F) {
        return;
    }
    const real_t sign = area > 0.0F ? 1.0F : -1.0F;
    real_t low_x = corners[0].x;
    real_t high_x = corners[0].x;
    real_t low_y = corners[0].y;
    real_t high_y = corners[0].y;
    for (const Vec2 c : corners) {
        low_x = std::min(low_x, c.x);
        high_x = std::max(high_x, c.x);
        low_y = std::min(low_y, c.y);
        high_y = std::max(high_y, c.y);
    }
    const int size = out.size;
    const int x0 = std::max(0, static_cast<int>(std::floor(low_x - 0.5F)));
    const int x1 = std::min(size - 1, static_cast<int>(std::ceil(high_x - 0.5F)));
    const int y0 = std::max(0, static_cast<int>(std::floor(low_y - 0.5F)));
    const int y1 = std::min(size - 1, static_cast<int>(std::ceil(high_y - 0.5F)));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const Vec2 p{static_cast<real_t>(x) + 0.5F, static_cast<real_t>(y) + 0.5F};
            if (sign * edge(corners[0], corners[1], p) >= 0.0F &&
                sign * edge(corners[1], corners[2], p) >= 0.0F &&
                sign * edge(corners[2], corners[0], p) >= 0.0F) {
                out.cells[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) +
                          static_cast<std::size_t>(x)] = 1;
            }
        }
    }
}

// The body's mesh as the model's one part, on a node of its own.
Model finish(Builder& out, const CharacterRecipe& recipe) {
    out.body.name = recipe.name.empty() ? std::string("Body") : recipe.name;
    out.model.parts.push_back({.node = "Body",
                               .mesh = 0,
                               .transform = {},
                               .node_index = static_cast<int>(out.model.nodes.size()),
                               .skin = 0});
    ModelNode node;
    node.name = "Body";
    node.file_index = static_cast<int>(out.model.nodes.size());
    out.model.nodes.push_back(std::move(node));
    out.model.meshes.push_back(std::move(out.body));
    return std::move(out.model);
}

} // namespace

Result<Model> build_character(const CharacterRecipe& recipe, const ModelReader& read,
                              std::string_view form) {
    if (!form.empty() && form != "skeleton" && form != "muscles") {
        return make_error(ErrorCode::InvalidArgument,
                          "a character's forms are skeleton and muscles, not " + std::string(form));
    }
    const CharacterMeasures m = world::character_measures(recipe);
    const std::vector<Bone> bones = skeleton(m, recipe.face);
    Builder out;
    // Each bone's rest in its parent, from its turn and head in the model's space.
    const auto global_of = [&](std::size_t i) {
        Affine3D global = bones[i].turn;
        global.origin = bones[i].head;
        return global;
    };
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const Bone& bone = bones[i];
        const Affine3D local =
            bone.parent < 0
                ? global_of(i)
                : global_of(static_cast<std::size_t>(bone.parent)).inverse() * global_of(i);
        ModelNode node;
        node.name = bone.name;
        node.parent = bone.parent;
        node.position = local.origin;
        node.rotation = decompose(local).rotation;
        node.file_index = static_cast<int>(out.model.nodes.size());
        out.model.nodes.push_back(std::move(node));
    }
    // Points sit unturned in the model's space at rest, whatever their bones' turns.
    const std::vector<Point> placed_points = points(m, bones);
    for (const Point& point : placed_points) {
        const std::size_t parent = bone_index(bones, point.bone);
        const Affine3D from_bone = global_of(parent).inverse();
        Affine3D local = from_bone;
        local.origin = from_bone.apply(point.at);
        ModelNode node;
        node.name = std::string(point.name);
        node.parent = static_cast<int>(parent);
        node.position = local.origin;
        node.rotation = decompose(local).rotation;
        node.file_index = static_cast<int>(out.model.nodes.size());
        out.model.nodes.push_back(std::move(node));
    }
    // The skin binds each joint where its node's rest puts it, so the model at rest is its mesh.
    const std::vector<Affine3D> rests = node_globals(out.model, rest_pose(out.model));
    ModelSkin skin;
    skin.name = "Skeleton";
    for (std::size_t i = 0; i < bones.size(); ++i) {
        skin.joints.push_back(i);
        skin.inverse_binds.push_back(rests[i].inverse());
    }
    out.model.skins.push_back(std::move(skin));
    add_clips(out.model, bones);

    if (!form.empty()) {
        add_anatomy(world::make_anatomy(recipe, world::body_frames_3d(m)), bones, out,
                    form == "muscles");
        return finish(out, recipe);
    }
    const Body body{m, bones, out};
    const int skin_material = out.material("skin", recipe.body.skin, 0.6F, 0.0F);
    body.torso(0.0F, skin_material);
    body.pelvis(0.0F, skin_material);
    for (const Region region : {Region::UpperArms, Region::LowerArms, Region::Hands}) {
        body.arms(region, 0.0F, 0.0F, 1.0F, skin_material);
    }
    for (const Region region : {Region::UpperLegs, Region::LowerLegs, Region::Feet}) {
        body.legs(region, 0.0F, 0.0F, 1.0F, skin_material);
    }
    add_face(recipe, body, skin_material);
    add_hair(recipe, body);
    for (std::size_t i = 0; i < recipe.clothing.size(); ++i) {
        add_garment(recipe, i, body);
    }
    for (const world::CharacterProp& prop : recipe.props) {
        const auto point = std::ranges::find_if(
            placed_points, [&](const Point& p) { return p.name == prop.point; });
        if (point == placed_points.end()) {
            return make_error(ErrorCode::InvalidArgument,
                              "a prop is at \"" + prop.point + "\", which is no point");
        }
        if (auto added = add_prop(prop, *point, bone_index(bones, point->bone), out, read);
            !added) {
            return added.error();
        }
    }
    return finish(out, recipe);
}

Result<Model> load_character_model(std::string_view bytes, const ModelReader& read,
                                   std::string_view form) {
    auto doc = json::parse(bytes);
    if (!doc) {
        return doc.error();
    }
    auto recipe = world::parse_character(doc.value(), read);
    if (!recipe) {
        return recipe.error();
    }
    return build_character(recipe.value(), read, form);
}

std::vector<json::ValidationError> check_character_model(std::string_view bytes,
                                                         const ModelReader& read) {
    auto doc = json::parse(bytes);
    if (!doc) {
        return {{"", doc.error().message}};
    }
    auto resolved = world::resolve_character(doc.value(), read);
    if (!resolved) {
        return {{"/extends", resolved.error().message}};
    }
    std::vector<json::ValidationError> errors = world::validate_character(resolved.value());
    if (!errors.empty()) {
        return errors;
    }
    auto recipe = world::parse_character(doc.value(), read);
    if (!recipe) {
        return {{"", recipe.error().message}};
    }
    for (std::size_t i = 0; i < recipe.value().props.size(); ++i) {
        const std::string& file = recipe.value().props[i].model;
        auto text = read(file);
        if (!text) {
            errors.push_back({"/props/" + std::to_string(i) + "/model",
                              "\"" + file + "\" can't be read: " + text.error().message});
            continue;
        }
        if (auto model = load_model(text.value(), file, read); !model) {
            errors.push_back({"/props/" + std::to_string(i) + "/model",
                              "\"" + file + "\" doesn't load: " + model.error().message});
        }
    }
    return errors;
}

Silhouette silhouette(const Model& model, bool side, int size) {
    Silhouette out;
    out.size = std::max(size, 1);
    out.cells.assign(static_cast<std::size_t>(out.size) * static_cast<std::size_t>(out.size), 0);
    const real_t cell = static_cast<real_t>(out.size) / kSilhouetteMeters;
    const auto project = [&](Vec3 p) {
        const real_t across = side ? -p.z : p.x;
        return Vec2{(across + (kSilhouetteMeters * 0.5F)) * cell, (kSilhouetteMeters - p.y) * cell};
    };
    for (const ModelPart& part : model.parts) {
        // A skinned mesh is where its joints put it, which at rest is where its vertices are.
        const Affine3D place = part.skin >= 0 ? Affine3D{} : part.transform;
        for (const ModelSurface& surface : model.meshes.at(part.mesh).surfaces) {
            for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
                std::array<Vec2, 3> corners;
                for (std::size_t c = 0; c < 3; ++c) {
                    const render::Vertex3D& v = surface.vertices.at(surface.indices[i + c]);
                    corners[c] =
                        project(place.apply({v.position[0], v.position[1], v.position[2]}));
                }
                fill_triangle(out, corners);
            }
        }
    }
    return out;
}

float silhouette_difference(const Silhouette& a, const Silhouette& b) {
    if (a.cells.size() != b.cells.size()) {
        return 1.0F;
    }
    std::size_t both = 0;
    std::size_t either = 0;
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        both += (a.cells[i] != 0 && b.cells[i] != 0) ? 1U : 0U;
        either += (a.cells[i] != 0 || b.cells[i] != 0) ? 1U : 0U;
    }
    if (either == 0) {
        return 0.0F;
    }
    return 1.0F - (static_cast<float>(both) / static_cast<float>(either));
}

} // namespace slash::scene3d
