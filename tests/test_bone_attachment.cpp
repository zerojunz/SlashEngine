#include "gltf.hpp"

#include "slash/animation/animation.hpp"
#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <map>
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
constexpr float kQuarterTurn = std::numbers::pi_v<float> / 2.0F;

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

bool near(Vec3 a, Vec3 b, float tolerance = 1e-3F) {
    const Vec3 d = a - b;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z)) < tolerance;
}

// An arm of four bones a meter long, straight up from the origin: Shoulder, Elbow, Wrist, then
// Hand at its end, under a Rig node beside a quad mesh.
std::string arm_model() {
    slash::test::Gltf out = slash::test::quad();
    out.document["nodes"] = parse(R"([
        {"name": "Rig", "children": [1, 5]},
        {"name": "Shoulder", "children": [2]},
        {"name": "Elbow", "translation": [0, 1, 0], "children": [3]},
        {"name": "Wrist", "translation": [0, 1, 0], "children": [4]},
        {"name": "Hand", "translation": [0, 1, 0]},
        {"name": "Mesh", "mesh": 0}
    ])");
    out.document["scenes"] = parse(R"([{"nodes": [0]}])");
    return out.gltf();
}

// A world with the built-in components, models, the animation and modifier systems and 3D
// physics.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        scene3d::add_asset_types(assets);
        animation::install(world, assets);
        physics3d::install(world, {});
        files.files["models/arm.gltf"] = arm_model();
        world.set_file_reader(files.reader());
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

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    // Where an entity is in the world.
    [[nodiscard]] world::Affine3D global(std::string_view id) const {
        return world::Tree3D(world).global(world.find(id));
    }
    world::Transform3D& transform(std::string_view id) {
        return world.ecs().get<world::Transform3D>(world.find(id));
    }

    std::vector<std::string> problems() {
        std::vector<std::string> out;
        for (const scene3d::Problem& problem : scene3d::check_meshes(world)) {
            out.push_back(std::string(world.id_of(problem.entity)) + " " + problem.path + ": " +
                          problem.message);
        }
        return out;
    }
};

} // namespace

TEST_CASE("a bone attachment goes where its bone is posed, carrying what hangs under it") {
    Scene scene;
    scene.create("sword", R"({"Transform3D": {"position": [0, 0.5, 0]}, "AnimatableBody3D": {},
        "CollisionShape3D": {"size": [0.2, 1, 0.2]}})");
    scene.create("grip", R"({"Transform3D": {"children": ["sword"]},
        "BoneAttachment3D": {"bone_name": "Hand"}})");
    const entt::entity arm = scene.create("arm", R"({"Transform3D": {"position": [0, 1, 0],
        "children": ["grip"]}, "MeshInstance3D": {"mesh": "models/arm.gltf"}})");
    CHECK(scene.problems().empty());
    scene.step();

    // Its Transform3D is the hand's in the arm's space, so it is at the hand in the world.
    CHECK(near(scene.transform("grip").position, {0.0F, 3.0F, 0.0F}));
    CHECK(near(scene.global("grip").origin, {0.0F, 4.0F, 0.0F}));
    CHECK(near(scene.global("sword").origin, {0.0F, 4.5F, 0.0F}));

    // The shoulder turned a quarter turn about z by a Skeleton3D lays the arm along +x, and the
    // grip turns with the hand.
    scene.world.ecs().emplace<world::Skeleton3D>(arm).bone_rotations["Shoulder"] = {0.0F, 0.0F,
                                                                                    -kQuarterTurn};
    scene.step();
    const world::Affine3D grip = scene.global("grip");
    CHECK(near(grip.origin, {3.0F, 1.0F, 0.0F}));
    CHECK(near(grip.basis({0.0F, 1.0F, 0.0F}), {1.0F, 0.0F, 0.0F}));
    CHECK(near(scene.global("sword").origin, {3.5F, 1.0F, 0.0F}));

    // Physics finds the sword where the hand put it this tick.
    scene.step();
    const auto hit = physics3d::raycast(scene.world, {3.5F, 3.0F, 0.0F}, {3.5F, -1.0F, 0.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == scene.world.find("sword"));

    // A bone's scale scales it, as Godot's set_transform.
    scene.world.ecs().get<world::Skeleton3D>(arm).bone_scales["Hand"] = {2.0F, 2.0F, 2.0F};
    scene.step();
    CHECK(near(scene.transform("grip").scale, {2.0F, 2.0F, 2.0F}));
}

TEST_CASE("a bone attachment follows the bone after the IK modifiers turn it") {
    Scene scene;
    scene.create("goal", R"({"Transform3D": {"position": [1, 2, 0]}})");
    scene.create("pole", R"({"Transform3D": {"position": [5, 1, 0]}})");
    scene.create("grip", R"({"Transform3D": {}, "BoneAttachment3D": {"bone_name": "Wrist"}})");
    scene.create("arm", R"({"Transform3D": {"position": [0, 1, 0], "children": ["grip"]},
        "MeshInstance3D": {"mesh": "models/arm.gltf"},
        "TwoBoneIK3D": {"settings": [{"root_bone": "Shoulder", "middle_bone": "Elbow",
            "end_bone": "Wrist", "target": "goal", "pole": "pole"}]}})");
    scene.step();
    CHECK(near(scene.global("grip").origin, {1.0F, 2.0F, 0.0F}));
}

TEST_CASE("external_skeleton follows a model from anywhere in the world") {
    Scene scene;
    scene.create("arm", R"({"Transform3D": {"position": [2, 0, 0], "rotation": [0, 0, -1.5707964]},
        "MeshInstance3D": {"mesh": "models/arm.gltf"}})");
    scene.create("shield", R"({"Transform3D": {}, "BoneAttachment3D": {"bone_name": "Elbow",
        "external_skeleton": "arm"}})");
    scene.create("held", R"({"Transform3D": {}, "BoneAttachment3D": {"bone_name": "Hand",
        "external_skeleton": "arm"}})");
    scene.create("holder", R"({"Transform3D": {"position": [0, 5, 0], "scale": [2, 2, 2],
        "children": ["held"]}})");
    scene.step();
    CHECK(near(scene.global("shield").origin, {3.0F, 0.0F, 0.0F}));
    // Under a parent of its own, its Transform3D is what puts it at the bone in the world.
    CHECK(near(scene.global("held").origin, {5.0F, 0.0F, 0.0F}));
    CHECK(near(scene.transform("held").position, {2.5F, -2.5F, 0.0F}));
}

TEST_CASE("bone attachments are checked against their models, with did you mean") {
    Scene scene;
    scene.create("typo", R"({"Transform3D": {}, "BoneAttachment3D": {"bone_name": "Hnd"}})");
    scene.create("unnamed", R"({"Transform3D": {}, "BoneAttachment3D": {}})");
    scene.create("arm", R"({"Transform3D": {"children": ["typo", "unnamed"]},
        "MeshInstance3D": {"mesh": "models/arm.gltf"}})");
    scene.create("loose", R"({"Transform3D": {}, "BoneAttachment3D": {"bone_name": "Hand"}})");
    scene.create("crate", R"({"Transform3D": {"children": ["loose"]}, "MeshInstance3D": {},
        "BoxMesh": {}})");
    scene.create("far", R"({"Transform3D": {}, "BoneAttachment3D": {"bone_name": "Hand",
        "external_skeleton": "crate"}})");
    CHECK(scene.problems() ==
          std::vector<std::string>{
              "typo /components/BoneAttachment3D/bone_name: \"models/arm.gltf\" has no bone or "
              "node named \"Hnd\"; did you mean \"Hand\"?",
              "unnamed /components/BoneAttachment3D/bone_name: names no bone",
              "loose /components/BoneAttachment3D: follows a bone, but its Transform3D parent "
              "draws no model file: put it in the Transform3D children of the MeshInstance3D's "
              "entity, or name that entity in external_skeleton",
              "far /components/BoneAttachment3D/external_skeleton: follows an entity that draws "
              "no model file: name the entity whose MeshInstance3D draws the model",
          });

    // Ones that can't follow stay where they are.
    scene.step();
    CHECK(near(scene.transform("typo").position, {0.0F, 0.0F, 0.0F}));
    CHECK(near(scene.transform("loose").position, {0.0F, 0.0F, 0.0F}));
}
