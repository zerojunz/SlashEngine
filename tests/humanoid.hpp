#pragma once

// Humanoid models made in code for retargeting tests: a skeleton laid out from its bones' heads,
// skinned by one mesh with a vertex at each head and one halfway to each bone's tail.

#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace test_humanoid {

using slash::Quat;
using slash::Vec3;

struct Bone {
    std::string name;
    std::string parent; // "" for the root bone
    Vec3 head;          // in the armature's space
    std::string tail;   // the bone it points at, or ""
};

// The shortest turn from one direction to another, for bones that point along their y axis as
// Blender's do.
inline Quat turn(Vec3 from, Vec3 to) {
    const auto norm = [](Vec3 v) {
        const float l = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
        return v * (1.0F / l);
    };
    from = norm(from);
    to = norm(to);
    const Vec3 c{(from.y * to.z) - (from.z * to.y), (from.z * to.x) - (from.x * to.z),
                 (from.x * to.y) - (from.y * to.x)};
    const float d = (from.x * to.x) + (from.y * to.y) + (from.z * to.z);
    if (d < -0.9999F) {
        return {0.0F, 0.0F, 1.0F, 0.0F};
    }
    const float s = std::sqrt((1.0F + d) * 2.0F);
    return {c.x / s, c.y / s, c.z / s, s * 0.5F};
}

struct Options {
    bool along_bones = false; // rests turned to point along each bone, as Blender's
    std::string armature;     // a node above the skeleton, or ""
    float armature_scale = 1.0F;
    Quat armature_rotation;
};

inline slash::scene3d::Model make(const std::vector<Bone>& bones, const Options& options = {}) {
    namespace scene3d = slash::scene3d;
    namespace world = slash::world;
    scene3d::Model model;
    const auto find = [&](const std::string& name) {
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            if (model.nodes[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    int armature = -1;
    if (!options.armature.empty()) {
        scene3d::ModelNode node;
        node.name = options.armature;
        node.rotation = options.armature_rotation;
        node.scale = {options.armature_scale, options.armature_scale, options.armature_scale};
        model.nodes.push_back(node);
        armature = 0;
    }
    // Each bone's transform in the armature's space.
    std::vector<world::Affine3D> armature_space;
    std::vector<std::size_t> joints;
    for (const Bone& bone : bones) {
        world::Affine3D transform;
        if (options.along_bones && !bone.tail.empty()) {
            for (const Bone& tail : bones) {
                if (tail.name == bone.tail) {
                    transform =
                        world::rotation_basis(turn({0.0F, 1.0F, 0.0F}, tail.head - bone.head));
                }
            }
        }
        transform.origin = bone.head;
        const int parent = bone.parent.empty() ? armature : find(bone.parent);
        const bool parent_bone = !bone.parent.empty();
        const world::Affine3D local =
            parent_bone
                ? armature_space[static_cast<std::size_t>(parent - (armature + 1))].inverse() *
                      transform
                : transform;
        armature_space.push_back(transform);
        const scene3d::NodePose pose = scene3d::decompose(local);
        scene3d::ModelNode node;
        node.name = bone.name;
        node.parent = parent;
        node.position = pose.position;
        node.rotation = pose.rotation;
        node.scale = pose.scale;
        node.file_index = static_cast<int>(model.nodes.size());
        joints.push_back(model.nodes.size());
        model.nodes.push_back(node);
    }
    scene3d::ModelNode body;
    body.name = "Body";
    body.file_index = static_cast<int>(model.nodes.size());
    model.nodes.push_back(body);

    const std::vector<world::Affine3D> globals =
        scene3d::node_globals(model, scene3d::rest_pose(model));
    scene3d::ModelSkin skin;
    skin.name = "Skin";
    skin.joints = joints;
    for (const std::size_t joint : joints) {
        skin.inverse_binds.push_back(globals[joint].inverse());
    }
    model.skins.push_back(skin);

    scene3d::ModelSurface surface;
    const auto add = [&](Vec3 at, std::uint16_t a, std::uint16_t b, float weight_a) {
        slash::render::Vertex3D vertex;
        vertex.position = {at.x, at.y, at.z};
        surface.vertices.push_back(vertex);
        scene3d::VertexWeights weights;
        weights.joints[0] = a;
        weights.joints[1] = b;
        weights.weights[0] = weight_a;
        weights.weights[1] = 1.0F - weight_a;
        surface.weights.push_back(weights);
    };
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const world::Affine3D& at = globals[joints[i]];
        add(at.apply({0.01F, 0.0F, 0.02F}), static_cast<std::uint16_t>(i),
            static_cast<std::uint16_t>(i), 1.0F);
        for (std::size_t t = 0; t < bones.size(); ++t) {
            if (bones[t].name == bones[i].tail) {
                const Vec3 mid = (at.origin + globals[joints[t]].origin) * 0.5F;
                add(mid + Vec3{0.0F, 0.0F, 0.03F}, static_cast<std::uint16_t>(i),
                    static_cast<std::uint16_t>(t), 0.5F);
            }
        }
    }
    for (std::uint32_t i = 0; i + 2 < surface.vertices.size(); i += 3) {
        surface.indices.insert(surface.indices.end(), {i, i + 1, i + 2});
    }
    scene3d::ModelMesh mesh;
    mesh.name = "Body";
    mesh.surfaces.push_back(surface);
    model.meshes.push_back(mesh);
    scene3d::ModelPart part;
    part.node = "Body";
    part.mesh = 0;
    part.node_index = static_cast<int>(model.nodes.size() - 1);
    part.skin = 0;
    model.parts.push_back(part);
    return model;
}

// A Mixamo character, its bones named as Godot names Mixamo's ("mixamorig:Hips" becomes
// "mixamorig_Hips"), standing in a T-pose with every rest unturned, its hips a meter up.
inline std::vector<Bone> mixamo() {
    const auto m = [](const char* name) { return std::string("mixamorig_") + name; };
    std::vector<Bone> bones = {
        {m("Hips"), "", {0.0F, 1.0F, 0.0F}, m("Spine")},
        {m("Spine"), m("Hips"), {0.0F, 1.1F, 0.0F}, m("Spine1")},
        {m("Spine1"), m("Spine"), {0.0F, 1.22F, 0.0F}, m("Spine2")},
        {m("Spine2"), m("Spine1"), {0.0F, 1.34F, 0.0F}, m("Neck")},
        {m("Neck"), m("Spine2"), {0.0F, 1.5F, 0.0F}, m("Head")},
        {m("Head"), m("Neck"), {0.0F, 1.6F, 0.0F}, m("HeadTop_End")},
        {m("HeadTop_End"), m("Head"), {0.0F, 1.78F, 0.0F}, ""},
    };
    for (const float side : {1.0F, -1.0F}) {
        const std::string s = side > 0.0F ? "Left" : "Right";
        bones.push_back({m((s + "Shoulder").c_str()),
                         m("Spine2"),
                         {0.06F * side, 1.44F, 0.0F},
                         m((s + "Arm").c_str())});
        bones.push_back({m((s + "Arm").c_str()),
                         m((s + "Shoulder").c_str()),
                         {0.18F * side, 1.44F, 0.0F},
                         m((s + "ForeArm").c_str())});
        bones.push_back({m((s + "ForeArm").c_str()),
                         m((s + "Arm").c_str()),
                         {0.45F * side, 1.44F, 0.0F},
                         m((s + "Hand").c_str())});
        bones.push_back(
            {m((s + "Hand").c_str()), m((s + "ForeArm").c_str()), {0.7F * side, 1.44F, 0.0F}, ""});
        bones.push_back({m((s + "UpLeg").c_str()),
                         m("Hips"),
                         {0.09F * side, 0.95F, 0.0F},
                         m((s + "Leg").c_str())});
        bones.push_back({m((s + "Leg").c_str()),
                         m((s + "UpLeg").c_str()),
                         {0.09F * side, 0.53F, 0.0F},
                         m((s + "Foot").c_str())});
        bones.push_back({m((s + "Foot").c_str()),
                         m((s + "Leg").c_str()),
                         {0.09F * side, 0.1F, 0.0F},
                         m((s + "ToeBase").c_str())});
        bones.push_back({m((s + "ToeBase").c_str()),
                         m((s + "Foot").c_str()),
                         {0.09F * side, 0.02F, 0.12F},
                         ""});
    }
    return bones;
}

// A smaller character named as Blender's metarig names it (Godot turning "thigh.L" into
// "thigh_L"), its heads given in centimeters, for an armature scaled by 0.01.
inline std::vector<Bone> blender() {
    std::vector<Bone> bones = {
        {"hips", "", {0.0F, 80.0F, 0.0F}, "spine"},
        {"spine", "hips", {0.0F, 90.0F, 0.0F}, "chest"},
        {"chest", "spine", {0.0F, 102.0F, 0.0F}, "neck"},
        {"neck", "chest", {0.0F, 120.0F, 1.0F}, "head"},
        {"head", "neck", {0.0F, 128.0F, 2.0F}, ""},
    };
    for (const float side : {1.0F, -1.0F}) {
        const std::string s = side > 0.0F ? "_L" : "_R";
        bones.push_back({"shoulder" + s, "chest", {3.0F * side, 116.0F, 1.0F}, "upper_arm" + s});
        // An A-pose: the arms hang 45 degrees down.
        bones.push_back(
            {"upper_arm" + s, "shoulder" + s, {14.0F * side, 116.0F, 0.0F}, "forearm" + s});
        bones.push_back({"forearm" + s, "upper_arm" + s, {30.0F * side, 100.0F, 0.0F}, "hand" + s});
        bones.push_back({"hand" + s, "forearm" + s, {45.0F * side, 85.0F, 0.0F}, ""});
        bones.push_back({"thigh" + s, "hips", {8.0F * side, 78.0F, 0.0F}, "shin" + s});
        bones.push_back({"shin" + s, "thigh" + s, {8.0F * side, 42.0F, 1.0F}, "foot" + s});
        bones.push_back({"foot" + s, "shin" + s, {8.0F * side, 8.0F, 0.0F}, "toe" + s});
        bones.push_back({"toe" + s, "foot" + s, {8.0F * side, 1.0F, 10.0F}, ""});
    }
    return bones;
}

// Adds a box to a surface, its faces wound clockwise from outside, as Godot's front faces are.
inline void add_box(slash::scene3d::ModelSurface& surface, Vec3 low, Vec3 high) {
    const std::array<Vec3, 8> corner{{{low.x, low.y, low.z},
                                      {high.x, low.y, low.z},
                                      {high.x, high.y, low.z},
                                      {low.x, high.y, low.z},
                                      {low.x, low.y, high.z},
                                      {high.x, low.y, high.z},
                                      {high.x, high.y, high.z},
                                      {low.x, high.y, high.z}}};
    // Each face's corners counterclockwise seen from outside, and its normal.
    const std::array<std::pair<std::array<int, 4>, Vec3>, 6> faces{{
        {{0, 3, 2, 1}, {0.0F, 0.0F, -1.0F}},
        {{4, 5, 6, 7}, {0.0F, 0.0F, 1.0F}},
        {{0, 4, 7, 3}, {-1.0F, 0.0F, 0.0F}},
        {{1, 2, 6, 5}, {1.0F, 0.0F, 0.0F}},
        {{0, 1, 5, 4}, {0.0F, -1.0F, 0.0F}},
        {{3, 7, 6, 2}, {0.0F, 1.0F, 0.0F}},
    }};
    for (const auto& [ring, normal] : faces) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (const int c : ring) {
            slash::render::Vertex3D vertex;
            const Vec3 p = corner.at(static_cast<std::size_t>(c));
            vertex.position = {p.x, p.y, p.z};
            vertex.normal = {normal.x, normal.y, normal.z};
            surface.vertices.push_back(vertex);
        }
        for (const std::uint32_t i : {0U, 2U, 1U, 0U, 3U, 2U}) {
            surface.indices.push_back(first + i);
        }
    }
}

// An unrigged figure 1.8 meters tall, facing +z, of boxes: legs, a body, a head and arms held
// out level (a T-pose) or hanging at its sides, with a helmet as a second part one node places.
inline slash::scene3d::Model figure(bool t_pose) {
    namespace scene3d = slash::scene3d;
    scene3d::Model model;
    scene3d::ModelMesh body;
    body.name = "Body";
    scene3d::ModelSurface& surface = body.surfaces.emplace_back();
    for (const float side : {1.0F, -1.0F}) {
        const auto x = [side](float a, float b) {
            return side > 0.0F ? std::pair{a, b} : std::pair{-b, -a};
        };
        add_box(surface, {x(0.03F, 0.15F).first, 0.0F, -0.07F},
                {x(0.03F, 0.15F).second, 0.9F, 0.07F});
        if (t_pose) {
            add_box(surface, {x(0.18F, 0.8F).first, 1.38F, -0.05F},
                    {x(0.18F, 0.8F).second, 1.48F, 0.05F});
        } else {
            add_box(surface, {x(0.19F, 0.27F).first, 0.8F, -0.05F},
                    {x(0.19F, 0.27F).second, 1.46F, 0.05F});
        }
    }
    add_box(surface, {-0.18F, 0.9F, -0.1F}, {0.18F, 1.5F, 0.1F});
    add_box(surface, {-0.1F, 1.55F, -0.1F}, {0.1F, 1.75F, 0.1F});
    scene3d::ModelMesh helmet;
    helmet.name = "Helmet";
    add_box(helmet.surfaces.emplace_back(), {-0.11F, 0.0F, -0.11F}, {0.11F, 0.05F, 0.11F});
    model.meshes = {body, helmet};
    scene3d::ModelNode body_node;
    body_node.name = "Body";
    scene3d::ModelNode helmet_node;
    helmet_node.name = "Helmet";
    helmet_node.position = {0.0F, 1.75F, 0.0F};
    model.nodes = {body_node, helmet_node};
    model.parts.push_back(
        {.node = "Body", .mesh = 0, .transform = {}, .node_index = 0, .skin = -1});
    slash::world::Affine3D on_head;
    on_head.origin = {0.0F, 1.75F, 0.0F};
    model.parts.push_back(
        {.node = "Helmet", .mesh = 1, .transform = on_head, .node_index = 1, .skin = -1});
    return model;
}

} // namespace test_humanoid
