#include "slash/scene3d/creature.hpp"

#include "rig_builder.hpp"

#include "slash/core/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Creatures are built of Godot's primitive meshes placed over creature_measures, as characters
// are, so the same recipe gives the same model, bit for bit, everywhere: the library builds with
// -ffp-contract=off and angles go through slash::cos_sin.
namespace slash::scene3d {
namespace {

using world::CreatureBone;
using world::CreatureMeasures;
using world::CreatureRecipe;
using namespace rig; // NOLINT(google-build-using-namespace)

// A cone radius wide at its base, length long along +y from its base.
MeshData cone() {
    world::CylinderMesh cylinder;
    cylinder.top_radius = 0.0F;
    cylinder.bottom_radius = 1.0F;
    cylinder.height = 1.0F;
    cylinder.radial_segments = 10;
    cylinder.rings = 1;
    return cylinder_mesh(cylinder);
}

Quat turn_about(Vec3 axis, float angle) {
    const auto [c, s] = cos_sin(angle * 0.5F);
    return {axis.x * s, axis.y * s, axis.z * s, c};
}

Quat compose(Quat a, Quat b) {
    return {(a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
            (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
            (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w),
            (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z)};
}

// The shapes of a creature over its skeleton, each moved by its bone.
struct Shaper {
    const CreatureRecipe& recipe;
    const CreatureMeasures& m;
    const std::vector<CreatureBone>& bones;
    Builder& out;

    [[nodiscard]] std::size_t joint(std::string_view name) const {
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].name == name) {
                return i;
            }
        }
        return 0;
    }

    [[nodiscard]] Vec3 head(std::string_view name) const { return bones[joint(name)].head; }

    [[nodiscard]] bool has(std::string_view name) const {
        return std::ranges::any_of(bones, [&](const CreatureBone& b) { return b.name == name; });
    }

    // An ellipsoid of radii r about at.
    void blob(Vec3 r, Vec3 at, int material, std::string_view bone) const {
        out.add(unit_sphere(), placed(r, at), material, joint(bone));
    }

    // A rounded bar radius wide from a to b.
    void bar(Vec3 a, Vec3 b, float radius, int material, std::string_view bone) const {
        const real_t l = length(b - a);
        if (l <= 0.0F) {
            return;
        }
        out.add(capsule(radius, static_cast<float>(l)),
                placed(facing(b - a), {1.0F, 1.0F, 1.0F}, lerp(a, b, 0.5F)), material, joint(bone));
    }

    // A cone radius wide at base, reaching to tip.
    void spike(Vec3 base, Vec3 tip, real_t radius, int material, std::string_view bone) const {
        const real_t l = length(tip - base);
        if (l <= 0.0F) {
            return;
        }
        out.add(cone(), placed(facing(tip - base), {radius, l, radius}, lerp(base, tip, 0.5F)),
                material, joint(bone));
    }

    // The pattern's marks over a body ellipsoid of radii r about center.
    void pattern(Vec3 r, Vec3 center, int material, std::string_view bone) const {
        const std::string& kind = recipe.look.pattern;
        if (kind == "stripes") {
            for (int i = 0; i < 5; ++i) {
                const real_t z = center.z + (r.z * (-0.6F + (0.3F * static_cast<float>(i))));
                blob({r.x * 1.02F, r.y * 1.02F, r.z * 0.05F}, {center.x, center.y, z}, material,
                     bone);
            }
        } else if (kind == "spots") {
            constexpr std::array<std::array<float, 3>, 6> kSpots{{{-0.55F, 0.35F, 0.16F},
                                                                  {-0.2F, 0.55F, 0.13F},
                                                                  {0.15F, 0.3F, 0.15F},
                                                                  {0.45F, 0.5F, 0.12F},
                                                                  {-0.35F, -0.05F, 0.12F},
                                                                  {0.3F, 0.0F, 0.11F}}};
            for (const auto& spot : kSpots) {
                const real_t dz = spot[0];
                const real_t dy = spot[1];
                const real_t across =
                    r.x * std::sqrt(std::max<real_t>(0.0F, 1.0F - (dz * dz) - (dy * dy)));
                const real_t size = r.z * spot[2];
                for (const real_t side : {1.0F, -1.0F}) {
                    blob({size * 0.35F, size, size},
                         {side * across, center.y + (r.y * dy), center.z + (r.z * dz)}, material,
                         bone);
                }
            }
        } else if (kind == "patches") {
            for (const real_t side : {1.0F, -1.0F}) {
                blob({r.x * 0.45F, r.y * 0.5F, r.z * 0.35F},
                     {side * r.x * 0.65F, center.y + (r.y * 0.25F), center.z - (r.z * 0.35F)},
                     material, bone);
                blob({r.x * 0.4F, r.y * 0.4F, r.z * 0.25F},
                     {side * r.x * 0.7F, center.y + (r.y * 0.1F), center.z + (r.z * 0.4F)},
                     material, bone);
            }
        }
    }
};

struct Materials {
    int coat = 0, belly = 0, markings = 0, eyes = 0;
};

void build_quadruped(const Shaper& s, const Materials& mat) {
    const CreatureMeasures& m = s.m;
    const CreatureRecipe& recipe = s.recipe;
    const Vec3 middle{0.0F, m.back, m.body_z};
    const Vec3 body{m.body_radius, m.body_depth, m.body_half};
    s.blob(body, middle, mat.coat, "body");
    s.blob({m.body_radius * 0.9F, m.body_depth * 0.55F, m.body_half * 0.85F},
           middle - Vec3{0.0F, m.body_depth * 0.45F, 0.0F}, mat.belly, "body");
    s.pattern(body, middle, mat.markings, "body");
    const real_t hr = m.head_radius;
    const Vec3 neck = s.head("neck");
    const Vec3 head = s.head("head");
    s.bar(neck, head, static_cast<float>(hr * 0.62F), mat.coat, "neck");
    if (recipe.parts.mane) {
        const Vec3 lift{0.0F, hr * 0.45F, -hr * 0.25F};
        s.bar(neck + lift, head + lift, static_cast<float>(hr * 0.3F), mat.markings, "neck");
    }
    s.blob({hr * 0.85F, hr * 0.85F, hr}, head, mat.coat, "head");
    s.blob({m.snout * 0.6F, hr * 0.5F, m.snout}, head + Vec3{0.0F, -hr * 0.25F, hr * 0.8F},
           mat.coat, "head");
    s.blob({hr * 0.18F, hr * 0.14F, hr * 0.12F},
           head + Vec3{0.0F, -hr * 0.15F, (hr * 0.8F) + (m.snout * 0.95F)}, mat.markings, "head");
    const std::string& ears = recipe.parts.ears;
    const std::string& horns = recipe.parts.horns;
    const int horn = s.out.material("horn", {0.88F, 0.84F, 0.72F, 1.0F}, 0.5F, 0.0F);
    for (const real_t side : {1.0F, -1.0F}) {
        s.blob({hr * 0.15F, hr * 0.15F, hr * 0.15F},
               head + Vec3{side * hr * 0.62F, hr * 0.25F, hr * 0.45F}, mat.eyes, "head");
        const Vec3 ear = head + Vec3{side * hr * 0.5F, hr * 0.6F, -hr * 0.2F};
        if (ears == "short") {
            s.spike(ear, ear + Vec3{side * hr * 0.15F, hr * 0.55F, -hr * 0.1F}, hr * 0.22F,
                    mat.coat, "head");
        } else if (ears == "pointed") {
            s.spike(ear, ear + Vec3{side * hr * 0.1F, hr * 0.95F, 0.0F}, hr * 0.25F, mat.coat,
                    "head");
        } else if (ears == "long") {
            s.blob({hr * 0.12F, hr * 0.8F, hr * 0.22F},
                   ear + Vec3{side * hr * 0.1F, hr * 0.6F, -hr * 0.1F}, mat.coat, "head");
        }
        const Vec3 root = head + Vec3{side * hr * 0.35F, hr * 0.7F, -hr * 0.1F};
        if (horns == "horns") {
            s.spike(root, root + Vec3{side * hr * 0.35F, hr * 1.0F, -hr * 0.55F}, hr * 0.13F, horn,
                    "head");
        } else if (horns == "antlers") {
            const int antler = s.out.material("antler", {0.5F, 0.38F, 0.25F, 1.0F}, 0.8F, 0.0F);
            const Vec3 top = root + Vec3{side * hr * 0.9F, hr * 2.2F, -hr * 0.6F};
            s.bar(root, top, static_cast<float>(hr * 0.08F), antler, "head");
            const Vec3 fork = lerp(root, top, 0.45F);
            s.bar(fork, fork + Vec3{side * hr * 0.2F, hr * 0.8F, hr * 0.6F},
                  static_cast<float>(hr * 0.06F), antler, "head");
            const Vec3 high = lerp(root, top, 0.75F);
            s.bar(high, high + Vec3{side * hr * 0.1F, hr * 0.7F, hr * 0.5F},
                  static_cast<float>(hr * 0.06F), antler, "head");
        }
    }
    if (m.tail > 0.0F) {
        const Vec3 base = s.head("tail");
        const Vec3 tip = base + Vec3{0.0F, -m.tail * 0.6F, -m.tail * 0.75F};
        s.bar(base, tip, static_cast<float>(m.tail_radius), mat.coat, "tail");
        const real_t tuft = m.tail_radius * 2.2F;
        s.blob({tuft, tuft, tuft * 1.4F}, tip, mat.markings, "tail");
    }
    for (const CreatureBone& bone : s.bones) {
        if (!bone.name.starts_with("leg")) {
            continue;
        }
        const Vec3 foot{bone.head.x, m.leg_radius, bone.head.z};
        s.bar(bone.head, foot, static_cast<float>(m.leg_radius), mat.coat, bone.name);
        s.out.add(disc_cylinder(),
                  placed({m.leg_radius * 1.1F, m.leg_radius * 0.8F, m.leg_radius * 1.1F},
                         {bone.head.x, m.leg_radius * 0.4F, bone.head.z}),
                  mat.markings, s.joint(bone.name));
    }
}

// Wings at rest are spread along x; animations fold them back along the body.
void build_wings(const Shaper& s, int material, real_t thick, real_t chord) {
    for (const char* wing : {"wing_near", "wing_far"}) {
        if (!s.has(wing)) {
            continue;
        }
        const real_t side = std::string_view(wing) == "wing_near" ? 1.0F : -1.0F;
        s.blob({s.m.wing * 0.5F, thick, chord},
               s.head(wing) + Vec3{side * s.m.wing * 0.5F, 0.0F, -chord * 0.4F}, material, wing);
    }
}

void build_bird(const Shaper& s, const Materials& mat) {
    const CreatureMeasures& m = s.m;
    const Vec3 middle{0.0F, m.back, m.body_z};
    const Vec3 body{m.body_radius, m.body_depth, m.body_half};
    s.blob(body, middle, mat.coat, "body");
    s.blob({m.body_radius * 0.85F, m.body_depth * 0.6F, m.body_half * 0.8F},
           middle - Vec3{0.0F, m.body_depth * 0.35F, 0.0F}, mat.belly, "body");
    s.pattern(body, middle, mat.markings, "body");
    const real_t hr = m.head_radius;
    const Vec3 neck = s.head("neck");
    const Vec3 head = s.head("head");
    s.bar(neck, head, static_cast<float>(hr * 0.6F), mat.coat, "neck");
    s.blob({hr * 0.9F, hr * 0.92F, hr}, head, mat.coat, "head");
    const int beak = s.out.material("beak", {0.92F, 0.68F, 0.2F, 1.0F}, 0.5F, 0.0F);
    const Vec3 base = head + Vec3{0.0F, -hr * 0.05F, hr * 0.75F};
    s.spike(base, base + Vec3{0.0F, -hr * 0.1F, m.snout}, hr * 0.3F, beak, "head");
    for (const real_t side : {1.0F, -1.0F}) {
        s.blob({hr * 0.17F, hr * 0.17F, hr * 0.17F},
               head + Vec3{side * hr * 0.7F, hr * 0.25F, hr * 0.35F}, mat.eyes, "head");
    }
    const Vec3 tail = s.head("tail");
    s.blob({m.body_radius * 0.6F, m.body_depth * 0.12F, m.tail * 0.5F},
           tail + Vec3{0.0F, 0.0F, -m.tail * 0.5F}, mat.coat, "tail");
    for (const char* leg : {"leg_near", "leg_far"}) {
        const Vec3 hip = s.head(leg);
        const Vec3 foot{hip.x, m.leg_radius, hip.z};
        s.bar(hip, foot, static_cast<float>(m.leg_radius), beak, leg);
        s.blob({m.leg_radius * 2.0F, m.leg_radius * 0.6F, m.body_half * 0.25F},
               foot + Vec3{0.0F, 0.0F, m.body_half * 0.1F}, beak, leg);
    }
    build_wings(s, mat.markings, m.body_depth * 0.1F, m.body_half * 0.6F);
}

void build_fish(const Shaper& s, const Materials& mat) {
    const CreatureMeasures& m = s.m;
    const Vec3 middle{0.0F, m.back, m.body_z};
    const Vec3 body{m.body_radius, m.body_depth, m.body_half};
    s.blob(body, middle, mat.coat, "body");
    s.blob({m.body_radius * 0.9F, m.body_depth * 0.5F, m.body_half * 0.85F},
           middle - Vec3{0.0F, m.body_depth * 0.45F, 0.0F}, mat.belly, "body");
    s.pattern(body, middle, mat.markings, "body");
    const real_t fin = std::max<real_t>(m.body_radius * 0.08F, 0.002F);
    s.blob({fin, m.body_depth * 0.45F, m.body_half * 0.3F},
           middle + Vec3{0.0F, m.body_depth * 0.9F, -m.body_half * 0.15F}, mat.markings, "body");
    for (const real_t side : {1.0F, -1.0F}) {
        s.blob({m.body_radius * 0.5F, fin, m.body_half * 0.18F},
               middle + Vec3{side * m.body_radius, -m.body_depth * 0.3F, m.body_half * 0.3F},
               mat.markings, "body");
        const Vec3 head = s.head("head");
        s.blob({m.head_radius * 0.15F, m.head_radius * 0.15F, m.head_radius * 0.15F},
               head +
                   Vec3{side * m.body_radius * 0.78F, m.body_depth * 0.3F, m.head_radius * 0.25F},
               mat.eyes, "head");
    }
    const Vec3 tail = s.head("tail");
    s.blob({fin * 1.5F, m.tail_radius, m.tail * 0.55F}, tail + Vec3{0.0F, 0.0F, -m.tail * 0.55F},
           mat.markings, "tail");
}

void build_insect(const Shaper& s, const Materials& mat) {
    const CreatureMeasures& m = s.m;
    const Vec3 middle{0.0F, m.back, m.body_z};
    s.blob({m.body_radius, m.body_depth, m.body_half * 1.1F}, middle, mat.coat, "body");
    const Vec3 tail = s.head("tail");
    const real_t abdomen = m.tail * 0.55F;
    const Vec3 abdomen_middle = tail + Vec3{0.0F, 0.0F, -abdomen};
    const Vec3 abdomen_radii{m.tail_radius * 0.9F, m.tail_radius, abdomen};
    s.blob(abdomen_radii, abdomen_middle, mat.coat, "tail");
    s.pattern(abdomen_radii, abdomen_middle, mat.markings, "tail");
    const real_t hr = m.head_radius;
    const Vec3 head = s.head("head") + Vec3{0.0F, 0.0F, hr * 0.6F};
    s.blob({hr, hr * 0.85F, hr}, head, mat.coat, "head");
    for (const real_t side : {1.0F, -1.0F}) {
        s.blob({hr * 0.4F, hr * 0.45F, hr * 0.4F},
               head + Vec3{side * hr * 0.6F, hr * 0.2F, hr * 0.4F}, mat.eyes, "head");
        if (s.recipe.parts.antennae) {
            const Vec3 root = head + Vec3{side * hr * 0.3F, hr * 0.7F, hr * 0.4F};
            s.bar(root, root + Vec3{side * hr * 0.8F, hr * 1.8F, hr * 1.6F},
                  static_cast<float>(hr * 0.06F), mat.markings, "head");
        }
    }
    for (const CreatureBone& bone : s.bones) {
        if (!bone.name.starts_with("leg")) {
            continue;
        }
        const real_t side = bone.name.ends_with("_near") ? 1.0F : -1.0F;
        real_t reach = 0.0F;
        if (bone.name.find("front") != std::string::npos) {
            reach = m.leg * 0.6F;
        } else if (bone.name.find("back") != std::string::npos) {
            reach = -m.leg * 0.6F;
        }
        const Vec3 knee = bone.head + Vec3{side * m.leg * 0.45F, m.leg * 0.35F, reach * 0.5F};
        const Vec3 foot{bone.head.x + (side * m.leg * 0.8F), m.leg_radius, bone.head.z + reach};
        s.bar(bone.head, knee, static_cast<float>(m.leg_radius), mat.markings, bone.name);
        s.bar(knee, foot, static_cast<float>(m.leg_radius * 0.8F), mat.markings, bone.name);
    }
    if (m.wing > 0.0F) {
        const int wings = s.out.material("wings", {0.85F, 0.9F, 0.95F, 0.45F}, 0.2F, 0.0F, true);
        s.out.model.materials[static_cast<std::size_t>(wings)].standard.transparency = "alpha";
        build_wings(s, wings, std::max<real_t>(m.wing * 0.01F, 0.0005F), m.wing * 0.18F);
    }
}

// A recipe's animations as node tracks: each bone's turn, a key every eighth of a cycle.
void add_animations(const CreatureRecipe& recipe, const std::vector<CreatureBone>& bones,
                    Model& model) {
    constexpr int kKeys = 8;
    for (const world::CreatureAnimation& animation : world::creature_animations(recipe)) {
        ModelAnimation made;
        made.name = animation.name;
        made.length = animation.length;
        made.loop = animation.loop;
        const int keys = animation.loop ? kKeys : kKeys + 1;
        std::vector<std::vector<std::array<float, 4>>> values(bones.size());
        std::vector<bool> moves(bones.size(), false);
        std::vector<float> times;
        for (int k = 0; k < keys; ++k) {
            const double time = animation.length * static_cast<double>(k) / kKeys;
            times.push_back(static_cast<float>(time));
            std::vector<Quat> turns(bones.size());
            for (const world::CreatureTurn& turn :
                 world::creature_pose(recipe, animation.name, time)) {
                for (std::size_t b = 0; b < bones.size(); ++b) {
                    if (bones[b].name == turn.bone) {
                        turns[b] = compose(turns[b], turn_about(turn.axis, turn.angle));
                        moves[b] = true;
                    }
                }
            }
            for (std::size_t b = 0; b < bones.size(); ++b) {
                values[b].push_back({static_cast<float>(turns[b].x), static_cast<float>(turns[b].y),
                                     static_cast<float>(turns[b].z),
                                     static_cast<float>(turns[b].w)});
            }
        }
        for (std::size_t b = 0; b < bones.size(); ++b) {
            if (!moves[b]) {
                continue;
            }
            NodeTrack track;
            track.node = b;
            track.property = NodeProperty::Rotation;
            track.times = times;
            track.values = std::move(values[b]);
            made.tracks.push_back(std::move(track));
        }
        model.animations.push_back(std::move(made));
    }
}

} // namespace

Model build_creature(const CreatureRecipe& recipe) {
    const CreatureMeasures m = world::creature_measures(recipe);
    const std::vector<CreatureBone> bones = world::creature_skeleton(recipe);
    Builder out;
    ModelSkin skin;
    skin.name = "Skeleton";
    for (const CreatureBone& bone : bones) {
        ModelNode node;
        node.name = bone.name;
        node.parent = bone.parent;
        node.position = bone.parent < 0
                            ? bone.head
                            : bone.head - bones.at(static_cast<std::size_t>(bone.parent)).head;
        node.file_index = static_cast<int>(out.model.nodes.size());
        skin.joints.push_back(out.model.nodes.size());
        skin.inverse_binds.push_back(moved(bone.head * -1.0F));
        out.model.nodes.push_back(std::move(node));
    }
    out.model.skins.push_back(std::move(skin));
    // Attachment points: a rider's seat and what it carries in its mouth.
    const auto point = [&](std::string name, std::size_t bone, Vec3 at) {
        ModelNode node;
        node.name = std::move(name);
        node.parent = static_cast<int>(bone);
        node.position = at - bones[bone].head;
        node.file_index = static_cast<int>(out.model.nodes.size());
        out.model.nodes.push_back(std::move(node));
    };
    point("saddle", 0, {0.0F, m.back + m.body_depth, m.body_z});
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == "head") {
            point("mouth", i,
                  bones[i].head + Vec3{0.0F, -m.head_radius * 0.3F, m.head_radius + m.snout});
        }
    }

    const world::CreatureLook& look = recipe.look;
    const bool scaly = recipe.body.plan == "fish" || recipe.body.plan == "insect";
    const Materials materials{
        .coat = out.material("coat", look.coat, scaly ? 0.45F : 0.9F, 0.0F),
        .belly = out.material("belly", look.belly, scaly ? 0.45F : 0.9F, 0.0F),
        .markings = out.material("markings", look.markings, 0.8F, 0.0F),
        .eyes = out.material("eyes", look.eyes, 0.15F, 0.0F),
    };
    const Shaper shaper{.recipe = recipe, .m = m, .bones = bones, .out = out};
    const std::string& plan = recipe.body.plan;
    if (plan == "fish") {
        build_fish(shaper, materials);
    } else if (plan == "insect") {
        build_insect(shaper, materials);
    } else if (plan == "bird") {
        build_bird(shaper, materials);
    } else {
        build_quadruped(shaper, materials);
    }
    add_animations(recipe, bones, out.model);

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

Result<Model> load_creature_model(std::string_view bytes, const ModelReader& read) {
    auto doc = json::parse(bytes);
    if (!doc) {
        return doc.error();
    }
    auto recipe = world::parse_creature(doc.value(), read);
    if (!recipe) {
        return recipe.error();
    }
    return build_creature(recipe.value());
}

std::vector<json::ValidationError> check_creature_model(std::string_view bytes,
                                                        const ModelReader& read) {
    auto doc = json::parse(bytes);
    if (!doc) {
        return {{"", doc.error().message}};
    }
    if (auto errors = world::validate_creature(doc.value()); !errors.empty()) {
        return errors;
    }
    auto resolved = world::resolve_creature(doc.value(), read);
    if (!resolved) {
        return {{"/extends", resolved.error().message}};
    }
    return world::validate_creature(resolved.value());
}

} // namespace slash::scene3d
