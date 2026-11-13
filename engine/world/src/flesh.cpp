// A character's anatomy: its body's parts' frames and the bones and muscles placed on them
// (slash/world/flesh.hpp).
#include "slash/world/flesh.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slash::world {

namespace {

constexpr real_t kPi = std::numbers::pi_v<real_t>;

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const real_t l = length(v);
    return l > 0.0F ? v * (1.0F / l) : Vec3{};
}

Vec3 lerp(Vec3 a, Vec3 b, real_t t) {
    return a + ((b - a) * t);
}

// v made square to axis, or fallback (made square too) where v lies along it.
Vec3 square_to(Vec3 v, Vec3 axis, Vec3 fallback) {
    const Vec3 a = normalized(axis);
    Vec3 out = v - (a * dot(v, a));
    if (length(out) < 0.0001F) {
        out = fallback - (a * dot(fallback, a));
    }
    return normalized(out);
}

PartFrame frame(Vec3 head, Vec3 tail, Vec3 front, Vec3 out, real_t depth, real_t width,
                std::string bone) {
    const Vec3 axis = tail - head;
    PartFrame f;
    f.head = head;
    f.tail = tail;
    f.front = square_to(front, axis, {0.0F, 1.0F, 0.0F});
    f.out = square_to(out - (f.front * dot(out, f.front)), axis, cross(axis, f.front));
    f.depth = depth;
    f.width = width;
    f.bone = std::move(bone);
    return f;
}

constexpr std::array<std::string_view, kBodyParts> kPartNames{
    "head",      "jaw",       "neck", "chest",     "abdomen",   "pelvis", "clavicle",
    "upper_arm", "lower_arm", "hand", "upper_leg", "lower_leg", "foot",
};

std::size_t index_of(BodyPart part) {
    return static_cast<std::size_t>(part);
}

// ---- Muscles ----

// Where a muscle's end is on a part: a share along it (0 at its head, 1 at its tail; past them
// beyond), then out to its front by depths and out from the body's middle by widths.
struct End {
    BodyPart part;
    float t;
    float f;
    float o;
};

struct MuscleSpec {
    std::string_view kind;
    std::string_view group;
    std::string_view action;
    End origin;
    End insertion;
    BodyPart rests_on;
    float size; // its belly's thickness, a share of the larger of its part's depth and width
    float wide; // its belly's width over its thickness
};

using P = BodyPart;
constexpr std::array<MuscleSpec, 28> kMuscles{{
    {"deltoid",
     "shoulder",
     "raises the arm out to the side",
     {P::Clavicle, 0.9F, 0.0F, 1.0F},
     {P::UpperArm, 0.45F, 0.0F, 1.0F},
     P::UpperArm,
     0.55F,
     1.6F},
    {"biceps",
     "arm",
     "bends the elbow and turns the palm up",
     {P::UpperArm, 0.05F, 0.6F, 0.1F},
     {P::LowerArm, 0.15F, 0.5F, 0.0F},
     P::UpperArm,
     0.45F,
     1.2F},
    {"triceps",
     "arm",
     "straightens the elbow",
     {P::UpperArm, 0.08F, -0.6F, 0.0F},
     {P::LowerArm, -0.02F, -0.8F, 0.0F},
     P::UpperArm,
     0.5F,
     1.3F},
    {"brachialis",
     "arm",
     "bends the elbow",
     {P::UpperArm, 0.5F, 0.4F, 0.2F},
     {P::LowerArm, 0.1F, 0.3F, 0.0F},
     P::UpperArm,
     0.3F,
     1.3F},
    {"brachioradialis",
     "arm",
     "bends the elbow with the thumb up",
     {P::UpperArm, 0.7F, 0.2F, 0.8F},
     {P::LowerArm, 0.95F, 0.2F, 0.7F},
     P::LowerArm,
     0.35F,
     1.2F},
    {"forearm_flexors",
     "arm",
     "curl the wrist and fingers",
     {P::UpperArm, 0.97F, 0.0F, -0.9F},
     {P::Hand, 0.2F, 0.0F, -0.5F},
     P::LowerArm,
     0.45F,
     1.3F},
    {"forearm_extensors",
     "arm",
     "lift the wrist and open the fingers",
     {P::UpperArm, 0.97F, 0.0F, 0.9F},
     {P::Hand, 0.2F, 0.0F, 0.5F},
     P::LowerArm,
     0.4F,
     1.3F},
    {"pectoralis",
     "chest",
     "pulls the arm across the chest",
     {P::Chest, 0.55F, 1.0F, 0.15F},
     {P::UpperArm, 0.15F, 0.5F, 0.1F},
     P::Chest,
     0.25F,
     2.5F},
    {"latissimus",
     "back",
     "pulls the arm down and back",
     {P::Abdomen, 0.1F, -0.9F, 0.2F},
     {P::UpperArm, 0.15F, 0.2F, -0.4F},
     P::Chest,
     0.18F,
     3.0F},
    {"trapezius",
     "back",
     "lifts and draws back the shoulder",
     {P::Neck, 0.7F, -0.8F, 0.0F},
     {P::Clavicle, 0.85F, -0.3F, 0.6F},
     P::Chest,
     0.2F,
     2.5F},
    {"rectus_abdominis",
     "core",
     "bends the trunk forward",
     {P::Pelvis, 0.6F, 0.95F, 0.15F},
     {P::Chest, 0.35F, 1.0F, 0.2F},
     P::Abdomen,
     0.12F,
     2.0F},
    {"external_oblique",
     "core",
     "twists and bends the trunk",
     {P::Pelvis, 0.95F, 0.3F, 0.85F},
     {P::Chest, 0.4F, 0.4F, 0.85F},
     P::Abdomen,
     0.12F,
     2.5F},
    {"erector_spinae",
     "back",
     "straightens the back",
     {P::Pelvis, 0.4F, -0.9F, 0.2F},
     {P::Chest, 0.9F, -0.9F, 0.15F},
     P::Abdomen,
     0.18F,
     1.3F},
    {"gluteus_maximus",
     "hip",
     "straightens the hip",
     {P::Pelvis, 0.3F, -1.0F, 0.35F},
     {P::UpperLeg, 0.22F, -0.5F, 0.6F},
     P::Pelvis,
     0.35F,
     1.6F},
    {"gluteus_medius",
     "hip",
     "raises the leg out to the side",
     {P::Pelvis, 0.8F, -0.2F, 0.95F},
     {P::UpperLeg, 0.02F, 0.0F, 1.0F},
     P::Pelvis,
     0.25F,
     1.6F},
    {"rectus_femoris",
     "thigh",
     "straightens the knee and lifts the thigh",
     {P::Pelvis, 0.2F, 0.7F, 0.6F},
     {P::LowerLeg, 0.05F, 1.0F, 0.0F},
     P::UpperLeg,
     0.35F,
     1.3F},
    {"vastus_lateralis",
     "thigh",
     "straightens the knee",
     {P::UpperLeg, 0.1F, 0.0F, 0.8F},
     {P::LowerLeg, 0.02F, 0.8F, 0.4F},
     P::UpperLeg,
     0.4F,
     1.3F},
    {"vastus_medialis",
     "thigh",
     "straightens the knee",
     {P::UpperLeg, 0.4F, 0.2F, -0.7F},
     {P::LowerLeg, 0.02F, 0.8F, -0.4F},
     P::UpperLeg,
     0.35F,
     1.3F},
    {"biceps_femoris",
     "thigh",
     "bends the knee",
     {P::Pelvis, 0.0F, -0.6F, 0.4F},
     {P::LowerLeg, 0.08F, -0.5F, 0.8F},
     P::UpperLeg,
     0.35F,
     1.2F},
    {"semitendinosus",
     "thigh",
     "bends the knee",
     {P::Pelvis, 0.0F, -0.6F, 0.25F},
     {P::LowerLeg, 0.1F, -0.3F, -0.7F},
     P::UpperLeg,
     0.3F,
     1.2F},
    {"adductors",
     "thigh",
     "draws the legs together",
     {P::Pelvis, 0.05F, 0.3F, 0.1F},
     {P::UpperLeg, 0.6F, 0.0F, -0.8F},
     P::UpperLeg,
     0.35F,
     1.4F},
    {"gastrocnemius",
     "calf",
     "points the foot and bends the knee",
     {P::UpperLeg, 0.95F, -0.6F, 0.0F},
     {P::Foot, -0.25F, -0.5F, 0.0F},
     P::LowerLeg,
     0.5F,
     1.3F},
    {"soleus",
     "calf",
     "points the foot",
     {P::LowerLeg, 0.2F, -0.4F, 0.0F},
     {P::Foot, -0.25F, -0.5F, 0.0F},
     P::LowerLeg,
     0.4F,
     1.4F},
    {"tibialis_anterior",
     "calf",
     "lifts the foot",
     {P::LowerLeg, 0.15F, 0.7F, 0.3F},
     {P::Foot, 0.4F, 0.3F, -0.4F},
     P::LowerLeg,
     0.3F,
     1.2F},
    {"sternocleidomastoid",
     "neck",
     "turns and bows the head",
     {P::Chest, 0.98F, 0.8F, 0.15F},
     {P::Head, 0.25F, -0.2F, 0.85F},
     P::Neck,
     0.3F,
     1.2F},
    {"masseter",
     "jaw",
     "closes the jaw",
     {P::Head, 0.4F, 0.4F, 0.8F},
     {P::Jaw, 0.6F, -0.2F, 0.85F},
     P::Jaw,
     0.15F,
     1.5F},
    {"temporalis",
     "jaw",
     "closes the jaw and draws it back",
     {P::Head, 0.75F, 0.1F, 0.9F},
     {P::Jaw, 0.1F, 0.6F, 0.5F},
     P::Head,
     0.12F,
     2.0F},
    {"rhomboids",
     "back",
     "draws the shoulder blades together",
     {P::Chest, 0.8F, -1.0F, 0.05F},
     {P::Clavicle, 0.4F, -1.0F, 0.0F},
     P::Chest,
     0.12F,
     2.0F},
}};

// How thick, as a share of the smaller of its depth and width, the bone a part's muscles rest
// on is: limbs' long bones, the trunk's ribs and spine, the skull.
real_t bone_share(BodyPart part) {
    switch (part) {
    case P::Head:
        return 0.85F;
    case P::Chest:
    case P::Abdomen:
    case P::Pelvis:
        return 0.7F;
    case P::Jaw:
        return 0.5F;
    case P::Neck:
    case P::Clavicle:
    case P::Hand:
    case P::Foot:
        return 0.4F;
    default:
        return 0.3F;
    }
}

// The muscle's rings: seven from origin to insertion, the ends its tendons, its belly a
// spindle, pushed out of the bone it rests on.
std::vector<MuscleRing> muscle_rings(Vec3 from, Vec3 to, const PartFrame& rest, real_t thickness,
                                     real_t wide, Vec3 bone_a, Vec3 bone_b, real_t bone_radius) {
    constexpr int kRings = 7;
    std::vector<MuscleRing> rings;
    const Vec3 axis = normalized(to - from);
    const Vec3 middle = lerp(from, to, 0.5F);
    // Its front faces away from the bone under its middle.
    const Vec3 span = bone_b - bone_a;
    const real_t along = std::clamp<real_t>(
        dot(middle - bone_a, span) / std::max<real_t>(dot(span, span), 0.000001F), 0.0F, 1.0F);
    const Vec3 off_bone = middle - lerp(bone_a, bone_b, along);
    const Vec3 u = square_to(off_bone, axis, rest.front);
    const Vec3 v = normalized(cross(axis, u));
    for (int k = 0; k < kRings; ++k) {
        const real_t t = static_cast<real_t>(k) / static_cast<real_t>(kRings - 1);
        const real_t belly = std::max<real_t>(cos_sin(static_cast<float>(kPi * t)).y, 0.0F);
        const real_t profile = 0.25F + (0.75F * std::sqrt(belly));
        MuscleRing ring;
        ring.center = lerp(from, to, t);
        ring.u = u;
        ring.v = v;
        ring.ru = thickness * profile;
        ring.rv = thickness * wide * profile;
        // Out of the bone: its inner side no nearer the bone's axis than the bone's radius.
        if (k > 0 && k < kRings - 1) {
            const real_t at = std::clamp<real_t>(dot(ring.center - bone_a, span) /
                                                     std::max<real_t>(dot(span, span), 0.000001F),
                                                 0.0F, 1.0F);
            const Vec3 nearest = lerp(bone_a, bone_b, at);
            const Vec3 away = ring.center - nearest;
            const real_t inner = dot(away, u) - ring.ru;
            if (inner < bone_radius) {
                ring.center = ring.center + (u * (bone_radius - inner));
            }
        }
        rings.push_back(ring);
    }
    return rings;
}

Vec3 end_point(const PartFrame& f, const End& end, real_t out_sign) {
    return lerp(f.head, f.tail, end.t) + (f.front * (end.f * f.depth)) +
           (f.out * (end.o * out_sign * f.width));
}

// ---- Bones ----

struct BoneMaker {
    const BodyFrames& frames;
    const CharacterMeasures& m;
    Anatomy& out;

    AnatomyBone& add(std::string_view kind, const PartFrame& on, int side) {
        AnatomyBone bone;
        bone.kind = std::string(kind);
        bone.name = side < 0 ? bone.kind
                             : bone.kind + "_" + frames.sides.at(static_cast<std::size_t>(side));
        bone.bone = on.bone;
        bone.side = side;
        out.bones.push_back(std::move(bone));
        return out.bones.back();
    }

    static Vec3 at(const PartFrame& f, real_t t, real_t front, real_t across) {
        return lerp(f.head, f.tail, t) + (f.front * (front * f.depth)) +
               (f.out * (across * f.width));
    }

    static void piece(AnatomyBone& bone, Vec3 a, Vec3 b, real_t radius) {
        bone.pieces.push_back({.a = a, .b = b, .radius = radius});
    }

    // A long bone of a limb: its shaft and its two ends a little thicker.
    void long_bone(std::string_view kind, const PartFrame& f, int side, real_t share,
                   real_t from = 0.03F, real_t to = 0.97F) {
        AnatomyBone& bone = add(kind, f, side);
        const real_t r = share * std::min(f.depth, f.width);
        const Vec3 a = lerp(f.head, f.tail, from);
        const Vec3 b = lerp(f.head, f.tail, to);
        piece(bone, a, b, r);
        piece(bone, a, a, r * 1.6F);
        piece(bone, b, b, r * 1.5F);
    }

    void head() {
        const PartFrame* f = frames.find(P::Head, 0);
        if (f == nullptr) {
            return;
        }
        AnatomyBone& skull = add("skull", *f, -1);
        const real_t r = m.head_radius;
        piece(skull, at(*f, 0.62F, -0.1F, 0.0F), at(*f, 0.62F, -0.1F, 0.0F), r * 0.88F);
        piece(skull, at(*f, 0.4F, 0.35F, 0.0F), at(*f, 0.5F, 0.55F, 0.0F), r * 0.45F);
        for (const real_t o : {-0.75F, 0.75F}) {
            piece(skull, at(*f, 0.42F, 0.45F, o), at(*f, 0.42F, -0.2F, o * 1.1F), r * 0.12F);
        }
        if (const PartFrame* jaw = frames.find(P::Jaw, 0); jaw != nullptr) {
            AnatomyBone& mandible = add("mandible", *jaw, -1);
            for (const real_t o : {-0.85F, 0.85F}) {
                piece(mandible, at(*jaw, 0.05F, -0.2F, o), at(*jaw, 0.7F, -0.1F, o * 0.9F),
                      r * 0.08F);
                piece(mandible, at(*jaw, 0.7F, -0.1F, o * 0.9F), at(*jaw, 1.0F, 0.4F, o * 0.25F),
                      r * 0.08F);
            }
            piece(mandible, at(*jaw, 1.0F, 0.4F, -0.25F), at(*jaw, 1.0F, 0.4F, 0.25F), r * 0.1F);
        }
    }

    void spine() {
        const std::array<std::pair<P, std::string_view>, 3> runs{{
            {P::Neck, "cervical_spine"},
            {P::Chest, "thoracic_spine"},
            {P::Abdomen, "lumbar_spine"},
        }};
        for (const auto& [part, kind] : runs) {
            const PartFrame* f = frames.find(part, 0);
            if (f == nullptr) {
                continue;
            }
            AnatomyBone& bone = add(kind, *f, -1);
            const real_t back = part == P::Neck ? -0.3F : -0.75F;
            const real_t r = 0.18F * std::min(f->depth, f->width);
            constexpr int kVertebrae = 4;
            for (int i = 0; i < kVertebrae; ++i) {
                const real_t t = (static_cast<real_t>(i) + 0.5F) / kVertebrae;
                const Vec3 c = at(*f, t, back, 0.0F);
                piece(bone, c, c, r);
                piece(bone, c, at(*f, t, back - 0.25F, 0.0F), r * 0.45F);
            }
        }
    }

    void ribs() {
        const PartFrame* f = frames.find(P::Chest, 0);
        if (f == nullptr) {
            return;
        }
        AnatomyBone& cage = add("ribcage", *f, -1);
        const real_t r = 0.05F * f->depth;
        constexpr int kRibs = 8;
        constexpr int kArc = 5;
        for (int i = 0; i < kRibs; ++i) {
            const real_t t = 0.05F + (0.85F * static_cast<real_t>(i) / (kRibs - 1));
            const real_t spread = 0.75F + (0.2F * cos_sin(static_cast<float>(kPi * t)).y);
            for (const real_t o : {-1.0F, 1.0F}) {
                Vec3 last;
                for (int j = 0; j <= kArc; ++j) {
                    const Vec2f round =
                        cos_sin(static_cast<float>(kPi * static_cast<real_t>(j) / kArc));
                    // From the spine behind, round the side, to the sternum in front, dropping
                    // toward the front as ribs do.
                    const real_t front = -round.x * 0.85F;
                    const real_t across = round.y * spread * o;
                    const real_t drop = 0.06F * static_cast<real_t>(j) / kArc;
                    const Vec3 p = at(*f, t - drop, front, across);
                    if (j > 0) {
                        piece(cage, last, p, r);
                    }
                    last = p;
                }
            }
        }
        AnatomyBone& sternum = add("sternum", *f, -1);
        piece(sternum, at(*f, 0.15F, 0.88F, 0.0F), at(*f, 0.95F, 0.85F, 0.0F), r * 1.6F);
    }

    void pelvis() {
        const PartFrame* f = frames.find(P::Pelvis, 0);
        if (f == nullptr) {
            return;
        }
        AnatomyBone& bone = add("pelvis", *f, -1);
        const real_t r = 0.12F * std::min(f->depth, f->width);
        for (const real_t o : {-1.0F, 1.0F}) {
            // The iliac crest, then down to the hip socket and round to the pubis.
            piece(bone, at(*f, 0.85F, -0.5F, 0.3F * o), at(*f, 0.95F, 0.1F, 0.85F * o), r);
            piece(bone, at(*f, 0.95F, 0.1F, 0.85F * o), at(*f, 0.1F, 0.1F, 0.6F * o), r);
            piece(bone, at(*f, 0.1F, 0.1F, 0.6F * o), at(*f, -0.1F, 0.7F, 0.1F * o), r * 0.8F);
            piece(bone, at(*f, 0.1F, 0.1F, 0.6F * o), at(*f, 0.0F, 0.1F, 0.6F * o), r * 1.6F);
        }
        AnatomyBone& sacrum = add("sacrum", *f, -1);
        piece(sacrum, at(*f, 0.75F, -0.6F, 0.0F), at(*f, 0.1F, -0.7F, 0.0F), r * 1.5F);
    }

    void limbs() {
        for (const int side : {0, 1}) {
            if (const PartFrame* f = frames.find(P::Clavicle, side); f != nullptr) {
                long_bone("clavicle", *f, side, 0.25F, 0.05F, 0.95F);
                AnatomyBone& scapula = add("scapula", *f, side);
                const real_t r = 0.2F * std::min(f->depth, f->width);
                const Vec3 top = at(*f, 0.9F, -1.4F, 0.0F);
                const Vec3 inner = at(*f, 0.15F, -2.0F, -1.5F);
                const Vec3 low = at(*f, 0.45F, -2.2F, -5.0F);
                piece(scapula, top, inner, r);
                piece(scapula, inner, low, r);
                piece(scapula, low, top, r);
            }
            if (const PartFrame* f = frames.find(P::UpperArm, side); f != nullptr) {
                long_bone("humerus", *f, side, 0.3F);
            }
            if (const PartFrame* f = frames.find(P::LowerArm, side); f != nullptr) {
                // Each bone's pieces go on before the next is added: adding one can move the
                // others, so a reference to one is good only until then.
                const real_t r = 0.2F * std::min(f->depth, f->width);
                AnatomyBone& radius = add("radius", *f, side);
                piece(radius, at(*f, 0.04F, 0.3F, 0.35F), at(*f, 0.96F, 0.3F, 0.5F), r);
                piece(radius, at(*f, 0.96F, 0.3F, 0.5F), at(*f, 0.96F, 0.3F, 0.5F), r * 1.6F);
                AnatomyBone& ulna = add("ulna", *f, side);
                piece(ulna, at(*f, -0.02F, -0.4F, -0.3F), at(*f, 0.96F, -0.2F, -0.45F), r);
                piece(ulna, at(*f, -0.02F, -0.4F, -0.3F), at(*f, -0.02F, -0.4F, -0.3F), r * 1.6F);
            }
            if (const PartFrame* f = frames.find(P::Hand, side); f != nullptr) {
                AnatomyBone& hand = add("hand", *f, side);
                const real_t r = 0.15F * std::max(f->depth, f->width);
                piece(hand, at(*f, 0.08F, 0.0F, -0.5F), at(*f, 0.08F, 0.0F, 0.5F), r * 1.4F);
                for (int i = 0; i < 4; ++i) {
                    const real_t o = -0.7F + (0.47F * static_cast<real_t>(i));
                    piece(hand, at(*f, 0.15F, 0.0F, o), at(*f, 0.95F, 0.0F, o * 1.1F), r * 0.6F);
                }
            }
            if (const PartFrame* f = frames.find(P::UpperLeg, side); f != nullptr) {
                long_bone("femur", *f, side, 0.3F);
                AnatomyBone& patella = add("patella", *f, side);
                const Vec3 knee = at(*f, 0.97F, 0.95F, 0.0F);
                piece(patella, knee, knee, 0.3F * std::min(f->depth, f->width));
            }
            if (const PartFrame* f = frames.find(P::LowerLeg, side); f != nullptr) {
                const real_t r = 0.3F * std::min(f->depth, f->width);
                AnatomyBone& tibia = add("tibia", *f, side);
                piece(tibia, at(*f, 0.03F, 0.25F, -0.1F), at(*f, 0.97F, 0.15F, -0.15F), r);
                piece(tibia, at(*f, 0.03F, 0.25F, -0.1F), at(*f, 0.03F, 0.25F, -0.1F), r * 1.7F);
                AnatomyBone& fibula = add("fibula", *f, side);
                piece(fibula, at(*f, 0.08F, -0.2F, 0.6F), at(*f, 1.0F, -0.1F, 0.65F), r * 0.45F);
            }
            if (const PartFrame* f = frames.find(P::Foot, side); f != nullptr) {
                AnatomyBone& foot = add("foot", *f, side);
                const real_t r = 0.25F * std::max(f->depth, f->width);
                piece(foot, at(*f, -0.3F, -0.2F, 0.0F), at(*f, 0.05F, 0.3F, 0.0F), r * 1.1F);
                for (int i = 0; i < 5; ++i) {
                    const real_t o = -0.8F + (0.4F * static_cast<real_t>(i));
                    piece(foot, at(*f, 0.15F, 0.2F, o * 0.6F), at(*f, 1.1F, -0.4F, o), r * 0.4F);
                }
            }
        }
    }
};

} // namespace

bool is_sided(BodyPart part) {
    switch (part) {
    case P::Clavicle:
    case P::UpperArm:
    case P::LowerArm:
    case P::Hand:
    case P::UpperLeg:
    case P::LowerLeg:
    case P::Foot:
        return true;
    default:
        return false;
    }
}

std::string_view body_part_name(BodyPart part) {
    return kPartNames.at(index_of(part));
}

const PartFrame* BodyFrames::find(BodyPart part, int side) const {
    const auto& pair = parts.at(index_of(part));
    const std::size_t s = is_sided(part) && side == 1 ? 1 : 0;
    const std::optional<PartFrame>& frame = pair.at(s);
    if (!frame.has_value()) {
        return nullptr;
    }
    return &frame.value();
}

BodyFrames body_frames_3d(const CharacterMeasures& m) {
    BodyFrames out;
    out.dimensions = 3;
    out.sides = {"left", "right"};
    const Vec3 front{0.0F, 0.0F, 1.0F};
    const Vec3 left{1.0F, 0.0F, 0.0F};
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const auto set = [&](BodyPart part, int side, PartFrame f) {
        out.parts.at(index_of(part)).at(static_cast<std::size_t>(side)) = std::move(f);
    };
    const real_t hr = m.head_radius;
    const real_t hh = m.head_height;
    const real_t base = m.head_base;
    set(P::Pelvis, 0,
        frame({0.0F, m.hip, 0.0F}, {0.0F, m.waist, 0.0F}, front, left, m.hip_depth,
              m.hip_half_width, "Hips"));
    set(P::Abdomen, 0,
        frame({0.0F, m.waist, 0.0F}, {0.0F, m.chest, 0.0F}, front, left,
              (m.hip_depth + m.chest_depth) * 0.5F, m.waist_half_width, "Spine"));
    set(P::Chest, 0,
        frame({0.0F, m.chest, 0.0F}, {0.0F, m.shoulder, 0.0F}, front, left, m.chest_depth,
              m.chest_half_width, "UpperChest"));
    set(P::Neck, 0,
        frame({0.0F, m.shoulder, 0.0F}, {0.0F, base, 0.0F}, front, left, m.neck_radius,
              m.neck_radius, "Neck"));
    set(P::Head, 0,
        frame({0.0F, base, 0.0F}, {0.0F, m.top, 0.0F}, front, left, hr, hr * 0.9F, "Head"));
    set(P::Jaw, 0,
        frame({0.0F, base + (hh * 0.42F), hr * 0.1F}, {0.0F, base + (hh * 0.12F), hr * 0.85F},
              front, left, hr * 0.3F, hr * 0.75F, "Jaw"));
    // Arms hang 12 degrees out from straight down, as the skeleton's.
    const Vec2f hang = cos_sin(0.2094395F);
    const real_t c = hang.x;
    const real_t s = hang.y;
    for (const int side : {0, 1}) {
        const real_t sign = side == 0 ? 1.0F : -1.0F;
        const std::string name = side == 0 ? "Left" : "Right";
        const Vec3 out_way{sign, 0.0F, 0.0F};
        const Vec3 down{sign * s, -c, 0.0F};
        const Vec3 upper{sign * m.shoulder_half_width, m.shoulder - (m.arm_radius * 0.5F), 0.0F};
        const Vec3 lower = upper + (down * m.upper_arm);
        const Vec3 wrist = lower + (down * m.lower_arm);
        set(P::Clavicle, side,
            frame({sign * m.neck_radius, m.shoulder, 0.0F}, upper, front, up, m.arm_radius * 0.5F,
                  m.arm_radius * 0.5F, name + "Shoulder"));
        set(P::UpperArm, side,
            frame(upper, lower, front, out_way, m.arm_radius, m.arm_radius, name + "UpperArm"));
        set(P::LowerArm, side,
            frame(lower, wrist, front, out_way, m.forearm_radius, m.forearm_radius,
                  name + "LowerArm"));
        set(P::Hand, side,
            frame(wrist, wrist + (down * m.hand), front, out_way, m.forearm_radius * 0.5F,
                  m.forearm_radius * 1.1F, name + "Hand"));
        const Vec3 hip{sign * m.leg_offset, m.hip, 0.0F};
        const Vec3 knee{sign * m.leg_offset, m.knee, 0.0F};
        const Vec3 ankle{sign * m.leg_offset, m.ankle, 0.0F};
        const Vec3 toes{sign * m.leg_offset, 0.0F, m.foot_length * 0.6F};
        set(P::UpperLeg, side,
            frame(hip, knee, front, out_way, m.thigh_radius, m.thigh_radius, name + "UpperLeg"));
        set(P::LowerLeg, side,
            frame(knee, ankle, front, out_way, m.calf_radius, m.calf_radius, name + "LowerLeg"));
        set(P::Foot, side,
            frame(ankle, toes, up, out_way, m.ankle * 0.5F, m.calf_radius, name + "Foot"));
    }
    return out;
}

BodyFrames body_frames_2d(const CharacterMeasures& m) {
    // The 3D frames seen from the side: the body faces +x with its right side toward whoever
    // looks (+z), so a 3D point (x, y, z) is (z, y, -x). Limbs hang straight down from where
    // the cut-out's bones are, as they do in it.
    const BodyFrames from = body_frames_3d(m);
    const auto turn = [](Vec3 v) { return Vec3{v.z, v.y, -v.x}; };
    BodyFrames out;
    out.dimensions = 2;
    out.sides = {"back", "front"};
    const real_t s = m.scale;
    const real_t arm_top = m.shoulder - (0.025F * s);
    const std::array<std::pair<P, std::array<real_t, 2>>, 6> limbs{{
        {P::UpperArm, {arm_top, arm_top - m.upper_arm}},
        {P::LowerArm, {arm_top - m.upper_arm, arm_top - m.upper_arm - m.lower_arm}},
        {P::Hand,
         {arm_top - m.upper_arm - m.lower_arm, arm_top - m.upper_arm - m.lower_arm - m.hand}},
        {P::UpperLeg, {m.hip, m.knee}},
        {P::LowerLeg, {m.knee, m.ankle}},
        {P::Foot, {m.ankle, 0.0F}},
    }};
    const auto cutout_bone = [](BodyPart part, int side) -> std::string {
        const std::string tag = side == 0 ? "_back" : "_front";
        switch (part) {
        case P::Pelvis:
            return "pelvis";
        case P::Abdomen:
        case P::Chest:
        case P::Clavicle:
            return "chest";
        case P::Neck:
        case P::Head:
            return "head";
        case P::Jaw:
            return "jaw";
        case P::UpperArm:
            return "upper_arm" + tag;
        case P::LowerArm:
            return "lower_arm" + tag;
        case P::Hand:
            return "hand" + tag;
        case P::UpperLeg:
            return "upper_leg" + tag;
        case P::LowerLeg:
            return "lower_leg" + tag;
        case P::Foot:
            return "foot" + tag;
        }
        return "chest";
    };
    for (std::size_t p = 0; p < kBodyParts; ++p) {
        for (std::size_t side = 0; side < 2; ++side) {
            const std::optional<PartFrame>& f = from.parts.at(p).at(side);
            if (!f.has_value()) {
                continue;
            }
            const auto part = static_cast<BodyPart>(p);
            Vec3 head = turn(f->head);
            Vec3 tail = turn(f->tail);
            for (const auto& [limb, heights] : limbs) {
                if (limb != part) {
                    continue;
                }
                if (part == P::Foot) {
                    head = {0.0F, heights[0], head.z};
                    tail = {m.foot_length * 0.6F, heights[1], tail.z};
                } else {
                    head = {0.0F, heights[0], head.z};
                    tail = {0.0F, heights[1], tail.z};
                }
            }
            out.parts.at(p).at(side) = frame(head, tail, turn(f->front), turn(f->out), f->depth,
                                             f->width, cutout_bone(part, static_cast<int>(side)));
        }
    }
    return out;
}

const std::vector<std::string_view>& muscle_kinds() {
    static const std::vector<std::string_view> kinds = [] {
        std::vector<std::string_view> out;
        out.reserve(kMuscles.size());
        for (const MuscleSpec& spec : kMuscles) {
            out.push_back(spec.kind);
        }
        return out;
    }();
    return kinds;
}

const std::vector<std::string_view>& anatomy_bone_kinds() {
    static const std::vector<std::string_view> kinds{
        "skull",   "mandible", "cervical_spine", "thoracic_spine", "lumbar_spine",
        "ribcage", "sternum",  "pelvis",         "sacrum",         "clavicle",
        "scapula", "humerus",  "radius",         "ulna",           "hand",
        "femur",   "patella",  "tibia",          "fibula",         "foot",
    };
    return kinds;
}

Anatomy make_anatomy(const CharacterRecipe& recipe, const BodyFrames& frames) {
    const CharacterMeasures m = character_measures(recipe);
    Anatomy out;
    out.dimensions = frames.dimensions;
    out.sides = frames.sides;
    BoneMaker bones{frames, m, out};
    bones.head();
    bones.spine();
    bones.ribs();
    bones.pelvis();
    bones.limbs();

    const real_t muscle = 0.7F + (0.6F * std::clamp(recipe.body.muscle, 0.0F, 1.0F));
    for (const MuscleSpec& spec : kMuscles) {
        const bool middle = !is_sided(spec.origin.part) && !is_sided(spec.insertion.part);
        for (const int side : {0, 1}) {
            // A side view shows the near one of a pair in the middle of the body.
            if (frames.dimensions == 2 && middle && side == 0) {
                continue;
            }
            const PartFrame* origin = frames.find(spec.origin.part, side);
            const PartFrame* insertion = frames.find(spec.insertion.part, side);
            const PartFrame* rest = frames.find(spec.rests_on, side);
            if (origin == nullptr || insertion == nullptr || rest == nullptr) {
                continue;
            }
            // A part in the middle's out is the body's left: a right muscle's is the other way.
            const auto sign_on = [&](BodyPart part) {
                return !is_sided(part) && side == 1 && frames.dimensions == 3 ? -1.0F : 1.0F;
            };
            AnatomyMuscle mu;
            mu.kind = std::string(spec.kind);
            mu.name = mu.kind + "_" + frames.sides.at(static_cast<std::size_t>(side));
            mu.group = std::string(spec.group);
            mu.action = std::string(spec.action);
            mu.side = side;
            const auto ref = [&](BodyPart part) {
                return PartSide{.part = part, .side = is_sided(part) ? side : -1};
            };
            mu.origin = ref(spec.origin.part);
            mu.insertion = ref(spec.insertion.part);
            mu.rests_on = ref(spec.rests_on);
            mu.origin_bone = origin->bone;
            mu.insertion_bone = insertion->bone;
            mu.rest_bone = rest->bone;
            mu.bone_a = rest->head;
            mu.bone_b = rest->tail;
            mu.bone_radius = bone_share(spec.rests_on) * std::min(rest->depth, rest->width);
            const Vec3 from = end_point(*origin, spec.origin, sign_on(spec.origin.part));
            const Vec3 to = end_point(*insertion, spec.insertion, sign_on(spec.insertion.part));
            const real_t thickness = spec.size * muscle * std::max(rest->depth, rest->width);
            mu.rings = muscle_rings(from, to, *rest, thickness, spec.wide, mu.bone_a, mu.bone_b,
                                    mu.bone_radius);
            out.muscles.push_back(std::move(mu));
        }
    }
    return out;
}

} // namespace slash::world
