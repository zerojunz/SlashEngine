#include "slash/scene3d/character.hpp"

#include "slash/core/json.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    return l > 0.0F ? v * (1.0F / l) : v;
}

Vec3 lerp(Vec3 a, Vec3 b, real_t t) {
    return a + ((b - a) * t);
}

// A turn that takes +y to d, keeping x level where it can.
Affine3D facing(Vec3 d) {
    const Vec3 up = normalized(d);
    const Vec3 reference = std::abs(up.z) < 0.9F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F};
    Affine3D out;
    out.x = normalized(cross(up, reference));
    out.y = up;
    out.z = cross(out.x, up);
    return out;
}

// A turn, then a stretch along its axes, then a move.
Affine3D placed(const Affine3D& turn, Vec3 stretch, Vec3 at) {
    Affine3D out;
    out.x = turn.x * stretch.x;
    out.y = turn.y * stretch.y;
    out.z = turn.z * stretch.z;
    out.origin = at;
    return out;
}

Affine3D placed(Vec3 stretch, Vec3 at) {
    return placed(Affine3D{}, stretch, at);
}

Affine3D moved(Vec3 at) {
    Affine3D out;
    out.origin = at;
    return out;
}

MeshData unit_sphere(bool hemisphere = false) {
    world::SphereMesh sphere;
    sphere.radius = 1.0F;
    // Godot's hemisphere is height tall, its sphere height across.
    sphere.height = hemisphere ? 1.0F : 2.0F;
    sphere.radial_segments = 16;
    sphere.rings = 8;
    sphere.is_hemisphere = hemisphere;
    return sphere_mesh(sphere);
}

// A tube that is radius wide with a round end each side, length long between their centers.
MeshData capsule(float radius, float length) {
    world::CapsuleMesh capsule;
    capsule.radius = radius;
    capsule.height = length + (2.0F * radius);
    capsule.radial_segments = 12;
    capsule.rings = 2;
    return capsule_mesh(capsule);
}

// A tube 1 high and 1 across at its top, bottom times as wide at its bottom, with no ends.
MeshData open_tube(float bottom = 1.0F) {
    world::CylinderMesh cylinder;
    cylinder.top_radius = 1.0F;
    cylinder.bottom_radius = bottom;
    cylinder.height = 1.0F;
    cylinder.radial_segments = 16;
    cylinder.rings = 1;
    cylinder.cap_top = false;
    cylinder.cap_bottom = false;
    return cylinder_mesh(cylinder);
}

MeshData disc_cylinder(float bottom = 1.0F) {
    world::CylinderMesh cylinder;
    cylinder.top_radius = 1.0F;
    cylinder.bottom_radius = bottom;
    cylinder.height = 1.0F;
    cylinder.radial_segments = 16;
    cylinder.rings = 1;
    return cylinder_mesh(cylinder);
}

// A bone of the skeleton: Godot's humanoid profile's name, its parent (before it) and where its
// head is in the model's space at rest.
struct Bone {
    std::string name;
    int parent = -1;
    Vec3 head;
};

struct Builder {
    Model model;
    ModelMesh body;
    std::vector<int> surface_of; // a material's surface in body, or -1

    int material(const std::string& name, Vec4 color, float roughness, float metallic,
                 bool two_sided = false) {
        for (std::size_t i = 0; i < model.materials.size(); ++i) {
            if (model.materials[i].name == name) {
                return static_cast<int>(i);
            }
        }
        ModelMaterial made;
        made.name = name;
        made.standard.albedo_color = color;
        made.standard.roughness = roughness;
        made.standard.metallic = metallic;
        if (two_sided) {
            made.standard.cull_mode = "disabled";
        }
        model.materials.push_back(std::move(made));
        surface_of.push_back(-1);
        return static_cast<int>(model.materials.size()) - 1;
    }

    // The mesh's corners placed by place, drawn with material and moved by one joint.
    void add(const MeshData& mesh, const Affine3D& place, int material, std::size_t joint) {
        if (surface_of.size() < model.materials.size()) {
            surface_of.resize(model.materials.size(), -1);
        }
        int& index = surface_of.at(static_cast<std::size_t>(material));
        if (index < 0) {
            index = static_cast<int>(body.surfaces.size());
            ModelSurface surface;
            surface.material = material;
            body.surfaces.push_back(std::move(surface));
        }
        ModelSurface& surface = body.surfaces.at(static_cast<std::size_t>(index));
        const Affine3D inverse = place.inverse();
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        VertexWeights weights;
        weights.joints[0] = static_cast<std::uint16_t>(joint);
        weights.weights[0] = 1.0F;
        for (render::Vertex3D vertex : mesh.vertices) {
            const Vec3 p =
                place.apply({vertex.position[0], vertex.position[1], vertex.position[2]});
            const Vec3 n{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
            // Normals turn by the inverse transpose, so stretched shapes keep them square.
            const Vec3 normal =
                normalized({dot(inverse.x, n), dot(inverse.y, n), dot(inverse.z, n)});
            const Vec3 tangent =
                normalized(place.basis({vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]}));
            vertex.position = {static_cast<float>(p.x), static_cast<float>(p.y),
                               static_cast<float>(p.z)};
            vertex.normal = {static_cast<float>(normal.x), static_cast<float>(normal.y),
                             static_cast<float>(normal.z)};
            vertex.tangent = {static_cast<float>(tangent.x), static_cast<float>(tangent.y),
                              static_cast<float>(tangent.z), vertex.tangent[3]};
            surface.vertices.push_back(vertex);
            surface.weights.push_back(weights);
        }
        for (const std::uint32_t i : mesh.indices) {
            surface.indices.push_back(first + i);
        }
    }
};

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

// The skeleton a recipe's measures place, Godot's humanoid profile's bones in its order.
std::vector<Bone> skeleton(const CharacterMeasures& m) {
    std::vector<Bone> bones{
        {"Root", -1, {0.0F, 0.0F, 0.0F}},
        {"Hips", 0, {0.0F, m.hip, 0.0F}},
        {"Spine", 1, {0.0F, m.waist, 0.0F}},
        {"Chest", 2, {0.0F, m.chest, 0.0F}},
        {"UpperChest", 3, {0.0F, m.chest + ((m.shoulder - m.chest) * 0.5F), 0.0F}},
        {"Neck", 4, {0.0F, m.shoulder, 0.0F}},
        {"Head", 5, {0.0F, m.head_base, 0.0F}},
    };
    // Arms hang 12 degrees out from straight down.
    const Vec2 out = cos_sin(0.2094395F);
    for (const char* side_name : {"Left", "Right"}) {
        const real_t side = std::string_view(side_name) == "Left" ? 1.0F : -1.0F;
        const std::string name(side_name);
        const Vec3 down{side * out.y, -out.x, 0.0F};
        const int first = static_cast<int>(bones.size());
        const Vec3 upper{side * m.shoulder_half_width, m.shoulder - (m.arm_radius * 0.5F), 0.0F};
        const Vec3 lower = upper + (down * m.upper_arm);
        bones.push_back({name + "Shoulder", 4, {side * m.neck_radius, m.shoulder, 0.0F}});
        bones.push_back({name + "UpperArm", first, upper});
        bones.push_back({name + "LowerArm", first + 1, lower});
        bones.push_back({name + "Hand", first + 2, lower + (down * m.lower_arm)});
        bones.push_back({name + "UpperLeg", 1, {side * m.leg_offset, m.hip, 0.0F}});
        bones.push_back({name + "LowerLeg", first + 4, {side * m.leg_offset, m.knee, 0.0F}});
        bones.push_back({name + "Foot", first + 5, {side * m.leg_offset, m.ankle, 0.0F}});
        bones.push_back(
            {name + "Toes", first + 6, {side * m.leg_offset, 0.0F, m.foot_length * 0.6F}});
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
// from the skin by grow.
struct Body {
    const CharacterMeasures& m;
    const std::vector<Bone>& bones;
    Builder& out;

    [[nodiscard]] std::size_t bone(std::string_view name) const { return bone_index(bones, name); }
    [[nodiscard]] Vec3 head_of(std::string_view name) const { return bones[bone(name)].head; }

    // A round-ended tube along a stretch of a limb, from and to as shares of it.
    void limb(Vec3 a, Vec3 b, float radius, float from, float to, int material,
              std::size_t joint) const {
        const Vec3 start = lerp(a, b, from);
        const Vec3 end = lerp(a, b, to);
        const real_t span = length(end - start);
        out.add(capsule(radius, static_cast<float>(span)),
                placed(facing(end - start), {1, 1, 1}, lerp(start, end, 0.5F)), material, joint);
    }

    void torso(float grow, int material) const {
        const Vec3 center{0.0F, ((m.waist + m.shoulder) * 0.5F) - (0.02F * m.scale), 0.0F};
        const Vec3 radii{m.chest_half_width + grow,
                         ((m.shoulder - m.waist) * 0.5F) + (0.03F * m.scale) + grow,
                         m.chest_depth + grow};
        out.add(unit_sphere(), placed(radii, center), material, bone("Chest"));
        // The shoulders, across from one arm to the other.
        const Vec3 left = head_of("LeftUpperArm");
        const Vec3 right = head_of("RightUpperArm");
        out.add(capsule((m.arm_radius * 1.1F) + grow, static_cast<float>(length(left - right))),
                placed(facing(left - right), {1, 1, 1}, lerp(left, right, 0.5F)), material,
                bone("UpperChest"));
    }

    void pelvis(float grow, int material) const {
        const Vec3 center{0.0F, (m.hip + m.waist) * 0.5F, 0.0F};
        const Vec3 radii{m.hip_half_width + grow,
                         ((m.waist - m.hip) * 0.5F) + (0.06F * m.scale) + grow, m.hip_depth + grow};
        out.add(unit_sphere(), placed(radii, center), material, bone("Hips"));
    }

    void arms(Region region, float grow, float from, float to, int material) const {
        for (const std::string side : {"Left", "Right"}) {
            const Vec3 upper = head_of(side + "UpperArm");
            const Vec3 lower = head_of(side + "LowerArm");
            const Vec3 hand = head_of(side + "Hand");
            if (region == Region::UpperArms) {
                limb(upper, lower, m.arm_radius + grow, from, to, material,
                     bone(side + "UpperArm"));
            } else if (region == Region::LowerArms) {
                limb(lower, hand, m.forearm_radius + grow, from, to, material,
                     bone(side + "LowerArm"));
            } else {
                const Vec3 down = normalized(hand - lower);
                out.add(unit_sphere(),
                        placed(facing(down),
                               {(m.forearm_radius * 1.15F) + grow, (m.hand * 0.5F) + grow,
                                (m.forearm_radius * 0.6F) + grow},
                               hand + (down * (m.hand * 0.5F))),
                        material, bone(side + "Hand"));
            }
        }
    }

    void legs(Region region, float grow, float from, float to, int material) const {
        for (const std::string side : {"Left", "Right"}) {
            const Vec3 hip = head_of(side + "UpperLeg");
            const Vec3 knee = head_of(side + "LowerLeg");
            const Vec3 ankle = head_of(side + "Foot");
            if (region == Region::UpperLegs) {
                limb(hip, knee, m.thigh_radius + grow, from, to, material, bone(side + "UpperLeg"));
            } else if (region == Region::LowerLegs) {
                // Ended high enough that its round end stays above the floor.
                const float radius = m.calf_radius + grow;
                const Vec3 end{ankle.x, std::max<real_t>(ankle.y, radius), ankle.z};
                limb(knee, end, radius, from, to, material, bone(side + "LowerLeg"));
            } else {
                // Standing on the floor, however thick the sole.
                const real_t half = (m.ankle * 0.6F) + grow;
                const Vec3 center{ankle.x, half, m.foot_length * 0.3F};
                out.add(
                    unit_sphere(),
                    placed({(m.calf_radius * 1.05F) + grow, half, (m.foot_length * 0.6F) + grow},
                           center),
                    material, bone(side + "Foot"));
            }
        }
    }
};

void add_face(const CharacterRecipe& recipe, const Body& body, int skin) {
    const CharacterMeasures& m = body.m;
    Builder& out = body.out;
    const std::size_t head = body.bone("Head");
    const real_t hr = m.head_radius;
    const real_t hh = m.head_height;
    const real_t base = m.head_base;
    // The neck and head.
    body.limb({0.0F, m.shoulder - (0.03F * m.scale), 0.0F}, {0.0F, base + (0.02F * m.scale), 0.0F},
              m.neck_radius, 0.0F, 1.0F, skin, body.bone("Neck"));
    out.add(unit_sphere(), placed({hr, hh * 0.5F, hr * 1.12F}, {0.0F, base + (hh * 0.5F), 0.0F}),
            skin, head);
    const world::CharacterFace& face = recipe.face;
    const int eyes = out.material("eyes", face.eyes, 0.2F, 0.0F);
    const int lips = out.material("lips", face.lips, 0.5F, 0.0F);
    for (const real_t side : {1.0F, -1.0F}) {
        const real_t eye = hr * 0.16F * face.eye_size;
        out.add(unit_sphere(),
                placed({eye, eye, eye * 0.6F},
                       {side * hr * 0.38F * face.eye_spacing, base + (hh * 0.56F), hr * 1.0F}),
                eyes, head);
        out.add(unit_sphere(),
                placed(Vec3{0.15F, 0.32F, 0.22F} * (hr * face.ears),
                       {side * hr * 0.98F, base + (hh * 0.5F), 0.0F}),
                skin, head);
    }
    out.add(
        unit_sphere(),
        placed(Vec3{0.13F, 0.2F, 0.16F} * (hr * face.nose), {0.0F, base + (hh * 0.42F), hr * 1.1F}),
        skin, head);
    out.add(unit_sphere(),
            placed(Vec3{0.32F, 0.07F, 0.1F} * hr, {0.0F, base + (hh * 0.27F), hr * 1.03F}), lips,
            head);
    if (face.beard != "none") {
        const int hair = out.material("hair", recipe.hair.color, 0.8F, 0.0F);
        const real_t grow = face.beard == "stubble" ? 0.01F : 0.06F;
        out.add(unit_sphere(),
                placed({hr * (1.0F + grow), hh * (0.16F + grow), hr * (1.0F + grow)},
                       {0.0F, base + (hh * 0.12F), hr * 0.15F}),
                hair, head);
        if (face.beard == "long") {
            body.limb({0.0F, base + (hh * 0.1F), hr * 0.9F},
                      {0.0F, m.shoulder - (0.08F * m.scale), m.chest_depth + (0.02F * m.scale)},
                      static_cast<float>(hr * 0.35F), 0.0F, 1.0F, hair, head);
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

} // namespace

Result<Model> build_character(const CharacterRecipe& recipe, const ModelReader& read) {
    const CharacterMeasures m = world::character_measures(recipe);
    const std::vector<Bone> bones = skeleton(m);
    Builder out;
    ModelSkin skin;
    skin.name = "Skeleton";
    for (const Bone& bone : bones) {
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
    const std::vector<Point> placed_points = points(m, bones);
    for (const Point& point : placed_points) {
        const std::size_t parent = bone_index(bones, point.bone);
        ModelNode node;
        node.name = std::string(point.name);
        node.parent = static_cast<int>(parent);
        node.position = point.at - bones[parent].head;
        node.file_index = static_cast<int>(out.model.nodes.size());
        out.model.nodes.push_back(std::move(node));
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

Result<Model> load_character_model(std::string_view bytes, const ModelReader& read) {
    auto doc = json::parse(bytes);
    if (!doc) {
        return doc.error();
    }
    auto recipe = world::parse_character(doc.value(), read);
    if (!recipe) {
        return recipe.error();
    }
    return build_character(recipe.value(), read);
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
