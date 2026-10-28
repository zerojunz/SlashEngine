// Two rigged humanoids from different tools, retargeted to Godot's humanoid skeleton so one's
// animation plays on the other: a Mixamo-style hero a meter to the hips, standing in a T-pose with
// a wave, and a Blender-style knight 80 centimeters to the hips, built in centimeters under an
// armature scaled by 0.01, in an A-pose, its bones pointing along themselves as Blender's do.
//
// The models are made in code and written as .glb files, so none is committed.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Quat;
using slash::Vec3;

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.ok ? reply.value : json::Value();
}

struct Bone {
    std::string name;
    std::string parent;
    Vec3 head; // in the armature's space
    std::string tail;
};

// The shortest turn from one direction to another.
Quat turn(Vec3 from, Vec3 to) {
    const auto norm = [](Vec3 v) {
        return v * (1.0F / std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z)));
    };
    from = norm(from);
    to = norm(to);
    const Vec3 c{(from.y * to.z) - (from.z * to.y), (from.z * to.x) - (from.x * to.z),
                 (from.x * to.y) - (from.y * to.x)};
    const float s = std::sqrt((1.0F + (from.x * to.x) + (from.y * to.y) + (from.z * to.z)) * 2.0F);
    return {c.x / s, c.y / s, c.z / s, s * 0.5F};
}

// A skeleton laid out from its bones' heads, under an armature node, skinned by a mesh with a
// vertex at each head.
scene3d::Model rig(const std::vector<Bone>& bones, bool along_bones, float armature_scale) {
    scene3d::Model model;
    scene3d::ModelNode armature;
    armature.name = "Armature";
    armature.scale = {armature_scale, armature_scale, armature_scale};
    model.nodes.push_back(armature);
    std::vector<world::Affine3D> placed;
    scene3d::ModelSkin skin;
    for (const Bone& bone : bones) {
        world::Affine3D transform;
        for (const Bone& tail : bones) {
            if (along_bones && tail.name == bone.tail) {
                transform = world::rotation_basis(turn({0.0F, 1.0F, 0.0F}, tail.head - bone.head));
            }
        }
        transform.origin = bone.head;
        int parent = 0;
        world::Affine3D local = transform;
        for (std::size_t i = 0; i < bones.size() && !bone.parent.empty(); ++i) {
            if (bones[i].name == bone.parent) {
                parent = static_cast<int>(i) + 1;
                local = placed[i].inverse() * transform;
            }
        }
        placed.push_back(transform);
        const scene3d::NodePose pose = scene3d::decompose(local);
        scene3d::ModelNode node;
        node.name = bone.name;
        node.parent = parent;
        node.position = pose.position;
        node.rotation = pose.rotation;
        node.scale = pose.scale;
        node.file_index = static_cast<int>(model.nodes.size());
        skin.joints.push_back(model.nodes.size());
        model.nodes.push_back(node);
    }
    scene3d::ModelNode body;
    body.name = "Body";
    body.file_index = static_cast<int>(model.nodes.size());
    model.nodes.push_back(body);
    const std::vector<world::Affine3D> globals =
        scene3d::node_globals(model, scene3d::rest_pose(model));
    scene3d::ModelSurface surface;
    for (std::size_t b = 0; b < skin.joints.size(); ++b) {
        const world::Affine3D& at = globals[skin.joints[b]];
        skin.inverse_binds.push_back(at.inverse());
        for (const Vec3 corner :
             {Vec3{0.03F, 0.0F, 0.0F}, Vec3{-0.03F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 0.03F}}) {
            slash::render::Vertex3D vertex;
            vertex.position = {at.origin.x + corner.x, at.origin.y + corner.y,
                               at.origin.z + corner.z};
            vertex.normal = {0.0F, 1.0F, 0.0F};
            surface.vertices.push_back(vertex);
            scene3d::VertexWeights weights;
            weights.joints[0] = static_cast<std::uint16_t>(b);
            weights.weights[0] = 1.0F;
            surface.weights.push_back(weights);
            surface.indices.push_back(static_cast<std::uint32_t>(surface.indices.size()));
        }
    }
    model.skins.push_back(skin);
    scene3d::ModelMesh mesh;
    mesh.name = "Body";
    mesh.surfaces.push_back(surface);
    model.meshes.push_back(mesh);
    scene3d::ModelPart part;
    part.node = "Body";
    part.node_index = static_cast<int>(model.nodes.size() - 1);
    part.skin = 0;
    model.parts.push_back(part);
    return model;
}

std::vector<Bone> limbs(std::vector<Bone> bones, const std::string& chest, float unit,
                        float shoulder_y, float arm_drop, const char* left, const char* right,
                        const std::vector<std::string>& names) {
    // names: shoulder, upper arm, forearm, hand, thigh, shin, foot, toe; each with a side.
    for (const float side : {1.0F, -1.0F}) {
        const std::string s = side > 0.0F ? left : right;
        const auto n = [&](std::size_t i) {
            return names[i].front() == '_' ? s + names[i].substr(1) : names[i] + s;
        };
        const float x = side * unit;
        bones.push_back({n(0), chest, {0.06F * x, shoulder_y, 0.0F}, n(1)});
        bones.push_back({n(1), n(0), {0.18F * x, shoulder_y, 0.0F}, n(2)});
        bones.push_back({n(2), n(1), {0.42F * x, shoulder_y - (arm_drop * unit), 0.0F}, n(3)});
        bones.push_back({n(3), n(2), {0.64F * x, shoulder_y - (2.0F * arm_drop * unit), 0.0F}, ""});
        bones.push_back({n(4),
                         bones.front().name,
                         {0.09F * x, bones.front().head.y - (0.05F * unit), 0.0F},
                         n(5)});
        bones.push_back({n(5), n(4), {0.09F * x, 0.52F * unit, 0.0F}, n(6)});
        bones.push_back({n(6), n(5), {0.09F * x, 0.1F * unit, 0.0F}, n(7)});
        bones.push_back({n(7), n(6), {0.09F * x, 0.02F * unit, 0.12F * unit}, ""});
    }
    return bones;
}

} // namespace

int main() {
    // The hero: Mixamo's names (Godot reads "mixamorig:Hips" as "mixamorig_Hips"), in meters.
    const auto m = [](const char* name) { return std::string("mixamorig_") + name; };
    std::vector<Bone> hero_bones = {
        {m("Hips"), "", {0.0F, 1.0F, 0.0F}, m("Spine")},
        {m("Spine"), m("Hips"), {0.0F, 1.1F, 0.0F}, m("Spine1")},
        {m("Spine1"), m("Spine"), {0.0F, 1.22F, 0.0F}, m("Spine2")},
        {m("Spine2"), m("Spine1"), {0.0F, 1.34F, 0.0F}, m("Neck")},
        {m("Neck"), m("Spine2"), {0.0F, 1.5F, 0.0F}, m("Head")},
        {m("Head"), m("Neck"), {0.0F, 1.6F, 0.0F}, ""},
    };
    hero_bones =
        limbs(hero_bones, m("Spine2"), 1.0F, 1.44F, 0.0F, "mixamorig_Left", "mixamorig_Right",
              {"_Shoulder", "_Arm", "_ForeArm", "_Hand", "_UpLeg", "_Leg", "_Foot", "_ToeBase"});
    scene3d::Model hero = rig(hero_bones, false, 1.0F);
    // A wave: the left arm raised 60 degrees, the elbow bent and the hips bobbing up 10 cm.
    scene3d::ModelAnimation wave;
    wave.name = "wave";
    wave.length = 1.0;
    wave.loop = true;
    const auto track = [&](const std::string& bone, scene3d::NodeProperty property,
                           std::vector<std::array<float, 4>> values) {
        scene3d::NodeTrack out;
        out.node = static_cast<std::size_t>(scene3d::find_node(hero, bone));
        out.property = property;
        out.times = {0.0F, 0.5F, 1.0F};
        out.values = std::move(values);
        wave.tracks.push_back(out);
    };
    track(m("LeftArm"), scene3d::NodeProperty::Rotation,
          {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.5F, 0.866F}, {0.0F, 0.0F, 0.0F, 1.0F}});
    track(m("LeftForeArm"), scene3d::NodeProperty::Rotation,
          {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.3827F, 0.9239F}, {0.0F, 0.0F, 0.0F, 1.0F}});
    track(m("Hips"), scene3d::NodeProperty::Position,
          {{0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 1.1F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}});
    hero.animations.push_back(wave);

    // The knight: Blender's names (Godot reads "thigh.L" as "thigh_L"), in centimeters, the
    // arms hanging 45 degrees down.
    std::vector<Bone> knight_bones = {
        {"hips", "", {0.0F, 80.0F, 0.0F}, "spine"},
        {"spine", "hips", {0.0F, 88.0F, 0.0F}, "chest"},
        {"chest", "spine", {0.0F, 100.0F, 0.0F}, "neck"},
        {"neck", "chest", {0.0F, 120.0F, 0.0F}, "head"},
        {"head", "neck", {0.0F, 128.0F, 0.0F}, ""},
    };
    knight_bones =
        limbs(knight_bones, "chest", 80.0F, 115.0F, 0.2F, "_L", "_R",
              {"shoulder", "upper_arm", "forearm", "hand", "thigh", "shin", "foot", "toe"});
    const scene3d::Model knight = rig(knight_bones, true, 0.01F);

    (void)slash::platform::create_directories("game/source");
    const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no files");
    };
    (void)slash::platform::write_file("game/source/hero.glb",
                                      scene3d::write_glb(hero, "source/hero.glb", none).value());
    (void)slash::platform::write_file(
        "game/source/knight.glb", scene3d::write_glb(knight, "source/knight.glb", none).value());

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    options.project_root = "game";
    command::Session game(components, commands, options);

    // Each is retargeted with the bone map guessed from its names; the knight's arms are turned
    // out to the profile's T-pose.
    call(game, "model.retarget", R"({"path": "source/hero.glb", "out": "characters/hero.glb"})");
    call(game, "model.retarget",
         R"({"path": "source/knight.glb", "out": "characters/knight.glb",
             "fix_silhouette": true})");

    // The hero's wave played on both, half way through: the knight's hand rises as the hero's
    // does, and its hips bob 8 centimeters, as far for its height.
    call(game, "entity.create",
         R"({"id": "hero", "components": {"Transform3D": {},
             "MeshInstance3D": {"mesh": "characters/hero.glb"},
             "AnimationPlayer": {"library": "characters/hero.glb", "animation": "wave-loop",
                                 "time": 0.5, "playing": false}}})");
    call(game, "entity.create",
         R"({"id": "knight", "components": {"Transform3D": {"position": [1, 0, 0]},
             "MeshInstance3D": {"mesh": "characters/knight.glb"},
             "AnimationPlayer": {"library": "characters/hero.glb", "animation": "wave-loop",
                                 "time": 0.5, "playing": false}}})");
    call(game, "skeleton.get", R"({"id": "hero", "bones": ["Hips", "LeftLowerArm", "LeftHand"]})");
    call(game, "skeleton.get",
         R"({"id": "knight", "bones": ["Hips", "LeftLowerArm", "LeftHand"]})");

    // A bone the knight doesn't have is refused, with the one it meant.
    call(game, "model.retarget",
         R"({"path": "source/knight.glb", "bone_map": {"LeftHand": "hand_l"}})");
    return 0;
}
