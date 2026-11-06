#include "fake_track_sound.hpp"

#include "slash/animation/animation.hpp"
#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
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
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

// Files in memory; editing one gives it a new stamp, so poll() reloads it.
struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> edits;

    void write(const std::string& path, std::string text) {
        files[path] = std::move(text);
        ++edits[path];
    }

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
                return asset::FileStamp{.modified = edits.find(path)->second,
                                        .size = it->second.size()};
            },
        };
    }
};

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

world::ComponentRegistry builtin_components() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    return components;
}

// Every error in a document, one "path: message" per line, or "(valid)".
std::string errors_of(std::string_view text, bool with_components = true) {
    const world::ComponentRegistry components = builtin_components();
    const auto errors =
        animation::validate_animation_library(parse(text), with_components ? &components : nullptr);
    if (errors.empty()) {
        return "(valid)";
    }
    std::string out;
    for (const auto& error : errors) {
        out += (out.empty() ? "" : "\n") + error.path + ": " + error.message;
    }
    return out;
}

// One animation with the given tracks, as a .anim.json document.
std::string library(std::string_view tracks, double length = 1.0, std::string_view loop = "none") {
    return R"({"format": 1, "animations": {"a": {"length": )" + std::to_string(length) +
           R"(, "loop": ")" + std::string(loop) + R"(", "tracks": [)" + std::string(tracks) +
           "]}}}";
}

animation::Animation animation_of(std::string_view text) {
    auto parsed = animation::parse_animation_library(parse(text));
    REQUIRE(parsed.ok());
    return parsed.value().animations.begin()->second;
}

double number_at(const animation::Animation& anim, double time, std::size_t track = 0) {
    return animation::sample(anim, anim.tracks.at(track), time, false).as_double();
}

// A world with the built-in components and the animation player, and the events it emits.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        animation::install(world, assets);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            REQUIRE(components.find(name)->assign(world, entity, fields, path).ok());
        }
        return entity;
    }

    world::AnimationPlayer& player(entt::entity entity) {
        return world.ecs().get<world::AnimationPlayer>(entity);
    }
    world::Transform2D& transform(entt::entity entity) {
        return world.ecs().get<world::Transform2D>(entity);
    }

    // The gameplay events since the last call, as "kind data" lines, without entity events.
    std::vector<std::string> take_events() {
        std::vector<std::string> out;
        for (const world::Event& event : events) {
            if (!event.kind.starts_with("entity.")) {
                out.push_back(event.kind + " " + json::write(event.data, json::Style::Compact));
            }
        }
        events.clear();
        return out;
    }
};

struct Warnings {
    std::vector<std::string> lines;
    Warnings() {
        slash::log::set_sink([this](slash::log::Level level, std::string_view line) {
            if (level == slash::log::Level::Warn) {
                lines.emplace_back(line);
            }
        });
    }
    ~Warnings() { slash::log::reset_sink(); }
    [[nodiscard]] bool logged(std::string_view text) const {
        return std::ranges::any_of(lines, [text](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    }
    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;
    Warnings(Warnings&&) = delete;
    Warnings& operator=(Warnings&&) = delete;
};

} // namespace

TEST_CASE("animation files are checked, with every error at its JSON Pointer") {
    CHECK(errors_of(library(R"({"path": "Transform2D/position/y",
                                "keys": [{"time": 0, "value": 0}, {"time": 1, "value": -8}]},
                               {"event": "footstep", "keys": [{"time": 0.5, "data": "left"}]})")) ==
          "(valid)");
    CHECK(errors_of(R"({"format": 1, "animations": {"a": {"tracks": []}}})") ==
          "/animations/a: an animation needs a \"length\", unless it comes \"from\" a model's "
          "animation");
    CHECK(errors_of(R"({"format": 1, "animations": {"a": {}}})") ==
          "/animations/a: an animation needs a \"length\" and \"tracks\", unless it comes "
          "\"from\" a model's animation");
    // One from a model needs neither; its keys can't be checked against a length it hasn't yet.
    CHECK(errors_of(R"({"format": 1, "animations": {"a": {
        "from": {"model": "hero.glb", "animation": "Walk"},
        "tracks": [{"event": "step", "keys": [{"time": 5}]}]}}})") == "(valid)");
    CHECK(errors_of(R"({"format": 1, "animations": {"a": {
        "from": {"model": "hero.obj", "animation": "Walk"}}}})") ==
          "/animations/a/from/model: expected a .gltf, .glb, .fbx or .blend file");

    // Keys in time order, within the length.
    CHECK(errors_of(library(R"({"path": "Transform2D/rotation",
                                "keys": [{"time": 0.5, "value": 0}, {"time": 0.5, "value": 1},
                                         {"time": 2, "value": 2}]})")) ==
          "/animations/a/tracks/0/keys/1/time: keys go in time order, each later than the one "
          "before: 0.5 comes after 0.5\n"
          "/animations/a/tracks/0/keys/2/time: 2 is past the animation's length, 1");

    // A track is a value track, an event track or a node track.
    CHECK(errors_of(library(R"({"keys": [{"time": 0}]})")) ==
          "/animations/a/tracks/0: a track has a \"path\", for a value track, an \"event\" or a "
          "\"node\", or \"type\": \"animation\" or \"audio\"");
    CHECK(errors_of(library(R"({"path": "Transform2D/rotation", "event": "hit",
                                "keys": [{"time": 0}]})")) ==
          "/animations/a/tracks/0: a track has one of \"path\", \"event\" and \"node\", not more");
    CHECK(errors_of(library(R"({"event": "entity.created", "interpolation": "cubic",
                                "keys": [{"time": 0, "value": 1}]})")) ==
          "/animations/a/tracks/0/event: \"entity.created\" is one of the engine's own events; "
          "name it something else\n"
          "/animations/a/tracks/0/interpolation: only value tracks have interpolation\n"
          "/animations/a/tracks/0/keys/0/value: an event track's keys carry \"data\", not "
          "\"value\"");
    CHECK(errors_of(library(R"({"path": "Transform2D/rotation", "keys": [{"time": 0}]},
                               {"path": "Transform2D/rotation",
                                "keys": [{"time": 0, "value": 1, "data": 2}]})")) ==
          "/animations/a/tracks/0/keys/0: a value track's keys need a \"value\"\n"
          "/animations/a/tracks/1/path: another track already sets \"Transform2D/rotation\"\n"
          "/animations/a/tracks/1/keys/0/data: a value track's keys carry \"value\", not \"data\"");
    CHECK(errors_of(library(R"({"path": "Transform2D", "keys": [{"time": 0, "value": 1}]})")) ==
          "/animations/a/tracks/0/path: a path is a component and one of its fields, as "
          "\"Transform2D/position\", then optionally a part, as \"Transform2D/position/y\"");

    // Given the component types, paths and values are checked against them.
    CHECK(errors_of(library(R"({"path": "Transfrom2D/rotation",
                                "keys": [{"time": 0, "value": 1}]})")) ==
          "/animations/a/tracks/0/path: no component \"Transfrom2D\"; did you mean "
          "\"Transform2D\"?");
    CHECK(errors_of(library(R"({"path": "Transfrom2D/rotation",
                                "keys": [{"time": 0, "value": 1}]})"),
                    false) == "(valid)");
    CHECK(errors_of(library(R"({"path": "Transform2D/rotaton",
                                "keys": [{"time": 0, "value": 1}]})")) ==
          "/animations/a/tracks/0/path: Transform2D has no field \"rotaton\"; did you mean "
          "\"rotation\"?");
    CHECK(errors_of(library(R"({"path": "Transform2D/position/q",
                                "keys": [{"time": 0, "value": 1}]})")) ==
          "/animations/a/tracks/0/path: Transform2D/position has no part \"q\"; pick x, y, z or "
          "w, r, g, b or a, or an index");
    CHECK(errors_of(library(R"({"path": "Transform2D/rotation/x",
                                "keys": [{"time": 0, "value": 1}]})")) ==
          "/animations/a/tracks/0/path: Transform2D/rotation is a number; it has no parts");
    CHECK(errors_of(library(R"({"path": "Transform2D/position",
                                "keys": [{"time": 0, "value": [1, 2, 3]}]})")) ==
          "/animations/a/tracks/0/keys/0/value: expected at most 2 items, got 3");
    CHECK(errors_of(library(R"({"path": "Sprite2D/texture", "update": "continuous",
                                "keys": [{"time": 0, "value": "a.png"}]})")) ==
          "/animations/a/tracks/0/keys/0/value: a continuous track's values are numbers or "
          "arrays of numbers, not a string; use \"update\": \"discrete\"");
    CHECK(errors_of(library(R"({"path": "Sprite2D/texture",
                                "keys": [{"time": 0, "value": "a.png"},
                                         {"time": 1, "value": "b.png"}]})")) == "(valid)");

    // Node tracks move a model node's position, rotation or scale, [x, y, z] at each key.
    CHECK(errors_of(library(R"({"node": "Elbow", "property": "rotation", "interpolation": "nearest",
                                "keys": [{"time": 0, "value": [0, 0, 0]},
                                         {"time": 1, "value": [0, 0, 1.5]}]},
                               {"node": "Elbow", "property": "scale", "loop_wrap": false,
                                "keys": [{"time": 0, "value": [1, 1, 1]}]})")) == "(valid)");
    CHECK(errors_of(library(R"({"node": "Elbow", "target": "arm", "update": "discrete",
                                "interpolation": "cubic",
                                "keys": [{"time": 0, "value": [0, 0], "transition": 2},
                                         {"time": 0.5, "data": 1}]},
                               {"node": "Hand", "property": "position",
                                "keys": [{"time": 0, "value": 1}]},
                               {"node": "Hand", "property": "position",
                                "keys": [{"time": 0, "value": [0, 0, 0]}]},
                               {"path": "Transform3D/position", "property": "position",
                                "keys": [{"time": 0, "value": [0, 0, 0]}]})")) ==
          "/animations/a/tracks/0/target: node tracks move the nodes of the model the entity "
          "draws, so they have no target\n"
          "/animations/a/tracks/0/update: node tracks change smoothly, or jump from key to key "
          "with \"interpolation\": \"nearest\", so they have no update\n"
          "/animations/a/tracks/0/interpolation: node tracks interpolate \"nearest\" or "
          "\"linear\", as model animations do\n"
          "/animations/a/tracks/0: a node track needs a \"property\": \"position\", \"rotation\" "
          "or \"scale\"\n"
          "/animations/a/tracks/0/keys/0/value: a node track's value is [x, y, z], not 2 numbers\n"
          "/animations/a/tracks/0/keys/0/transition: node tracks go straight from key to key, "
          "so their keys have no transition\n"
          "/animations/a/tracks/0/keys/1/data: a node track's keys carry \"value\", not \"data\"\n"
          "/animations/a/tracks/0/keys/1: a node track's keys need a \"value\", [x, y, z]\n"
          "/animations/a/tracks/1/keys/0/value: a node track's value is [x, y, z], not a number\n"
          "/animations/a/tracks/2/node: another track already moves \"Hand\"'s position\n"
          "/animations/a/tracks/3/property: only node tracks have property");
    CHECK(errors_of(R"({"format": 1, "animations": {"a": {
        "from": {"model": "hero.glb", "animation": "Walk"},
        "tracks": [{"node": "Hand", "property": "scale", "keys": [{"time": 0, "value": [2, 2, 2]}]}]}}})") ==
          "/animations/a/tracks/0: an animation from a model moves its nodes by the model's "
          "tracks; give node tracks an animation of their own");
}

TEST_CASE("node tracks interpolate as Godot's 3D tracks: rotations the shorter way round") {
    auto parsed = animation::parse_animation_library(parse(R"({"format": 1, "animations": {
        "spin": {"length": 1, "loop": "linear", "tracks": [
            {"node": "Elbow", "property": "rotation",
             "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 0.5, "value": [0, 3, 0]}]},
            {"node": "Elbow", "property": "position",
             "keys": [{"time": 0.25, "value": [0, 0, 0]}, {"time": 0.75, "value": [4, 0, 0]}]},
            {"node": "Hand", "property": "position", "loop_wrap": false,
             "keys": [{"time": 0.25, "value": [0, 0, 0]}, {"time": 0.75, "value": [4, 0, 0]}]},
            {"node": "Hand", "property": "scale", "interpolation": "nearest",
             "keys": [{"time": 0, "value": [1, 1, 1]}, {"time": 0.5, "value": [2, 2, 2]}]}]},
        "still": {"length": 1, "tracks": [
            {"path": "Transform3D/position", "keys": [{"time": 0, "value": [0, 0, 0]}]}]}}})"));
    REQUIRE(parsed.ok());
    const animation::AnimationLibrary& library = parsed.value();
    const animation::Animation& spin = library.animations.at("spin");
    CHECK(animation::has_node_tracks(spin));
    CHECK_FALSE(animation::has_node_tracks(library.animations.at("still")));
    const auto at = [&](std::size_t track, double time) {
        return json::write(animation::sample_node(spin, spin.tracks[track], time),
                           json::Style::Compact);
    };
    const auto angle = [&](double time) {
        return animation::sample_node(spin, spin.tracks[0], time).as_array()[1].as_double();
    };
    // Three radians about y, then back round the loop's end, a quarter way each 0.125 s.
    CHECK(angle(0.25) == doctest::Approx(1.5).epsilon(1e-5));
    CHECK(angle(0.5) == doctest::Approx(3.0).epsilon(1e-5));
    CHECK(angle(0.75) == doctest::Approx(1.5).epsilon(1e-5));
    // Round the loop's end from the last key to the first, or held without loop_wrap.
    CHECK(at(1, 0.0) == "[2.0,0.0,0.0]");
    CHECK(at(1, 0.5) == "[2.0,0.0,0.0]");
    CHECK(at(2, 0.0) == "[0.0,0.0,0.0]");
    CHECK(at(2, 1.0) == "[4.0,0.0,0.0]");
    CHECK(at(3, 0.49) == "[1.0,1.0,1.0]");
    CHECK(at(3, 0.5) == "[2.0,2.0,2.0]");

    // As a model's animations, which pose the nodes of the same names.
    const std::shared_ptr<const scene3d::Model> nodes = animation::node_animations(library);
    REQUIRE(nodes != nullptr);
    REQUIRE(nodes->nodes.size() == 2);
    CHECK(nodes->nodes[0].name == "Elbow");
    CHECK(nodes->nodes[1].name == "Hand");
    REQUIRE(nodes->animations.size() == 1);
    CHECK(nodes->animations[0].name == "spin");
    CHECK(nodes->animations[0].loop);
    CHECK(nodes->animations[0].tracks.size() == 4);
    CHECK(nodes->animations[0].tracks[3].step);
    CHECK_FALSE(nodes->animations[0].tracks[2].loop_wrap);
    animation::AnimationLibrary none;
    none.animations.emplace("still", library.animations.at("still"));
    CHECK(animation::node_animations(none) == nullptr);
}

TEST_CASE("track paths lead to a component's field, or a part of it") {
    const world::ComponentRegistry components = builtin_components();
    auto whole = animation::resolve_target("Transform2D/position", components);
    REQUIRE(whole.ok());
    CHECK(whole.value().component->name() == "Transform2D");
    CHECK(whole.value().field == "position");
    CHECK(whole.value().part.empty());
    CHECK_FALSE(whole.value().discrete);

    auto y = animation::resolve_target("Transform2D/position/y", components);
    REQUIRE(y.ok());
    CHECK(y.value().part == std::vector<std::string>{"1"});
    CHECK_FALSE(y.value().discrete);
    auto alpha = animation::resolve_target("Sprite2D/modulate/a", components);
    REQUIRE(alpha.ok());
    CHECK(alpha.value().part == std::vector<std::string>{"3"});

    // Integers, strings and booleans jump from key to key unless a track says otherwise.
    CHECK(animation::resolve_target("Sprite2D/frame", components).value().discrete);
    CHECK(animation::resolve_target("Sprite2D/texture", components).value().discrete);
    CHECK(animation::resolve_target("Sprite2D/visible", components).value().discrete);
}

TEST_CASE("values between keys follow Godot's interpolation and easing") {
    const auto linear = animation_of(library(R"({"path": "Transform2D/rotation",
        "keys": [{"time": 0.25, "value": 0}, {"time": 0.75, "value": 10}]})"));
    CHECK(number_at(linear, 0.5) == doctest::Approx(5.0));
    // Before the first key and after the last, an animation that doesn't loop holds them.
    CHECK(number_at(linear, 0.0) == doctest::Approx(0.0));
    CHECK(number_at(linear, 1.0) == doctest::Approx(10.0));
    // Discrete values are the last key's at or before the time.
    CHECK(animation::sample(linear, linear.tracks[0], 0.7, true).as_double() == 0.0);
    CHECK(animation::sample(linear, linear.tracks[0], 0.75, true).as_double() == 10.0);

    // A looping animation goes from the last key to the first across the loop's end.
    const auto looped = animation_of(library(R"({"path": "Transform2D/rotation",
        "keys": [{"time": 0.25, "value": 0}, {"time": 0.75, "value": 10}]})",
                                             1.0, "linear"));
    CHECK(number_at(looped, 0.0) == doctest::Approx(5.0));
    CHECK(number_at(looped, 0.9) == doctest::Approx(7.0));
    const auto unwrapped = animation_of(library(R"({"path": "Transform2D/rotation",
        "loop_wrap": false, "keys": [{"time": 0.25, "value": 0}, {"time": 0.75, "value": 10}]})",
                                                1.0, "linear"));
    CHECK(number_at(unwrapped, 0.0) == doctest::Approx(0.0));

    // Nearest holds each key until the next.
    const auto nearest = animation_of(library(R"({"path": "Transform2D/rotation",
        "interpolation": "nearest", "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 10}]})"));
    CHECK(number_at(nearest, 0.99) == 0.0);

    // A key's transition eases the way to the next key: 2 eases in, -2 eases in and out, 0 holds.
    const auto eased = animation_of(library(R"({"path": "Transform2D/rotation", "keys": [
        {"time": 0, "value": 0, "transition": 2}, {"time": 1, "value": 10, "transition": -2},
        {"time": 2, "value": 20, "transition": 0}, {"time": 3, "value": 30}]})",
                                            3.0));
    CHECK(number_at(eased, 0.5) == doctest::Approx(2.5));
    CHECK(number_at(eased, 1.25) == doctest::Approx(11.25));
    CHECK(number_at(eased, 2.9) == doctest::Approx(20.0));

    // Cubic passes through every key smoothly; on evenly spaced keys in a line it stays on it.
    const auto cubic = animation_of(library(R"({"path": "Transform2D/rotation",
        "interpolation": "cubic", "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 10},
        {"time": 2, "value": 20}, {"time": 3, "value": 30}]})",
                                            3.0));
    CHECK(number_at(cubic, 1.5) == doctest::Approx(15.0));
    CHECK(number_at(cubic, 2.0) == doctest::Approx(20.0));
    // Round a loop, the keys either side of the end are each other's neighbors.
    const auto bob = animation_of(library(R"({"path": "Transform2D/rotation",
        "interpolation": "cubic", "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": -8}]})",
                                          1.0, "linear"));
    CHECK(number_at(bob, 0.25) == doctest::Approx(-4.0));
    CHECK(number_at(bob, 0.75) == doctest::Approx(-4.0));
    CHECK(number_at(bob, 0.1) == doctest::Approx(number_at(bob, 0.9)));

    // Angles turn the short way round.
    const auto angle = animation_of(library(R"({"path": "Transform2D/rotation",
        "interpolation": "linear_angle", "keys": [{"time": 0, "value": 3}, {"time": 1, "value": -3}]})"));
    CHECK(number_at(angle, 0.5) == doctest::Approx(3.0 + (std::numbers::pi - 3.0)));

    // Arrays change component by component.
    const auto scale = animation_of(library(R"({"path": "Transform2D/scale",
        "keys": [{"time": 0, "value": [0, 0]}, {"time": 1, "value": [10, 20]}]})"));
    CHECK(animation::sample(scale, scale.tracks[0], 0.5, false) == parse("[5, 10]"));
}

TEST_CASE("AnimationPlayer sets fields each tick and stops at the end, as Godot's does") {
    Scene scene;
    scene.files.write("pop.anim.json", library(R"({"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": 30}]})",
                                               0.5));
    const entt::entity hero = scene.create("hero", R"({"AnimationPlayer":
        {"library": "pop.anim.json", "animation": "a"}})");

    // The track adds the Transform2D the entity didn't have.
    scene.world.step(0.1);
    CHECK(scene.player(hero).time == doctest::Approx(0.1));
    CHECK(scene.transform(hero).position.x == doctest::Approx(6.0F));
    for (int i = 0; i < 4; ++i) {
        scene.world.step(0.1);
    }
    CHECK(scene.transform(hero).position.x == doctest::Approx(30.0F));
    CHECK(scene.player(hero).time == doctest::Approx(0.5));
    CHECK_FALSE(scene.player(hero).playing);
    CHECK(scene.take_events() ==
          std::vector<std::string>{R"(animation_finished {"animation":"a"})"});

    // Stopped, it leaves the entity alone.
    scene.transform(hero).position.x = 99.0F;
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == 99.0F);
    CHECK(scene.take_events().empty());

    // Played again from the end, it starts over.
    scene.player(hero).playing = true;
    scene.world.step(0.1);
    CHECK(scene.player(hero).time == doctest::Approx(0.1));
    CHECK(scene.transform(hero).position.x == doctest::Approx(6.0F));

    // Backwards, it plays toward the start and ends there.
    scene.player(hero).backwards = true;
    scene.player(hero).time = 0.5;
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == doctest::Approx(24.0F));
    scene.player(hero).speed_scale = 4.0F;
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == doctest::Approx(0.0F));
    CHECK_FALSE(scene.player(hero).playing);
    CHECK(scene.take_events() ==
          std::vector<std::string>{R"(animation_finished {"animation":"a"})"});
}

TEST_CASE("at 60 Hz, a key at 0.3 seconds is passed on tick 18 and a half-second animation ends "
          "on tick 30") {
    Scene scene;
    scene.files.write("pop.anim.json", library(R"({"path": "Transform2D/rotation",
        "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": 1}]},
        {"event": "beat", "keys": [{"time": 0.3}]})",
                                               0.5));
    const entt::entity hero = scene.create("hero", R"({"AnimationPlayer":
        {"library": "pop.anim.json", "animation": "a"}})");
    for (int tick = 1; tick <= 29; ++tick) {
        scene.world.step(1.0 / 60.0);
        CHECK(scene.take_events() ==
              (tick == 18 ? std::vector<std::string>{"beat null"} : std::vector<std::string>{}));
    }
    CHECK(scene.player(hero).playing);
    scene.world.step(1.0 / 60.0);
    CHECK_FALSE(scene.player(hero).playing);
    CHECK(scene.transform(hero).rotation == 1.0F);
}

TEST_CASE("loops wrap or turn round, and each event key fires once as playback passes it") {
    Scene scene;
    scene.files.write("loop.anim.json",
                      library(R"({"event": "step", "keys": [{"time": 0, "data": "start"},
                                                            {"time": 0.5, "data": "mid"}]},
                                 {"event": "hit", "keys": [{"time": 0.4, "data": 1}]})",
                              1.0, "linear"));
    const entt::entity hero = scene.create("hero", R"({"AnimationPlayer":
        {"library": "loop.anim.json", "animation": "a"}})");
    scene.world.step(0.3);
    CHECK(scene.take_events() == std::vector<std::string>{R"(step "start")"});
    // Keys of every event track, in time order.
    scene.world.step(0.3);
    CHECK(scene.take_events() == std::vector<std::string>{"hit 1", R"(step "mid")"});
    scene.world.step(0.3);
    CHECK(scene.take_events().empty());
    // Across the loop's end.
    scene.world.step(0.3);
    CHECK(scene.player(hero).time == doctest::Approx(0.2));
    CHECK(scene.take_events() == std::vector<std::string>{R"(step "start")"});
    // A tick longer than the loop passes what's left of this loop and the next one's start; the
    // loops in between pass nothing.
    scene.world.step(2.5);
    CHECK(scene.player(hero).time == doctest::Approx(0.7));
    CHECK(scene.take_events() == std::vector<std::string>{"hit 1", R"(step "mid")",
                                                          R"(step "start")", "hit 1",
                                                          R"(step "mid")"});
    // Backwards.
    scene.player(hero).backwards = true;
    scene.world.step(0.25);
    CHECK(scene.take_events() == std::vector<std::string>{R"(step "mid")"});
    scene.world.step(0.25);
    CHECK(scene.take_events() == std::vector<std::string>{"hit 1"});
    scene.world.step(0.25);
    CHECK(scene.player(hero).time == doctest::Approx(0.95));
    CHECK(scene.take_events() == std::vector<std::string>{R"(step "start")"});

    // Pingpong turns round at each end, flipping backwards, and passes a key at an end once.
    scene.files.write("pingpong.anim.json", library(R"({"path": "Transform2D/rotation",
                                  "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 10}]},
                                 {"event": "edge", "keys": [{"time": 0, "data": "low"},
                                                            {"time": 1, "data": "high"}]})",
                                                    1.0, "pingpong"));
    const entt::entity ball = scene.create("ball", R"({"AnimationPlayer":
        {"library": "pingpong.anim.json", "animation": "a"}})");
    scene.world.destroy(hero);
    scene.take_events();
    std::vector<std::string> edges;
    std::vector<double> rotations;
    for (int i = 0; i < 5; ++i) {
        scene.world.step(0.5);
        for (const std::string& event : scene.take_events()) {
            edges.push_back(event);
        }
        rotations.push_back(scene.transform(ball).rotation);
    }
    CHECK(edges == std::vector<std::string>{R"(edge "low")", R"(edge "high")", R"(edge "low")"});
    CHECK(rotations == std::vector<double>{5, 10, 5, 0, 5});
    CHECK_FALSE(scene.player(ball).backwards);
    scene.world.step(0.7);
    CHECK(scene.player(ball).backwards);
    CHECK(scene.player(ball).time == doctest::Approx(0.8));
    CHECK(scene.take_events() == std::vector<std::string>{R"(edge "high")"});
}

TEST_CASE("discrete tracks set a key's value only when playback passes it") {
    Scene scene;
    scene.files.write("frames.anim.json", library(R"({"path": "Sprite2D/frame",
        "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": 1}]},
        {"path": "Sprite2D/texture", "keys": [{"time": 0.75, "value": "b.png"}]})"));
    const entt::entity hero = scene.create("hero", R"({"AnimationPlayer":
        {"library": "frames.anim.json", "animation": "a"},
        "Sprite2D": {"texture": "a.png", "frame": 3}})");
    const auto sprite = [&]() -> world::Sprite2D& {
        return scene.world.ecs().get<world::Sprite2D>(hero);
    };
    scene.world.step(0.2);
    CHECK(sprite().frame == 0);
    CHECK(sprite().texture == "a.png");
    // Gameplay code may change the field between keys.
    sprite().frame = 7;
    scene.world.step(0.2);
    CHECK(sprite().frame == 7);
    scene.world.step(0.2);
    CHECK(sprite().frame == 1);
    scene.world.step(0.2);
    CHECK(sprite().texture == "b.png");
}

TEST_CASE("an AnimationPlayer that can't play warns once, and its other tracks still play") {
    const Warnings warnings;
    Scene scene;
    scene.files.write("bob.anim.json", R"({"format": 1, "animations": {
        "bob": {"length": 1, "tracks": [
          {"path": "Health/current", "keys": [{"time": 0, "value": 1}]},
          {"path": "Transform2D/rotation", "keys": [{"time": 0, "value": 0},
                                                    {"time": 1, "value": 1}]}]}}})");
    const entt::entity missing = scene.create("missing", R"({"AnimationPlayer":
        {"library": "nope.anim.json", "animation": "bob"}})");
    const entt::entity misspelled = scene.create("misspelled", R"({"AnimationPlayer":
        {"library": "bob.anim.json", "animation": "bop"}})");
    const entt::entity partial = scene.create("partial", R"({"AnimationPlayer":
        {"library": "bob.anim.json", "animation": "bob"}})");
    const entt::entity idle = scene.create("idle", R"({"AnimationPlayer": {}})");
    scene.world.step(0.5);
    scene.world.step(0.5);
    CHECK(warnings.lines.size() == 3);
    CHECK(warnings.logged("nope.anim.json: "));
    CHECK(warnings.logged(R"(bob.anim.json has no animation \"bop\"; did you mean \"bob\"?)"));
    CHECK(warnings.logged(R"(Health/current: no component \"Health\")"));
    CHECK_FALSE(scene.world.ecs().all_of<world::Transform2D>(missing));
    CHECK_FALSE(scene.world.ecs().all_of<world::Transform2D>(misspelled));
    CHECK_FALSE(scene.world.ecs().all_of<world::Transform2D>(idle));
    CHECK(scene.transform(partial).rotation == doctest::Approx(1.0F));
    CHECK(scene.player(missing).time == 0.0);
}

TEST_CASE("a track with a target acts on the entity the AnimationPlayer's targets name") {
    const Warnings warnings;
    Scene scene;
    scene.files.write("swing.anim.json", R"({"format": 1, "animations": {
        "swing": {"length": 1, "tracks": [
          {"target": "sword", "path": "Transform2D/rotation",
           "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 2}]},
          {"path": "Transform2D/rotation", "keys": [{"time": 0, "value": 0},
                                                    {"time": 1, "value": 1}]},
          {"target": "sword", "event": "clang", "keys": [{"time": 0.5}]},
          {"event": "grunt", "keys": [{"time": 0.5}]},
          {"target": "shield", "path": "Transform2D/rotation", "keys": [{"time": 0, "value": 1}]},
          {"target": "helmet", "path": "Transform2D/rotation", "keys": [{"time": 0, "value": 1}]}
        ]}}})");
    const entt::entity sword = scene.create("sword", R"({"Transform2D": {}})");
    const entt::entity hero = scene.create("hero", R"({"Transform2D": {}, "AnimationPlayer":
        {"library": "swing.anim.json", "animation": "swing",
         "targets": {"sword": "sword", "shield": null}}})");
    scene.world.step(0.5);
    CHECK(scene.transform(sword).rotation == doctest::Approx(1.0F));
    CHECK(scene.transform(hero).rotation == doctest::Approx(0.5F));
    // Events come from the entity the track acts on.
    std::vector<std::string> sources;
    for (const world::Event& event : scene.events) {
        if (event.kind == "clang" || event.kind == "grunt") {
            sources.push_back(event.kind + " " + event.id);
        }
    }
    CHECK(sources == std::vector<std::string>{"clang sword", "grunt hero"});
    // Targets with no entity warn once and leave their tracks unplayed.
    CHECK(warnings.lines.size() == 2);
    CHECK(warnings.logged(R"(target \"shield\" names no entity)"));
    CHECK(warnings.logged(R"(no entity for target \"helmet\" in the AnimationPlayer's targets)"));

    // A destroyed target stops its tracks; the player's own go on.
    scene.world.destroy(sword);
    scene.world.step(0.25);
    CHECK(scene.transform(hero).rotation == doctest::Approx(0.75F));
    CHECK(warnings.logged(R"(target \"sword\" names no entity)"));
}

TEST_CASE("AnimationPlayers in a scene are checked against their libraries") {
    const std::map<std::string, std::string, std::less<>> files{
        {"swing.anim.json", R"({"format": 1, "animations": {"swing": {"length": 1, "tracks": [
            {"target": "sword", "path": "Transform2D/rotation", "keys": [{"time": 0, "value": 0}]},
            {"target": "shield", "event": "block", "keys": [{"time": 0}]},
            {"path": "Transform2D/rotation", "keys": [{"time": 0, "value": 0}]}]}}})"},
    };
    const world::FileReader reader = [&files](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    const json::Value scene = parse(R"({"format": 1, "entities": {
        "bad": {"components": {"AnimationPlayer": {"library": "swing.json"}}},
        "ghost": {"components": {"AnimationPlayer": {"library": "gone.anim.json"}}},
        "hero": {"components": {"AnimationPlayer": {"library": "swing.anim.json",
            "animation": "swng", "targets": {"swrd": "hero", "shield": "hero"}}}},
        "knight": {"components": {"AnimationPlayer": {"library": "swing.anim.json"}}},
        "page": {"components": {"AnimationPlayer": {"library": "swing.anim.json",
            "animation": "swing", "targets": {"sword": "page", "shield": null}}}},
        "squire": {"extends": "squire.prefab.json",
                   "components": {"AnimationPlayer": {"library": "swing.anim.json"}}},
        "idle": {"components": {"AnimationPlayer": {}}}}})");
    std::vector<std::string> errors;
    for (const json::ValidationError& error : animation::check_players(scene, reader)) {
        errors.push_back(error.path + ": " + error.message);
    }
    CHECK(errors ==
          std::vector<std::string>{
              "/entities/bad/components/AnimationPlayer/library: animation libraries are "
              ".anim.json files or models, as \"anim/hero.anim.json\" or \"models/hero.glb\"",
              "/entities/ghost/components/AnimationPlayer/library: gone.anim.json: no such file",
              "/entities/hero/components/AnimationPlayer/animation: swing.anim.json has no "
              "animation \"swng\"; did you mean \"swing\"?",
              "/entities/hero/components/AnimationPlayer/targets: no entity for target "
              "\"sword\", which swing.anim.json's tracks name; add it to targets; did you mean "
              "\"swrd\"?",
              "/entities/knight/components/AnimationPlayer: no entity for target \"shield\", "
              "which swing.anim.json's tracks name; add it to targets",
              "/entities/knight/components/AnimationPlayer: no entity for target \"sword\", "
              "which swing.anim.json's tracks name; add it to targets",
          });
    CHECK(animation::check_players(scene, {}).empty());
}

TEST_CASE("an edited animation file plays its new keys from the next tick") {
    Scene scene;
    scene.files.write("spin.anim.json", library(R"({"path": "Transform2D/rotation",
        "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 1}]})",
                                                1.0, "linear"));
    const entt::entity hero = scene.create("hero", R"({"AnimationPlayer":
        {"library": "spin.anim.json", "animation": "a"}})");
    scene.world.step(0.5);
    CHECK(scene.transform(hero).rotation == doctest::Approx(0.5F));
    scene.files.write("spin.anim.json", library(R"({"path": "Transform2D/rotation",
        "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 4}]})",
                                                1.0, "linear"));
    scene.assets.poll();
    scene.world.step(0.25);
    CHECK(scene.transform(hero).rotation == doctest::Approx(3.0F));
}

TEST_CASE("bezier tracks follow their keys' handles, as Godot's bezier_track_interpolate") {
    const animation::Animation anim = animation_of(library(R"({"type": "bezier",
        "path": "Camera3D/fov", "keys": [
          {"time": 0, "value": 0, "out": [1, 0]},
          {"time": 2, "value": 10, "in": [-1, 0]},
          {"time": 3, "value": 4, "in": [-0.5, 3]}]})",
                                                           4.0, "linear"));
    CHECK(anim.tracks[0].type == animation::TrackType::Bezier);
    // Eased in and out between the first two keys, by handles a second long.
    CHECK(number_at(anim, 0.0) == doctest::Approx(0.0));
    CHECK(number_at(anim, 0.5) == doctest::Approx(1.0589292831913322));
    CHECK(number_at(anim, 1.0) == doctest::Approx(5.0));
    CHECK(number_at(anim, 1.5) == doctest::Approx(8.941070716808667));
    CHECK(number_at(anim, 2.0) == doctest::Approx(10.0));
    CHECK(number_at(anim, 2.5) == doctest::Approx(7.0));
    // Past the last key it holds, even in a loop: a bezier curve never wraps.
    CHECK(number_at(anim, 3.5) == doctest::Approx(4.0));

    // With no handles, the curve is a straight line.
    const animation::Animation straight = animation_of(library(R"({"type": "bezier",
        "path": "Camera3D/fov", "keys": [{"time": 1, "value": 0}, {"time": 3, "value": 10}]})",
                                                               4.0));
    CHECK(number_at(straight, 0.0) == doctest::Approx(0.0));
    CHECK(number_at(straight, 1.5) == doctest::Approx(2.5));
    CHECK(number_at(straight, 2.0) == doctest::Approx(5.0));
}

TEST_CASE("cinematic tracks and markers are checked, each error at its JSON Pointer") {
    CHECK(errors_of(R"({"format": 1, "animations": {"shot": {"length": 2,
        "markers": {"wide": 0.5, "late": 3},
        "tracks": [
          {"type": "bezier", "path": "Transform3D/position",
           "keys": [{"time": 0, "value": 1}]},
          {"type": "bezier", "path": "Camera3D/fov", "interpolation": "cubic",
           "keys": [{"time": 0, "value": 70, "out": [-1, 0], "transition": 2},
                    {"time": 1, "value": [1], "in": [1, 0]}]},
          {"type": "animation", "path": "Camera3D/fov", "keys": [{"time": 0, "value": 1}]},
          {"type": "animation", "keys": [{"time": 0, "value": "open"}]},
          {"type": "event", "path": "Camera3D/fov", "keys": [{"time": 0, "value": 1}]},
          {"path": "Camera3D/near", "keys": [{"time": 0, "value": 1, "in": [0, 0],
                                              "animation": "open"}]}
        ]}}})") == "/animations/shot/markers/late: 3 is past the animation's length, 2\n"
                   "/animations/shot/tracks/0/path: a bezier track sets one number, as "
                   "\"Camera3D/fov\" or \"Transform3D/position/y\"; Transform3D/position isn't "
                   "one\n"
                   "/animations/shot/tracks/1/interpolation: a bezier track's curve comes from "
                   "its keys' handles, so it has no interpolation\n"
                   "/animations/shot/tracks/1/keys/0/transition: a bezier key's handles shape "
                   "the curve, so it has no transition\n"
                   "/animations/shot/tracks/1/keys/0/out/0: an out handle points on in time: 0 "
                   "seconds or more\n"
                   "/animations/shot/tracks/1/keys/1/value: a bezier track's keys need a "
                   "\"value\": a number\n"
                   "/animations/shot/tracks/1/keys/1/in/0: an in handle points back in time: 0 "
                   "seconds or less\n"
                   "/animations/shot/tracks/2: an animation track plays the animations of its "
                   "target's AnimationPlayer, so it has no path, event or node\n"
                   "/animations/shot/tracks/3: an animation track needs a \"target\": the entity "
                   "whose AnimationPlayer it plays\n"
                   "/animations/shot/tracks/3/keys/0: an animation track's keys need an "
                   "\"animation\": the one its player plays from there, or \"[stop]\"\n"
                   "/animations/shot/tracks/3/keys/0/value: an animation track's keys carry "
                   "\"animation\", not \"value\"\n"
                   "/animations/shot/tracks/4/type: an event track has a \"event\"\n"
                   "/animations/shot/tracks/5/keys/0/in: only bezier tracks' keys have handles\n"
                   "/animations/shot/tracks/5/keys/0/animation: only animation tracks' keys name "
                   "an animation");
}

TEST_CASE("audio tracks are checked, each error at its JSON Pointer") {
    CHECK(errors_of(R"({"format": 1, "animations": {"talk": {"length": 2, "tracks": [
          {"type": "audio", "keys": [{"time": 0, "stream": "a.wav", "start_offset": 0.5}]},
          {"type": "audio", "event": "x", "keys": [{"time": 0, "stream": "a.wav"}]},
          {"type": "audio", "update": "discrete", "keys": [{"time": 0, "value": "a.wav"}]},
          {"path": "Camera3D/fov", "use_blend": false,
           "keys": [{"time": 0, "value": 1, "stream": "a.wav", "end_offset": 1}]}
        ]}}})") == "/animations/talk/tracks/1: an audio track plays sounds on its target's "
                   "audio player, so it has no path, event or node\n"
                   "/animations/talk/tracks/2/update: only value tracks have update\n"
                   "/animations/talk/tracks/2/keys/0: an audio track's keys need a \"stream\": "
                   "the sound it plays from there\n"
                   "/animations/talk/tracks/2/keys/0/value: an audio track's keys carry "
                   "\"stream\", not \"value\"\n"
                   "/animations/talk/tracks/3/use_blend: only audio tracks have use_blend\n"
                   "/animations/talk/tracks/3/keys/0/stream: only audio tracks' keys have "
                   "stream\n"
                   "/animations/talk/tracks/3/keys/0/end_offset: only audio tracks' keys have "
                   "end_offset");
    const animation::Animation talk = animation::parse_animation_library(parse(R"({"format": 1,
        "animations": {"talk": {"length": 2, "tracks": [{"type": "audio", "use_blend": false,
        "keys": [{"time": 1, "stream": "a.wav", "start_offset": 0.5, "end_offset": 0.25}]}]}}})"))
                                          .value()
                                          .animations.at("talk");
    REQUIRE(talk.tracks.size() == 1);
    CHECK(talk.tracks[0].type == animation::TrackType::Audio);
    CHECK_FALSE(talk.tracks[0].use_blend);
    CHECK(talk.tracks[0].keys[0].value == json::Value("a.wav"));
    CHECK(talk.tracks[0].keys[0].start_offset == 0.5);
    CHECK(talk.tracks[0].keys[0].end_offset == 0.25);
}

TEST_CASE("an animation track plays and stops another AnimationPlayer's animations") {
    Scene scene;
    scene.files.write("door.anim.json", R"({"format": 1, "animations": {
        "open": {"length": 1, "tracks": [{"path": "Transform2D/rotation",
          "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 2}]}]},
        "shut": {"length": 1, "tracks": [{"path": "Transform2D/rotation",
          "keys": [{"time": 0, "value": 2}, {"time": 1, "value": 0}]}]}}})");
    scene.files.write("scene.anim.json", R"({"format": 1, "animations": {
        "intro": {"length": 4, "tracks": [{"type": "animation", "target": "door", "keys": [
          {"time": 0.5, "animation": "open"}, {"time": 2, "animation": "[stop]"},
          {"time": 3, "animation": "shut"}]}]}}})");
    // The door's player waits on its own. The director's plays first, as the newer entity, so
    // the door plays on from the tick its key starts it.
    const entt::entity door = scene.create("door", R"({"Transform2D": {}, "AnimationPlayer":
        {"library": "door.anim.json", "playing": false}})");
    scene.create("director", R"({"AnimationPlayer": {"library": "scene.anim.json",
        "animation": "intro", "targets": {"door": "door"}}})");
    scene.world.step(0.25);
    CHECK_FALSE(scene.player(door).playing);
    scene.world.step(0.5);
    CHECK(scene.player(door).animation == "open");
    CHECK(scene.player(door).playing);
    CHECK(scene.player(door).time == doctest::Approx(0.5));
    CHECK(scene.transform(door).rotation == doctest::Approx(1.0F));
    scene.world.step(0.75);
    CHECK_FALSE(scene.player(door).playing);
    CHECK(scene.transform(door).rotation == doctest::Approx(2.0F));
    // "[stop]" stops it and moves it back to its start, as Godot's stop().
    scene.world.step(0.5);
    CHECK_FALSE(scene.player(door).playing);
    CHECK(scene.player(door).time == 0.0);
    CHECK(scene.transform(door).rotation == doctest::Approx(0.0F));
    scene.world.step(0.75);
    CHECK_FALSE(scene.player(door).playing);
    scene.world.step(0.5);
    CHECK(scene.player(door).animation == "shut");
    CHECK(scene.transform(door).rotation == doctest::Approx(1.0F));
}

TEST_CASE("audio tracks start their keys' streams and stop them as Godot's AnimationMixer does") {
    Scene scene;
    slash::test::FakeSound sound;
    sound.install(scene.world);
    sound.lengths["line.wav"] = 1.0;
    scene.files.write("talk.anim.json", R"({"format": 1, "animations": {
        "talk": {"length": 2, "tracks": [{"type": "audio", "target": "voice", "keys": [
          {"time": 0.5, "stream": "line.wav", "start_offset": 0.25, "end_offset": 0.25},
          {"time": 1.5, "stream": "sigh.wav"}]}]}}})");
    scene.create("voice", R"({"AudioStreamPlayer": {}})");
    const entt::entity talker = scene.create("talker", R"({"AnimationPlayer": {
        "library": "talk.anim.json", "animation": "talk", "targets": {"voice": "voice"}}})");
    std::vector<std::string> calls;
    for (int tick = 1; tick <= 20; ++tick) {
        scene.world.step(0.1);
        for (const std::string& call : sound.take()) {
            calls.push_back(std::to_string(tick) + " " + call);
        }
    }
    // The line plays from a quarter second in, and is cut half a second later, a quarter second
    // before its end; the sigh plays until the animation ends, as a finished player stops its
    // sounds.
    CHECK(calls == std::vector<std::string>{"5 play voice line.wav 0.25", "5 volume 1 0",
                                            "11 stop 1", "15 play voice sigh.wav 0",
                                            "15 volume 2 0", "20 stop 2"});
    CHECK_FALSE(scene.player(talker).playing);

    // A seek back before a key stops its stream; stopping the player stops the rest.
    scene.player(talker).playing = true;
    scene.player(talker).time = 0.0;
    for (int tick = 0; tick < 16; ++tick) {
        scene.world.step(0.1);
    }
    CHECK(sound.take() == std::vector<std::string>{"play voice line.wav 0.25", "volume 3 0",
                                                   "stop 3", "play voice sigh.wav 0",
                                                   "volume 4 0"});
    REQUIRE(animation::seek(scene.world, scene.assets, talker, 1.0).ok());
    CHECK(sound.take() == std::vector<std::string>{"stop 4"});
    scene.world.step(0.1);
    scene.world.step(0.1);
    scene.world.step(0.1);
    scene.world.step(0.1);
    scene.world.step(0.1);
    CHECK(sound.take() == std::vector<std::string>{"play voice sigh.wav 0", "volume 5 0"});
    scene.player(talker).playing = false;
    scene.world.step(0.1);
    CHECK(sound.take() == std::vector<std::string>{"stop 5"});

    // A stream that can't play warns once.
    const Warnings warnings;
    scene.world.ecs().remove<world::AudioStreamPlayer>(scene.world.find("voice"));
    scene.player(talker).playing = true;
    scene.player(talker).time = 0.0;
    for (int tick = 0; tick < 6; ++tick) {
        scene.world.step(0.1);
    }
    CHECK(warnings.logged("an audio track's stream: no player"));
}

TEST_CASE("a track that makes a camera current cuts to it, as Godot's Camera3D.current") {
    Scene scene;
    scene.files.write("cut.anim.json", library(R"({"target": "close", "path": "Camera3D/current",
        "keys": [{"time": 0.5, "value": true}]},
        {"type": "bezier", "target": "close", "path": "Camera3D/fov",
         "keys": [{"time": 0, "value": 60}, {"time": 1, "value": 40}]})"));
    const entt::entity wide = scene.create("wide", R"({"Camera3D": {"current": true}})");
    const entt::entity close = scene.create("close", R"({"Camera3D": {}})");
    scene.create("director", R"({"AnimationPlayer": {"library": "cut.anim.json",
        "animation": "a", "targets": {"close": "close"}}})");
    const auto camera = [&](entt::entity entity) -> world::Camera3D& {
        return scene.world.ecs().get<world::Camera3D>(entity);
    };
    scene.world.step(0.25);
    CHECK(camera(wide).current);
    CHECK_FALSE(camera(close).current);
    CHECK(camera(close).fov == doctest::Approx(55.0F));
    scene.world.step(0.25);
    CHECK_FALSE(camera(wide).current);
    CHECK(camera(close).current);
}

TEST_CASE("seeking sets what an animation's tracks set there, its started players' included") {
    Scene scene;
    scene.files.write("door.anim.json", R"({"format": 1, "animations": {
        "swing": {"length": 1, "loop": "pingpong", "tracks": [{"path": "Transform2D/rotation",
          "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 2}]}]}}})");
    scene.files.write("scene.anim.json", R"({"format": 1, "animations": {
        "intro": {"length": 4, "markers": {"reveal": 2.5}, "tracks": [
          {"type": "bezier", "path": "Transform2D/position/x",
           "keys": [{"time": 0, "value": 0}, {"time": 4, "value": 40}]},
          {"path": "Sprite2D/texture", "keys": [{"time": 0, "value": "a.png"},
                                                {"time": 2, "value": "b.png"}]},
          {"event": "boom", "keys": [{"time": 1}]},
          {"type": "animation", "target": "door", "keys": [{"time": 1, "animation": "swing"}]}
        ]}}})");
    const entt::entity door = scene.create("door", R"({"Transform2D": {}, "AnimationPlayer":
        {"library": "door.anim.json", "playing": false}})");
    const entt::entity director = scene.create("director", R"({"Transform2D": {}, "AnimationPlayer":
        {"library": "scene.anim.json", "animation": "intro", "playing": false,
         "targets": {"door": "door"}}})");
    REQUIRE(animation::seek(scene.world, scene.assets, director, 2.5).ok());
    CHECK(scene.player(director).time == 2.5);
    CHECK(scene.transform(director).position.x == doctest::Approx(25.0F));
    CHECK(scene.world.ecs().get<world::Sprite2D>(director).texture == "b.png");
    // The door swung there and halfway back, as it would have from the key at 1 second.
    CHECK(scene.player(door).animation == "swing");
    CHECK(scene.player(door).time == doctest::Approx(0.5));
    CHECK(scene.transform(door).rotation == doctest::Approx(1.0F));
    CHECK_FALSE(scene.player(door).playing);
    // Events wait for playback.
    CHECK(scene.take_events().empty());

    // Before the key, the door is left alone.
    scene.transform(door).rotation = 9.0F;
    REQUIRE(animation::seek(scene.world, scene.assets, director, 0.5).ok());
    CHECK(scene.transform(door).rotation == 9.0F);
    CHECK(scene.world.ecs().get<world::Sprite2D>(director).texture == "a.png");

    const auto failed = animation::seek(scene.world, scene.assets, director, 1, "outro");
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message ==
          "scene.anim.json has no animation \"outro\"; did you mean \"intro\"?");
    REQUIRE_FALSE(animation::seek(scene.world, scene.assets, director, 5).ok());
}
