#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/scene3d/streaming.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// A world as sessions make one, its files in memory.
struct Scene {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{asset::FileSource{
        .read = [](std::string_view /*path*/) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no files here");
        },
        .stamp = [](std::string_view /*path*/) -> std::optional<asset::FileStamp> {
            return std::nullopt;
        }}};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        world.set_file_reader([this](std::string_view path) -> slash::Result<std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound,
                                         "\"" + std::string(path) + "\" doesn't exist");
            }
            return found->second;
        });
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    std::vector<std::string> kinds(std::string_view prefix) const {
        std::vector<std::string> found;
        for (const world::Event& event : events) {
            if (event.kind.starts_with(prefix)) {
                found.push_back(event.kind + " " + event.id);
            }
        }
        return found;
    }
};

constexpr std::string_view kCell = R"({"format": 1, "root": "ground", "entities": {
    "ground": {"components": {"Transform3D": {"children": ["rock"]}, "BoxMesh": {},
                              "MeshInstance3D": {}}},
    "rock": {"components": {"Transform3D": {"position": [1, 0, 0]}}}}})";

} // namespace

TEST_CASE("streaming: a StreamedScene3D loads its scene near a source and unloads it past the "
          "margin") {
    Scene scene;
    scene.files["cells/a.scene.json"] = std::string(kCell);
    const entt::entity cell = scene.create("a", R"({"Transform3D": {"position": [0, 0, 50]},
        "StreamedScene3D": {"scene": "cells/a.scene.json", "size": [10, 10, 10],
                            "load_radius": 20, "unload_margin": 5}})");
    const entt::entity player =
        scene.create("player", R"({"Transform3D": {}, "StreamingSource3D": {}})");
    scene.world.ecs().get<world::Transform3D>(player).position = {100.0F, 0.0F, 50.0F};
    scene.world.step(1.0 / 60.0);
    REQUIRE((scene.world.find("a_scene") == entt::null));
    std::vector<scene3d::StreamedCell> cells = scene3d::streamed_cells(scene.world);
    REQUIRE(cells.size() == 1);
    CHECK_FALSE(cells[0].loaded);
    REQUIRE(cells[0].distance.has_value());
    CHECK(*cells[0].distance == doctest::Approx(95.0));

    // 19 meters from the box: in.
    scene.world.ecs().get<world::Transform3D>(player).position = {24.0F, 0.0F, 50.0F};
    scene.world.step(1.0 / 60.0);
    const entt::entity root = scene.world.find("a_scene");
    REQUIRE((root != entt::null));
    const entt::entity rock = scene.world.find("a_scene/rock");
    REQUIRE((rock != entt::null));
    CHECK(scene.world.ecs().get<world::Transform3D>(cell).children ==
          std::vector<entt::entity>{root});
    const world::Tree3D tree(scene.world);
    CHECK(tree.global(rock).origin == slash::Vec3{1.0F, 0.0F, 50.0F});
    CHECK(scene3d::streamed_cells(scene.world)[0].loaded);
    REQUIRE(scene.kinds("stream") == std::vector<std::string>{"streamed_in a"});
    CHECK(scene.events.back().data ==
          parse(R"({"scene": "cells/a.scene.json", "root": "a_scene"})"));

    // 23 meters: inside the margin, so it stays.
    scene.world.ecs().get<world::Transform3D>(player).position = {28.0F, 0.0F, 50.0F};
    scene.world.step(1.0 / 60.0);
    CHECK((scene.world.find("a_scene") != entt::null));

    // 26 meters: out, and the scene's entities go with it.
    scene.world.ecs().get<world::Transform3D>(player).position = {31.0F, 0.0F, 50.0F};
    scene.world.step(1.0 / 60.0);
    CHECK((scene.world.find("a_scene") == entt::null));
    CHECK((scene.world.find("a_scene/rock") == entt::null));
    CHECK(scene.world.ecs().get<world::Transform3D>(cell).children.empty());
    CHECK(scene.kinds("stream") == std::vector<std::string>{"streamed_in a", "streamed_out a"});

    // A disabled source streams nothing in, and with no source everything goes.
    scene.world.ecs().get<world::Transform3D>(player).position = {0.0F, 0.0F, 50.0F};
    scene.world.ecs().get<world::StreamingSource3D>(player).enabled = false;
    scene.world.step(1.0 / 60.0);
    CHECK((scene.world.find("a_scene") == entt::null));
    CHECK_FALSE(scene3d::streamed_cells(scene.world)[0].distance.has_value());
    scene.world.ecs().get<world::StreamingSource3D>(player).enabled = true;
    scene.world.step(1.0 / 60.0);
    CHECK((scene.world.find("a_scene") != entt::null));
    scene.world.destroy(player);
    scene.world.step(1.0 / 60.0);
    CHECK((scene.world.find("a_scene") == entt::null));
}

TEST_CASE("streaming: a scene that can't load says so once, and scene validate finds why") {
    Scene scene;
    scene.files["cells/rootless.scene.json"] = R"({"format": 1, "entities": {"x": {}}})";
    scene.create("player", R"({"Transform3D": {}, "StreamingSource3D": {}})");
    const entt::entity missing = scene.create(
        "missing", R"({"Transform3D": {}, "StreamedScene3D": {"scene": "cells/gone.scene.json"}})");
    for (int i = 0; i < 3; ++i) {
        scene.world.step(1.0 / 60.0);
    }
    CHECK((scene.world.find("missing_scene") == entt::null));
    REQUIRE(scene.kinds("stream") == std::vector<std::string>{"stream_failed missing"});
    CHECK(scene.events.back().data.find("scene")->as_string() == "cells/gone.scene.json");
    // A new scene path is tried again.
    scene.files["cells/b.scene.json"] = std::string(kCell);
    scene.world.ecs().get<world::StreamedScene3D>(missing).scene = "cells/b.scene.json";
    scene.world.step(1.0 / 60.0);
    CHECK((scene.world.find("missing_scene") != entt::null));

    const entt::entity rootless = scene.create(
        "rootless",
        R"({"Transform3D": {}, "StreamedScene3D": {"scene": "cells/rootless.scene.json"}})");
    const entt::entity loose = scene.create("loose", R"({"StreamedScene3D": {"scene": "a.json"}})");
    const entt::entity unread = scene.create(
        "unread", R"({"Transform3D": {}, "StreamedScene3D": {"scene": "x.scene.json"}})");
    const std::vector<scene3d::Problem> problems = scene3d::check_streaming(scene.world);
    REQUIRE(problems.size() == 4);
    CHECK(problems[0].entity == rootless);
    CHECK(problems[0].path == "/components/StreamedScene3D/scene");
    CHECK(problems[0].message == "\"cells/rootless.scene.json\" names no root: save it with "
                                 "scene.save's root");
    CHECK(problems[1].entity == loose);
    CHECK(problems[1].message == "needs a Transform3D to place its scene under");
    CHECK(problems[2].entity == loose);
    CHECK(problems[2].message == "\"a.json\" is no .scene.json file");
    CHECK(problems[3].entity == unread);
    CHECK(problems[3].message.starts_with("\"x.scene.json\" can't be read: "));
}
