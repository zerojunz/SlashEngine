#include "gltf.hpp"

#include "slash/animation/animation.hpp"
#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/ik.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/physical_bones.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/physical_bones.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace animation = slash::animation;
namespace asset = slash::asset;
namespace json = slash::json;
namespace physics3d = slash::physics3d;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3;

namespace {

constexpr double kTick = 1.0 / 60.0;

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }

    world::FileReader reader() {
        return [this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        };
    }
};

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}
float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

bool near(Vec3 a, Vec3 b, float tolerance = 1e-3F) {
    return length(a - b) < tolerance;
}

// An arm of five bones a meter long, straight up from the origin: Shoulder, Elbow, Wrist, Hand,
// then Tip at its end, under a Rig node beside a quad mesh.
std::string arm_model() {
    slash::test::Gltf out = slash::test::quad();
    out.document["nodes"] = parse(R"([
        {"name": "Rig", "children": [1, 6]},
        {"name": "Shoulder", "children": [2]},
        {"name": "Elbow", "translation": [0, 1, 0], "children": [3]},
        {"name": "Wrist", "translation": [0, 1, 0], "children": [4]},
        {"name": "Hand", "translation": [0, 1, 0], "children": [5]},
        {"name": "Tip", "translation": [0, 1, 0]},
        {"name": "Mesh", "mesh": 0}
    ])");
    out.document["scenes"] = parse(R"([{"nodes": [0]}])");
    return out.gltf();
}

// A world with the built-in components, models, the animation and modifier systems and 3D
// physics, with a floor whose top is at y 0.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    scene3d::ModelLoader load =
        [this](const std::string& path) -> slash::Result<std::shared_ptr<const scene3d::Model>> {
        auto found = assets.load<scene3d::Model>(path);
        if (!found) {
            return found.error();
        }
        return found.value().share();
    };

    Scene() {
        world::register_builtin_components(components);
        scene3d::add_asset_types(assets);
        animation::install(world, assets);
        physics3d::install(world, {});
        files.files["models/arm.gltf"] = arm_model();
        world.set_file_reader(files.reader());
        create("floor", R"({"Transform3D": {"position": [0, -1, 0]}, "StaticBody3D": {},
            "CollisionShape3D": {"size": [20, 2, 20]}})");
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), path);
        }
        return entity;
    }

    // The arm at position, turned about z, with a simulator whose physical bones are capsules
    // along the shoulder, elbow and wrist, each held to the one above by a joint of joint_json.
    entt::entity arm(Vec3 position, float turn,
                     std::string_view joint_json = R"("joint_type": "pin")") {
        const std::string joint(joint_json);
        const auto bone = [&](std::string_view id, std::string_view name) {
            create(id, R"({"Transform3D": {}, "CollisionShape3D": {"shape": "capsule",
                "radius": 0.1, "height": 0.6}, "PhysicalBone3D": {"bone_name": ")" +
                           std::string(name) + R"(", "body_offset_position": [0, 0.5, 0], )" +
                           joint + "}}");
        };
        bone("shoulder", "Shoulder");
        bone("elbow", "Elbow");
        bone("wrist", "Wrist");
        create("ragdoll", R"({"Transform3D": {"children": ["shoulder", "elbow", "wrist"]},
            "PhysicalBoneSimulator3D": {}})");
        const entt::entity entity = create("arm", R"({"Transform3D": {"children": ["ragdoll"]},
            "MeshInstance3D": {"mesh": "models/arm.gltf"}})");
        auto& transform = world.ecs().get<world::Transform3D>(entity);
        transform.position = position;
        transform.rotation = {0.0F, 0.0F, turn};
        return entity;
    }

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    world::PhysicalBoneSimulator3D& simulator() {
        return world.ecs().get<world::PhysicalBoneSimulator3D>(world.find("ragdoll"));
    }

    // Where an entity is in the world.
    world::Affine3D global(std::string_view id) const {
        return world::Tree3D(world).global(world.find(id));
    }
    // The ends of a physical bone's capsule's bone: where the bone starts and where it ends.
    std::pair<Vec3, Vec3> ends(std::string_view id) {
        const world::Affine3D body = global(id);
        return {body.apply({0.0F, -0.5F, 0.0F}), body.apply({0.0F, 0.5F, 0.0F})};
    }
    bool top_level(std::string_view id) {
        return world.ecs().get<world::Transform3D>(world.find(id)).top_level;
    }

    // The arm's bones in the world, as drawn.
    std::map<std::string, Vec3> positions(entt::entity entity) {
        const auto& instance = world.ecs().get<world::MeshInstance3D>(entity);
        auto model = load(instance.mesh);
        REQUIRE(model.ok());
        std::vector<scene3d::NodePose> pose =
            scene3d::pose_of(*model.value(), scene3d::clips(world, entity),
                             world.ecs().try_get<world::Skeleton3D>(entity), load);
        const world::Tree3D tree(world);
        const world::Affine3D place = tree.global(entity);
        scene3d::apply_modifiers(
            world, entity, *model.value(), place,
            [&tree](entt::entity e) { return tree.global(e); }, pose);
        const std::vector<world::Affine3D> globals = scene3d::node_globals(*model.value(), pose);
        std::map<std::string, Vec3> out;
        for (std::size_t i = 0; i < globals.size(); ++i) {
            out[model.value()->nodes[i].name] = (place * globals[i]).origin;
        }
        return out;
    }

    // The mode each physical bone takes in physics, by ID.
    std::map<std::string, world::PhysicalBoneMode> modes() const {
        std::map<std::string, world::PhysicalBoneMode> out;
        for (const auto& [entity, link] : world::physical_bone_links(world)) {
            out[std::string(world.id_of(entity))] = link.mode;
        }
        return out;
    }
};

using Mode = world::PhysicalBoneMode;

} // namespace

TEST_CASE("physical bones follow their bones while the simulator is active") {
    Scene scene;
    const entt::entity arm = scene.arm({0.0F, 1.0F, 0.0F}, 0.0F);
    CHECK(scene3d::check_meshes(scene.world).empty());
    scene.step();

    // Each body is half way along its bone, and each joins the one of the bone above.
    CHECK(near(scene.global("shoulder").origin, {0.0F, 1.5F, 0.0F}));
    CHECK(near(scene.global("elbow").origin, {0.0F, 2.5F, 0.0F}));
    CHECK(near(scene.global("wrist").origin, {0.0F, 3.5F, 0.0F}));
    CHECK(scene.modes() == std::map<std::string, Mode>{{"elbow", Mode::Follow},
                                                       {"shoulder", Mode::Follow},
                                                       {"wrist", Mode::Follow}});
    std::map<std::string, entt::entity> parents;
    for (const auto& [entity, link] : world::physical_bone_links(scene.world)) {
        parents[std::string(scene.world.id_of(entity))] = link.parent;
    }
    CHECK(parents["shoulder"] == entt::entity{entt::null});
    CHECK(parents["elbow"] == scene.world.find("shoulder"));
    CHECK(parents["wrist"] == scene.world.find("elbow"));

    // Following bodies don't fall, and the pose is the model's.
    scene.step(30);
    CHECK(near(scene.global("wrist").origin, {0.0F, 3.5F, 0.0F}));
    CHECK(!scene.top_level("wrist"));
    CHECK(near(scene.positions(arm)["Hand"], {0.0F, 4.0F, 0.0F}));

    // They turn with their bones.
    scene.world.ecs().emplace<world::Skeleton3D>(arm).bone_rotations["Shoulder"] = {
        0.0F, 0.0F, -std::numbers::pi_v<float> / 2.0F};
    scene.step();
    const world::Affine3D shoulder = scene.global("shoulder");
    CHECK(near(shoulder.origin, {0.5F, 1.0F, 0.0F}));
    CHECK(near(shoulder.basis({0.0F, 1.0F, 0.0F}), {1.0F, 0.0F, 0.0F}));
    CHECK(near(scene.global("wrist").origin, {2.5F, 1.0F, 0.0F}));

    // A crate lands on them, since bodies that follow collide.
    scene.create("crate", R"({"Transform3D": {"position": [1.5, 3, 0]}, "RigidBody3D": {},
        "CollisionShape3D": {"size": [0.4, 0.4, 0.4]}})");
    scene.step(90);
    CHECK(std::abs(scene.global("crate").origin.y - 1.3F) < 0.02F);
}

TEST_CASE("a ragdoll falls when it simulates, and its bones go where their bodies are") {
    Scene scene;
    const entt::entity arm = scene.arm({0.0F, 1.0F, 0.0F}, -1.2F);
    scene.step();
    scene.simulator().simulating = true;
    scene.step();
    CHECK(scene.modes() == std::map<std::string, Mode>{{"elbow", Mode::Simulate},
                                                       {"shoulder", Mode::Simulate},
                                                       {"wrist", Mode::Simulate}});
    CHECK(scene.top_level("shoulder"));
    CHECK(scene.top_level("wrist"));

    scene.step(180);
    for (const std::string_view id : {"shoulder", "elbow", "wrist"}) {
        CAPTURE(id);
        // Each capsule lies on the floor.
        CHECK(std::abs(scene.global(id).origin.y - 0.1F) < 0.02F);
    }
    // The joints hold the arm together.
    CHECK(near(scene.ends("shoulder").second, scene.ends("elbow").first, 0.02F));
    CHECK(near(scene.ends("elbow").second, scene.ends("wrist").first, 0.02F));

    // The bones are where their bodies are, and the bones under them follow.
    auto at = scene.positions(arm);
    CHECK(near(at["Shoulder"], scene.ends("shoulder").first));
    CHECK(near(at["Elbow"], scene.ends("elbow").first));
    CHECK(near(at["Wrist"], scene.ends("wrist").first));
    CHECK(near(at["Hand"], scene.ends("wrist").second));
    CHECK(std::abs(length(at["Tip"] - at["Hand"]) - 1.0F) < 1e-3F);

    // At half influence the shoulder goes half way between its pose and its body.
    scene.simulator().influence = 0.5F;
    const Vec3 posed = scene.global("arm").origin;
    at = scene.positions(arm);
    CHECK(near(at["Shoulder"], (posed + scene.ends("shoulder").first) * 0.5F));
    scene.simulator().influence = 1.0F;

    // Stopping puts the bodies back on the pose.
    scene.simulator().simulating = false;
    scene.step();
    CHECK(!scene.top_level("shoulder"));
    CHECK(near(scene.ends("shoulder").first, posed));
    CHECK(near(scene.positions(arm)["Elbow"], scene.ends("elbow").first));
}

TEST_CASE("simulated_bones simulates the bones it names and those under them") {
    Scene scene;
    const entt::entity arm = scene.arm({0.0F, 1.0F, 0.0F}, -0.2F);
    scene.step();
    scene.simulator().simulating = true;
    scene.simulator().simulated_bones = {"Elbow"};
    scene.step();
    CHECK(scene.modes() == std::map<std::string, Mode>{{"elbow", Mode::Simulate},
                                                       {"shoulder", Mode::Follow},
                                                       {"wrist", Mode::Simulate}});
    const world::Affine3D shoulder = scene.global("shoulder");

    // The forearm falls and hangs from the shoulder, which keeps to its pose.
    scene.step(120);
    CHECK(!scene.top_level("shoulder"));
    CHECK(scene.top_level("elbow"));
    CHECK(near(scene.global("shoulder").origin, shoulder.origin));
    CHECK(near(scene.ends("shoulder").second, scene.ends("elbow").first, 0.02F));
    CHECK(scene.ends("wrist").second.y < scene.ends("shoulder").second.y);
    auto at = scene.positions(arm);
    CHECK(near(at["Elbow"], scene.ends("elbow").first));
    CHECK(near(at["Wrist"], scene.ends("wrist").first));
}

TEST_CASE("physical bone joints keep to their limits") {
    // A hinge about z that bends at most half a radian: the forearm falls and stops there.
    Scene scene;
    scene.arm({0.0F, 1.0F, 0.0F}, 0.0F,
              R"("joint_type": "hinge", "angular_limit_enabled": true,
                 "angular_limit_upper": 0.5, "angular_limit_lower": -0.5)");
    scene.world.ecs().emplace<world::Skeleton3D>(scene.world.find("arm")).bone_rotations["Elbow"] =
        {0.0F, 0.0F, -0.1F};
    scene.step();
    scene.simulator().simulating = true;
    scene.simulator().simulated_bones = {"Elbow"};
    scene.step(120);
    const auto [shoulder_start, shoulder_end] = scene.ends("shoulder");
    const auto [elbow_start, elbow_end] = scene.ends("elbow");
    const Vec3 upper = shoulder_end - shoulder_start;
    const Vec3 fore = elbow_end - elbow_start;
    const float bend = std::acos(std::clamp(dot(upper, fore), -1.0F, 1.0F));
    // Half a radian past the 0.1 it was bent when the joint was made.
    CHECK(bend > 0.55F);
    CHECK(bend < 0.65F);
    // A hinge about z keeps the forearm in the plane it bends in.
    CHECK(std::abs(fore.z) < 1e-3F);

    // A pin lets it fall all the way.
    Scene pinned;
    pinned.arm({0.0F, 1.0F, 0.0F}, 0.0F);
    pinned.world.ecs()
        .emplace<world::Skeleton3D>(pinned.world.find("arm"))
        .bone_rotations["Elbow"] = {0.0F, 0.0F, -0.1F};
    pinned.step();
    pinned.simulator().simulating = true;
    pinned.simulator().simulated_bones = {"Elbow"};
    pinned.step(120);
    const Vec3 hanging = pinned.ends("elbow").second - pinned.ends("elbow").first;
    CHECK(hanging.y < 0.0F);
}

TEST_CASE("a simulator that isn't active turns its physical bones off") {
    Scene scene;
    const entt::entity arm = scene.arm({0.0F, 1.0F, 0.0F}, 0.0F);
    scene.step();
    scene.simulator().active = false;
    scene.step();
    CHECK(scene.modes() == std::map<std::string, Mode>{{"elbow", Mode::Off},
                                                       {"shoulder", Mode::Off},
                                                       {"wrist", Mode::Off}});
    // The bodies stay where they were while the bones turn, and nothing collides with them.
    scene.world.ecs().emplace<world::Skeleton3D>(arm).bone_rotations["Shoulder"] = {
        0.0F, 0.0F, -std::numbers::pi_v<float> / 2.0F};
    scene.step();
    CHECK(near(scene.global("elbow").origin, {0.0F, 2.5F, 0.0F}));
    CHECK(!physics3d::raycast(scene.world, {-1.0F, 2.5F, 0.0F}, {1.0F, 2.5F, 0.0F}).has_value());
    // Simulating, the bodies fall, but the bones keep to the pose while it isn't active.
    scene.simulator().simulating = true;
    scene.step(10);
    CHECK(scene.modes().at("elbow") == Mode::Simulate);
    CHECK(scene.global("elbow").origin.y < 2.4F);
    CHECK(near(scene.positions(arm)["Elbow"], {1.0F, 1.0F, 0.0F}));

    // Active again, they collide.
    scene.simulator().active = true;
    scene.simulator().simulating = false;
    scene.step();
    const auto hit = physics3d::raycast(scene.world, {1.5F, 3.0F, 0.0F}, {1.5F, 0.5F, 0.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == scene.world.find("elbow"));
}

TEST_CASE("physical bones are checked against the model, with did you mean") {
    Scene scene;
    scene.create("upper", R"({"Transform3D": {}, "PhysicalBone3D": {"bone_name": "Shoulder"}})");
    scene.create("again", R"({"Transform3D": {}, "PhysicalBone3D": {"bone_name": "Shoulder"}})");
    scene.create("fore", R"({"Transform3D": {}, "PhysicalBone3D": {"bone_name": "Elbw"}})");
    scene.create("unnamed", R"({"Transform3D": {}, "PhysicalBone3D": {}})");
    scene.create("ragdoll", R"({"Transform3D": {"children": ["upper", "again", "fore", "unnamed"]},
        "PhysicalBoneSimulator3D": {"simulated_bones": ["Wrst"]}})");
    scene.create("arm", R"({"Transform3D": {"children": ["ragdoll"]},
        "MeshInstance3D": {"mesh": "models/arm.gltf"}})");
    scene.create("loose", R"({"Transform3D": {}, "PhysicalBone3D": {"bone_name": "Hand"}})");
    std::vector<std::string> problems;
    for (const scene3d::Problem& problem : scene3d::check_meshes(scene.world)) {
        problems.push_back(std::string(scene.world.id_of(problem.entity)) + " " + problem.path +
                           ": " + problem.message);
    }
    CHECK(problems ==
          std::vector<std::string>{
              "ragdoll /components/PhysicalBoneSimulator3D/simulated_bones/0: the model has no "
              "bone or node named \"Wrst\"; did you mean \"Wrist\"?",
              "again /components/PhysicalBone3D/bone_name: \"Shoulder\" already has a physical "
              "bone: \"upper\"",
              "fore /components/PhysicalBone3D/bone_name: the model has no bone or node named "
              "\"Elbw\"; did you mean \"Elbow\"?",
              "unnamed /components/PhysicalBone3D/bone_name: names no bone: name the model's "
              "bone it moves",
              "loose /components/PhysicalBone3D: isn't in the Transform3D children of a "
              "PhysicalBoneSimulator3D's entity, so it is a static body that never moves: put it "
              "there",
          });

    // Bones a simulator can't bind are off; one outside any simulator is a static body.
    scene.step();
    CHECK(scene.modes() == std::map<std::string, Mode>{{"again", Mode::Off},
                                                       {"fore", Mode::Off},
                                                       {"unnamed", Mode::Off},
                                                       {"upper", Mode::Follow}});
}
