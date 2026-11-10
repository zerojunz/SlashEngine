// physics.debug, physics' visual debugger (contacts, joints and fields per step), and
// physics.cache, simulations recorded as animations and played back, in 3D and 2D.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
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
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

constexpr std::string_view kScene3D = R"({"format": 1, "entities": {
  "floor": {"components": {"Transform3D": {}, "StaticBody3D": {},
                           "CollisionShape3D": {"size": [40, 1, 40]}}},
  "crate": {"components": {"Transform3D": {"position": [0, 3, 0]}, "RigidBody3D": {},
                           "CollisionShape3D": {}}},
  "lamp": {"components": {"Transform3D": {"position": [6, 4, 0]}, "RigidBody3D": {"mass": 2},
                          "CollisionShape3D": {"shape": "sphere", "radius": 0.25}}},
  "chain": {"components": {"Transform3D": {"position": [6, 5, 0]},
                           "PinJoint3D": {"node_a": "lamp"}}},
  "ball": {"components": {"Transform3D": {"position": [-6, 0.75, 0]}, "RigidBody3D": {},
                          "CollisionShape3D": {"shape": "sphere", "radius": 0.25}}},
  "gust": {"components": {"Transform3D": {"position": [-6, 1, 0]},
                          "PhysicsField3D": {"type": "directional_force", "magnitude": 4,
                                             "shape": "box", "size": [2, 2, 40]}}}}})";

constexpr std::string_view kScene2D = R"({"format": 1, "entities": {
  "floor": {"components": {"Transform2D": {"position": [0, 200]}, "StaticBody2D": {},
                           "CollisionShape2D": {"size": [2000, 40]}}},
  "crate": {"components": {"Transform2D": {"position": [0, 100]}, "RigidBody2D": {},
                           "CollisionShape2D": {"size": [40, 40]}}},
  "lamp": {"components": {"Transform2D": {"position": [300, -100]}, "RigidBody2D": {},
                          "CollisionShape2D": {"shape": "circle", "radius": 10}}},
  "chain": {"components": {"Transform2D": {"position": [300, -160]},
                           "PinJoint2D": {"node_b": "lamp"}}}}})";

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string name, std::string_view scene) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json", scene).ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply fails(std::string_view name, std::string_view params) {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        return reply;
    }

    void steps(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(1.0 / 60.0);
        }
    }

    [[nodiscard]] const world::Transform3D& at3(std::string_view id) const {
        return session->world().ecs().get<const world::Transform3D>(session->world().find(id));
    }
    [[nodiscard]] const world::Transform2D& at2(std::string_view id) const {
        return session->world().ecs().get<const world::Transform2D>(session->world().find(id));
    }
};

double number(const json::Value& value, std::size_t index) {
    return value.as_array()[index].as_double();
}

} // namespace

TEST_CASE("physics.debug records 3D contacts, joints, fields and sleeping bodies per step") {
    Project project("physics_debug_3d_project", kScene3D);
    CHECK(project.fails("physics.debug", "{}").message.find("isn't recording") !=
          std::string::npos);
    CHECK(*project.ok("physics.debug", R"({"record": 30})").find("recording") == json::Value(30));
    project.steps(60);

    const json::Value all = project.ok("physics.debug", R"({"ticks": [0, 1000]})");
    REQUIRE(all.find("steps")->as_array().size() == 30); // the last 30 kept
    const json::Value& step = all.find("steps")->as_array().back();
    CHECK(step.find("dimensions")->as_int() == 3);
    CHECK(step.find("awake")->as_int() + step.find("sleeping")->as_int() == 3);

    // The crate rests on the floor: the normal points from the crate (a) down into the floor.
    const json::Value crate = project.ok("physics.debug", R"({"id": "crate"})");
    const json::Value& last = crate.find("steps")->as_array()[0];
    REQUIRE(last.find("contacts") != nullptr);
    const json::Value& contact = last.find("contacts")->as_array()[0];
    CHECK(contact.find("a")->as_string() == "crate");
    CHECK(contact.find("b")->as_string() == "floor");
    CHECK(number(*contact.find("normal"), 1) == doctest::Approx(-1.0).epsilon(0.01));
    CHECK(number(*contact.find("point"), 1) == doctest::Approx(0.5).epsilon(0.05));
    CHECK(contact.find("depth")->as_double() >= 0.0);
    CHECK(last.find("joints") == nullptr);

    // The lamp hangs from its chain: the joint holds up its weight and barely stretches.
    const json::Value lamp = project.ok("physics.debug", R"({"id": "lamp"})");
    const json::Value& hanging = lamp.find("steps")->as_array()[0];
    REQUIRE(hanging.find("joints") != nullptr);
    const json::Value& joint = hanging.find("joints")->as_array()[0];
    CHECK(joint.find("id")->as_string() == "chain");
    CHECK(joint.find("kind")->as_string() == "PinJoint3D");
    CHECK(joint.find("a")->as_string() == "lamp");
    CHECK(joint.find("b") == nullptr);
    CHECK(joint.find("stretch")->as_double() < 0.05);
    CHECK(joint.find("force")->as_double() > 5.0); // swinging: at least part of 2 kg's weight

    // The gust pushes the ball inside its box.
    const json::Value ball = project.ok("physics.debug", R"({"id": "ball", "last": 1})");
    const json::Value& pushed = ball.find("steps")->as_array()[0];
    REQUIRE(pushed.find("fields") != nullptr);
    const json::Value& field = pushed.find("fields")->as_array()[0];
    CHECK(field.find("body")->as_string() == "ball");
    CHECK(number(*field.find("force"), 2) == doctest::Approx(-4.0));

    CHECK(project.fails("physics.debug", R"({"id": "crat"})").message.find("did you mean") !=
          std::string::npos);
    CHECK(*project.ok("physics.debug", R"({"record": 0})").find("recording") == json::Value(0));
    project.fails("physics.debug", "{}");
}

TEST_CASE("physics.debug records 2D contacts with their impulses and joints") {
    Project project("physics_debug_2d_project", kScene2D);
    project.ok("physics.debug", R"({"record": 10})");
    project.steps(90);
    const json::Value crate = project.ok("physics.debug", R"({"id": "crate"})");
    const json::Value& step = crate.find("steps")->as_array()[0];
    CHECK(step.find("dimensions")->as_int() == 2);
    REQUIRE(step.find("contacts") != nullptr);
    const json::Value& contact = step.find("contacts")->as_array()[0];
    CHECK(contact.find("a")->as_string() == "crate");
    CHECK(contact.find("b")->as_string() == "floor");
    CHECK(contact.find("point")->as_array().size() == 2);
    // y grows down the screen: from the crate down into the floor.
    CHECK(number(*contact.find("normal"), 1) == doctest::Approx(1.0).epsilon(0.01));
    CHECK(contact.find("impulse")->as_double() > 0.0);

    const json::Value lamp = project.ok("physics.debug", R"({"id": "chain"})");
    const json::Value& joint = lamp.find("steps")->as_array()[0].find("joints")->as_array()[0];
    CHECK(joint.find("kind")->as_string() == "PinJoint2D");
    CHECK(joint.find("b")->as_string() == "lamp");
    CHECK(joint.find("force")->as_double() > 0.0);
}

TEST_CASE("physics.cache records a 3D fall as an animation that plays it back the same") {
    Project project("physics_cache_3d_project", kScene3D);
    // Where the crate starts, to play the recording back from.
    project.ok("world.snapshot");
    CHECK(*project.ok("physics.cache", R"({"record": ["crate", "ball"]})").find("recording") ==
          parse(R"(["ball", "crate"])"));
    project.steps(90);
    const world::Transform3D fallen = project.at3("crate");
    CHECK(fallen.position.y < 1.0F);
    project.fails("physics.cache", R"({"save": "fall.json"})");
    const json::Value saved = project.ok(
        "physics.cache", R"({"save": "anims/fall.anim.json", "name": "fall", "player": "floor"})");
    CHECK(saved.find("out")->as_string() == "anims/fall.anim.json");
    CHECK(*saved.find("bodies") == parse(R"(["ball", "crate"])"));
    CHECK(saved.find("length")->as_double() == doctest::Approx(1.5));
    // Keys only where the motion bends: fewer than a key a tick for each of the 4 tracks moving.
    CHECK(saved.find("keys")->as_int() < 4 * 91);
    CHECK(slash::platform::read_file(project.root + "/anims/fall.anim.json").ok());
    // Saving stopped the recording.
    project.fails("physics.cache", R"({"save": "anims/again.anim.json"})");

    // Back at the start, the floor's player plays the fall: the crate lands where it did,
    // frozen as a kinematic body.
    project.ok("world.restore");
    project.ok("component.set", R"({"id": "floor", "components": {"AnimationPlayer": {
        "library": "anims/fall.anim.json", "animation": "fall", "playing": true,
        "targets": {"crate": "crate", "ball": "ball"}}}})");
    project.steps(90);
    const world::Transform3D replayed = project.at3("crate");
    CHECK(replayed.position.y == doctest::Approx(fallen.position.y).epsilon(0.01));
    CHECK(replayed.position.x == doctest::Approx(fallen.position.x).epsilon(0.01));
    const auto& body = project.session->world().ecs().get<const world::RigidBody3D>(
        project.session->world().find("crate"));
    CHECK(body.freeze);
    CHECK(body.freeze_mode == "kinematic");

    project.ok("physics.cache", R"({"record": []})");
    CHECK(*project.ok("physics.cache", R"({"stop": true})").find("recording") == parse("[]"));
    project.fails("physics.cache", R"({"record": ["floor"]})");
    project.fails("physics.cache", R"({"record": [], "stop": true})");
}

TEST_CASE("physics.cache records 2D bodies' places and turns") {
    Project project("physics_cache_2d_project", kScene2D);
    project.ok("physics.cache", R"({"record": []})");
    project.steps(60);
    const json::Value saved =
        project.ok("physics.cache", R"({"save": "anims/swing.anim.json", "tolerance": 0.5})");
    CHECK(*saved.find("bodies") == parse(R"(["crate", "lamp"])"));
    auto text = slash::platform::read_file(project.root + "/anims/swing.anim.json");
    REQUIRE(text.ok());
    const json::Value file = parse(text.value());
    const json::Value& tracks = *file.find("animations")->find("cache")->find("tracks");
    bool turned = false;
    for (const json::Value& track : tracks.as_array()) {
        if (track.find("path")->as_string() == "Transform2D/rotation") {
            turned = true;
            CHECK(track.find("keys")->as_array()[0].find("value")->is_number());
        }
        if (track.find("path")->as_string() == "Transform2D/position") {
            CHECK(track.find("keys")->as_array()[0].find("value")->as_array().size() == 2);
        }
    }
    CHECK(turned);
}
