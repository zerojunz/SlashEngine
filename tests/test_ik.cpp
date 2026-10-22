#include "gltf.hpp"

#include "slash/animation/animation.hpp"
#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/scene3d/ik.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace animation = slash::animation;
namespace asset = slash::asset;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3;

namespace {

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

// A world with the built-in components, models and the animation and modifier systems.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        scene3d::add_asset_types(assets);
        animation::install(world, assets);
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

    // Places an entity in the world.
    void move(entt::entity entity, Vec3 position) {
        world.ecs().get<world::Transform3D>(entity).position = position;
    }

    // The arm's bones in the world, as drawn: animated, then turned by its modifiers.
    std::map<std::string, world::Affine3D> bones(entt::entity entity) {
        const auto& instance = world.ecs().get<world::MeshInstance3D>(entity);
        auto model = assets.load<scene3d::Model>(instance.mesh);
        REQUIRE(model.ok());
        const scene3d::ModelLoader load =
            [&](const std::string& path) -> slash::Result<std::shared_ptr<const scene3d::Model>> {
            auto found = assets.load<scene3d::Model>(path);
            if (!found) {
                return found.error();
            }
            return found.value().share();
        };
        std::vector<scene3d::NodePose> pose =
            scene3d::pose_of(*model.value(), scene3d::clips(world, entity),
                             world.ecs().try_get<world::Skeleton3D>(entity), load);
        const world::Tree3D tree(world);
        const world::Affine3D place = tree.global(entity);
        scene3d::apply_modifiers(
            world, entity, *model.value(), place,
            [&tree](entt::entity e) { return tree.global(e); }, pose);
        const std::vector<world::Affine3D> globals = scene3d::node_globals(*model.value(), pose);
        std::map<std::string, world::Affine3D> out;
        for (std::size_t i = 0; i < globals.size(); ++i) {
            out[model.value()->nodes[i].name] = place * globals[i];
        }
        return out;
    }
    std::map<std::string, Vec3> positions(entt::entity entity) {
        std::map<std::string, Vec3> out;
        for (const auto& [name, transform] : bones(entity)) {
            out[name] = transform.origin;
        }
        return out;
    }
};

// Each bone of the chain from Shoulder keeps its meter.
void check_lengths(const std::map<std::string, Vec3>& at, const std::vector<std::string>& chain) {
    for (std::size_t i = 1; i < chain.size(); ++i) {
        CAPTURE(chain[i]);
        CHECK(std::abs(length(at.at(chain[i]) - at.at(chain[i - 1])) - 1.0F) < 1e-3F);
    }
}

} // namespace

TEST_CASE("TwoBoneIK3D bends two bones so the end reaches the target, toward the pole") {
    Scene scene;
    const entt::entity goal = scene.create("goal", R"({"Transform3D": {"position": [1, 1, 0]}})");
    const entt::entity pole = scene.create("pole", R"({"Transform3D": {"position": [2, -1, 0]}})");
    const entt::entity arm = scene.create("arm", R"({"Transform3D": {},
        "MeshInstance3D": {"mesh": "models/arm.gltf"},
        "TwoBoneIK3D": {"settings": [{"root_bone": "Shoulder", "middle_bone": "Elbow",
            "end_bone": "Wrist", "target": "goal", "pole": "pole"}]}})");
    CHECK(scene3d::has_modifiers(scene.world, arm));
    CHECK(scene3d::check_meshes(scene.world).empty());

    // The wrist is on the target, and the elbow where the two circles meet on the pole's side.
    auto at = scene.positions(arm);
    CHECK(near(at["Shoulder"], {0.0F, 0.0F, 0.0F}));
    CHECK(near(at["Elbow"], {1.0F, 0.0F, 0.0F}));
    CHECK(near(at["Wrist"], {1.0F, 1.0F, 0.0F}));
    check_lengths(at, {"Shoulder", "Elbow", "Wrist", "Hand", "Tip"});
    // The bones below the end follow it.
    CHECK(near(at["Hand"] - at["Wrist"], at["Wrist"] - at["Elbow"]));

    // The other side's pole bends it the other way.
    scene.move(pole, {-1.0F, 2.0F, 0.0F});
    at = scene.positions(arm);
    CHECK(near(at["Elbow"], {0.0F, 1.0F, 0.0F}));
    CHECK(near(at["Wrist"], {1.0F, 1.0F, 0.0F}));

    // Out of reach, the two bones point straight at the target.
    scene.move(goal, {3.0F, 0.0F, 0.0F});
    at = scene.positions(arm);
    CHECK(near(at["Elbow"], {1.0F, 0.0F, 0.0F}));
    CHECK(near(at["Wrist"], {2.0F, 0.0F, 0.0F}));

    // Targets are found in the world, so moving the arm moves where they are from it.
    scene.move(goal, {1.0F, 1.0F, 0.0F});
    scene.move(pole, {2.0F, -1.0F, 0.0F});
    scene.world.ecs().get<world::Transform3D>(arm).position = {0.0F, 0.0F, 5.0F};
    scene.move(goal, {1.0F, 1.0F, 5.0F});
    scene.move(pole, {2.0F, -1.0F, 5.0F});
    at = scene.positions(arm);
    CHECK(near(at["Elbow"], {1.0F, 0.0F, 5.0F}));
    CHECK(near(at["Wrist"], {1.0F, 1.0F, 5.0F}));

    // With a pole direction, the middle bone rolls so that axis faces the pole.
    scene.world.ecs().get<world::Transform3D>(arm).position = {};
    scene.move(goal, {1.0F, 1.0F, 0.0F});
    scene.move(pole, {2.0F, -1.0F, 0.0F});
    auto& setting = scene.world.ecs().get<world::TwoBoneIK3D>(arm).settings[0];
    setting.pole_direction = "plus_z";
    const auto posed = scene.bones(arm);
    const world::Affine3D& elbow = posed.at("Elbow");
    const Vec3 z = elbow.apply({0.0F, 0.0F, 1.0F}) - elbow.origin;
    CHECK(std::abs(z.z) < 1e-3F);
    CHECK(dot(z, Vec3{2.0F, -1.0F, 0.0F} - elbow.origin) > 0.0F);
    CHECK(near(elbow.origin, {1.0F, 0.0F, 0.0F}));
    CHECK(near(posed.at("Wrist").origin, {1.0F, 1.0F, 0.0F}));
}

TEST_CASE("TwoBoneIK3D reaches with a point past its end bone") {
    Scene scene;
    scene.create("goal", R"({"Transform3D": {"position": [1, 0.5, 0]}})");
    scene.create("pole", R"({"Transform3D": {"position": [2, -1, 0]}})");
    // A virtual end a meter past Elbow: Elbow is the middle and the end is where Wrist is.
    const entt::entity arm = scene.create("arm", R"({"Transform3D": {},
        "MeshInstance3D": {"mesh": "models/arm.gltf"},
        "TwoBoneIK3D": {"settings": [{"root_bone": "Shoulder", "middle_bone": "Elbow",
            "end_bone": "Elbow", "use_virtual_end": true, "end_bone_length": 1,
            "target": "goal", "pole": "pole"}]}})");
    CHECK(scene3d::check_meshes(scene.world).empty());
    const auto at = scene.positions(arm);
    CHECK(near(at.at("Wrist"), {1.0F, 0.5F, 0.0F}));
    check_lengths(at, {"Shoulder", "Elbow", "Wrist"});
}

TEST_CASE("FABRIK3D, CCDIK3D and JacobianIK3D carry their chains to the target tick by tick") {
    for (const std::string kind : {"FABRIK3D", "CCDIK3D", "JacobianIK3D"}) {
        CAPTURE(kind);
        Scene scene;
        scene.create("goal", R"({"Transform3D": {"position": [2, 2, 0]}})");
        const entt::entity arm =
            scene.create("arm",
                         R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/arm.gltf"},
            ")" + kind + R"(": {"settings": [{"root_bone": "Shoulder", "end_bone": "Tip",
                "target": "goal"}]}})");
        CHECK(scene3d::check_meshes(scene.world).empty());

        // Godot's solvers turn each joint at most angular_delta_limit a step, max_iterations
        // steps a tick, so the chain gets there over ticks rather than at once.
        const Vec3 rest = scene.positions(arm).at("Tip");
        CHECK(near(rest, {0.0F, 4.0F, 0.0F}));
        scene.world.step(1.0 / 60.0);
        const Vec3 first = scene.positions(arm).at("Tip");
        CHECK(length(first - Vec3{2.0F, 2.0F, 0.0F}) < length(rest - Vec3{2.0F, 2.0F, 0.0F}));
        CHECK(length(first - Vec3{2.0F, 2.0F, 0.0F}) > 0.01F);
        for (int i = 0; i < 300; ++i) {
            scene.world.step(1.0 / 60.0);
        }
        const auto at = scene.positions(arm);
        CHECK(length(at.at("Tip") - Vec3{2.0F, 2.0F, 0.0F}) < 0.01F);
        check_lengths(at, {"Shoulder", "Elbow", "Wrist", "Hand", "Tip"});
        // Asking again doesn't step it.
        CHECK(near(scene.positions(arm).at("Tip"), at.at("Tip"), 1e-6F));
    }
}

TEST_CASE("deterministic chains solve from the pose each time") {
    for (const std::string kind : {"FABRIK3D", "CCDIK3D", "JacobianIK3D"}) {
        CAPTURE(kind);
        Scene scene;
        scene.create("goal", R"({"Transform3D": {"position": [2, 2, 0]}})");
        const entt::entity arm =
            scene.create("arm",
                         R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/arm.gltf"},
            ")" + kind + R"(": {"deterministic": true, "max_iterations": 200,
                "angular_delta_limit": 3.14, "settings": [{"root_bone": "Shoulder",
                "end_bone": "Tip", "target": "goal"}]}})");
        // No ticks: one solve from the rest pose gets there, and gives the same each time.
        const auto at = scene.positions(arm);
        CHECK(length(at.at("Tip") - Vec3{2.0F, 2.0F, 0.0F}) < 0.01F);
        check_lengths(at, {"Shoulder", "Elbow", "Wrist", "Hand", "Tip"});
        scene.world.step(1.0 / 60.0);
        const auto again = scene.positions(arm);
        for (const auto& [name, position] : at) {
            CAPTURE(name);
            CHECK(near(again.at(name), position, 1e-6F));
        }
    }
}

TEST_CASE("chain joints keep to their rotation axes and cones") {
    for (const std::string kind : {"FABRIK3D", "CCDIK3D", "JacobianIK3D"}) {
        CAPTURE(kind);
        Scene scene;
        // Off the arm's plane: joints that only turn about z stay in it.
        scene.create("goal", R"({"Transform3D": {"position": [2, 2, 1]}})");
        const entt::entity arm =
            scene.create("arm",
                         R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/arm.gltf"},
            ")" + kind + R"(": {"settings": [{"root_bone": "Shoulder", "end_bone": "Tip",
                "target": "goal", "joints": {"Shoulder": {"rotation_axis": "z"},
                "Elbow": {"rotation_axis": "z"}, "Wrist": {"rotation_axis": "z"},
                "Hand": {"rotation_axis": "z"}}}]}})");
        CHECK(scene3d::check_meshes(scene.world).empty());
        for (int i = 0; i < 300; ++i) {
            scene.world.step(1.0 / 60.0);
        }
        auto at = scene.positions(arm);
        for (const std::string bone : {"Elbow", "Wrist", "Hand", "Tip"}) {
            CAPTURE(bone);
            CHECK(std::abs(at[bone].z) < 1e-3F);
        }
        CHECK(at["Tip"].x > 1.0F);
        check_lengths(at, {"Shoulder", "Elbow", "Wrist", "Hand", "Tip"});
    }
}

TEST_CASE("a cone keeps a chain joint's bone near the way it points at rest") {
    for (const std::string kind : {"FABRIK3D", "CCDIK3D", "JacobianIK3D"}) {
        CAPTURE(kind);
        Scene scene;
        scene.create("goal", R"({"Transform3D": {"position": [3, 0.5, 0]}})");
        // Half a radian across, so the shoulder's bone stays within a quarter radian of up.
        const entt::entity arm =
            scene.create("arm",
                         R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/arm.gltf"},
            ")" + kind + R"(": {"settings": [{"root_bone": "Shoulder", "end_bone": "Tip",
                "target": "goal", "joints": {"Shoulder": {"limitation": "cone",
                "limitation_angle": 0.5}}}]}})");
        CHECK(scene3d::check_meshes(scene.world).empty());
        for (int i = 0; i < 300; ++i) {
            scene.world.step(1.0 / 60.0);
        }
        const auto at = scene.positions(arm);
        const Vec3 upper = at.at("Elbow") - at.at("Shoulder");
        CHECK(dot(upper, {0.0F, 1.0F, 0.0F}) / length(upper) >= std::cos(0.25F) - 1e-3F);
        // The joints below bend the rest of the way.
        CHECK(at.at("Tip").x > 1.5F);
        check_lengths(at, {"Shoulder", "Elbow", "Wrist", "Hand", "Tip"});
    }
}

TEST_CASE("influence blends a modifier's result into the pose, and inactive ones do nothing") {
    Scene scene;
    scene.create("goal", R"({"Transform3D": {"position": [3, 0, 0]}})");
    scene.create("pole", R"({"Transform3D": {"position": [2, -1, 0]}})");
    const entt::entity arm = scene.create("arm", R"({"Transform3D": {},
        "MeshInstance3D": {"mesh": "models/arm.gltf"},
        "TwoBoneIK3D": {"influence": 0.5, "settings": [{"root_bone": "Shoulder",
            "middle_bone": "Elbow", "end_bone": "Wrist", "target": "goal", "pole": "pole"}]}})");
    // Pointing along x at full influence; halfway, the shoulder turns an eighth of a turn.
    auto at = scene.positions(arm);
    CHECK(near(at["Elbow"], {std::sqrt(0.5F), std::sqrt(0.5F), 0.0F}));
    check_lengths(at, {"Shoulder", "Elbow", "Wrist", "Hand", "Tip"});

    scene.world.ecs().get<world::TwoBoneIK3D>(arm).active = false;
    at = scene.positions(arm);
    CHECK(near(at["Elbow"], {0.0F, 1.0F, 0.0F}));
    CHECK(near(at["Tip"], {0.0F, 4.0F, 0.0F}));
}

TEST_CASE("modifiers run on the drawing entity, then on its Transform3D children, in order") {
    Scene scene;
    scene.create("left", R"({"Transform3D": {"position": [-3, 0, 0]}})");
    scene.create("right", R"({"Transform3D": {"position": [3, 0, 0]}})");
    scene.create("pole", R"({"Transform3D": {"position": [0, -1, 0]}})");
    const std::string two_bone = R"({"settings": [{"root_bone": "Shoulder",
        "middle_bone": "Elbow", "end_bone": "Wrist", "target": "TARGET", "pole": "pole"}]})";
    const auto with_target = [&](std::string target) {
        std::string out = two_bone;
        out.replace(out.find("TARGET"), 6, target);
        return out;
    };
    const entt::entity first =
        scene.create("first", R"({"Transform3D": {}, "TwoBoneIK3D": )" + with_target("left") + "}");
    const entt::entity second = scene.create("second", R"({"Transform3D": {}, "TwoBoneIK3D": )" +
                                                           with_target("right") + "}");
    const entt::entity arm = scene.create("arm", R"({"Transform3D": {"children": ["first",
        "second"]}, "MeshInstance3D": {"mesh": "models/arm.gltf"}})");
    CHECK(scene3d::has_modifiers(scene.world, arm));
    // The last child's wins.
    CHECK(near(scene.positions(arm)["Wrist"], {2.0F, 0.0F, 0.0F}));
    scene.world.ecs().get<world::Transform3D>(arm).children = {second, first};
    CHECK(near(scene.positions(arm)["Wrist"], {-2.0F, 0.0F, 0.0F}));
    // The drawing entity's own run before its children's.
    scene.world.ecs().emplace<world::TwoBoneIK3D>(arm) =
        scene.world.ecs().get<world::TwoBoneIK3D>(second);
    scene.world.ecs().get<world::Transform3D>(arm).children = {first};
    CHECK(near(scene.positions(arm)["Wrist"], {-2.0F, 0.0F, 0.0F}));
    scene.world.ecs().get<world::Transform3D>(arm).children = {};
    CHECK(near(scene.positions(arm)["Wrist"], {2.0F, 0.0F, 0.0F}));
    CHECK(scene3d::check_meshes(scene.world).size() == 2); // first and second are strays now
}

TEST_CASE("modifier settings are checked against the model, with did you mean") {
    Scene scene;
    scene.create("goal", R"({"Transform3D": {}})");
    scene.create("arm", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/arm.gltf"},
        "TwoBoneIK3D": {"settings": [
            {"root_bone": "Shoulder", "middle_bone": "Elbw", "end_bone": "Wrist",
             "target": "goal"},
            {"root_bone": "Shoulder", "middle_bone": "Wrist", "end_bone": "Elbow",
             "target": "goal", "pole_direction": "plus_w"}]},
        "CCDIK3D": {"settings": [{"root_bone": "Shoulder", "end_bone": "Wrist",
            "joints": {"Hand": {}, "Elbow": {"rotation_axis": "w"}}}]}})");
    scene.create("loose", R"({"Transform3D": {}, "FABRIK3D": {}})");
    std::vector<std::string> problems;
    for (const scene3d::Problem& problem : scene3d::check_meshes(scene.world)) {
        problems.push_back(std::string(scene.world.id_of(problem.entity)) + " " + problem.path +
                           ": " + problem.message);
    }
    CHECK(problems ==
          std::vector<std::string>{
              "arm /components/TwoBoneIK3D/settings/0/middle_bone: the model has no bone or node "
              "named \"Elbw\"; did you mean \"Elbow\"?",
              "arm /components/TwoBoneIK3D/settings/0/pole: names no pole: name the entity the "
              "middle bone bends toward",
              "arm /components/TwoBoneIK3D/settings/1/end_bone: \"Elbow\" isn't under \"Wrist\"",
              "arm /components/TwoBoneIK3D/settings/1/pole_direction: \"plus_w\" isn't one of "
              "\"none\", \"plus_x\", \"minus_x\", \"plus_y\", \"minus_y\", \"plus_z\", "
              "\"minus_z\", \"custom\"; did you mean \"plus_x\"?",
              "arm /components/TwoBoneIK3D/settings/1/pole: names no pole: name the entity the "
              "middle bone bends toward",
              "arm /components/CCDIK3D/settings/0/target: names no target: name the entity the end "
              "reaches for",
              "arm /components/CCDIK3D/settings/0/joints/Elbow/rotation_axis: \"w\" isn't one of "
              "\"all\", \"x\", \"y\", \"z\", \"custom\"; did you mean \"x\"?",
              "arm /components/CCDIK3D/settings/0/joints/Hand: \"Hand\" isn't a bone of the chain "
              "from \"Shoulder\" to \"Wrist\"",
              "loose /components/FABRIK3D: turns bones, but neither its entity nor a parent's "
              "draws a model: put it on the MeshInstance3D's entity or in its Transform3D "
              "children",
          });
}
