#pragma once

// Humanoid models made in code for retargeting tests: a skeleton laid out from its bones' heads,
// skinned by one mesh with a vertex at each head and one halfway to each bone's tail.

#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
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

} // namespace test_humanoid
