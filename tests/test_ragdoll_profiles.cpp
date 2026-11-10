// Ragdolls: .ragdoll.json profiles, profiles generated from skeletons and 2D trees, and
// Ragdoll3D's and Ragdoll2D's bodies, muscles, hits, falls and getting back up, through the
// commands an agent calls.

#include "humanoid.hpp"

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/physics2d/ragdoll.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/ragdoll.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

struct Engine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(command::SessionOptions options = {}) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply fails(std::string_view name, std::string_view params, int expected_code) {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        CHECK_MESSAGE(reply.code == expected_code, reply.message);
        return reply;
    }

    void steps(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(1.0 / 60.0);
        }
    }

    [[nodiscard]] entt::entity find(std::string_view id) const { return session->world().find(id); }

    [[nodiscard]] const world::Ragdoll3D& ragdoll() const {
        return session->world().ecs().get<const world::Ragdoll3D>(find("hero"));
    }

    // Where a ragdoll bone's body is in the world.
    [[nodiscard]] Vec3 body(std::string_view bone) const {
        const world::Tree3D tree(session->world());
        return tree.global(find("hero_mixamorig_" + std::string(bone))).origin;
    }
};

// A project with a Mixamo hero standing on a floor.
struct Project {
    std::string root;
    std::unique_ptr<Engine> engine;

    explicit Project(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root + "/characters").ok());
        const scene3d::Model hero = test_humanoid::make(test_humanoid::mixamo());
        const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no files");
        };
        REQUIRE(slash::platform::write_file(
                    root + "/characters/hero.glb",
                    scene3d::write_glb(hero, "characters/hero.glb", none).value())
                    .ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json", R"({"format": 1,
            "entities": {
              "floor": {"components": {"Transform3D": {"position": [0, -0.5, 0]},
                                       "StaticBody3D": {},
                                       "CollisionShape3D": {"size": [20, 1, 20]}}},
              "hero": {"components": {"Transform3D": {},
                                      "MeshInstance3D": {"mesh": "characters/hero.glb"}}}}})")
                    .ok());
        command::SessionOptions options;
        options.project_root = root;
        engine = std::make_unique<Engine>(options);
        engine->ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() {
        engine.reset();
        std::filesystem::remove_all(root);
    }
};

} // namespace

TEST_CASE("Ragdoll profiles merge defaults and overrides into each bone") {
    const json::Value file = parse(R"({"format": 1,
        "defaults": {"stiffness": 300, "damping": 30, "radius": 0.06},
        "bones": {"Hips": {"shape": "sphere", "radius": 0.12, "joint": "none", "mass": 12},
                  "Spine": {"height": 0.3, "mass": 8}},
        "balance": {"bone": "Hips", "stiffness": 200},
        "hit": {"knockdown": 6}})");
    auto profile = world::parse_ragdoll(file);
    REQUIRE_MESSAGE(profile.ok(), profile.error().message);
    const world::RagdollBone& hips = profile.value().bones.at("Hips");
    CHECK(hips.shape == "sphere");
    CHECK(hips.radius == doctest::Approx(0.12F));
    CHECK(hips.stiffness == doctest::Approx(300.0F));
    const world::RagdollBone& spine = profile.value().bones.at("Spine");
    CHECK(spine.radius == doctest::Approx(0.06F));
    CHECK(spine.joint == "cone");
    CHECK(profile.value().hit.knockdown == doctest::Approx(6.0F));
    CHECK_FALSE(profile.value().self_collision);

    // An instance's overrides win, and null takes one out.
    auto tuned = world::parse_ragdoll(
        file, parse(R"({"bones": {"Spine": {"stiffness": 50}}, "hit": {"knockdown": null}})"));
    REQUIRE(tuned.ok());
    CHECK(tuned.value().bones.at("Spine").stiffness == doctest::Approx(50.0F));
    CHECK(tuned.value().bones.at("Hips").stiffness == doctest::Approx(300.0F));
    CHECK(tuned.value().hit.knockdown == doctest::Approx(0.0F));

    // Errors name the bad value.
    auto bad = world::parse_ragdoll(file, parse(R"({"bones": {"Spine": {"joint": "ball"}}})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("/bones/Spine/joint") != std::string::npos);
    auto balance = world::parse_ragdoll(file, parse(R"({"balance": {"bone": "Chest"}})"));
    REQUIRE_FALSE(balance.ok());
    CHECK(balance.error().message.find("/balance/bone") != std::string::npos);
}

TEST_CASE("generate_ragdoll fits bodies to a humanoid's main bones") {
    const scene3d::Model hero = test_humanoid::make(test_humanoid::mixamo());
    auto made = scene3d::generate_ragdoll(hero, {.mass = 80.0F});
    REQUIRE_MESSAGE(made.ok(), made.error().message);
    const json::Value& file = made.value();
    auto profile = world::parse_ragdoll(file);
    REQUIRE_MESSAGE(profile.ok(), profile.error().message);
    const auto& bones = profile.value().bones;
    // The main bones only: no shoulders, neck, toes or ends.
    CHECK(bones.contains("mixamorig_Hips"));
    CHECK(bones.contains("mixamorig_Head"));
    CHECK(bones.contains("mixamorig_LeftForeArm"));
    CHECK(bones.contains("mixamorig_RightLeg"));
    CHECK_FALSE(bones.contains("mixamorig_LeftShoulder"));
    CHECK_FALSE(bones.contains("mixamorig_Neck"));
    CHECK_FALSE(bones.contains("mixamorig_LeftToeBase"));
    CHECK(bones.at("mixamorig_Hips").joint == "none");
    CHECK(bones.at("mixamorig_Hips").shape == "sphere");
    CHECK(bones.at("mixamorig_LeftForeArm").joint == "hinge");
    CHECK(bones.at("mixamorig_LeftLeg").joint == "hinge");
    CHECK(bones.at("mixamorig_LeftArm").joint == "cone");
    // A capsule spans its bone: the thigh runs from the hip to the knee, 0.42 meters.
    const world::RagdollBone& thigh = bones.at("mixamorig_LeftUpLeg");
    CHECK(thigh.shape == "capsule");
    CHECK(thigh.height - (2.0F * thigh.radius) == doctest::Approx(0.42F).epsilon(0.01));
    CHECK(thigh.offset.y == doctest::Approx(-0.21F).epsilon(0.01));
    float mass = 0.0F;
    for (const auto& [name, bone] : bones) {
        mass += bone.mass;
    }
    CHECK(mass == doctest::Approx(80.0F).epsilon(0.01));
    CHECK(profile.value().balance.bone == "mixamorig_Hips");
}

TEST_CASE("A Ragdoll3D's bodies follow its animation until a hit knocks it down") {
    Project project("ragdoll_project");
    Engine& engine = *project.engine;
    const json::Value made = engine.ok("ragdoll.generate", R"({"id": "hero"})");
    CHECK(made.find("out")->as_string() == "characters/hero.ragdoll.json");
    CHECK(made.find("humanoid")->as_bool());
    CHECK(std::filesystem::exists(project.root + "/characters/hero.ragdoll.json"));
    engine.steps(2);

    // A body for each profile bone, following the bone while the ragdoll is animated.
    REQUIRE((engine.find("hero_mixamorig_Hips") != entt::null));
    REQUIRE((engine.find("hero_mixamorig_LeftForeArm") != entt::null));
    CHECK(engine.ragdoll().state == "animated");
    CHECK(engine.body("LeftForeArm").x == doctest::Approx(0.575F).epsilon(0.02));
    CHECK(engine.body("LeftForeArm").y == doctest::Approx(1.44F).epsilon(0.02));

    // A hard hit to the chest knocks it down; it falls and lies on the floor.
    engine.ok("ragdoll.tune", R"({"id": "hero", "overrides": {"hit": {"knockdown": 20}}})");
    const json::Value hit = engine.ok(
        "ragdoll.hit", R"({"id": "hero", "bone": "mixamorig_Spine2", "impulse": [0, 0, -30]})");
    CHECK(hit.find("state")->as_string() == "limp");
    CHECK(hit.find("knocked_down")->as_bool());
    engine.steps(90);
    CHECK(engine.ragdoll().state == "limp");
    CHECK(engine.body("Hips").y < 0.5F);
    CHECK(engine.body("Head").y < 0.6F);
    CHECK(engine.body("Head").z < -0.2F);

    // Once it lies still it gets up where it lies, and the animation takes over again.
    for (int i = 0; i < 600 && engine.ragdoll().state == "limp"; ++i) {
        engine.steps(1);
    }
    CHECK(engine.ragdoll().state == "getting_up");
    const world::Tree3D tree(engine.session->world());
    CHECK(tree.global(engine.find("hero")).origin.z < -0.05F);
    engine.steps(30);
    CHECK(engine.ragdoll().state == "animated");
    engine.steps(2);
    CHECK(engine.body("Head").y > 1.4F);
}

TEST_CASE("Active ragdolls hold their pose with their muscles; limp ones fall") {
    Project project("ragdoll_active_project");
    Engine& engine = *project.engine;
    engine.ok("ragdoll.generate", R"({"id": "hero"})");
    engine.steps(2);
    engine.ok("component.set",
              R"({"id": "hero", "components": {"Ragdoll3D": {"state": "active"}}})");
    engine.steps(60);
    CHECK(engine.ragdoll().state == "active");
    // It stands as the pose has it, within a few centimeters: arms out, hips and head up.
    CHECK(engine.body("Hips").y > 1.2F);
    CHECK(engine.body("LeftForeArm").y > 1.35F);
    CHECK(engine.body("Head").y > 1.55F);

    engine.ok("component.set", R"({"id": "hero", "components": {"Ragdoll3D": {"state": "limp"}}})");
    engine.steps(90);
    CHECK(engine.body("Hips").y < 0.4F);
}

TEST_CASE("A hit on an animated ragdoll weakens the bone and those under it, then recovers") {
    Project project("ragdoll_hit_project");
    Engine& engine = *project.engine;
    engine.ok("ragdoll.generate", R"({"id": "hero"})");
    engine.steps(2);
    const json::Value hit = engine.ok(
        "ragdoll.hit", R"({"id": "hero", "bone": "mixamorig_LeftArm", "impulse": [0, 2, 0]})");
    CHECK(hit.find("state")->as_string() == "active");
    const json::Value& weakness = *hit.find("weakness");
    CHECK(weakness.find("mixamorig_LeftArm")->as_double() == doctest::Approx(1.0));
    CHECK(weakness.find("mixamorig_LeftForeArm")->as_double() == doctest::Approx(0.5));
    CHECK(weakness.find("mixamorig_LeftHand")->as_double() == doctest::Approx(0.25));
    CHECK(weakness.find("mixamorig_RightArm") == nullptr);
    engine.steps(30);
    CHECK(engine.ragdoll().state == "active");
    engine.steps(40);
    CHECK(engine.ragdoll().state == "animated");
    CHECK(engine.ragdoll().weakness.empty());

    // Wrong names are caught with a suggestion.
    const command::Reply wrong = engine.fails(
        "ragdoll.hit", R"({"id": "hero", "bone": "mixamorig_LeftArn", "impulse": [0, 1, 0]})",
        command::code::kNotFound);
    CHECK(wrong.message.find("did you mean \"mixamorig_LeftArm\"?") != std::string::npos);
}

TEST_CASE("ragdoll.tune merges overrides and reports the bones it touched") {
    Project project("ragdoll_tune_project");
    Engine& engine = *project.engine;
    engine.ok("ragdoll.generate", R"({"id": "hero"})");
    const json::Value tuned = engine.ok(
        "ragdoll.tune",
        R"({"id": "hero", "overrides": {"bones": {"mixamorig_Head": {"stiffness": 900}}}})");
    CHECK(tuned.find("bones")->find("mixamorig_Head")->find("stiffness")->as_double() == 900.0);
    engine.ok("ragdoll.tune",
              R"({"id": "hero", "overrides": {"bones": {"mixamorig_Head": {"mass": 6}}}})");
    const json::Value& overrides = engine.ragdoll().overrides;
    CHECK(overrides.find("bones")->find("mixamorig_Head")->find("stiffness")->as_double() == 900.0);
    CHECK(overrides.find("bones")->find("mixamorig_Head")->find("mass")->as_double() == 6.0);
    engine.steps(2);
    CHECK(engine.session->world()
              .ecs()
              .get<const world::PhysicalBone3D>(engine.find("hero_mixamorig_Head"))
              .mass == doctest::Approx(6.0F));

    const command::Reply bad = engine.fails(
        "ragdoll.tune",
        R"({"id": "hero", "overrides": {"bones": {"mixamorig_Head": {"shape": "cube"}}}})",
        command::code::kInvalidParams);
    CHECK(bad.message.find("/bones/mixamorig_Head/shape") != std::string::npos);
    engine.ok("undo");
    engine.ok("undo");
    CHECK(engine.ragdoll().overrides.find("bones")->find("mixamorig_Head")->find("mass") ==
          nullptr);
}

namespace {

// A cut-out kid standing on a floor: pelvis, chest, head, an arm with a hand and a leg with a
// foot, as entities under "kid", whose origin is at its feet.
struct Project2D {
    std::string root;
    std::unique_ptr<Engine> engine;

    explicit Project2D(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json", R"({"format": 1,
            "entities": {
              "floor": {"components": {"Transform2D": {"position": [200, 320]},
                                       "StaticBody2D": {},
                                       "CollisionShape2D": {"size": [2000, 40]}}},
              "kid": {"components": {"Transform2D": {"position": [200, 300],
                                                     "children": ["pelvis"]}}},
              "pelvis": {"components": {"Transform2D": {"position": [0, -60],
                                                        "children": ["chest", "thigh"]}}},
              "chest": {"components": {"Transform2D": {"position": [0, -40],
                                                       "children": ["head", "arm"]}}},
              "head": {"components": {"Transform2D": {"position": [0, -25]}, "Sprite2D": {}}},
              "arm": {"components": {"Transform2D": {"position": [10, 0],
                                                     "children": ["hand"]}}},
              "hand": {"components": {"Transform2D": {"position": [0, 30]}, "Sprite2D": {}}},
              "thigh": {"components": {"Transform2D": {"children": ["shin"]}}},
              "shin": {"components": {"Transform2D": {"position": [0, 30],
                                                      "children": ["foot"]}}},
              "foot": {"components": {"Transform2D": {"position": [0, 25]}, "Sprite2D": {}}}}})")
                    .ok());
        command::SessionOptions options;
        options.project_root = root;
        engine = std::make_unique<Engine>(options);
        engine->ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project2D(const Project2D&) = delete;
    Project2D& operator=(const Project2D&) = delete;
    Project2D(Project2D&&) = delete;
    Project2D& operator=(Project2D&&) = delete;
    ~Project2D() {
        engine.reset();
        std::filesystem::remove_all(root);
    }

    [[nodiscard]] const world::Ragdoll2D& ragdoll() const {
        return engine->session->world().ecs().get<const world::Ragdoll2D>(engine->find("kid"));
    }

    [[nodiscard]] slash::Vec2 at(std::string_view id) const {
        const world::Tree2D tree(engine->session->world());
        return tree.global(engine->find(id)).position;
    }
};

} // namespace

TEST_CASE("ragdoll.generate makes a 2D profile of a cut-out's tree and a Ragdoll2D") {
    Project2D project("ragdoll_2d_generate_project");
    Engine& engine = *project.engine;
    const json::Value made = engine.ok("ragdoll.generate", R"({"id": "kid"})");
    CHECK(made.find("out")->as_string() == "ragdolls/kid.ragdoll.json");
    CHECK(*made.find("bones") ==
          parse(R"(["arm", "chest", "foot", "hand", "head", "pelvis", "shin", "thigh"])"));
    CHECK(project.ragdoll().profile == "ragdolls/kid.ragdoll.json");
    const auto profile =
        slash::physics2d::ragdoll_profile(engine.session->world(), engine.find("kid"));
    REQUIRE_MESSAGE(profile.ok(), profile.error().message);
    const world::RagdollProfile& p = profile.value();
    CHECK(p.balance.bone == "pelvis");
    CHECK(p.bones.at("pelvis").joint == "none");
    CHECK(p.bones.at("pelvis").shape == "rectangle");
    CHECK(p.bones.at("arm").shape == "capsule");
    CHECK(p.bones.at("arm").joint == "hinge");
    CHECK(p.bones.at("arm").height == doctest::Approx(30.0F));
    CHECK(p.bones.at("hand").shape == "circle");
    float mass = 0.0F;
    for (const auto& [name, bone] : p.bones) {
        mass += bone.mass;
    }
    CHECK(mass == doctest::Approx(70.0F).epsilon(0.01));

    // Its bodies, shapes and joints, frozen on the pose while it is animated.
    engine.steps(30);
    const auto& ecs = engine.session->world().ecs();
    REQUIRE(ecs.all_of<world::RigidBody2D, world::CollisionShape2D>(engine.find("arm")));
    CHECK(ecs.get<const world::RigidBody2D>(engine.find("arm")).freeze);
    REQUIRE((engine.find("arm_joint") != entt::null));
    CHECK(ecs.get<const world::PinJoint2D>(engine.find("arm_joint")).node_a ==
          engine.find("chest"));
    CHECK((engine.find("pelvis_joint") == entt::null));
    CHECK(project.at("hand").x == doctest::Approx(210.0F));
    CHECK(project.at("hand").y == doctest::Approx(230.0F));

    // A scene with a bone the tree lacks fails validation, with a suggestion.
    const auto problems =
        slash::physics2d::check_ragdoll(engine.session->world(), engine.find("kid"));
    CHECK(problems.empty());
    engine.ok("ragdoll.tune", R"({"id": "kid", "overrides": {"bones": {"hnd": {}}}})");
    const auto wrong = slash::physics2d::check_ragdoll(engine.session->world(), engine.find("kid"));
    REQUIRE_FALSE(wrong.empty());
    CHECK(wrong[0].message.find("did you mean \"hand\"?") != std::string::npos);
}

TEST_CASE("A Ragdoll2D holds its pose when active, falls when knocked down and gets back up") {
    Project2D project("ragdoll_2d_project");
    Engine& engine = *project.engine;
    engine.ok("ragdoll.generate", R"({"id": "kid"})");
    engine.steps(2);
    const slash::Vec2 head = project.at("head");
    engine.ok("component.set",
              R"({"id": "kid", "components": {"Ragdoll2D": {"state": "active"}}})");
    engine.steps(60);
    CHECK(project.ragdoll().state == "active");
    CHECK(std::abs(static_cast<float>(project.at("head").y - head.y)) < 12.0F);
    CHECK(std::abs(static_cast<float>(project.at("head").x - head.x)) < 12.0F);

    // A hit weakens the arm and, half as much, the hand under it.
    const json::Value hit =
        engine.ok("ragdoll.hit", R"({"id": "kid", "bone": "arm", "impulse": [0, 2]})");
    CHECK(hit.find("weakness")->find("arm")->as_double() == doctest::Approx(1.0));
    CHECK(hit.find("weakness")->find("hand")->as_double() == doctest::Approx(0.5));
    CHECK(hit.find("weakness")->find("head") == nullptr);

    // A hard one knocks it down; it lies on the floor, then gets up where it lies.
    engine.ok("ragdoll.tune", R"({"id": "kid", "overrides": {"hit": {"knockdown": 500}}})");
    const json::Value down =
        engine.ok("ragdoll.hit", R"({"id": "kid", "bone": "chest", "impulse": [-3000, 0]})");
    CHECK(down.find("knocked_down")->as_bool());
    engine.steps(60);
    CHECK(project.ragdoll().state == "limp");
    CHECK(project.at("head").y > head.y + 40.0F);
    for (int i = 0; i < 900 && project.ragdoll().state == "limp"; ++i) {
        engine.steps(1);
    }
    REQUIRE(project.ragdoll().state == "getting_up");
    CHECK(project.at("kid").y == doctest::Approx(300.0F));
    CHECK(project.at("kid").x < 195.0F);
    engine.steps(40);
    CHECK(project.ragdoll().state == "animated");
    const slash::Vec2 kid = project.at("kid");
    CHECK(project.at("head").x == doctest::Approx(static_cast<float>(kid.x)).epsilon(0.001));
    CHECK(project.at("head").y == doctest::Approx(static_cast<float>(kid.y) - 125.0F));
}
