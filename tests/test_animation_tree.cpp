#include "gltf.hpp"

#include "slash/animation/animation.hpp"
#include "slash/animation/tree.hpp"
#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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
using slash::Quat;
using slash::Vec3;

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

// A world with the built-in components, models and the animation systems, and its events.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        scene3d::add_asset_types(assets);
        animation::install(world, assets);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
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

    world::AnimationTree& tree(entt::entity entity) {
        return world.ecs().get<world::AnimationTree>(entity);
    }
    world::Transform2D& transform(entt::entity entity) {
        return world.ecs().get<world::Transform2D>(entity);
    }
    void set(entt::entity entity, const std::string& parameter, json::Value value) {
        tree(entity).parameters[parameter] = std::move(value);
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

    // What the tree played on its last tick, as "animation@time*weight" items.
    std::string playing(entt::entity entity) {
        std::string out;
        for (const world::TreeAnimation& played : tree(entity).playing) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s@%.3g*%.3g", played.animation.c_str(),
                          played.time, static_cast<double>(played.weight));
            out += (out.empty() ? "" : " ") + std::string(buffer);
        }
        return out;
    }
};

// Animations that each hold Transform2D/position/x at one value, looping over a second, and a
// RESET that puts it at 0.
constexpr std::string_view kHolds = R"({"format": 1, "animations": {
    "RESET": {"length": 1, "tracks": [{"path": "Transform2D/position/x", "keys": [{"time": 0, "value": 0}]}]},
    "ten": {"length": 1, "loop": "linear", "tracks": [{"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 10}]}]},
    "twenty": {"length": 0.5, "tracks": [{"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 20}]}]},
    "thirty": {"length": 1, "loop": "linear", "tracks": [{"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 30}]}]}
}})";

constexpr std::string_view kTreeComponent = R"({"AnimationTree":
    {"tree_root": "hero.animtree.json", "libraries": {"": "hero.anim.json"}}})";

std::string errors_of(const json::Value& doc) {
    const auto errors = animation::validate_tree(doc);
    if (errors.empty()) {
        return "(valid)";
    }
    std::string out;
    for (const auto& error : errors) {
        out += (out.empty() ? "" : "\n") + error.path + ": " + error.message;
    }
    return out;
}

bool near(Vec3 a, Vec3 b) {
    return std::abs(a.x - b.x) < 1e-4F && std::abs(a.y - b.y) < 1e-4F &&
           std::abs(a.z - b.z) < 1e-4F;
}

} // namespace

TEST_CASE("a Blend2 mixes two animations by its blend_amount, from their start on the first "
          "tick, as Godot's tree does") {
    Scene scene;
    scene.files.write("hero.anim.json", R"({"format": 1, "animations": {
        "RESET": {"length": 1, "tracks": [{"path": "Transform2D/position/x", "keys": [{"time": 0, "value": 0}]}]},
        "walk": {"length": 1, "loop": "linear", "tracks": [{"path": "Transform2D/position/x",
            "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 10}]}]},
        "run": {"length": 1, "loop": "linear", "tracks": [{"path": "Transform2D/position/x",
            "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 30}]}]}
    }})");
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"walk": {"type": "Animation", "animation": "walk"},
                  "run": {"type": "Animation", "animation": "run"},
                  "speed": {"type": "Blend2", "inputs": {"in": "walk", "blend": "run"}}},
        "output": "speed"}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    scene.set(hero, "speed/blend_amount", 0.25);

    // The first tick seeks to the start: nothing has moved yet.
    scene.world.step(0.1);
    CHECK(scene.tree(hero).invalid.empty());
    CHECK(scene.transform(hero).position.x == doctest::Approx(0.0));
    CHECK(scene.playing(hero) == "walk@0*0.75 run@0*0.25");
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == doctest::Approx(1.5));
    CHECK(scene.playing(hero) == "walk@0.1*0.75 run@0.1*0.25");
    // The tree keeps each node's time in state, and only what differs from its default.
    const json::Object& state = scene.tree(hero).state;
    CHECK(state.at("walk/current_position").as_double() == doctest::Approx(0.1));
    CHECK(state.at("walk/current_length").as_double() == doctest::Approx(1.0));
    CHECK_FALSE(state.contains("walk/backward"));

    // A new blend amount takes effect on the next tick.
    scene.set(hero, "speed/blend_amount", 1.0);
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == doctest::Approx(6.0));

    // Clearing state starts the tree over, and turning it off clears what it reported.
    scene.tree(hero).state.clear();
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == doctest::Approx(0.0));
    scene.tree(hero).active = false;
    scene.world.step(0.1);
    CHECK(scene.tree(hero).state.empty());
    CHECK(scene.tree(hero).playing.empty());
    CHECK(scene.transform(hero).position.x == doctest::Approx(0.0));
}

TEST_CASE("a OneShot fires on request, fades in and out over its input and reports its "
          "animation's start and end, as Godot's") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kHolds));
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"idle": {"type": "Animation", "animation": "ten"},
                  "wave": {"type": "Animation", "animation": "twenty"},
                  "shot": {"type": "OneShot", "inputs": {"in": "idle", "shot": "wave"},
                           "fadein_time": 0.2, "fadeout_time": 0.2}},
        "output": "shot"}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    const auto x = [&] { return static_cast<double>(scene.transform(hero).position.x); };

    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    CHECK(scene.take_events().empty());

    // Fired, the shot starts with a weight just above 0, so its first keys play, and the tree
    // sets the request back to none.
    scene.set(hero, "shot/request", "fire");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0002).epsilon(1e-6));
    CHECK(scene.tree(hero).parameters.empty());
    CHECK(scene.tree(hero).state.at("shot/active").as_bool());
    CHECK(scene.take_events() ==
          std::vector<std::string>{R"(animation_started {"animation":"twenty"})"});
    // It fades in over 0.2 seconds, a tick behind its time, as Godot's does.
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0002).epsilon(1e-6));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(15.0));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(20.0));
    // With 0.2 seconds left, it fades out.
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(15.0));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0002).epsilon(1e-6));
    CHECK(scene.take_events() ==
          std::vector<std::string>{R"(animation_finished {"animation":"twenty"})"});
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    CHECK_FALSE(scene.tree(hero).state.contains("shot/active"));
    // The input didn't move on while the shot hid it, as it doesn't sync.
    CHECK(scene.playing(hero) == "ten@0.6*1");
}

TEST_CASE("a Transition cross-fades to the input a request names, starting it over") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kHolds));
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"a": {"type": "Animation", "animation": "ten"},
                  "b": {"type": "Animation", "animation": "thirty"},
                  "state": {"type": "Transition", "xfade_time": 0.2,
                            "inputs": [{"name": "idle", "node": "a"}, {"name": "run", "node": "b"}]}},
        "output": "state"}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    const auto x = [&] { return static_cast<double>(scene.transform(hero).position.x); };

    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    CHECK(scene.tree(hero).state.at("state/current_state").as_string() == "idle");

    scene.set(hero, "state/transition_request", "jump");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0)); // no such input: the request is dropped
    CHECK(scene.tree(hero).parameters.empty());

    scene.set(hero, "state/transition_request", "run");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0003).epsilon(1e-6));
    CHECK(scene.tree(hero).state.at("state/current_state").as_string() == "run");
    CHECK(scene.playing(hero) == "thirty@0*1e-05 ten@0.2*1");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(20.0));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(30.0001).epsilon(1e-6));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(30.0));
    CHECK(scene.playing(hero) == "thirty@0.3*1");
}

TEST_CASE("blend spaces blend the points around their position, or play the closest") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kHolds));
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendSpace1D",
        "points": [{"position": -1, "node": {"type": "Animation", "animation": "ten"}},
                   {"position": 0, "node": {"type": "Animation", "animation": "twenty"}},
                   {"position": 1, "node": {"type": "Animation", "animation": "thirty"}}]}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    const auto x = [&] { return static_cast<double>(scene.transform(hero).position.x); };

    scene.set(hero, "blend_position", 0.5);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(25.0));
    scene.set(hero, "blend_position", -3.0);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    // A point's parameters are named by its index.
    CHECK(scene.tree(hero).state.contains("0/current_position"));

    // In two dimensions, by the triangle the position is in.
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendSpace2D",
        "points": [{"position": [0, 0], "node": {"type": "Animation", "animation": "RESET"}},
                   {"position": [1, 0], "node": {"type": "Animation", "animation": "ten"}},
                   {"position": [0, 1], "node": {"type": "Animation", "animation": "twenty"}}]}})");
    scene.assets.poll();
    scene.tree(hero).parameters = {};
    scene.set(hero, "blend_position", json::Array{0.25, 0.25});
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(7.5));
    // Outside every triangle, by the closest point on its edges.
    scene.set(hero, "blend_position", json::Array{2.0, -1.0});
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));

    // Discrete, only the closest point plays.
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendSpace2D",
        "blend_mode": "discrete",
        "points": [{"position": [0, 0], "node": {"type": "Animation", "animation": "RESET"}},
                   {"position": [1, 0], "node": {"type": "Animation", "animation": "ten"}},
                   {"position": [0, 1], "node": {"type": "Animation", "animation": "twenty"}}]}})");
    scene.assets.poll();
    scene.set(hero, "blend_position", json::Array{0.2, 0.7});
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(20.0));
    CHECK(scene.tree(hero).state.at("closest").as_int() == 2);
}

TEST_CASE("filters choose the tracks a node blends; the rest pass through as Godot's") {
    Scene scene;
    scene.files.write("hero.anim.json", R"({"format": 1, "animations": {
        "RESET": {"length": 1, "tracks": [
            {"path": "Transform2D/position", "keys": [{"time": 0, "value": [0, 0]}]}]},
        "low": {"length": 1, "loop": "linear", "tracks": [
            {"path": "Transform2D/position", "keys": [{"time": 0, "value": [10, 10]}]}]},
        "high": {"length": 1, "loop": "linear", "tracks": [
            {"path": "Transform2D/position/y", "keys": [{"time": 0, "value": 30}]}]}
    }})");
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"low": {"type": "Animation", "animation": "low"},
                  "high": {"type": "Animation", "animation": "high"},
                  "mix": {"type": "Blend2", "inputs": {"in": "low", "blend": "high"},
                          "filter_enabled": true, "filters": ["Transform2D/position/y"]}},
        "output": "mix"}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    scene.set(hero, "mix/blend_amount", 1.0);
    scene.world.step(0.1);
    // The whole position is one track, which the filter doesn't name, so low keeps it.
    CHECK(scene.transform(hero).position.x == doctest::Approx(10.0));
    CHECK(scene.transform(hero).position.y == doctest::Approx(30.0));
}

TEST_CASE("deterministic trees put unplayed tracks back to RESET once; others scale each track's "
          "weights to 1") {
    Scene scene;
    scene.files.write("hero.anim.json", R"({"format": 1, "animations": {
        "RESET": {"length": 1, "tracks": [
            {"path": "Transform2D/position/x", "keys": [{"time": 0, "value": 0}]},
            {"path": "Transform2D/position/y", "keys": [{"time": 0, "value": 0}]},
            {"path": "Transform2D/rotation", "keys": [{"time": 0, "value": 1}]}]},
        "x": {"length": 1, "loop": "linear", "tracks": [
            {"path": "Transform2D/position/x", "keys": [{"time": 0, "value": 10}]}]},
        "y": {"length": 1, "loop": "linear", "tracks": [
            {"path": "Transform2D/position/y", "keys": [{"time": 0, "value": 30}]}]},
        "turn": {"length": 1, "tracks": [{"path": "Transform2D/rotation", "keys": [{"time": 0, "value": 2}]}]}
    }})");
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"x": {"type": "Animation", "animation": "x"},
                  "y": {"type": "Animation", "animation": "y"},
                  "mix": {"type": "Blend2", "inputs": {"in": "x", "blend": "y"}}},
        "output": "mix"}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    scene.set(hero, "mix/blend_amount", 0.5);
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == doctest::Approx(5.0));
    CHECK(scene.transform(hero).position.y == doctest::Approx(15.0));
    // No animation plays rotation: the first tick puts it to RESET's, then leaves it.
    CHECK(scene.transform(hero).rotation == doctest::Approx(1.0));
    scene.transform(hero).rotation = 5.0F;
    scene.world.step(0.1);
    CHECK(scene.transform(hero).rotation == doctest::Approx(5.0));

    scene.tree(hero).deterministic = false;
    scene.tree(hero).state.clear();
    scene.world.step(0.1);
    CHECK(scene.transform(hero).position.x == doctest::Approx(10.0));
    CHECK(scene.transform(hero).position.y == doctest::Approx(30.0));
    CHECK(scene.transform(hero).rotation == doctest::Approx(5.0));
}

TEST_CASE("event tracks and discrete keys fire as the tree's playback passes them, the first "
          "tick's at the start") {
    Scene scene;
    scene.files.write("hero.anim.json", R"({"format": 1, "animations": {
        "step": {"loop": "linear", "length": 0.5, "tracks": [
            {"event": "footstep", "keys": [{"time": 0, "data": "left"}, {"time": 0.25,
                                                                      "data": "right"}]},
            {"path": "Sprite2D/visible", "keys": [{"time": 0, "value": true},
                                                  {"time": 0.3, "value": false}]}]}
    }})");
    scene.files.write("hero.animtree.json",
                      R"({"format": 1, "root": {"type": "Animation", "animation": "step"}})");
    const entt::entity hero = scene.create("hero", R"({"AnimationTree":
        {"tree_root": "hero.animtree.json", "libraries": {"": "hero.anim.json"},
         "deterministic": false}, "Sprite2D": {"visible": false}})");
    std::vector<std::string> events;
    for (int tick = 0; tick < 7; ++tick) {
        scene.world.step(0.1);
        for (const std::string& event : scene.take_events()) {
            events.push_back(std::to_string(tick) + " " + event);
        }
        if (tick == 0) {
            CHECK(scene.world.ecs().get<world::Sprite2D>(hero).visible);
        }
        if (tick == 3) {
            CHECK_FALSE(scene.world.ecs().get<world::Sprite2D>(hero).visible);
        }
    }
    CHECK(events == std::vector<std::string>{"0 footstep \"left\"", "3 footstep \"right\"",
                                             "5 footstep \"left\""});
}

TEST_CASE("a tree reports why it can't play, and its parameters are checked") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kHolds));
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"a": {"type": "Animation", "animation": "tem"},
                  "b": {"type": "Animation", "animation": "ten"},
                  "mix": {"type": "Blend2", "inputs": {"in": "a", "blend": "b"}}},
        "output": "mix"}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    scene.world.step(0.1);
    CHECK(scene.tree(hero).invalid == "On BlendTree node 'a', animation not found: 'tem'");
    CHECK(scene.tree(hero).playing.empty());

    scene.tree(hero).tree_root = "missing.animtree.json";
    scene.world.step(0.1);
    CHECK(scene.tree(hero).invalid.starts_with("missing.animtree.json: "));
}

TEST_CASE("a model node's motion is reported as root motion rather than posed") {
    Scene scene;
    slash::test::Gltf walker;
    walker.document["nodes"] = slash::test::gltf_json(R"([
        {"name": "Root", "children": [1]}, {"name": "Body", "translation": [0, 1, 0]}])");
    json::Value walk;
    walk["name"] = "Walk";
    std::int64_t samplers = 0;
    const auto channel = [&](int node, std::string_view path, const std::vector<float>& values,
                             std::string_view type) {
        json::Value sampler;
        sampler["input"] = walker.floats({0.0F, 1.0F}, "SCALAR");
        sampler["output"] = walker.floats(values, type);
        walk["samplers"].push_back(std::move(sampler));
        json::Value target;
        target["sampler"] = samplers++;
        target["target"]["node"] = node;
        target["target"]["path"] = path;
        walk["channels"].push_back(std::move(target));
    };
    constexpr float kHalfRoot2 = 0.70710678F;
    channel(0, "translation", {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 2.0F}, "VEC3");
    channel(0, "rotation", {0.0F, kHalfRoot2, 0.0F, kHalfRoot2, 0.0F, kHalfRoot2, 0.0F, kHalfRoot2},
            "VEC4");
    channel(1, "translation", {0.0F, 1.0F, 0.0F, 0.0F, 2.0F, 0.0F}, "VEC3");
    walker.document["animations"].push_back(std::move(walk));
    walker.document["scenes"] = slash::test::gltf_json(R"([{"nodes": [0]}])");
    walker.document["scene"] = 0;
    scene.files.write("walker.gltf", walker.gltf());
    scene.files.write("hero.anim.json", R"({"format": 1, "animations": {
        "walk": {"from": {"model": "walker.gltf", "animation": "Walk"}, "loop": "linear"}}})");
    scene.files.write("hero.animtree.json",
                      R"({"format": 1, "root": {"type": "Animation", "animation": "walk"}})");
    const entt::entity hero = scene.create("hero", R"({"AnimationTree":
        {"tree_root": "hero.animtree.json", "libraries": {"": "hero.anim.json"},
         "root_motion_track": "Root"}, "MeshInstance3D": {"mesh": "walker.gltf"}})");

    // Seeking to the start moves nothing; then a quarter second walks half a unit, round the
    // loop's end too.
    scene.world.step(0.25);
    CHECK(near(scene.tree(hero).root_motion_position, {0.0F, 0.0F, 0.0F}));
    Vec3 walked;
    for (int tick = 0; tick < 8; ++tick) {
        scene.world.step(0.25);
        CAPTURE(tick);
        CHECK(near(scene.tree(hero).root_motion_position, {0.0F, 0.0F, 0.5F}));
        walked = walked + scene.tree(hero).root_motion_position;
    }
    CHECK(near(walked, {0.0F, 0.0F, 4.0F}));
    CHECK(near(scene.tree(hero).root_motion_position_accumulator, {0.0F, 0.0F, 0.0F}));
    CHECK(std::abs(scene.tree(hero).root_motion_rotation.w - 1.0F) < 1e-4F);
    CHECK(std::abs(scene.tree(hero).root_motion_rotation_accumulator.y - kHalfRoot2) < 1e-4F);

    // The model is posed without the root's motion, but with the rest of the animation.
    const std::vector<scene3d::ClipSample> clips = scene3d::clips(scene.world, hero);
    REQUIRE(clips.size() == 1);
    CHECK(clips[0].animation == "Walk");
    CHECK(clips[0].node_weights == std::map<std::string, float>{{"Root", 0.0F}});
    auto model = scene.assets.load<scene3d::Model>("walker.gltf");
    REQUIRE(model.ok());
    const scene3d::ModelLoader load =
        [&](const std::string& path) -> slash::Result<std::shared_ptr<const scene3d::Model>> {
        auto found = scene.assets.load<scene3d::Model>(path);
        if (!found) {
            return found.error();
        }
        return found.value().share();
    };
    scene.world.step(0.25); // a quarter of the way through
    const auto pose =
        scene3d::pose_of(*model.value(), scene3d::clips(scene.world, hero), nullptr, load);
    CHECK(near(pose[0].position, {0.0F, 0.0F, 0.0F}));
    CHECK(std::abs(pose[0].rotation.w - 1.0F) < 1e-4F);
    CHECK(near(pose[1].position, {0.0F, 1.25F, 0.0F}));

    // In the node's own frame, a quarter turn about y, the walk goes along -x.
    scene.tree(hero).root_motion_local = true;
    scene.world.step(0.25);
    CHECK(near(scene.tree(hero).root_motion_position, {-0.5F, 0.0F, 0.0F}));
}

TEST_CASE("tree files are checked, with every error at its JSON Pointer") {
    CHECK(errors_of(parse(R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"a": {"type": "Animation", "animation": "walk"},
                  "mix": {"type": "Blend2", "inputs": {"in": "a", "blend": "b"}}},
        "output": "mix"}})")) == "/root/nodes/mix/inputs/blend: the blend tree has no node \"b\"; "
                                 "did you mean \"a\"?");
    const auto tree = animation::parse_tree(parse(R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"walk": {"type": "Animation", "animation": "walk"},
                  "speed": {"type": "TimeScale", "inputs": {"in": "walk"}}},
        "output": "speed"}})"));
    REQUIRE(tree.ok());
    std::vector<std::string> inputs;
    for (const animation::TreeParameter& parameter : animation::tree_parameters(tree.value())) {
        if (parameter.input) {
            inputs.push_back(parameter.key + "=" + json::write(parameter.default_value));
        }
    }
    CHECK(inputs == std::vector<std::string>{"speed/scale=1.0"});
}

TEST_CASE("AnimationTrees in a scene are checked against their trees and libraries") {
    MemoryFiles files;
    files.write("hero.anim.json", R"({"format": 1, "animations": {
        "walk": {"length": 1, "tracks": [{"path": "Transform2D/position/x", "keys": [{"time": 0, "value": 0}]}]}
    }})");
    files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {"walk": {"type": "Animation", "animation": "wakl"},
                  "speed": {"type": "TimeScale", "inputs": {"in": "walk"}}},
        "output": "speed"}})");
    const json::Value scene = parse(R"({"format": 1, "entities": {"hero": {"components": {
        "AnimationTree": {"tree_root": "hero.animtree.json", "libraries": {"": "hero.anim.json"},
                          "parameters": {"speed/scael": 2, "speed/current_position": 1}}}}}})");
    std::vector<std::string> errors;
    for (const auto& error : animation::check_trees(scene, files.reader())) {
        errors.push_back(error.path + ": " + error.message);
    }
    CHECK(errors ==
          std::vector<std::string>{
              "/entities/hero/components/AnimationTree/libraries: "
              "Transform2D/position/x has no key in a RESET animation in the library "
              "\"\"; a deterministic tree sets it to 0 while no animation plays it, as "
              "Godot's does: add a RESET animation with its rest value, or set "
              "deterministic to false",
              "/entities/hero/components/AnimationTree/libraries: hero.animtree.json: "
              "/root/nodes/walk/animation: the libraries have no animation \"wakl\"; "
              "did you mean \"walk\"?",
              "/entities/hero/components/AnimationTree/parameters/speed~1current_position: "
              "\"speed/current_position\" is kept by the tree as it plays, in state",
              "/entities/hero/components/AnimationTree/parameters/speed~1scael: "
              "hero.animtree.json has no parameter \"speed/scael\"; did you mean "
              "\"speed/scale\"?"});
}

// Animations for state machines: each holds Transform2D/position/x at one value, idle, walk and
// run looping over a second, jump once over half a second; RESET puts it at 0.
constexpr std::string_view kMoves = R"({"format": 1, "animations": {
    "RESET": {"length": 1, "tracks": [{"path": "Transform2D/position/x", "keys": [{"time": 0, "value": 0}]}]},
    "idle": {"length": 1, "loop": "linear", "tracks": [{"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 10}]}]},
    "walk": {"length": 1, "loop": "linear", "tracks": [{"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 20}]}]},
    "run": {"length": 1, "loop": "linear", "tracks": [{"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 30}]}]},
    "jump": {"length": 0.5, "tracks": [{"path": "Transform2D/position/x",
        "keys": [{"time": 0, "value": 40}]}]}
}})";

TEST_CASE("a state machine starts at the state after Start and travels the cheapest way through "
          "its transitions, cross-fading, as Godot's") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kMoves));
    // Walking is the shorter way to run, unless leaving idle for walk costs more.
    const std::string machine = R"({"format": 1, "root": {"type": "StateMachine",
        "states": {"idle": {"node": {"type": "Animation", "animation": "idle"}, "position": [0, 0]},
                   "walk": {"node": {"type": "Animation", "animation": "walk"}, "position": [100, 0]},
                   "sneak": {"node": {"type": "Animation", "animation": "jump"}, "position": [100, 300]},
                   "run": {"node": {"type": "Animation", "animation": "run"}, "position": [200, 0]},
                   "hurt": {"node": {"type": "Animation", "animation": "walk"}, "position": [300, 0]}},
        "transitions": [{"from": "Start", "to": "idle", "advance_mode": "auto"},
                        {"from": "idle", "to": "walk", "xfade_time": 0.2, "priority": PRIORITY},
                        {"from": "walk", "to": "run", "xfade_time": 0.2},
                        {"from": "idle", "to": "sneak"},
                        {"from": "sneak", "to": "run"},
                        {"from": "run", "to": "idle"}]}})";
    const auto write = [&](std::string_view priority) {
        std::string text = machine;
        text.replace(text.find("PRIORITY"), 8, priority);
        scene.files.write("hero.animtree.json", text);
    };
    write("1");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    const auto x = [&] { return static_cast<double>(scene.transform(hero).position.x); };
    const auto playback = [&] {
        return json::write(scene.tree(hero).state.at("playback"), json::Style::Compact);
    };

    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    CHECK(playback() == R"({"current":"idle","playing":true})");
    scene.world.step(0.1);
    CHECK(scene.playing(hero) == "idle@0.1*1");

    // A travel finds its path, then takes the first transition at the end of the tick.
    scene.set(hero, "travel", "run");
    scene.world.step(0.1);
    CHECK(scene.tree(hero).parameters.empty());
    CHECK(scene.playing(hero) == "idle@0.2*1");
    CHECK(
        playback() ==
        R"({"current":"walk","fading_from":"idle","fading_time":0.2,"path":["run"],"playing":true,"reset_request":true})");
    // Walk starts over and cross-fades in; the next transition waits for the cross-fade.
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(15.0));
    CHECK(scene.playing(hero) == "walk@0*0.5 idle@0.3*0.5");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(20.0).epsilon(1e-4));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(25.0));
    scene.world.step(0.1);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(30.0));
    CHECK(playback() == R"({"current":"run","playing":true})");

    // Back at idle, a costly first step sends the travel the other way.
    write("6");
    scene.assets.poll();
    scene.set(hero, "start", "idle");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    scene.set(hero, "travel", "run");
    scene.world.step(0.1);
    // Without cross-fades, every step is taken at the end of the tick: sneak counts for nothing
    // and run starts over on the next tick.
    CHECK(x() == doctest::Approx(10.0));
    CHECK(scene.playing(hero) == "idle@0.1*1 jump@0*0");
    CHECK(
        playback() ==
        R"({"current":"run","playing":true,"reset_request":true,"reset_request_for_fading_from":true})");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(30.0));
    CHECK(scene.playing(hero) == "run@0*1");

    // A travel to the state playing does nothing; to one with no way there, it jumps there.
    scene.set(hero, "travel", "run");
    scene.world.step(0.1);
    CHECK(scene.playing(hero) == "run@0.1*1");
    scene.set(hero, "travel", "hurt");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(20.0));
    CHECK(scene.playing(hero) == "walk@0*1");
}

TEST_CASE("auto transitions wait for their conditions or the end of the state, and reach End") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kMoves));
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "StateMachine",
        "states": {"idle": {"node": {"type": "Animation", "animation": "idle"}},
                   "jump": {"node": {"type": "Animation", "animation": "jump"}}},
        "transitions": [{"from": "Start", "to": "idle", "advance_mode": "auto"},
                        {"from": "idle", "to": "jump", "advance_mode": "auto",
                         "advance_condition": "jumping"},
                        {"from": "jump", "to": "idle", "advance_mode": "auto",
                         "switch_mode": "at_end", "xfade_time": 0.2},
                        {"from": "idle", "to": "End", "advance_mode": "auto",
                         "advance_condition": "done"}]}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    const auto x = [&] { return static_cast<double>(scene.transform(hero).position.x); };

    scene.world.step(0.1);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    scene.take_events();

    // The condition lets the transition go at the end of the tick; jump starts on the next.
    scene.set(hero, "conditions/jumping", true);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    scene.set(hero, "conditions/jumping", false);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(40.0));
    CHECK(scene.take_events() ==
          std::vector<std::string>{R"(animation_started {"animation":"jump"})"});
    // At its end less the cross-fade, it goes back to idle, cross-fading.
    scene.world.step(0.1);
    scene.world.step(0.1);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(40.0));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(25.0));
    CHECK(scene.playing(hero) == "idle@0*0.5 jump@0.4*0.5");
    scene.world.step(0.1);
    CHECK(scene.take_events() ==
          std::vector<std::string>{R"(animation_finished {"animation":"jump"})"});
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    CHECK(scene.playing(hero) == "idle@0.2*1");

    // End plays nothing, as the tree's RESET values show.
    scene.set(hero, "conditions/done", true);
    scene.world.step(0.1);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(0.0));
    CHECK(scene.playing(hero).empty());
    CHECK(json::write(scene.tree(hero).state.at("playback"), json::Style::Compact) ==
          R"({"current":"End","playing":true})");
    // A start plays it again, and a stop plays nothing, keeping where it was.
    scene.set(hero, "conditions/done", false);
    scene.set(hero, "start", "jump");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(40.0));
    // next takes the transition now, without waiting for jump's end.
    scene.set(hero, "next", true);
    scene.world.step(0.1);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(25.0));
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0).epsilon(1e-4));
    // A stop plays nothing for a tick, then the state machine goes on, stopped: a travel then
    // starts over from Start.
    scene.set(hero, "stop", true);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(0.0));
    CHECK(json::write(scene.tree(hero).state.at("playback"), json::Style::Compact) ==
          R"({"current":"idle"})");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    scene.set(hero, "travel", "jump");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(40.0));
    CHECK(json::write(scene.tree(hero).state.at("playback"), json::Style::Compact) ==
          R"({"current":"jump","playing":true})");
}

TEST_CASE("a travel into a grouped state machine goes on through its Start, and one out of it "
          "finds the way to its End") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kMoves));
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "StateMachine",
        "states": {
            "idle": {"node": {"type": "Animation", "animation": "idle"}, "position": [0, 0]},
            "combat": {"position": [100, 0], "node": {"type": "StateMachine",
                "state_machine_type": "grouped",
                "states": {"aim": {"node": {"type": "Animation", "animation": "jump"}},
                           "shoot": {"node": {"type": "Animation", "animation": "run"}}},
                "transitions": [{"from": "Start", "to": "aim"},
                                {"from": "aim", "to": "shoot", "switch_mode": "at_end"},
                                {"from": "shoot", "to": "End"}]}}},
        "transitions": [{"from": "Start", "to": "idle", "advance_mode": "auto"},
                        {"from": "idle", "to": "combat"},
                        {"from": "combat", "to": "idle"}]}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    const auto x = [&] { return static_cast<double>(scene.transform(hero).position.x); };
    const auto playback = [&](const std::string& key) {
        return json::write(scene.tree(hero).state.at(key), json::Style::Compact);
    };

    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));

    // A travel to a state in the group finds the way inside it too: Start, aim, then shoot.
    scene.set(hero, "travel", "combat/shoot");
    scene.world.step(0.1);
    CHECK(playback("playback") == R"({"current":"combat","playing":true,"reset_request":true})");
    CHECK(
        playback("combat/playback") ==
        R"({"current":"Start","path":["aim","shoot"],"playing":true,"reset_request_on_teleport":true,"teleport_request":true})");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(40.0));
    CHECK(playback("combat/playback") == R"({"current":"aim","path":["shoot"],"playing":true})");
    // aim goes on to shoot at its end.
    for (int i = 0; i < 6; ++i) {
        scene.world.step(0.1);
    }
    CHECK(x() == doctest::Approx(30.0));
    CHECK(playback("combat/playback") == R"({"current":"shoot","playing":true})");

    // A travel out of the group goes on to its End and leaves it.
    scene.set(hero, "travel", "idle");
    scene.world.step(0.1);
    CHECK(playback("combat/playback") == R"({"current":"End","playing":true})");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(10.0));
    CHECK(playback("playback") == R"({"current":"idle","playing":true})");
}

TEST_CASE("a nested state machine carries on when its parent seeks to its start, until it ends") {
    Scene scene;
    scene.files.write("hero.anim.json", std::string(kMoves));
    scene.files.write("hero.animtree.json", R"({"format": 1, "root": {"type": "BlendTree",
        "nodes": {
            "legs": {"type": "StateMachine", "state_machine_type": "nested",
                "states": {"walk": {"node": {"type": "Animation", "animation": "walk"}},
                           "run": {"node": {"type": "Animation", "animation": "run"}}},
                "transitions": [{"from": "Start", "to": "walk", "advance_mode": "auto"},
                                {"from": "walk", "to": "run"}]},
            "wave": {"type": "Animation", "animation": "jump"},
            "pick": {"type": "Transition", "inputs": [{"name": "legs", "node": "legs"},
                                                      {"name": "wave", "node": "wave"}]}},
        "output": "pick"}})");
    const entt::entity hero = scene.create("hero", kTreeComponent);
    const auto x = [&] { return static_cast<double>(scene.transform(hero).position.x); };

    scene.world.step(0.1);
    scene.set(hero, "legs/travel", "run");
    scene.world.step(0.1);
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(30.0));
    // Switched away and back, the Transition starts it over: a nested machine keeps its state.
    scene.set(hero, "pick/transition_request", "wave");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(40.0));
    scene.set(hero, "pick/transition_request", "legs");
    scene.world.step(0.1);
    CHECK(x() == doctest::Approx(30.0));
    // The Transition fades wave out over no time, as Godot's.
    CHECK(scene.playing(hero) == "run@0*1 jump@0.1*0");
}

TEST_CASE("state machine files and travel requests are checked") {
    CHECK(errors_of(parse(R"({"format": 1, "root": {"type": "StateMachine",
        "states": {"idle": {"node": {"type": "Animation", "animation": "idle"}},
                   "a/b": {"node": {"type": "Animation", "animation": "idle"}},
                   "empty": {},
                   "End": {"node": {"type": "Animation", "animation": "idle"}},
                   "group": {"node": {"type": "StateMachine", "state_machine_type": "grouped",
                       "states": {"x": {"node": {"type": "Animation", "animation": "idle"}}},
                       "transitions": [{"from": "Start", "to": "x"}, {"from": "Start", "to": "End"}]}}},
        "transitions": [{"from": "End", "to": "idle"},
                        {"from": "Start", "to": "Start"},
                        {"from": "idel", "to": "idle"},
                        {"from": "idle", "to": "idle"},
                        {"from": "idle", "to": "group", "advance_condition": "a/b"},
                        {"from": "idle", "to": "group"}]}})")) ==
          "/root/states/End/node: \"End\" is the state machine's own and plays nothing; it takes "
          "only a position\n"
          "/root/states/a~1b: state names can't have \"/\", which goes between a grouped state "
          "machine's name and its states'\n"
          "/root/states/empty: a state needs a node, as {\"node\": {\"type\": \"Animation\", "
          "\"animation\": \"Idle\"}}\n"
          "/root/transitions/0/from: nothing leaves End\n"
          "/root/transitions/1/to: nothing goes to Start; the state machine's \"start\" "
          "parameter starts it over\n"
          "/root/transitions/2/from: the state machine has no state \"idel\"; did you mean "
          "\"idle\"?\n"
          "/root/transitions/3: a transition goes from one state to another; with "
          "allow_transition_to_self, a travel to the current state starts it over\n"
          "/root/transitions/4/advance_condition: condition names can't have \"/\" or \":\"\n"
          "/root/transitions/5: another transition goes from \"idle\" to \"group\"\n"
          "/root/states/group: the transitions to a grouped state machine's End go on through "
          "those out of it, so there are as many of each; \"group\" has 1 to its End and 0 out "
          "of it");
    CHECK(errors_of(parse(R"({"format": 1, "root": {"type": "StateMachine",
        "state_machine_type": "grouped"}})")) ==
          "/root/state_machine_type: a grouped state machine is a state of another state "
          "machine, which plays it as part of itself");

    const auto tree = animation::parse_tree(parse(R"({"format": 1, "root": {"type": "StateMachine",
        "states": {"idle": {"node": {"type": "Animation", "animation": "idle"}},
                   "fight": {"node": {"type": "StateMachine", "state_machine_type": "grouped",
                       "states": {"punch": {"node": {"type": "Animation", "animation": "idle"}}},
                       "transitions": [{"from": "Start", "to": "punch"}]}}},
        "transitions": [{"from": "idle", "to": "fight", "advance_mode": "auto",
                         "advance_condition": "angry"}]}})"));
    REQUIRE(tree.ok());
    const auto parameters = animation::tree_parameters(tree.value());
    std::vector<std::string> keys;
    for (const auto& parameter : parameters) {
        keys.push_back(parameter.key + (parameter.input ? " (input)" : ""));
    }
    CHECK(keys == std::vector<std::string>{"current_length",
                                           "current_position",
                                           "current_delta",
                                           "playback",
                                           "travel (input)",
                                           "start (input)",
                                           "next (input)",
                                           "stop (input)",
                                           "conditions/angry (input)",
                                           "fight/current_length",
                                           "fight/current_position",
                                           "fight/current_delta",
                                           "fight/playback",
                                           "fight/punch/current_length",
                                           "fight/punch/current_position",
                                           "fight/punch/current_delta",
                                           "fight/punch/backward",
                                           "idle/current_length",
                                           "idle/current_position",
                                           "idle/current_delta",
                                           "idle/backward"});
    const auto travel = std::ranges::find(parameters, "travel", &animation::TreeParameter::key);
    const auto check = [&](const json::Value& value) {
        return animation::check_parameter(tree.value(), *travel, value);
    };
    CHECK(check("fight/punch").empty());
    CHECK(check("").empty());
    CHECK(check("idel") == "the state machine has no state \"idel\"; did you mean \"idle\"?");
    CHECK(check("fight/punsh") == "\"fight\" has no state \"punsh\"; did you mean \"punch\"?");
    CHECK(check("idle/x") == "\"idle\" isn't a grouped state machine, so a path can't go into it");
    CHECK(check("fight/End") ==
          "a grouped state machine's End isn't played on its own; play the state before or "
          "after the group");
    CHECK(check(3) == "expected the name of one of the state machine's states");
}
