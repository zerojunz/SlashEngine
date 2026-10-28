#include "humanoid.hpp"

#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Quat;
using slash::Vec3;

namespace {

float distance(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

Vec3 direction(Vec3 from, Vec3 to) {
    const Vec3 d = to - from;
    return d * (1.0F / distance(from, to));
}

// The skinned mesh's vertices in a pose.
std::vector<Vec3> skinned(const scene3d::Model& model, const std::vector<scene3d::NodePose>& pose) {
    const auto joints =
        scene3d::joint_transforms(model.skins[0], scene3d::node_globals(model, pose));
    std::vector<slash::render::Vertex3D> vertices;
    scene3d::skin_vertices(model.meshes[0].surfaces[0], joints, vertices);
    std::vector<Vec3> out;
    for (const auto& vertex : vertices) {
        out.push_back({vertex.position[0], vertex.position[1], vertex.position[2]});
    }
    return out;
}

std::vector<scene3d::NodePose> posed(const scene3d::Model& model, double time) {
    std::vector<scene3d::NodePose> pose = scene3d::rest_pose(model);
    scene3d::blend_animation(model, model, model.animations[0], time, 1.0F, {}, pose);
    return pose;
}

void check_close(const std::vector<Vec3>& a, const std::vector<Vec3>& b, float tolerance) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        INFO("vertex " << i << ": " << a[i].x << " " << a[i].y << " " << a[i].z << " vs " << b[i].x
                       << " " << b[i].y << " " << b[i].z);
        CHECK(distance(a[i], b[i]) < tolerance);
    }
}

std::size_t node(const scene3d::Model& model, const char* name) {
    const int found = scene3d::find_node(model, name);
    REQUIRE(found >= 0);
    return static_cast<std::size_t>(found);
}

scene3d::NodeTrack track(std::size_t node, scene3d::NodeProperty property,
                         std::vector<std::array<float, 4>> values) {
    scene3d::NodeTrack out;
    out.node = node;
    out.property = property;
    out.times = {0.0F, 1.0F};
    out.values = std::move(values);
    return out;
}

// Raises the left arm 60 degrees and bends the elbow, and lifts the hips 10 centimeters.
void add_wave(scene3d::Model& model) {
    scene3d::ModelAnimation wave;
    wave.name = "wave";
    wave.length = 1.0;
    const float s30 = std::sin(0.5236F);
    const float c30 = std::cos(0.5236F);
    wave.tracks.push_back(track(node(model, "mixamorig_LeftArm"), scene3d::NodeProperty::Rotation,
                                {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, s30, c30}}));
    wave.tracks.push_back(track(node(model, "mixamorig_LeftForeArm"),
                                scene3d::NodeProperty::Rotation,
                                {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.3827F, 0.0F, 0.9239F}}));
    wave.tracks.push_back(track(node(model, "mixamorig_Hips"), scene3d::NodeProperty::Position,
                                {{0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 1.1F, 0.0F, 0.0F}}));
    // At rest all along: the track organizer drops it.
    wave.tracks.push_back(track(node(model, "mixamorig_LeftHand"), scene3d::NodeProperty::Position,
                                {{0.25F, 0.0F, 0.0F, 0.0F}, {0.25F, 0.0F, 0.0F, 0.0F}}));
    model.animations.push_back(wave);
}

scene3d::Model mixamo_model() {
    scene3d::Model model = test_humanoid::make(test_humanoid::mixamo());
    add_wave(model);
    return model;
}

scene3d::Model blender_model() {
    test_humanoid::Options options;
    options.along_bones = true;
    options.armature = "Armature";
    options.armature_scale = 0.01F;
    return test_humanoid::make(test_humanoid::blender(), options);
}

} // namespace

TEST_CASE("humanoid profile: Godot's 56 bones, parents first") {
    const auto profile = scene3d::humanoid_profile();
    CHECK(profile.size() == 56);
    CHECK(profile[0].name == "Root");
    CHECK(scene3d::find_profile_bone("LeftUpperArm") >= 0);
    CHECK(scene3d::find_profile_bone("Tail") == -1);
    for (std::size_t i = 1; i < profile.size(); ++i) {
        const int parent = scene3d::find_profile_bone(profile[i].parent);
        CHECK(parent >= 0);
        CHECK(static_cast<std::size_t>(parent) < i);
    }
}

TEST_CASE("auto_map_bones maps Mixamo's and Blender's bone names") {
    const scene3d::BoneMap mixamo = scene3d::auto_map_bones(mixamo_model());
    CHECK(mixamo.at("Hips") == "mixamorig_Hips");
    CHECK(mixamo.at("Spine") == "mixamorig_Spine");
    CHECK(mixamo.at("Chest") == "mixamorig_Spine1");
    CHECK(mixamo.at("UpperChest") == "mixamorig_Spine2");
    CHECK(mixamo.at("Neck") == "mixamorig_Neck");
    CHECK(mixamo.at("Head") == "mixamorig_Head");
    CHECK(mixamo.at("LeftShoulder") == "mixamorig_LeftShoulder");
    CHECK(mixamo.at("LeftUpperArm") == "mixamorig_LeftArm");
    CHECK(mixamo.at("LeftLowerArm") == "mixamorig_LeftForeArm");
    CHECK(mixamo.at("LeftHand") == "mixamorig_LeftHand");
    CHECK(mixamo.at("RightUpperLeg") == "mixamorig_RightUpLeg");
    CHECK(mixamo.at("RightLowerLeg") == "mixamorig_RightLeg");
    CHECK(mixamo.at("RightFoot") == "mixamorig_RightFoot");
    CHECK(mixamo.at("RightToes") == "mixamorig_RightToeBase");
    CHECK(!mixamo.contains("Root"));

    const scene3d::BoneMap blender = scene3d::auto_map_bones(blender_model());
    CHECK(blender.at("Hips") == "hips");
    CHECK(blender.at("Spine") == "spine");
    CHECK(blender.at("Head") == "head");
    CHECK(blender.at("LeftUpperArm") == "upper_arm_L");
    CHECK(blender.at("LeftLowerArm") == "forearm_L");
    CHECK(blender.at("RightHand") == "hand_R");
    CHECK(blender.at("LeftUpperLeg") == "thigh_L");
    CHECK(blender.at("LeftLowerLeg") == "shin_L");
    CHECK(blender.at("RightFoot") == "foot_R");

    CHECK(scene3d::auto_map_bones(scene3d::Model{}).empty());
}

TEST_CASE("retarget_model renames bones and keeps the skin and animations where they were") {
    const scene3d::Model original = mixamo_model();
    scene3d::Model model = original;
    scene3d::RetargetOptions options;
    options.bone_map = scene3d::auto_map_bones(model);
    const auto report = scene3d::retarget_model(model, options);
    REQUIRE(report.ok());
    CHECK(report.value().missing.empty());
    CHECK(std::ranges::find(report.value().unmapped, "mixamorig_HeadTop_End") !=
          report.value().unmapped.end());
    CHECK(report.value().motion_scale == doctest::Approx(1.0F));

    CHECK(scene3d::find_node(model, "mixamorig_Hips") == -1);
    const std::size_t hips = node(model, "Hips");
    CHECK(node(model, "LeftUpperArm") == node(original, "mixamorig_LeftArm"));

    // At rest and as it animates, the mesh is where it was.
    check_close(skinned(model, scene3d::rest_pose(model)),
                skinned(original, scene3d::rest_pose(original)), 1e-4F);
    for (const double time : {0.0, 0.5, 1.0}) {
        check_close(skinned(model, posed(model, time)), skinned(original, posed(original, time)),
                    1e-4F);
    }

    // Each mapped bone lies as the profile's does.
    const auto globals = scene3d::node_globals(model, scene3d::rest_pose(model));
    const world::Affine3D upper_arm = globals[node(model, "LeftUpperArm")];
    CHECK(upper_arm.x.x == doctest::Approx(0.0F).epsilon(1e-5));
    CHECK(upper_arm.y.x == doctest::Approx(1.0F).epsilon(1e-5)); // y points along the arm
    CHECK(globals[hips].y.y == doctest::Approx(1.0F).epsilon(1e-5));

    // The hand's position track went; Hips' stayed; the bones it left alone got rest rotations.
    const auto& tracks = model.animations[0].tracks;
    const auto has = [&](const char* name, scene3d::NodeProperty property) {
        const std::size_t n = node(model, name);
        return std::ranges::any_of(tracks, [&](const scene3d::NodeTrack& t) {
            return t.node == n && t.property == property;
        });
    };
    CHECK(!has("LeftHand", scene3d::NodeProperty::Position));
    CHECK(has("Hips", scene3d::NodeProperty::Position));
    CHECK(has("RightFoot", scene3d::NodeProperty::Rotation));
    CHECK(!has("mixamorig_HeadTop_End", scene3d::NodeProperty::Rotation));
}

TEST_CASE("retarget_model folds the armature's scale and turn into the bones") {
    const scene3d::Model original = blender_model();
    scene3d::Model model = original;
    scene3d::RetargetOptions options;
    options.bone_map = scene3d::auto_map_bones(model);
    const auto report = scene3d::retarget_model(model, options);
    REQUIRE(report.ok());
    CHECK(report.value().missing.empty());
    CHECK(report.value().motion_scale == doctest::Approx(0.8F));
    const scene3d::ModelNode& armature = model.nodes[node(model, "Armature")];
    CHECK(armature.scale.x == doctest::Approx(1.0F));
    CHECK(model.nodes[node(model, "Hips")].position.y == doctest::Approx(0.8F));
    check_close(skinned(model, scene3d::rest_pose(model)),
                skinned(original, scene3d::rest_pose(original)), 1e-4F);
}

TEST_CASE("fix_silhouette turns an A-pose's arms out to the profile's T-pose") {
    scene3d::Model model = blender_model();
    scene3d::RetargetOptions options;
    options.bone_map = scene3d::auto_map_bones(model);
    options.fix_silhouette = true;
    REQUIRE(scene3d::retarget_model(model, options).ok());
    const auto globals = scene3d::node_globals(model, scene3d::rest_pose(model));
    const Vec3 arm = direction(globals[node(model, "LeftUpperArm")].origin,
                               globals[node(model, "LeftLowerArm")].origin);
    CHECK(arm.x == doctest::Approx(1.0F).epsilon(1e-4));
    CHECK(std::abs(arm.y) < 1e-3F);
}

TEST_CASE("a retargeted animation plays on another retargeted humanoid") {
    scene3d::Model mixamo = mixamo_model();
    scene3d::Model blender = blender_model();
    for (scene3d::Model* model : {&mixamo, &blender}) {
        scene3d::RetargetOptions options;
        options.bone_map = scene3d::auto_map_bones(*model);
        options.fix_silhouette = true;
        REQUIRE(scene3d::retarget_model(*model, options).ok());
    }
    std::vector<scene3d::NodePose> pose = scene3d::rest_pose(blender);
    scene3d::blend_animation(blender, mixamo, mixamo.animations[0], 1.0, 1.0F,
                             scene3d::node_map(blender, mixamo), pose);
    const auto theirs = scene3d::node_globals(blender, pose);
    const auto ours = scene3d::node_globals(mixamo, posed(mixamo, 1.0));
    for (const auto& [from, to] :
         {std::pair{"LeftUpperArm", "LeftLowerArm"}, std::pair{"LeftLowerArm", "LeftHand"},
          std::pair{"Spine", "Chest"}}) {
        INFO(from);
        const Vec3 a = direction(ours[node(mixamo, from)].origin, ours[node(mixamo, to)].origin);
        const Vec3 b =
            direction(theirs[node(blender, from)].origin, theirs[node(blender, to)].origin);
        CHECK(distance(a, b) < 1e-3F);
    }
    // The hips rise 10 centimeters on a meter-high character: 8 on this one.
    CHECK(theirs[node(blender, "Hips")].origin.y == doctest::Approx(0.88F).epsilon(1e-4));
}

TEST_CASE("retarget_model moves a node with a profile bone's name aside") {
    scene3d::Model model = mixamo_model();
    scene3d::ModelNode helper;
    helper.name = "Spine";
    model.nodes.push_back(helper);
    scene3d::RetargetOptions options;
    options.bone_map = scene3d::auto_map_bones(model);
    const auto report = scene3d::retarget_model(model, options);
    REQUIRE(report.ok());
    CHECK(report.value().moved_aside.at("Spine2") == "Spine");
    CHECK(model.nodes.back().name == "Spine2");
    CHECK(model.nodes[node(model, "Spine")].parent == static_cast<int>(node(model, "Hips")));
}

TEST_CASE("retarget_model refuses bad maps and models without skins") {
    scene3d::Model model = mixamo_model();
    scene3d::RetargetOptions options;
    options.bone_map = {{"Hips", "mixamorig_Hips"}, {"Tail", "mixamorig_Spine"}};
    auto report = scene3d::retarget_model(model, options);
    REQUIRE(!report.ok());
    CHECK(report.error().message == "the humanoid profile has no bone 'Tail'");

    options.bone_map = {{"Hips", "pelvis"}};
    report = scene3d::retarget_model(model, options);
    REQUIRE(!report.ok());
    CHECK(report.error().message == "the model's skeleton has no bone 'pelvis'");

    options.bone_map = {{"Hips", "mixamorig_Hips"}, {"Spine", "mixamorig_Hips"}};
    report = scene3d::retarget_model(model, options);
    REQUIRE(!report.ok());
    CHECK(report.error().message == "bone 'mixamorig_Hips' is mapped twice, as 'Hips' and 'Spine'");

    options.bone_map = {{"Hips", "mixamorig_Hips"}};
    report = scene3d::retarget_model(model, options);
    REQUIRE(report.ok());
    CHECK(report.value().missing.size() == 16);

    scene3d::Model plain;
    plain.nodes.emplace_back();
    report = scene3d::retarget_model(plain, options);
    REQUIRE(!report.ok());
    CHECK(report.error().message == "the model has no skin to retarget");
}

TEST_CASE("motion_scale is the hips' height, for skinned models only") {
    const scene3d::Model model = mixamo_model();
    CHECK(scene3d::motion_scale(model) == doctest::Approx(0.0F)); // no node named Hips yet
    scene3d::Model retargeted = model;
    scene3d::RetargetOptions options;
    options.bone_map = scene3d::auto_map_bones(retargeted);
    REQUIRE(scene3d::retarget_model(retargeted, options).ok());
    CHECK(scene3d::motion_scale(retargeted) == doctest::Approx(1.0F));
    retargeted.skins.clear();
    CHECK(scene3d::motion_scale(retargeted) == doctest::Approx(0.0F));
}

TEST_CASE("write_glb keeps a retargeted model's skin and animations") {
    scene3d::Model model = mixamo_model();
    model.animations[0].loop = true;
    scene3d::RetargetOptions options;
    options.bone_map = scene3d::auto_map_bones(model);
    REQUIRE(scene3d::retarget_model(model, options).ok());
    const scene3d::ModelReader no_files = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no files");
    };
    auto glb = scene3d::write_glb(model, "hero.glb", no_files);
    REQUIRE(glb.ok());
    auto back = scene3d::load_model(glb.value(), "hero.glb", no_files);
    REQUIRE(back.ok());
    const scene3d::Model& loaded = back.value();
    REQUIRE(loaded.skins.size() == 1);
    CHECK(loaded.skins[0].joints.size() == model.skins[0].joints.size());
    REQUIRE(loaded.animations.size() == 1);
    CHECK(loaded.animations[0].name == "wave-loop");
    CHECK(loaded.animations[0].loop);
    CHECK(loaded.animations[0].tracks.size() == model.animations[0].tracks.size());
    REQUIRE(loaded.parts.size() == 1);
    CHECK(loaded.parts[0].skin == 0);
    CHECK(scene3d::motion_scale(loaded) == doctest::Approx(1.0F));

    // Posed, the vertices are where they were, though the loader may order them otherwise.
    const auto sorted = [](std::vector<Vec3> points) {
        std::ranges::sort(points, [](Vec3 a, Vec3 b) {
            return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
        });
        return points;
    };
    std::vector<scene3d::NodePose> pose = scene3d::rest_pose(loaded);
    scene3d::blend_animation(loaded, loaded, loaded.animations[0], 0.5, 1.0F, {}, pose);
    const auto ours = sorted(skinned(model, posed(model, 0.5)));
    const auto theirs = sorted(skinned(loaded, pose));
    REQUIRE(ours.size() == theirs.size());
    check_close(ours, theirs, 1e-4F);
}

TEST_CASE("rig_humanoid places a skeleton in an unrigged figure and keeps it where it was") {
    for (const bool t_pose : {true, false}) {
        INFO("t_pose " << t_pose);
        const scene3d::Model figure = test_humanoid::figure(t_pose);
        auto rigged = scene3d::rig_humanoid(figure);
        REQUIRE(rigged);
        scene3d::Model& model = rigged.value();
        REQUIRE(model.skins.size() == 1);
        CHECK(model.skins[0].joints.size() == 23);
        REQUIRE(model.parts.size() == 2);
        CHECK(model.parts[1].node == "Helmet");
        CHECK(model.parts[1].skin == 0);

        const auto globals = scene3d::node_globals(model, scene3d::rest_pose(model));
        CHECK(globals[node(model, "Hips")].origin.y == doctest::Approx(0.53F * 1.8F));
        const Vec3 hand = globals[node(model, "LeftHand")].origin;
        CHECK(hand.x > 0.0F); // the figure's left is +x, as it faces +z
        if (t_pose) {
            CHECK(hand.y == doctest::Approx(1.43F).epsilon(0.01));
            CHECK(hand.x == doctest::Approx(0.8F - (0.04F * 1.8F)).epsilon(0.01));
        } else {
            CHECK(hand.y == doctest::Approx((0.81F - 0.34F) * 1.8F).epsilon(0.01));
        }

        // At rest, the skin puts every vertex where the figure had it: the helmet's moved into
        // the model's space.
        const auto joints = scene3d::joint_transforms(model.skins[0], globals);
        for (std::size_t p = 0; p < figure.parts.size(); ++p) {
            const scene3d::ModelSurface& before = figure.meshes[p].surfaces[0];
            const scene3d::ModelSurface& after = model.meshes[p].surfaces[0];
            std::vector<slash::render::Vertex3D> placed;
            scene3d::skin_vertices(after, joints, placed);
            REQUIRE(placed.size() == before.vertices.size());
            for (std::size_t v = 0; v < placed.size(); ++v) {
                const Vec3 was = figure.parts[p].transform.apply({before.vertices[v].position[0],
                                                                  before.vertices[v].position[1],
                                                                  before.vertices[v].position[2]});
                CHECK(distance(was, {placed[v].position[0], placed[v].position[1],
                                     placed[v].position[2]}) < 1e-4F);
                const auto& weights = after.weights[v];
                CHECK(weights.weights[0] + weights.weights[1] == doctest::Approx(1.0F));
                // A vertex on the left of the figure follows no bone on its right.
                if (was.x > 0.05F) {
                    for (std::size_t w = 0; w < 2; ++w) {
                        if (weights.weights.at(w) > 0.0F) {
                            const std::string& bone =
                                model.nodes[model.skins[0].joints[weights.joints.at(w)]].name;
                            CHECK_FALSE(bone.starts_with("Right"));
                        }
                    }
                }
            }
        }

        // The hands' vertices follow the hands and forearms.
        if (t_pose) {
            const scene3d::ModelSurface& body = model.meshes[0].surfaces[0];
            bool found = false;
            for (std::size_t v = 0; v < body.vertices.size(); ++v) {
                if (body.vertices[v].position[0] > 0.79F) {
                    const std::string& bone =
                        model.nodes[model.skins[0].joints[body.weights[v].joints[0]]].name;
                    CHECK((bone == "LeftHand" || bone == "LeftLowerArm"));
                    found = true;
                }
            }
            CHECK(found);
        }

        // Its bones are the profile's, so it retargets by name with nothing required missing.
        scene3d::RetargetOptions options;
        for (const std::size_t joint : model.skins[0].joints) {
            options.bone_map[model.nodes[joint].name] = model.nodes[joint].name;
        }
        auto report = scene3d::retarget_model(model, options);
        REQUIRE(report);
        CHECK(report.value().missing.empty());
        CHECK(report.value().motion_scale == doctest::Approx(0.53F * 1.8F));
    }

    auto again = scene3d::rig_humanoid(scene3d::rig_humanoid(test_humanoid::figure(true)).value());
    REQUIRE_FALSE(again);
    CHECK(again.error().message == "the model is rigged already");
    REQUIRE_FALSE(scene3d::rig_humanoid(scene3d::Model{}));
}
