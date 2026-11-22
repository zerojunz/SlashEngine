// Layers over a model's animation in .anim.json files, and animation.edit, animation.preview and
// animation.tune on them.
#include "gltf.hpp"

#include "slash/animation/animation.hpp"
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

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace animation = slash::animation;
namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

// Two legs that swing 0.4 radians each way about x in a looping second, in turn: the left one
// forward first. Their feet are a meter below the hips, which stand a meter up.
slash::test::Gltf legs() {
    slash::test::Gltf out;
    out.document["nodes"] = slash::test::gltf_json(R"([
        {"name": "Hips", "translation": [0, 1, 0], "children": [1, 3]},
        {"name": "LeftUpperLeg", "translation": [0.1, 0, 0], "children": [2]},
        {"name": "LeftFoot", "translation": [0, -1, 0]},
        {"name": "RightUpperLeg", "translation": [-0.1, 0, 0], "children": [4]},
        {"name": "RightFoot", "translation": [0, -1, 0]}
    ])");
    const float s = std::sin(0.2F);
    const float c = std::cos(0.2F);
    const std::vector<float> times{0.0F, 0.25F, 0.5F, 0.75F, 1.0F};
    // The left leg turns -0.4 (its foot forward, +z) then +0.4; the right the other way.
    const std::vector<float> left{0, 0, 0, 1, -s, 0, 0, c, 0, 0, 0, 1, s, 0, 0, c, 0, 0, 0, 1};
    const std::vector<float> right{0, 0, 0, 1, s, 0, 0, c, 0, 0, 0, 1, -s, 0, 0, c, 0, 0, 0, 1};
    json::Value walk;
    walk["name"] = "Walk-loop";
    for (const auto& [node, values] : {std::pair{1, &left}, std::pair{3, &right}}) {
        json::Value sampler;
        sampler["input"] = out.floats(times, "SCALAR");
        sampler["output"] = out.floats(*values, "VEC4");
        walk["samplers"].push_back(std::move(sampler));
        json::Value channel;
        channel["sampler"] = static_cast<std::int64_t>(walk["samplers"].size()) - 1;
        channel["target"]["node"] = node;
        channel["target"]["path"] = "rotation";
        walk["channels"].push_back(std::move(channel));
    }
    out.document["animations"].push_back(std::move(walk));
    out.document["scenes"] = slash::test::gltf_json(R"([{"nodes": [0]}])");
    out.document["scene"] = 0;
    return out;
}

// The left leg's turn about x at a time of a library's animation, as its clip poses the model.
float left_turn(const scene3d::Model& model, const animation::Animation& walk, double time) {
    const scene3d::Model& source = animation::clip_source(walk, model);
    const auto clip =
        std::ranges::find(source.animations, walk.from.animation, &scene3d::ModelAnimation::name);
    REQUIRE(clip != source.animations.end());
    std::vector<scene3d::NodePose> pose = scene3d::rest_pose(model);
    const std::vector<int> map =
        &source == &model ? std::vector<int>{} : scene3d::node_map(model, source);
    scene3d::blend_animation(model, source, *clip, time, 1.0F, map, pose);
    const int leg = scene3d::find_node(model, "LeftUpperLeg");
    return static_cast<float>(
        world::euler(world::rotation_basis(pose[static_cast<std::size_t>(leg)].rotation)).x);
}

struct LegsProject {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit LegsProject(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/legs.glb", legs().glb()).ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {}})")
                    .ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "main.scene.json"})");
    }
    LegsProject(const LegsProject&) = delete;
    LegsProject& operator=(const LegsProject&) = delete;
    LegsProject(LegsProject&&) = delete;
    LegsProject& operator=(LegsProject&&) = delete;
    ~LegsProject() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }
};

} // namespace

TEST_CASE("animation layers: speed and layers bake into the model animation they play") {
    auto model = scene3d::load_model(
        legs().glb(), "legs.glb", [](std::string_view) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no files");
        });
    REQUIRE(model.ok());
    auto library = animation::parse_animation_library(parse(R"({"format": 1, "animations": {
        "walk": {"from": {"model": "legs.glb", "animation": "Walk-loop"}},
        "stride": {"from": {"model": "legs.glb", "animation": "Walk-loop"}, "speed": 2,
                   "layers": [{"name": "big", "bones": {"LeftUpperLeg": {"scale": 2}}},
                              {"name": "half", "weight": 0.5, "amplitude": 0,
                               "bones": {"RightUpperLeg": {"pitch": 0.2}}}]},
        "late": {"from": {"model": "legs.glb", "animation": "Walk-loop"},
                 "layers": [{"name": "shift", "bones": {"LeftUpperLeg": {"shift": 0.25}}}]}}})"));
    REQUIRE(library.ok());
    auto& animations = library.value().animations;
    animation::Animation& walk = animations.at("walk");
    REQUIRE(animation::resolve_clip(walk, model.value()).ok());
    // As it is, nothing is baked: it plays the model's.
    CHECK(walk.layered == nullptr);
    CHECK(walk.length == doctest::Approx(1.0));
    CHECK(left_turn(model.value(), walk, 0.25) == doctest::Approx(-0.4).epsilon(0.001));

    animation::Animation& stride = animations.at("stride");
    REQUIRE(animation::resolve_clip(stride, model.value()).ok());
    REQUIRE(stride.layered != nullptr);
    // Twice as fast is half as long; the left leg swings twice as far, then half of a layer
    // with no amplitude stills it (1 + 0.5 * (0 - 1)) to its swing.
    CHECK(stride.length == doctest::Approx(0.5));
    CHECK(stride.loop == animation::LoopMode::Linear);
    CHECK(left_turn(model.value(), stride, 0.125) == doctest::Approx(-0.4).epsilon(0.01));

    // A quarter cycle later, the left leg is where it would be at 0.25 at 0.
    animation::Animation& late = animations.at("late");
    REQUIRE(animation::resolve_clip(late, model.value()).ok());
    CHECK(left_turn(model.value(), late, 0.0) == doctest::Approx(-0.4).epsilon(0.01));
    CHECK(left_turn(model.value(), late, 0.75) == doctest::Approx(0.0).epsilon(0.01));

    // A bone the model lacks is named at its place, with the one meant.
    auto typo = animation::parse_animation_library(parse(R"({"format": 1, "animations": {
        "walk": {"from": {"model": "legs.glb", "animation": "Walk-loop"},
                 "layers": [{"name": "a", "bones": {"LeftUperLeg": {"scale": 2}}}]}}})"));
    REQUIRE(typo.ok());
    auto failed = animation::resolve_clip(typo.value().animations.at("walk"), model.value());
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message.starts_with("layers/0/bones/LeftUperLeg: "));
    CHECK(failed.error().message.find("\"LeftUpperLeg\"") != std::string::npos);

    // Only an animation from a model takes layers, and two layers can't share a name.
    const auto errors = animation::validate_animation_library(parse(R"({"format": 1, "animations": {
        "bob": {"length": 1, "tracks": [], "speed": 2},
        "walk": {"from": {"model": "legs.glb", "animation": "Walk-loop"},
                 "layers": [{"name": "a"}, {"name": "a"}]}}})"));
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/animations/bob/speed");
    CHECK(errors[1].path == "/animations/walk/layers/1/name");
}

TEST_CASE("animation layers: animation.edit writes layers, preview measures, tune plants feet") {
    LegsProject project("animation_layers_project");
    // A new animation from the model's walk: 1 second, the legs swinging 0.4 each way.
    json::Value made = project.ok("animation.edit", R"({"library": "walks.anim.json",
        "animation": "walk", "set": {"from": {"model": "legs.glb", "animation": "Walk-loop"}}})");
    CHECK(made.find("length")->as_double() == doctest::Approx(1.0));
    json::Value seen =
        project.ok("animation.preview",
                   R"({"library": "walks.anim.json", "animation": "walk", "move_speed": 1.2})");
    const json::Value* left = seen.find("bones")->find("LeftUpperLeg");
    REQUIRE(left != nullptr);
    CHECK(left->find("pitch")->as_array()[0].as_double() == doctest::Approx(-0.4));
    CHECK(left->find("pitch")->as_array()[1].as_double() == doctest::Approx(0.4));
    // Its feet sweep 2 sin(0.4) fore and aft, and back faster than it moves.
    const json::Value& gait = *seen.find("gait");
    CHECK(gait.find("stride")->as_double() == doctest::Approx(0.779).epsilon(0.01));
    const double foot_speed = gait.find("foot_speed")->as_double();
    CHECK(gait.find("slide")->as_double() == doctest::Approx(1.2 - foot_speed));
    CHECK(foot_speed > 1.2);

    // A layer that swings the right leg less, tried first, then written.
    const std::string_view tired = R"({"library": "walks.anim.json", "animation": "walk",
        "layer": {"name": "tired", "bones": {"RightUpperLeg": {"scale": 0.5}}}})";
    seen = project.ok("animation.preview", tired);
    CHECK(seen.find("bones")->find("RightUpperLeg")->find("pitch")->as_array()[1].as_double() ==
          doctest::Approx(0.2).epsilon(0.01));
    project.ok("animation.edit", tired);
    auto text = slash::platform::read_file(project.root + "/walks.anim.json");
    REQUIRE(text.ok());
    CHECK(text.value().find("\"tired\"") != std::string::npos);

    // Tuning: the left leg to a span of 1.2 radians, and the feet planted at 1.2 m/s.
    json::Value tuned =
        project.ok("animation.tune", R"({"library": "walks.anim.json", "animation": "walk",
        "ranges": {"LeftUpperLeg": 1.2}, "slide": "length", "move_speed": 1.2})");
    const json::Value& after = *tuned.find("after");
    const json::Value& span = *after.find("bones")->find("LeftUpperLeg")->find("pitch");
    CHECK(span.as_array()[1].as_double() - span.as_array()[0].as_double() ==
          doctest::Approx(1.2).epsilon(0.02));
    CHECK(after.find("gait")->find("slide")->as_double() == doctest::Approx(0.0).epsilon(0.01));
    CHECK(tuned.find("change")->find("speed")->as_double() < 1.0);
    const json::Value& layers = *tuned.find("change")->find("layers");
    REQUIRE(layers.as_array().size() == 2);
    CHECK(layers.as_array()[1].find("name")->as_string() == "tune");

    // Undo puts back the file as the layer left it; remove_layer takes the layer out.
    project.ok("undo");
    text = slash::platform::read_file(project.root + "/walks.anim.json");
    REQUIRE(text.ok());
    CHECK(text.value().find("\"tune\"") == std::string::npos);
    project.ok("animation.edit", R"({"library": "walks.anim.json", "animation": "walk",
        "remove_layer": "tired"})");
    text = slash::platform::read_file(project.root + "/walks.anim.json");
    REQUIRE(text.ok());
    CHECK(text.value().find("layers") == std::string::npos);

    // Mistakes, each at its param.
    command::Reply bad = project.call("animation.edit", R"({"library": "walks.anim.json",
        "animation": "walk", "remove_layer": "tird"})");
    CHECK_FALSE(bad.ok);
    CHECK(bad.data.find("path")->as_string() == "/remove_layer");
    bad = project.call("animation.edit", R"({"library": "walks.anim.json", "animation": "walk",
        "layer": {"name": "x", "bones": {"Lefthip": {"scale": 2}}}})");
    CHECK_FALSE(bad.ok);
    CHECK(bad.data.find("path")->as_string() == "/layer");
    CHECK(bad.message.find("/animations/walk/layers/0/bones/Lefthip") != std::string::npos);
    bad =
        project.call("animation.preview", R"({"library": "walks.anim.json", "animation": "wlak"})");
    CHECK_FALSE(bad.ok);
    CHECK(bad.data.find("path")->as_string() == "/animation");
    CHECK(bad.message.find("\"walk\"") != std::string::npos);
}

TEST_CASE("animation layers: an Animator poses its model by the layered animation") {
    LegsProject project("animation_layers_player_project");
    project.ok("animation.edit", R"({"library": "walks.anim.json", "animation": "walk",
        "set": {"from": {"model": "legs.glb", "animation": "Walk-loop"},
                "layers": [{"name": "big", "amplitude": 2}]}})");
    project.ok("entity.create", R"({"id": "legs", "components": {
        "Transform3D": {}, "Model3D": {"mesh": "legs.glb"},
        "Animator": {"library": "walks.anim.json", "animation": "walk",
                            "playing": false, "time": 0.25}}})");
    const json::Value bones =
        project.ok("skeleton.get", R"({"id": "legs", "bones": ["LeftUpperLeg"]})");
    INFO(json::write(bones));
    const json::Value* leg =
        bones.find("bones")->as_array().empty() ? nullptr : &bones.find("bones")->as_array()[0];
    REQUIRE(leg != nullptr);
    CHECK(leg->find("rotation")->as_array()[0].as_double() == doctest::Approx(-0.8).epsilon(0.01));
}
