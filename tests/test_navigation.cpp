#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace json = slash::json;
namespace navigation = slash::navigation;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

namespace {

constexpr double kTick = 1.0 / 60.0;

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

// A world with the built-in components and the navigation system, and the events it emits.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    explicit Scene(const navigation::Settings& settings = {}) {
        world::register_builtin_components(components);
        navigation::install(world, assets, settings);
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

    std::vector<std::string> kinds(std::string_view entity) const {
        std::vector<std::string> out;
        for (const world::Event& event : events) {
            if (event.id == entity && !event.kind.starts_with("entity.")) {
                out.push_back(event.kind);
            }
        }
        return out;
    }
};

// Four squares of side 4 in the x-z plane, an L from (0, 0) to (8, 8) with the corner at (8, 0):
// a at x 0-4, b at x 4-8 and c above b at z 4-8.
constexpr std::string_view kL = R"({"format": 1,
    "vertices": [[0, 0, 0], [4, 0, 0], [8, 0, 0], [8, 0, 4], [4, 0, 4], [0, 0, 4],
                 [8, 0, 8], [4, 0, 8]],
    "polygons": [[0, 5, 4, 1], [1, 4, 3, 2], [4, 7, 6, 3]]})";

std::string text_of(Vec3 v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g)", static_cast<double>(v.x),
                  static_cast<double>(v.y), static_cast<double>(v.z));
    return buffer;
}

std::string text_of(Vec2 v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "(%g, %g)", static_cast<double>(v.x),
                  static_cast<double>(v.y));
    return buffer;
}

template <typename V>
std::string text_of(const std::vector<V>& points) {
    std::string out;
    for (const V& point : points) {
        out += (out.empty() ? "" : " ") + text_of(point);
    }
    return out;
}

} // namespace

TEST_CASE("navigation files are validated with the JSON Pointer of each error") {
    CHECK(navigation::validate_mesh(parse(kL)).empty());
    CHECK(json::validate(parse(kL), navigation::mesh_schema()).empty());

    const auto errors = navigation::validate_mesh(parse(R"({"format": 1,
        "vertices": [[0, 0, 0], [4, 0, 0], [4, 0, 4], [0, 0, 4], [2, 0, 1]],
        "polygons": [[0, 1, 7], [0, 1, 1], [0, 1, 2, 4, 3], [0, 1, 1, 0], [0, 4, 1]]})"));
    REQUIRE(errors.size() == 5);
    CHECK(errors[0].path == "/polygons/0/2");
    CHECK(errors[0].message == "vertex 7 is past the last of 5 vertices");
    CHECK(errors[1].path == "/polygons/1/2");
    CHECK(errors[1].message == "vertex 1 is in the polygon twice");
    CHECK(errors[2].path == "/polygons/2");
    CHECK(errors[2].message == "the polygon isn't convex; split it into convex polygons");
    CHECK(errors[3].path == "/polygons/3/2");
    CHECK(errors[4].path == "/polygons/3/3");

    const auto flat = navigation::validate_polygon(parse(R"({"format": 1,
        "vertices": [[0, 0], [4, 0], [8, 0]], "polygons": [[0, 1, 2]]})"));
    REQUIRE(flat.size() == 1);
    CHECK(flat[0].path == "/polygons/0");
    CHECK(flat[0].message == "the polygon has no area");

    // Either way round is fine, as in Godot.
    CHECK(navigation::validate_polygon(parse(R"({"format": 1,
        "vertices": [[0, 0], [4, 0], [4, 4]], "polygons": [[0, 1, 2], [2, 1, 0]]})"))
              .empty());
    CHECK(!navigation::validate_polygon(parse(R"({"format": 1, "vertices": [[0, 0, 0]],
        "polygons": []})"))
               .empty());

    const auto mesh = navigation::parse_mesh(parse(kL));
    REQUIRE(mesh.ok());
    CHECK(mesh.value().vertices.size() == 8);
    CHECK(mesh.value().polygons.size() == 3);
    CHECK(mesh.value().cell_size == 0.25F);
}

TEST_CASE("paths over a navigation mesh are shaped as Godot's three postprocessings") {
    Scene scene;
    scene.files.write("l.navmesh.json", std::string(kL));
    const entt::entity floor = scene.create("floor", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "l.navmesh.json"}})");
    const auto info = navigation::map_info(scene.world, navigation::Map::ThreeD);
    CHECK(info.regions == 1);
    CHECK(info.polygons == 3);
    CHECK(info.free_edges == 8);
    CHECK(info.merge_errors == 0);
    CHECK(info.errors.empty());

    const auto path_with = [&](navigation::Postprocessing postprocessing) {
        navigation::PathOptions options;
        options.postprocessing = postprocessing;
        return navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{6, 0, 7}, options);
    };
    // The funnel pulls the path tight round the L's inner corner.
    const auto funnel = path_with(navigation::Postprocessing::CorridorFunnel);
    CHECK(text_of(funnel.points) == "(1, 0, 1) (4, 0, 4) (6, 0, 7)");
    CHECK(funnel.length == doctest::Approx(std::sqrt(18.0) + std::sqrt(13.0)));
    REQUIRE(funnel.owners.size() == 3);
    CHECK(funnel.owners[1] == floor);
    CHECK(funnel.types[1] == navigation::Segment::Region);
    // Through the middle of each edge it crosses.
    CHECK(text_of(path_with(navigation::Postprocessing::EdgeCentered).points) ==
          "(1, 0, 1) (4, 0, 2) (6, 0, 4) (6, 0, 7)");
    // Where it first touches each edge.
    CHECK(text_of(path_with(navigation::Postprocessing::None).points) ==
          "(1, 0, 1) (4, 0, 1) (4, 0, 4) (6, 0, 7)");

    std::string name(navigation::postprocessing_name(navigation::Postprocessing::EdgeCentered));
    CHECK(name == "edgecentered");
    navigation::Postprocessing parsed{};
    CHECK(navigation::postprocessing_from_name("none", parsed));
    CHECK(parsed == navigation::Postprocessing::None);
    CHECK(!navigation::postprocessing_from_name("funnel", parsed));
}

TEST_CASE("paths from off the map start and end at its closest points") {
    Scene scene;
    scene.files.write("l.navmesh.json", std::string(kL));
    const entt::entity floor = scene.create("floor", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "l.navmesh.json"}})");
    const auto path = navigation::query_path(scene.world, Vec3{-3, 2, 1}, Vec3{20, 0, 20});
    CHECK(text_of(path.points) == "(0, 0, 1) (4, 0, 4) (8, 0, 8)");

    const auto closest = navigation::closest_point(scene.world, Vec3{-3, 2, 1});
    CHECK(text_of(closest.point) == "(0, 0, 1)");
    CHECK(closest.owner == floor);
    // Godot's: the cross of the polygon's first two edges, not normalized.
    CHECK(text_of(closest.normal) == "(0, 16, 0)");
    const auto above = navigation::closest_point(scene.world, Vec3{6, 3, 6});
    CHECK(text_of(above.point) == "(6, 0, 6)");

    Scene empty;
    CHECK(navigation::query_path(empty.world, Vec3{0, 0, 0}, Vec3{1, 0, 1}).points.empty());
    CHECK((navigation::closest_point(empty.world, Vec3{1, 2, 3}).owner == entt::null));
}

TEST_CASE("random points repeat from a seed and land on the map") {
    Scene scene;
    scene.files.write("l.navmesh.json", std::string(kL));
    scene.create("floor", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "l.navmesh.json"}})");
    for (const bool uniformly : {false, true}) {
        for (std::uint64_t seed = 0; seed < 20; ++seed) {
            const Vec3 point = navigation::random_point_3d(scene.world, 1, uniformly, seed);
            CHECK(text_of(point) ==
                  text_of(navigation::random_point_3d(scene.world, 1, uniformly, seed)));
            CHECK(point.y == 0.0F);
            // In the L: x 0-8, z 0-4, or x 4-8 above that.
            const bool in_bottom = point.x >= 0 && point.x <= 8 && point.z >= 0 && point.z <= 4;
            const bool in_top = point.x >= 4 && point.x <= 8 && point.z >= 4 && point.z <= 8;
            CHECK((in_bottom || in_top));
        }
    }
    CHECK(text_of(navigation::random_point_3d(scene.world, 1, false, 1)) !=
          text_of(navigation::random_point_3d(scene.world, 1, false, 2)));
    // No region on layer 2.
    CHECK(text_of(navigation::random_point_3d(scene.world, 2, true, 1)) == "(0, 0, 0)");
}

TEST_CASE("regions join where edges meet or come within the edge connection margin") {
    Scene scene;
    scene.files.write("a.navmesh.json", R"({"format": 1,
        "vertices": [[0, 0, 0], [0, 0, 4], [4, 0, 4], [4, 0, 0]], "polygons": [[0, 1, 2, 3]]})");
    scene.create("a", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "a.navmesh.json"}})");
    const entt::entity b = scene.create("b", R"({"Transform3D": {"position": [4, 0, 0]},
        "NavigationRegion3D": {"navigation_mesh": "a.navmesh.json"}})");
    auto info = navigation::map_info(scene.world, navigation::Map::ThreeD);
    CHECK(info.regions == 2);
    // The shared edge joins them; the path has a point where it crosses it, as Godot's.
    CHECK(info.edge_connections == 1);
    CHECK(info.free_edges == 6);
    CHECK(text_of(navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 3}).points) ==
          "(1, 0, 1) (4, 0, 2) (7, 0, 3)");

    // A gap narrower than the margin (0.25 by default) is joined by an edge connection.
    scene.world.ecs().get<world::Transform3D>(b).position = {4.2F, 0, 0};
    info = navigation::map_info(scene.world, navigation::Map::ThreeD);
    CHECK(info.edge_connections == 2);
    const auto across = navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 3});
    CHECK(across.points.back().x == doctest::Approx(7));

    // A wider one isn't: the path ends as close as it can get.
    scene.world.ecs().get<world::Transform3D>(b).position = {5, 0, 0};
    info = navigation::map_info(scene.world, navigation::Map::ThreeD);
    CHECK(info.edge_connections == 0);
    CHECK(text_of(navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 3}).points) ==
          "(1, 0, 1) (4, 0, 3)");

    // Unless the map's margin is wider, or a region turns edge connections off.
    navigation::Settings wide;
    wide.map_3d.edge_connection_margin = 1.5F;
    navigation::set_settings(scene.world, wide);
    CHECK(navigation::map_info(scene.world, navigation::Map::ThreeD).edge_connections == 2);
    scene.world.ecs().get<world::NavigationRegion3D>(b).use_edge_connections = false;
    CHECK(navigation::map_info(scene.world, navigation::Map::ThreeD).edge_connections == 0);
}

TEST_CASE("links carry paths between regions, and travel costs steer them") {
    Scene scene;
    // Two rooms joined the long way round by a corridor, and a link straight across.
    scene.files.write("u.navmesh.json", R"({"format": 1,
        "vertices": [[0, 0, 0], [0, 0, 4], [4, 0, 4], [4, 0, 0], [0, 0, 8], [4, 0, 8],
                     [8, 0, 0], [8, 0, 4], [12, 0, 4], [12, 0, 0], [8, 0, 8], [12, 0, 8],
                     [0, 0, 12], [12, 0, 12], [4, 0, 12], [8, 0, 12]],
        "polygons": [[0, 1, 2, 3], [1, 4, 5, 2], [4, 12, 14, 5], [5, 14, 15, 10],
                     [10, 15, 13, 11], [7, 10, 11, 8], [6, 7, 8, 9]]})");
    scene.create("rooms", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "u.navmesh.json"}})");
    const entt::entity link = scene.create("door", R"({"Transform3D": {},
        "NavigationLink3D": {"start_position": [3, 0, 2], "end_position": [9, 0, 2]}})");
    CHECK(navigation::map_info(scene.world, navigation::Map::ThreeD).links == 1);

    const auto through = navigation::query_path(scene.world, Vec3{2, 0, 2}, Vec3{10, 0, 2});
    CHECK(text_of(through.points) == "(2, 0, 2) (3, 0, 2) (9, 0, 2) (10, 0, 2)");
    CHECK(through.length == doctest::Approx(8));
    REQUIRE(through.types.size() == 4);
    CHECK(through.types[1] == navigation::Segment::Link);
    CHECK(through.owners[1] == link);
    // The point it leaves the link at is on the room it leads to, as in Godot.
    CHECK(through.types[2] == navigation::Segment::Region);

    // Going through the link costs ten times its length: the corridor is cheaper.
    scene.world.ecs().get<world::NavigationLink3D>(link).travel_cost = 10;
    const auto round = navigation::query_path(scene.world, Vec3{2, 0, 2}, Vec3{10, 0, 2});
    CHECK(round.length > 20);
    CHECK(std::ranges::find(round.owners, link) == round.owners.end());
    scene.world.ecs().get<world::NavigationLink3D>(link).travel_cost = 1;

    // A one-way link only goes from start to end.
    scene.world.ecs().get<world::NavigationLink3D>(link).bidirectional = false;
    CHECK(navigation::query_path(scene.world, Vec3{2, 0, 2}, Vec3{10, 0, 2}).length ==
          doctest::Approx(8));
    CHECK(navigation::query_path(scene.world, Vec3{10, 0, 2}, Vec3{2, 0, 2}).length > 20);
}

TEST_CASE("navigation layers and region filters keep paths off regions") {
    Scene scene;
    scene.files.write("a.navmesh.json", R"({"format": 1,
        "vertices": [[0, 0, 0], [0, 0, 4], [4, 0, 4], [4, 0, 0]], "polygons": [[0, 1, 2, 3]]})");
    scene.create("a", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "a.navmesh.json"}})");
    const entt::entity b = scene.create("b", R"({"Transform3D": {"position": [4, 0, 0]},
        "NavigationRegion3D": {"navigation_mesh": "a.navmesh.json", "navigation_layers": 2}})");
    navigation::PathOptions options;
    CHECK(text_of(
              navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 1}, options).points) ==
          "(1, 0, 1) (4, 0, 1)");
    options.navigation_layers = 3;
    CHECK(text_of(
              navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 1}, options).points) ==
          "(1, 0, 1) (4, 0, 1) (7, 0, 1)");
    options.excluded_regions = {b};
    CHECK(text_of(
              navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 1}, options).points) ==
          "(1, 0, 1) (4, 0, 1)");
    options.excluded_regions.clear();
    options.included_regions = {b};
    CHECK(text_of(
              navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 1}, options).points) ==
          "(4, 0, 1) (7, 0, 1)");

    // A disabled region is off the map.
    options.included_regions.clear();
    scene.world.ecs().get<world::NavigationRegion3D>(b).enabled = false;
    CHECK(text_of(
              navigation::query_path(scene.world, Vec3{1, 0, 1}, Vec3{7, 0, 1}, options).points) ==
          "(1, 0, 1) (4, 0, 1)");
}

TEST_CASE("the map is rebuilt only when what it is made of changes") {
    Scene scene;
    scene.files.write("a.navmesh.json", R"({"format": 1,
        "vertices": [[0, 0, 0], [0, 0, 4], [4, 0, 4], [4, 0, 0]], "polygons": [[0, 1, 2, 3]]})");
    const entt::entity a = scene.create("a", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "a.navmesh.json"}})");
    scene.world.step(kTick);
    const std::uint64_t first =
        navigation::map_info(scene.world, navigation::Map::ThreeD).iteration_id;
    CHECK(first == 1);
    for (int tick = 0; tick < 3; ++tick) {
        scene.world.step(kTick);
    }
    CHECK(navigation::map_info(scene.world, navigation::Map::ThreeD).iteration_id == first);

    scene.world.ecs().get<world::Transform3D>(a).position = {1, 0, 0};
    scene.world.step(kTick);
    CHECK(navigation::map_info(scene.world, navigation::Map::ThreeD).iteration_id == first + 1);
    CHECK(text_of(navigation::closest_point(scene.world, Vec3{0, 0, 2}).point) == "(1, 0, 2)");

    // Editing the file rebuilds the map once it reloads.
    scene.files.write("a.navmesh.json", R"({"format": 1,
        "vertices": [[0, 0, 0], [0, 0, 8], [8, 0, 8], [8, 0, 0]], "polygons": [[0, 1, 2, 3]]})");
    scene.assets.poll();
    scene.world.step(kTick);
    CHECK(navigation::map_info(scene.world, navigation::Map::ThreeD).iteration_id == first + 2);
    CHECK(text_of(navigation::closest_point(scene.world, Vec3{20, 0, 2}).point) == "(9, 0, 2)");

    // Files that don't load are left out and reported.
    scene.create("bad", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "missing.navmesh.json"}})");
    scene.create("wrong", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "a.navpoly.json"}})");
    const auto info = navigation::map_info(scene.world, navigation::Map::ThreeD);
    CHECK(info.regions == 1);
    REQUIRE(info.errors.size() == 2);
    CHECK(info.errors[0].id == "bad");
    CHECK(info.errors[0].message.starts_with("missing.navmesh.json: "));
    CHECK(info.errors[1].id == "wrong");
    CHECK(info.errors[1].message == "a.navpoly.json: not a .navmesh.json file");
    CHECK(info.iteration_id == first + 2);
}

TEST_CASE("the 2D map is in pixels and follows Transform2D") {
    Scene scene;
    scene.files.write("room.navpoly.json", R"({"format": 1,
        "vertices": [[0, 0], [64, 0], [64, 64], [0, 64]], "polygons": [[0, 1, 2, 3]]})");
    const entt::entity room = scene.create("room", R"({"Transform2D": {"position": [100, 50]},
        "NavigationRegion2D": {"navigation_polygon": "room.navpoly.json"}})");
    scene.create("hall", R"({"Transform2D": {"position": [164, 50]},
        "NavigationRegion2D": {"navigation_polygon": "room.navpoly.json"}})");
    CHECK(navigation::map_info(scene.world, navigation::Map::TwoD).regions == 2);
    CHECK(navigation::map_info(scene.world, navigation::Map::ThreeD).regions == 0);
    const auto path = navigation::query_path(scene.world, Vec2{110, 60}, Vec2{200, 100});
    CHECK(text_of(path.points) == "(110, 60) (164, 84) (200, 100)");
    CHECK(path.owners.front() == room);

    const auto closest = navigation::closest_point(scene.world, Vec2{0, 0});
    CHECK(text_of(closest.point) == "(100, 50)");
    CHECK(closest.owner == room);
    const Vec2 random = navigation::random_point_2d(scene.world, 1, true, 7);
    CHECK(random.x >= 100);
    CHECK(random.x <= 228);
    CHECK(random.y >= 50);
    CHECK(random.y <= 114);

    // A link on the 2D map, in the link's space.
    scene.create("jump", R"({"Transform2D": {"position": [100, 50]}, "NavigationLink2D":
        {"start_position": [10, 10], "end_position": [10, 300]}})");
    scene.create("roof", R"({"Transform2D": {"position": [100, 340]},
        "NavigationRegion2D": {"navigation_polygon": "room.navpoly.json"}})");
    const auto up = navigation::query_path(scene.world, Vec2{110, 60}, Vec2{120, 380});
    CHECK(text_of(up.points) == "(110, 60) (110, 350) (120, 380)");
}

TEST_CASE("an agent follows its path and emits Godot's signals as events") {
    Scene scene;
    scene.files.write("l.navmesh.json", std::string(kL));
    scene.create("floor", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "l.navmesh.json"}})");
    const entt::entity walker = scene.create("walker", R"({"Transform3D": {"position": [1, 0, 1]},
        "NavigationAgent3D": {"target_position": [6, 0, 7], "path_desired_distance": 0.5,
        "target_desired_distance": 0.5}})");
    CHECK(!navigation::is_navigation_finished(scene.world, walker));
    CHECK(navigation::is_target_reachable(scene.world, walker));
    CHECK(text_of(navigation::final_position_3d(scene.world, walker)) == "(6, 0, 7)");
    CHECK(navigation::distance_to_target(scene.world, walker) == doctest::Approx(std::sqrt(61.0)));
    // The first point of the path is where it stands, so it heads for the corner.
    CHECK(text_of(navigation::next_path_position_3d(scene.world, walker)) == "(4, 0, 4)");
    navigation::AgentInfo info = navigation::agent_info(scene.world, walker);
    CHECK(info.target_submitted);
    CHECK(info.path_index == 1);
    CHECK(info.path.points.size() == 3);

    int ticks = 0;
    while (!navigation::is_navigation_finished(scene.world, walker) && ticks < 200) {
        scene.world.step(kTick);
        auto& transform = scene.world.ecs().get<world::Transform3D>(walker);
        const Vec3 next = navigation::next_path_position_3d(scene.world, walker);
        const Vec3 to = next - transform.position;
        const float length = std::sqrt((to.x * to.x) + (to.y * to.y) + (to.z * to.z));
        constexpr float kStep = 0.2F;
        transform.position = length <= kStep ? next : transform.position + (to * (kStep / length));
        ++ticks;
    }
    CHECK(ticks == 37);
    CHECK(navigation::is_target_reached(scene.world, walker));
    CHECK(navigation::distance_to_target(scene.world, walker) < 0.5F);
    CHECK(scene.kinds("walker") ==
          std::vector<std::string>{"path_changed", "waypoint_reached", "waypoint_reached",
                                   "waypoint_reached", "target_reached", "navigation_finished"});
    std::vector<std::string> waypoints;
    for (const world::Event& event : scene.events) {
        if (event.kind == "waypoint_reached") {
            waypoints.push_back(json::write(event.data, json::Style::Compact));
        }
    }
    CHECK(waypoints[1] == R"({"owner":"floor","position":[4.0,0.0,4.0],"type":"region"})");

    // A new target starts it again.
    scene.events.clear();
    navigation::set_target_position(scene.world, walker, Vec3{1, 0, 3});
    CHECK(!navigation::is_navigation_finished(scene.world, walker));
    CHECK(!navigation::is_target_reached(scene.world, walker));
    CHECK(text_of(scene.world.ecs().get<world::NavigationAgent3D>(walker).target_position) ==
          "(1, 0, 3)");
    CHECK(scene.kinds("walker").front() == "path_changed");
}

TEST_CASE("an agent that can't reach its target finishes at the end of its path") {
    Scene scene;
    scene.files.write("a.navmesh.json", R"({"format": 1,
        "vertices": [[0, 0, 0], [0, 0, 4], [4, 0, 4], [4, 0, 0]], "polygons": [[0, 1, 2, 3]]})");
    scene.create("a", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "a.navmesh.json"}})");
    const entt::entity walker = scene.create("walker", R"({"Transform3D": {"position": [1, 0, 1]},
        "NavigationAgent3D": {"target_position": [9, 0, 1]}})");
    CHECK(!navigation::is_target_reachable(scene.world, walker));
    CHECK(text_of(navigation::final_position_3d(scene.world, walker)) == "(4, 0, 1)");
    scene.world.ecs().get<world::Transform3D>(walker).position = {3.5F, 0, 1};
    scene.world.step(kTick);
    CHECK(navigation::is_navigation_finished(scene.world, walker));
    CHECK(!navigation::is_target_reached(scene.world, walker));
    CHECK(scene.kinds("walker") == std::vector<std::string>{"path_changed", "waypoint_reached",
                                                            "waypoint_reached",
                                                            "navigation_finished"});
}

TEST_CASE("agents' paths are saved and put back with the world") {
    Scene scene;
    scene.files.write("l.navmesh.json", std::string(kL));
    scene.create("floor", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "l.navmesh.json"}})");
    const entt::entity walker = scene.create("walker", R"({"Transform3D": {"position": [1, 0, 1]},
        "NavigationAgent3D": {"target_position": [6, 0, 7]}})");
    scene.world.step(kTick);
    const navigation::Snapshot saved = navigation::save(scene.world);
    const auto before = navigation::agent_info(scene.world, walker);

    navigation::set_target_position(scene.world, walker, Vec3{7, 0, 1});
    CHECK(text_of(navigation::agent_info(scene.world, walker).path.points) !=
          text_of(before.path.points));
    navigation::restore(scene.world, saved);
    scene.world.ecs().get<world::NavigationAgent3D>(walker).target_position = {6, 0, 7};
    const auto after = navigation::agent_info(scene.world, walker);
    CHECK(text_of(after.path.points) == text_of(before.path.points));
    CHECK(after.path_index == before.path_index);
    CHECK(after.path.owners == before.path.owners);
    CHECK(after.navigation_finished == before.navigation_finished);
}

TEST_CASE("scene checks find navigation files that don't load") {
    const std::map<std::string, std::string, std::less<>> files{
        {"ok.navmesh.json", std::string(kL)},
        {"bad.navpoly.json", R"({"format": 1, "vertices": [[0, 0]], "polygons": [[0, 1, 2]]})"}};
    const slash::world::FileReader reader =
        [&](std::string_view path) -> slash::Result<std::string> {
        const auto found = files.find(path);
        if (found == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return found->second;
    };
    const auto errors = navigation::check_scene(parse(R"({"format": 1, "entities": {
        "a": {"components": {"NavigationRegion3D": {"navigation_mesh": "ok.navmesh.json"}}},
        "b": {"components": {"NavigationRegion3D": {"navigation_mesh": "gone.navmesh.json"}}},
        "c": {"components": {"NavigationRegion2D": {"navigation_polygon": "bad.navpoly.json"}}},
        "d": {"components": {"NavigationRegion2D": {"navigation_polygon": "ok.navmesh.json"}}}
    }})"),
                                                reader);
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].path == "/entities/b/components/NavigationRegion3D/navigation_mesh");
    CHECK(errors[0].message == "gone.navmesh.json: no such file");
    CHECK(errors[1].path == "/entities/c/components/NavigationRegion2D/navigation_polygon");
    CHECK(errors[1].message == "bad.navpoly.json: /polygons/0/1: vertex 1 is past the last of 1 "
                               "vertices");
    CHECK(errors[2].path == "/entities/d/components/NavigationRegion2D/navigation_polygon");
    CHECK(errors[2].message == "navigation polygons are .navpoly.json files");
}

TEST_CASE("navigation settings come from project.json's navigation_2d and navigation_3d") {
    const json::Value two_d = parse(R"({"cell_size": 2, "edge_connection_margin": 3})");
    CHECK(json::validate(two_d, navigation::settings_schema(navigation::Map::TwoD)).empty());
    const navigation::MapSettings map_2d = navigation::parse_settings(navigation::Map::TwoD, two_d);
    CHECK(map_2d.cell_size == 2.0F);
    CHECK(map_2d.edge_connection_margin == 3.0F);
    CHECK(map_2d.link_connection_radius == 4.0F);

    const json::Value three_d = parse(R"({"cell_height": 0.5, "up": [0, 0, 1]})");
    CHECK(json::validate(three_d, navigation::settings_schema(navigation::Map::ThreeD)).empty());
    const navigation::MapSettings map_3d =
        navigation::parse_settings(navigation::Map::ThreeD, three_d);
    CHECK(map_3d.cell_height == 0.5F);
    CHECK(text_of(map_3d.up) == "(0, 0, 1)");
    CHECK(!json::validate(parse(R"({"cell_size": 0})"),
                          navigation::settings_schema(navigation::Map::TwoD))
               .empty());
    CHECK(!json::validate(parse(R"({"up": [0, 1, 0]})"),
                          navigation::settings_schema(navigation::Map::TwoD))
               .empty());
}

// ---- Baking

namespace {

// A 10 by 10 static floor whose top is at y 0, and a 1 by 3 by 1 pillar mesh standing on it at
// (2, 2), both under a region.
entt::entity floor_and_pillar(Scene& scene) {
    scene.create("floor", R"({"Transform3D": {"position": [0, -0.5, 0]}, "StaticBody3D": {},
                              "CollisionShape3D": {"size": [10, 1, 10]}})");
    scene.create("pillar", R"({"Transform3D": {"position": [2, 1.5, 2]}, "MeshInstance3D": {},
                               "BoxMesh": {"size": [1, 3, 1]}})");
    return scene.create("level", R"({"Transform3D": {"children": ["floor", "pillar"]},
                                     "NavigationRegion3D": {}})");
}

std::size_t triangles(const navigation::SourceGeometry3D& geometry) {
    return geometry.indices.size() / 3;
}

template <typename V>
float lowest(const std::vector<V>& points, float V::*axis) {
    float out = points.front().*axis;
    for (const V& point : points) {
        out = std::min(out, point.*axis);
    }
    return out;
}

template <typename V>
float highest(const std::vector<V>& points, float V::*axis) {
    float out = points.front().*axis;
    for (const V& point : points) {
        out = std::max(out, point.*axis);
    }
    return out;
}

// The area a 2D bake's polygons cover.
double area(const navigation::NavigationPolygon& polygon) {
    double out = 0.0;
    for (const std::vector<int>& indices : polygon.polygons) {
        double twice = 0.0;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const Vec2 a = polygon.vertices[static_cast<std::size_t>(indices[i])];
            const Vec2 b =
                polygon.vertices[static_cast<std::size_t>(indices[(i + 1) % indices.size()])];
            twice += (static_cast<double>(a.x) * b.y) - (static_cast<double>(b.x) * a.y);
        }
        out += std::abs(twice) / 2.0;
    }
    return out;
}

navigation::NavigationPolygon square_outline(float size) {
    navigation::NavigationPolygon settings;
    settings.outlines = {{{0.0F, 0.0F}, {size, 0.0F}, {size, size}, {0.0F, size}}};
    return settings;
}

} // namespace

TEST_CASE("a 3D bake finds the walkable floor around what stands on it") {
    Scene scene;
    const entt::entity level = floor_and_pillar(scene);
    const navigation::NavigationMesh settings;
    std::vector<std::string> warnings;
    const navigation::SourceGeometry3D geometry =
        navigation::parse_source_geometry(scene.world, scene.assets, level, settings, &warnings);
    CHECK(warnings.empty());
    CHECK(triangles(geometry) == 24); // the floor's box and the pillar's mesh, 12 each

    auto baked = navigation::bake(settings, geometry, &warnings);
    REQUIRE(baked.ok());
    CHECK(warnings.empty());
    const navigation::NavigationMesh& mesh = baked.value();
    CHECK(mesh.polygons.size() == 11);
    // The floor's top, raised a cell as Recast's detail mesh raises it and to the top of the
    // cell it rasterizes into, shrunk by agent_radius: 4.5 from the middle.
    CHECK(lowest(mesh.vertices, &Vec3::y) == 0.5F);
    CHECK(highest(mesh.vertices, &Vec3::y) == 0.5F);
    CHECK(lowest(mesh.vertices, &Vec3::x) == -4.5F);
    CHECK(highest(mesh.vertices, &Vec3::x) == 4.5F);
    CHECK(lowest(mesh.vertices, &Vec3::z) == -4.5F);
    CHECK(highest(mesh.vertices, &Vec3::z) == 4.5F);
    // Nothing within agent_radius of the pillar, which spans 1.5 to 2.5.
    for (const Vec3 vertex : mesh.vertices) {
        CHECK_FALSE((vertex.x > 1.0F && vertex.x < 3.0F && vertex.z > 1.0F && vertex.z < 3.0F));
    }
    // The file it makes loads, and paths go round the pillar.
    const json::Value doc = navigation::with_polygons(nullptr, mesh);
    CHECK(navigation::validate_mesh(doc).empty());
    scene.files.write("level.navmesh.json", json::write(doc));
    scene.create("baked", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "level.navmesh.json"}})");
    const auto path =
        navigation::query_path(scene.world, Vec3{-4.0F, 0.0F, -4.0F}, Vec3{4.0F, 0.0F, 4.0F});
    CHECK(text_of(path.points) == "(-4, 0.5, -4) (1.71212, 0.5, 0.0800866) (3, 0.5, 1) "
                                  "(3.47727, 0.5, 2.43182) (3.875, 0.5, 3.625) (4, 0.5, 4)");
}

TEST_CASE("what a 3D bake takes follows the parsed geometry type and the collision mask") {
    Scene scene;
    const entt::entity level = floor_and_pillar(scene);
    const auto parsed = [&](std::string_view settings_json) {
        auto settings = navigation::parse_mesh(parse(settings_json));
        REQUIRE(settings.ok());
        return triangles(
            navigation::parse_source_geometry(scene.world, scene.assets, level, settings.value()));
    };
    CHECK(parsed(R"({"format": 1, "geometry_parsed_geometry_type": "mesh_instances"})") == 12);
    CHECK(parsed(R"({"format": 1, "geometry_parsed_geometry_type": "static_colliders"})") == 12);
    CHECK(parsed(R"({"format": 1, "geometry_collision_mask": 2})") == 12); // the pillar alone

    // A rigid body's shape is left out, and so is a child shape with a body of its own; a
    // shape on a child with no body of its own is the static body's.
    scene.create("crate", R"({"Transform3D": {}, "RigidBody3D": {}, "CollisionShape3D": {}})");
    scene.create("trigger", R"({"Transform3D": {}, "Area3D": {}, "CollisionShape3D": {}})");
    scene.create("rail", R"({"Transform3D": {}, "CollisionShape3D": {"shape": "sphere"}})");
    scene.create("wall", R"({"Transform3D": {"children": ["trigger", "rail"]},
                             "StaticBody3D": {}})");
    const entt::entity yard = scene.create("yard", R"({"Transform3D": {
        "position": [0, 10, 0], "children": ["crate", "wall"]}, "NavigationRegion3D": {}})");
    const navigation::SourceGeometry3D geometry = navigation::parse_source_geometry(
        scene.world, scene.assets, yard, navigation::NavigationMesh{});
    const auto sphere = slash::scene3d::make_mesh(world::SphereMesh{});
    REQUIRE(sphere.ok());
    CHECK(triangles(geometry) == sphere.value().indices.size() / 3);

    // Geometry is in the region's space, wherever the region is.
    scene.create("ground", R"({"Transform3D": {"position": [0, 3, 0], "top_level": true},
        "StaticBody3D": {}, "CollisionShape3D": {"size": [2, 2, 2]}})");
    const entt::entity high = scene.create("high", R"({"Transform3D": {
        "position": [0, 10, 0], "children": ["ground"]}, "NavigationRegion3D": {}})");
    const navigation::SourceGeometry3D placed = navigation::parse_source_geometry(
        scene.world, scene.assets, high, navigation::NavigationMesh{});
    float low = placed.vertices[1];
    float top = placed.vertices[1];
    for (std::size_t i = 1; i < placed.vertices.size(); i += 3) {
        low = std::min(low, placed.vertices[i]);
        top = std::max(top, placed.vertices[i]);
    }
    CHECK(low == -8.0F);
    CHECK(top == -6.0F);
}

TEST_CASE("3D bakes warn of settings that don't fit whole cells and refuse too many cells") {
    Scene scene;
    const entt::entity level = floor_and_pillar(scene);
    navigation::NavigationMesh settings;
    settings.agent_radius = 0.3F;
    settings.agent_max_climb = 0.3F;
    std::vector<std::string> warnings;
    const navigation::SourceGeometry3D geometry =
        navigation::parse_source_geometry(scene.world, scene.assets, level, settings);
    REQUIRE(navigation::bake(settings, geometry, &warnings).ok());
    CHECK(warnings ==
          std::vector<std::string>{"agent_max_climb is rounded down to whole cells of cell_height",
                                   "agent_radius is rounded up to whole cells of cell_size"});

    settings.cell_size = 0.001F;
    auto refused = navigation::bake(settings, geometry);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message.find("too many to bake") != std::string::npos);

    // Only what is in filter_baking_aabb bakes: here the floor's quarter at x and z below 0.
    settings = {};
    settings.filter_baking_aabb_position = {-5.0F, -1.0F, -5.0F};
    settings.filter_baking_aabb_size = {5.0F, 2.0F, 5.0F};
    auto quarter = navigation::bake(settings, geometry);
    REQUIRE(quarter.ok());
    CHECK(lowest(quarter.value().vertices, &Vec3::x) == -4.5F);
    CHECK(highest(quarter.value().vertices, &Vec3::x) <= 0.0F);
    CHECK(highest(quarter.value().vertices, &Vec3::z) <= 0.0F);

    // No geometry bakes no polygons.
    auto empty = navigation::bake(navigation::NavigationMesh{}, navigation::SourceGeometry3D{});
    REQUIRE(empty.ok());
    CHECK(empty.value().polygons.empty());
}

TEST_CASE("a 2D bake merges outlines, cuts out obstructions and shrinks by the agent's radius") {
    Scene scene;
    scene.create("rock", R"({"Transform2D": {"position": [100, 100]}, "StaticBody2D": {},
        "CollisionShape2D": {"shape": "circle", "radius": 20}})");
    const entt::entity yard = scene.create(
        "yard", R"({"Transform2D": {"children": ["rock"]}, "NavigationRegion2D": {}})");
    const navigation::NavigationPolygon settings = square_outline(200.0F);
    const navigation::SourceGeometry2D geometry =
        navigation::parse_source_geometry(scene.world, scene.assets, yard, settings);
    REQUIRE(geometry.obstruction_outlines.size() == 1);
    CHECK(geometry.obstruction_outlines[0].size() == 12); // Godot's circle has 12 sides

    auto baked = navigation::bake(settings, geometry);
    REQUIRE(baked.ok());
    const navigation::NavigationPolygon& polygon = baked.value();
    CHECK(polygon.polygons.size() == 12);
    CHECK(polygon.vertices.size() == 16);
    CHECK(lowest(polygon.vertices, &Vec2::x) == 10.0F);
    CHECK(highest(polygon.vertices, &Vec2::x) == 190.0F);
    // The rock's 12 sides moved out by the agent's radius, the corners mitered.
    for (const Vec2 vertex : polygon.vertices) {
        CHECK(std::hypot(vertex.x - 100.0F, vertex.y - 100.0F) >= 30.0F - 0.01F);
    }
    const json::Value doc = navigation::with_polygons(nullptr, polygon);
    CHECK(navigation::validate_polygon(doc).empty());

    // Triangulated instead, every polygon is a triangle over the same area.
    navigation::NavigationPolygon triangulate = settings;
    triangulate.sample_partition_type = navigation::Partition2D::Triangulate;
    auto triangles_2d = navigation::bake(triangulate, geometry);
    REQUIRE(triangles_2d.ok());
    for (const std::vector<int>& indices : triangles_2d.value().polygons) {
        CHECK(indices.size() == 3);
    }
    CHECK(area(triangles_2d.value()) == doctest::Approx(area(polygon)).epsilon(0.0001));

    // Overlapping outlines merge, and with no agent radius nothing shrinks.
    navigation::NavigationPolygon two = square_outline(100.0F);
    two.outlines.push_back({{50.0F, 50.0F}, {150.0F, 50.0F}, {150.0F, 150.0F}, {50.0F, 150.0F}});
    two.agent_radius = 0.0F;
    auto merged = navigation::bake(two, {});
    REQUIRE(merged.ok());
    CHECK(area(merged.value()) == doctest::Approx(17500.0));

    // baking_rect clips what bakes, and border_size keeps it off the rect's edges.
    navigation::NavigationPolygon clipped = square_outline(200.0F);
    clipped.agent_radius = 0.0F;
    clipped.baking_rect_size = {100.0F, 200.0F};
    clipped.border_size = 5.0F;
    auto half = navigation::bake(clipped, {});
    REQUIRE(half.ok());
    CHECK(highest(half.value().vertices, &Vec2::x) == 95.0F);
    CHECK(area(half.value()) == doctest::Approx(90.0 * 190.0));

    // A rigid body is no obstruction, nor a static body off parsed_collision_mask.
    scene.create("ball", R"({"Transform2D": {}, "RigidBody2D": {}, "CollisionShape2D": {}})");
    scene.create("wall", R"({"Transform2D": {}, "StaticBody2D": {"collision_layer": 2},
        "CollisionPolygon2D": {"polygon": [[0, 0], [10, 0], [10, 10]]}})");
    const entt::entity park = scene.create("park", R"({"Transform2D": {
        "children": ["ball", "wall"]}, "NavigationRegion2D": {}})");
    CHECK(navigation::parse_source_geometry(scene.world, scene.assets, park, settings)
              .obstruction_outlines.size() == 1);
    navigation::NavigationPolygon layer_one = settings;
    layer_one.parsed_collision_mask = 1;
    CHECK(navigation::parse_source_geometry(scene.world, scene.assets, park, layer_one)
              .obstruction_outlines.empty());
}

TEST_CASE("baked polygons replace a file's own, keeping its settings") {
    const json::Value doc = parse(R"({"format": 1, "agent_radius": 4,
        "outlines": [[[0, 0], [64, 0], [64, 64], [0, 64]]],
        "vertices": [[0, 0], [1, 0], [1, 1]], "polygons": [[0, 1, 2]]})");
    REQUIRE(navigation::validate_polygon(doc).empty());
    auto settings = navigation::parse_polygon(doc);
    REQUIRE(settings.ok());
    CHECK(settings.value().agent_radius == 4.0F);
    REQUIRE(settings.value().outlines.size() == 1);
    auto baked = navigation::bake(settings.value(), {});
    REQUIRE(baked.ok());
    CHECK(navigation::with_polygons(doc, baked.value()) == parse(R"({"format": 1, "agent_radius": 4,
                    "outlines": [[[0, 0], [64, 0], [64, 64], [0, 64]]],
                    "vertices": [[60.0, 60.0], [4.0, 60.0], [4.0, 4.0], [60.0, 4.0]],
                    "polygons": [[0, 1, 2, 3]]})"));
    // A new file is the format and the polygons.
    CHECK(navigation::with_polygons(nullptr, baked.value()) == parse(R"({"format": 1,
                    "vertices": [[60.0, 60.0], [4.0, 60.0], [4.0, 4.0], [60.0, 4.0]],
                    "polygons": [[0, 1, 2, 3]]})"));

    // Bake settings are checked as every field is.
    const auto errors = navigation::validate_mesh(parse(R"({"format": 1,
        "sample_partition_type": "watershad", "agent_max_slope": 120})"));
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/agent_max_slope");
    CHECK(errors[0].message == "must be at most 90, got 120");
    CHECK(errors[1].path == "/sample_partition_type");
    CHECK(errors[1].message ==
          R"(expected one of "watershed", "monotone", "layers", got "watershad")");
}

namespace {

// A 2D map of one square region, 1000 pixels a side.
void open_field(Scene& scene) {
    scene.files.write("field.navpoly.json", R"({"format": 1,
        "vertices": [[0, 0], [1000, 0], [1000, 1000], [0, 1000]], "polygons": [[0, 1, 2, 3]]})");
    scene.create("field", R"({"Transform2D": {}, "NavigationRegion2D":
        {"navigation_polygon": "field.navpoly.json"}})");
}

// A 3D map of one square region, 100 meters a side.
void open_ground(Scene& scene) {
    scene.files.write("ground.navmesh.json", R"({"format": 1, "vertices": [[-50, 0, -50],
        [-50, 0, 50], [50, 0, 50], [50, 0, -50]], "polygons": [[0, 1, 2, 3]]})");
    scene.create("ground", R"({"Transform3D": {}, "NavigationRegion3D":
        {"navigation_mesh": "ground.navmesh.json"}})");
}

float distance(Vec2 a, Vec2 b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// Moves 2D agents as gameplay code does: before each tick each asks to go at speed toward its
// next path position, and after it moves at its safe velocity. Returns the closest any two came.
float drive(Scene& scene, const std::vector<entt::entity>& agents, float speed, int ticks) {
    float closest = std::numeric_limits<float>::max();
    for (int tick = 0; tick < ticks; ++tick) {
        for (const entt::entity agent : agents) {
            const Vec2 position = scene.world.ecs().get<world::Transform2D>(agent).position;
            const Vec2 to = navigation::next_path_position_2d(scene.world, agent) - position;
            const float length = std::hypot(to.x, to.y);
            scene.world.ecs().get<world::NavigationAgent2D>(agent).velocity =
                length > 0.0F ? to * (speed / length) : Vec2{};
        }
        scene.world.step(kTick);
        for (const entt::entity agent : agents) {
            auto& transform = scene.world.ecs().get<world::Transform2D>(agent);
            transform.position =
                transform.position +
                (navigation::safe_velocity_2d(scene.world, agent) * static_cast<float>(kTick));
        }
        for (std::size_t i = 0; i < agents.size(); ++i) {
            for (std::size_t j = i + 1; j < agents.size(); ++j) {
                closest = std::min(
                    closest,
                    distance(scene.world.ecs().get<world::Transform2D>(agents[i]).position,
                             scene.world.ecs().get<world::Transform2D>(agents[j]).position));
            }
        }
    }
    return closest;
}

Vec2 position_of(Scene& scene, entt::entity entity) {
    return scene.world.ecs().get<world::Transform2D>(entity).position;
}

} // namespace

TEST_CASE("agents with avoidance_enabled steer round each other, as Godot's RVO2 avoidance") {
    const auto meet = [](bool avoid) {
        Scene scene;
        open_field(scene);
        const std::string flag = avoid ? "true" : "false";
        const entt::entity a = scene.create("a", R"({"Transform2D": {"position": [100, 500]},
            "NavigationAgent2D": {"target_position": [500, 500], "avoidance_enabled": )" +
                                                     flag + "}}");
        const entt::entity b = scene.create("b", R"({"Transform2D": {"position": [500, 503]},
            "NavigationAgent2D": {"target_position": [100, 503], "avoidance_enabled": )" +
                                                     flag + "}}");
        const float closest = drive(scene, {a, b}, 100.0F, 400);
        CHECK(navigation::is_target_reached(scene.world, a));
        CHECK(navigation::is_target_reached(scene.world, b));
        return std::pair{closest, scene.kinds("a")};
    };
    const auto [apart, kinds] = meet(true);
    CHECK(apart >= 19.0F); // two radii of 10, less what one tick's step lets them overlap
    CHECK(std::count(kinds.begin(), kinds.end(), "velocity_computed") == 400);
    const auto [through, quiet] = meet(false);
    CHECK(through < 5.0F);
    CHECK(std::count(quiet.begin(), quiet.end(), "velocity_computed") == 0);
}

TEST_CASE("avoidance layers, masks and priorities pick whom an agent makes way for") {
    // Whether a and b, heading at each other, each moved at anything but what it asked for.
    const auto gives_way = [](std::string_view a_fields, std::string_view b_fields) {
        Scene scene;
        open_field(scene);
        const entt::entity a =
            scene.create("a", R"({"Transform2D": {"position": [100, 500]}, "NavigationAgent2D": {
            "target_position": [500, 500], "avoidance_enabled": true)" +
                                  std::string(a_fields) + "}}");
        const entt::entity b =
            scene.create("b", R"({"Transform2D": {"position": [500, 503]}, "NavigationAgent2D": {
            "target_position": [100, 503], "avoidance_enabled": true)" +
                                  std::string(b_fields) + "}}");
        std::pair<bool, bool> out{false, false};
        for (int tick = 0; tick < 240; ++tick) {
            for (const entt::entity agent : {a, b}) {
                const Vec2 to = navigation::next_path_position_2d(scene.world, agent) -
                                position_of(scene, agent);
                const float length = std::hypot(to.x, to.y);
                scene.world.ecs().get<world::NavigationAgent2D>(agent).velocity =
                    length > 0.0F ? to * (100.0F / length) : Vec2{};
            }
            scene.world.step(kTick);
            for (const entt::entity agent : {a, b}) {
                const Vec2 asked = scene.world.ecs().get<world::NavigationAgent2D>(agent).velocity;
                const Vec2 safe = navigation::safe_velocity_2d(scene.world, agent);
                // The tick it finishes, it stops.
                if (!navigation::is_navigation_finished(scene.world, agent)) {
                    (agent == a ? out.first : out.second) |= distance(asked, safe) > 0.01F;
                }
                auto& transform = scene.world.ecs().get<world::Transform2D>(agent);
                transform.position = transform.position + (safe * static_cast<float>(kTick));
            }
        }
        return out;
    };
    CHECK(gives_way("", "") == std::pair{true, true});
    // a looks only at layer 2 and b is on layer 1, so a goes straight and b makes way.
    CHECK(gives_way(R"(, "avoidance_mask": 2)", "") == std::pair{false, true});
    CHECK(gives_way("", R"(, "avoidance_layers": 2)") == std::pair{false, true});
    // An agent of higher priority makes way for none of lower priority.
    CHECK(gives_way(R"(, "avoidance_priority": 0.5)", "") == std::pair{true, false});
    CHECK(gives_way("", R"(, "avoidance_priority": 0.5)") == std::pair{false, true});
}

TEST_CASE("agents keep out of obstacles' circles and outlines") {
    // How close an agent heading along y from x 100 to 500 came to the box 280-320 by 480-520
    // (or a circle of 20 at its middle), and where it ended.
    const auto pass = [](std::string_view obstacle, bool box, float y) {
        Scene scene;
        open_field(scene);
        scene.create("rock", R"({"Transform2D": {"position": [300, 500]},
                                 "NavigationObstacle2D": )" +
                                 std::string(obstacle) + "}");
        const entt::entity walker = scene.create(
            "walker", R"({"Transform2D": {"position": [100, )" + std::to_string(y) +
                          R"(]}, "NavigationAgent2D": {"target_position": [500, )" +
                          std::to_string(y) +
                          R"(], "avoidance_enabled": true, "time_horizon_obstacles": 1}})");
        float closest = std::numeric_limits<float>::max();
        for (int tick = 0; tick < 400; ++tick) {
            drive(scene, {walker}, 100.0F, 1);
            const Vec2 p = position_of(scene, walker);
            const float dx = std::max(std::abs(p.x - 300.0F) - (box ? 20.0F : 0.0F), 0.0F);
            const float dy = std::max(std::abs(p.y - 500.0F) - (box ? 20.0F : 0.0F), 0.0F);
            closest = std::min(closest, std::hypot(dx, dy));
        }
        return std::pair{closest, position_of(scene, walker)};
    };
    const auto [round, round_end] = pass(R"({"radius": 20})", false, 502.0F);
    CHECK(round >= 29.5F); // the obstacle's radius and the agent's
    CHECK(round_end.x > 490.0F);

    // Listed counterclockwise in x and y, which is clockwise on screen with y down, an outline
    // keeps agents out: one that would clip its corner slides past it.
    constexpr std::string_view kOut =
        R"({"vertices": [[-20, -20], [20, -20], [20, 20], [-20, 20]]})";
    const auto [slid, slid_end] = pass(kOut, true, 525.0F);
    CHECK(slid >= 9.5F);
    CHECK(slid_end.x > 490.0F);
    // Avoidance finds no way round, though: one heading straight at it stops at its edge. Bake
    // obstacles into the navigation mesh (affect_navigation_mesh) for paths to go round them.
    const auto [stopped, stopped_end] = pass(kOut, true, 502.0F);
    CHECK(stopped >= 9.5F);
    CHECK(stopped_end.x < 271.0F);
    // The other way round it keeps them in: one outside walks in and stays there.
    const auto [held, held_end] =
        pass(R"({"vertices": [[-20, -20], [-20, 20], [20, 20], [20, -20]]})", true, 502.0F);
    CHECK(held == 0.0F);
    CHECK(std::abs(held_end.x - 300.0F) < 20.0F);
    // Off its layers, or with avoidance off, an obstacle is in no one's way.
    CHECK(pass(R"({"radius": 20, "avoidance_layers": 2})", false, 502.0F).first < 3.0F);
    CHECK(pass(R"({"radius": 20, "avoidance_enabled": false})", false, 502.0F).first < 3.0F);
}

namespace {

struct Crossing {
    float closest = std::numeric_limits<float>::max(); // the closest a and b came
    bool ground_closest = true;                        // measured on the ground, else in 3D
    std::vector<float> safe_y;                         // a's safe velocities' y, each tick
};

// Moves 3D agents across the ground as gameplay code does, asking for speed toward each one's
// next path position on the ground plus y_velocity, then moving at the safe velocity on the
// ground, so they keep their height.
Crossing cross(Scene& scene, entt::entity a, entt::entity b, float speed, float y_velocity,
               int ticks, bool ground) {
    Crossing out;
    out.ground_closest = ground;
    for (int tick = 0; tick < ticks; ++tick) {
        for (const entt::entity agent : {a, b}) {
            const Vec3 position = scene.world.ecs().get<world::Transform3D>(agent).position;
            Vec3 to = navigation::next_path_position_3d(scene.world, agent) - position;
            to.y = 0.0F;
            const float length = std::hypot(to.x, to.z);
            Vec3 velocity = length > 0.0F ? to * (speed / length) : Vec3{};
            velocity.y = y_velocity;
            scene.world.ecs().get<world::NavigationAgent3D>(agent).velocity = velocity;
        }
        scene.world.step(kTick);
        for (const entt::entity agent : {a, b}) {
            const Vec3 safe = navigation::safe_velocity_3d(scene.world, agent);
            if (agent == a) {
                out.safe_y.push_back(safe.y);
            }
            auto& transform = scene.world.ecs().get<world::Transform3D>(agent);
            transform.position.x += safe.x * static_cast<float>(kTick);
            transform.position.z += safe.z * static_cast<float>(kTick);
        }
        const Vec3 pa = scene.world.ecs().get<world::Transform3D>(a).position;
        const Vec3 pb = scene.world.ecs().get<world::Transform3D>(b).position;
        const float dy = ground ? 0.0F : pa.y - pb.y;
        out.closest = std::min(out.closest, std::sqrt(((pa.x - pb.x) * (pa.x - pb.x)) + (dy * dy) +
                                                      ((pa.z - pb.z) * (pa.z - pb.z))));
    }
    return out;
}

// Two 3D agents heading at each other along x, b at height b_y over the ground (its path raised
// to it by path_height_offset), with fields added to both.
Crossing head_on_3d(float b_y, std::string_view fields, float y_velocity = 0.0F,
                    bool ground = true) {
    Scene scene;
    open_ground(scene);
    const std::string y = std::to_string(b_y);
    const entt::entity a = scene.create("a", R"({"Transform3D": {"position": [-10, 0, 0]},
        "NavigationAgent3D": {"target_position": [10, 0, 0], "avoidance_enabled": true)" +
                                                 std::string(fields) + "}}");
    const entt::entity b =
        scene.create("b", R"({"Transform3D": {"position": [10, )" + y + R"(, 0.1]},
        "NavigationAgent3D": {"target_position": [-10, )" +
                              y + R"(, 0.1], "path_height_offset": )" + std::to_string(-b_y) +
                              R"(, "avoidance_enabled": true)" + std::string(fields) + "}}");
    return cross(scene, a, b, 2.0F, y_velocity, 360, ground);
}

} // namespace

TEST_CASE("3D agents avoid on the ground by elevation and height, or in 3D") {
    // On the ground, as Godot's 3D agents avoid by default: those whose heights overlap.
    CHECK(head_on_3d(0.0F, "").closest >= 0.95F); // two radii of 0.5
    CHECK(head_on_3d(0.5F, "").closest >= 0.95F);
    CHECK(head_on_3d(3.0F, "").closest < 0.2F); // b is above a's height of 1
    CHECK(head_on_3d(3.0F, R"(, "height": 4)").closest >= 0.95F);

    // y velocity is left out of avoidance and put back after, as keep_y_velocity does (for
    // gravity, say), or dropped.
    const Crossing kept = head_on_3d(0.0F, "", -2.0F);
    CHECK(kept.closest >= 0.95F);
    CHECK(std::all_of(kept.safe_y.begin(), kept.safe_y.end(), [](float y) { return y == -2.0F; }));
    const Crossing dropped = head_on_3d(0.0F, R"(, "keep_y_velocity": false)", -2.0F);
    CHECK(std::all_of(dropped.safe_y.begin(), dropped.safe_y.end(),
                      [](float y) { return y == 0.0F; }));

    // With use_3d_avoidance they avoid as spheres in every direction, y included.
    const Crossing above = head_on_3d(3.0F, R"(, "use_3d_avoidance": true)", 0.0F, false);
    CHECK(above.closest >= 2.9F);
    const Crossing spheres = head_on_3d(0.5F, R"(, "use_3d_avoidance": true)", 0.0F, false);
    CHECK(spheres.closest >= 0.95F);
    CHECK(std::any_of(spheres.safe_y.begin(), spheres.safe_y.end(),
                      [](float y) { return y != 0.0F; }));
}

TEST_CASE("avoidance velocities stop at the target, can be forced, and are saved with the world") {
    // a and b heading at each other; positions and the safe velocities of both, each tick.
    struct Run {
        Scene scene;
        entt::entity a = entt::null;
        entt::entity b = entt::null;
        Run() {
            open_field(scene);
            a = scene.create("a", R"({"Transform2D": {"position": [100, 500]}, "NavigationAgent2D":
                {"target_position": [500, 500], "avoidance_enabled": true}})");
            b = scene.create("b", R"({"Transform2D": {"position": [500, 503]}, "NavigationAgent2D":
                {"target_position": [100, 503], "avoidance_enabled": true}})");
        }
        std::vector<Vec2> ticks(int count) {
            std::vector<Vec2> out;
            for (int i = 0; i < count; ++i) {
                drive(scene, {a, b}, 100.0F, 1);
                out.push_back(navigation::safe_velocity_2d(scene.world, a));
                out.push_back(navigation::safe_velocity_2d(scene.world, b));
            }
            return out;
        }
    };
    const auto bits = [](const std::vector<Vec2>& points) { return text_of(points); };

    // The same every run.
    Run first;
    Run second;
    const std::vector<Vec2> once = first.ticks(150);
    CHECK(bits(once) == bits(second.ticks(150)));
    CHECK(text_of(navigation::agent_info(first.scene.world, first.a).safe_velocity) ==
          text_of(Vec3{once[once.size() - 2].x, once[once.size() - 2].y, 0.0F}));

    // Saved and put back, avoidance carries on as it would have.
    const navigation::Snapshot saved = navigation::save(first.scene.world);
    const Vec2 at_a = position_of(first.scene, first.a);
    const Vec2 at_b = position_of(first.scene, first.b);
    const std::vector<Vec2> on = first.ticks(30);
    navigation::restore(first.scene.world, saved);
    first.scene.world.ecs().get<world::Transform2D>(first.a).position = at_a;
    first.scene.world.ecs().get<world::Transform2D>(first.b).position = at_b;
    CHECK(bits(first.ticks(30)) == bits(on));

    // A forced velocity replaces the one avoidance starts the next tick from, here as they
    // close in on each other.
    Run unforced;
    Run forced;
    unforced.ticks(90);
    forced.ticks(90);
    navigation::set_velocity_forced(forced.scene.world, forced.a, Vec2{0.0F, 0.0F});
    CHECK(bits(forced.ticks(1)) != bits(unforced.ticks(1)));

    // Once an agent finishes, it asks avoidance for nothing, whatever its velocity says.
    first.ticks(300);
    REQUIRE(navigation::is_navigation_finished(first.scene.world, first.a));
    first.scene.world.ecs().get<world::NavigationAgent2D>(first.a).velocity = {100.0F, 0.0F};
    first.scene.world.step(kTick);
    CHECK(text_of(navigation::safe_velocity_2d(first.scene.world, first.a)) == "(0, 0)");
    // Without avoidance, its velocity is its safe velocity.
    first.scene.world.ecs().get<world::NavigationAgent2D>(first.a).avoidance_enabled = false;
    CHECK(text_of(navigation::safe_velocity_2d(first.scene.world, first.a)) == "(100, 0)");
}

TEST_CASE("obstacles that affect the navigation mesh are cut out of 2D bakes") {
    Scene scene;
    const entt::entity crate = scene.create("crate", R"({"Transform2D": {"position": [100, 100]},
        "NavigationObstacle2D": {"vertices": [[-20, -20], [20, -20], [20, 20], [-20, 20]],
        "affect_navigation_mesh": true}})");
    const entt::entity yard = scene.create(
        "yard", R"({"Transform2D": {"children": ["crate"]}, "NavigationRegion2D": {}})");
    const navigation::NavigationPolygon settings = square_outline(200.0F);
    const auto baked_area = [&] {
        const navigation::SourceGeometry2D geometry =
            navigation::parse_source_geometry(scene.world, scene.assets, yard, settings);
        auto baked = navigation::bake(settings, geometry);
        REQUIRE(baked.ok());
        return area(baked.value());
    };
    const navigation::SourceGeometry2D geometry =
        navigation::parse_source_geometry(scene.world, scene.assets, yard, settings);
    REQUIRE(geometry.projected_obstructions.size() == 1);
    CHECK(text_of(geometry.projected_obstructions[0].vertices) ==
          "(80, 80) (120, 80) (120, 120) (80, 120)");
    CHECK(geometry.obstruction_outlines.empty());
    // Grown by the agent's radius, 10, as obstructions are: 180 squared less 60 squared.
    CHECK(baked_area() == doctest::Approx(28800.0));
    // Carving, it cuts out its own shape only.
    auto& obstacle = scene.world.ecs().get<world::NavigationObstacle2D>(crate);
    obstacle.carve_navigation_mesh = true;
    CHECK(baked_area() == doctest::Approx(32400.0 - 1600.0));
    // A circle is 12 sides, as Godot's, turned with the obstacle and scaled by its largest scale.
    obstacle.vertices.clear();
    obstacle.radius = 10.0F;
    scene.world.ecs().get<world::Transform2D>(crate).scale = {2.0F, 1.0F};
    const navigation::SourceGeometry2D round =
        navigation::parse_source_geometry(scene.world, scene.assets, yard, settings);
    REQUIRE(round.projected_obstructions.size() == 1);
    CHECK(round.projected_obstructions[0].vertices.size() == 12);
    CHECK(text_of(round.projected_obstructions[0].vertices[0]) == "(120, 100)");
    CHECK(text_of(round.projected_obstructions[0].vertices[3]) == "(100, 120)");
    // Not affecting the mesh, it is left out.
    obstacle.affect_navigation_mesh = false;
    CHECK(navigation::parse_source_geometry(scene.world, scene.assets, yard, settings)
              .projected_obstructions.empty());
    CHECK(baked_area() == doctest::Approx(32400.0));
}

TEST_CASE("obstacles that affect the navigation mesh are cut out of 3D bakes") {
    Scene scene;
    scene.create("floor", R"({"Transform3D": {"position": [0, -0.5, 0]}, "StaticBody3D": {},
                              "CollisionShape3D": {"size": [10, 1, 10]}})");
    const entt::entity crate = scene.create("crate", R"({"Transform3D": {"position": [2, 0, 2]},
        "NavigationObstacle3D": {"vertices": [[-0.5, 0, -0.5], [0.5, 0, -0.5], [0.5, 0, 0.5],
        [-0.5, 0, 0.5]], "height": 2, "affect_navigation_mesh": true}})");
    const entt::entity level = scene.create("level", R"({"Transform3D": {"children": ["floor",
        "crate"]}, "NavigationRegion3D": {}})");
    const navigation::NavigationMesh settings;
    const auto bake = [&] {
        const navigation::SourceGeometry3D geometry =
            navigation::parse_source_geometry(scene.world, scene.assets, level, settings, nullptr);
        auto baked = navigation::bake(settings, geometry, nullptr);
        REQUIRE(baked.ok());
        return baked.value();
    };
    const auto inside = [](const navigation::NavigationMesh& mesh, float half) {
        return std::count_if(mesh.vertices.begin(), mesh.vertices.end(), [&](Vec3 v) {
            return std::abs(v.x - 2.0F) < half && std::abs(v.z - 2.0F) < half;
        });
    };
    const navigation::SourceGeometry3D geometry =
        navigation::parse_source_geometry(scene.world, scene.assets, level, settings, nullptr);
    REQUIRE(geometry.projected_obstructions.size() == 1);
    CHECK(text_of(geometry.projected_obstructions[0].vertices) ==
          "(1.5, 0, 1.5) (2.5, 0, 1.5) (2.5, 0, 2.5) (1.5, 0, 2.5)");
    CHECK(geometry.projected_obstructions[0].elevation == 0.0F);
    CHECK(geometry.projected_obstructions[0].height == 2.0F);

    // Marked before the walkable area shrinks by agent_radius, so it grows by it too.
    const navigation::NavigationMesh grown = bake();
    CHECK(inside(grown, 0.5F + settings.agent_radius) == 0);
    // Carving, it is marked after, so only its own shape goes.
    scene.world.ecs().get<world::NavigationObstacle3D>(crate).carve_navigation_mesh = true;
    const navigation::NavigationMesh carved = bake();
    CHECK(inside(carved, 0.5F + settings.agent_radius) > 0);
    CHECK(inside(carved, 0.4F) == 0);
    CHECK(lowest(carved.vertices, &Vec3::x) == -4.5F);
    // Above the floor's walkable top, it cuts nothing, as with affect_navigation_mesh off.
    scene.world.ecs().get<world::Transform3D>(crate).position.y = 3.0F;
    const navigation::NavigationMesh above = bake();
    scene.world.ecs().get<world::NavigationObstacle3D>(crate).affect_navigation_mesh = false;
    const navigation::NavigationMesh plain = bake();
    CHECK(plain.polygons.size() == 2);
    CHECK(text_of(above.vertices) == text_of(plain.vertices));
}

namespace {

// Tiles of 32 pixels: floor walks everywhere, a wall is solid, a ledge walks on its top half and
// water is in the second navigation layer, whose regions are in navigation layer 2.
constexpr std::string_view kTiles = R"({"format": 1, "tile_size": [32, 32],
    "navigation_layers": [{}, {"layers": 2}], "physics_layers": [{}, {"collision_layer": 4}],
    "sources": {"t": {"texture": "t.png", "tiles": {
        "0,0": {"name": "floor",
                "navigation": [{"points": [[-16, -16], [16, -16], [16, 16], [-16, 16]]}]},
        "1,0": {"name": "wall", "collision": [{"points": [[-16, -16], [16, -16], [16, 16],
                                                          [-16, 16]]},
                                              {"layer": 1, "points": [[-8, -8], [8, -8], [8, 8]]}]},
        "2,0": {"name": "ledge",
                "navigation": [{"points": [[-16, -16], [16, -16], [16, 0], [-16, 0]]}]},
        "3,0": {"name": "water", "navigation": [{"layer": 1,
                "points": [[-16, -16], [16, -16], [16, 16], [-16, 16]]}]}}}}})";

} // namespace

TEST_CASE("a tile map layer's cells are regions of their tiles' navigation polygons") {
    Scene scene;
    slash::canvas::add_asset_types(scene.assets);
    scene.files.write("tiles.tileset.json", std::string(kTiles));
    const entt::entity ground = scene.create("ground", R"({"Transform2D": {"position": [100, 0]},
        "TileMapLayer": {"tile_set": "tiles.tileset.json",
            "legend": {"f": "floor", "w": "wall", "l": "ledge", "a": "water"},
            "rows": ["fff", "fwa", "fla"]}})");
    auto info = navigation::map_info(scene.world, navigation::Map::TwoD);
    CHECK(info.errors.empty());
    CHECK(info.regions == 8); // five floors and a ledge in layer 1, two waters in layer 2
    const std::uint64_t first = info.iteration_id;

    // Round the wall to the ledge's top half, cell to cell. Godot's funnel gives the corner where
    // the path turns into the next cell twice.
    const auto around = navigation::query_path(scene.world, Vec2{148, 16}, Vec2{148, 90});
    CHECK(text_of(around.points) == "(148, 16) (132, 32) (132, 32) (132, 64) (148, 80)");
    CHECK(around.owners.front() == ground);
    CHECK(text_of(navigation::closest_point(scene.world, Vec2{148, 48}).point) == "(148, 32)");

    // Water is only on paths that ask for navigation layer 2.
    navigation::PathOptions swim;
    swim.navigation_layers = 2;
    const auto water = navigation::query_path(scene.world, Vec2{180, 40}, Vec2{180, 90}, swim);
    CHECK(text_of(water.points) == "(180, 40) (180, 64) (180, 90)"); // crossing a cell's edge
    const auto walk = navigation::query_path(scene.world, Vec2{180, 40}, Vec2{180, 90});
    CHECK(text_of(walk.points.front()) == "(180, 32)");
    CHECK(text_of(walk.points.back()) == "(164, 80)");

    // A turned tile turns its polygon: the ledge flipped upside down walks on its bottom half.
    auto& layer = scene.world.ecs().get<world::TileMapLayer>(ground);
    layer.legend["l"] = "ledge:v";
    scene.world.step(kTick);
    CHECK(navigation::map_info(scene.world, navigation::Map::TwoD).iteration_id == first + 1);
    CHECK(text_of(navigation::closest_point(scene.world, Vec2{148, 70}).point) == "(148, 80)");

    // Nothing changes, nothing is rebuilt; moving the layer moves its regions.
    scene.world.step(kTick);
    CHECK(navigation::map_info(scene.world, navigation::Map::TwoD).iteration_id == first + 1);
    scene.world.ecs().get<world::Transform2D>(ground).position = {0, 0};
    scene.world.step(kTick);
    CHECK(text_of(navigation::closest_point(scene.world, Vec2{-50, 16}).point) == "(0, 16)");

    // Editing the tile set rebuilds the map once it reloads: floors in navigation layer 4 are off
    // the paths that don't ask for it.
    const std::uint64_t moved =
        navigation::map_info(scene.world, navigation::Map::TwoD).iteration_id;
    std::string four(kTiles);
    four.replace(four.find("[{}, "), 5, R"([{"layers": 4}, )");
    scene.files.write("tiles.tileset.json", four);
    scene.assets.poll();
    CHECK(navigation::map_info(scene.world, navigation::Map::TwoD).iteration_id == moved + 1);
    CHECK(navigation::query_path(scene.world, Vec2{10, 16}, Vec2{40, 80}).points.empty());

    // A tile set with no navigation layers makes no regions.
    scene.files.write("plain.tileset.json", R"({"format": 1, "tile_size": [32, 32],
        "sources": {"t": {"texture": "t.png", "tiles": {"0,0": {"name": "floor"},
            "1,0": {"name": "wall"}, "2,0": {"name": "ledge"}, "3,0": {"name": "water"}}}}})");
    auto& layer_now = scene.world.ecs().get<world::TileMapLayer>(ground);
    layer_now.tile_set = "plain.tileset.json";
    CHECK(navigation::map_info(scene.world, navigation::Map::TwoD).regions == 0);
    layer_now.tile_set = "tiles.tileset.json";

    // navigation_enabled off leaves a layer out; a tile set that doesn't load is reported.
    scene.files.write("tiles.tileset.json", std::string(kTiles));
    scene.assets.poll();
    CHECK(navigation::map_info(scene.world, navigation::Map::TwoD).regions == 8);
    layer.navigation_enabled = false;
    CHECK(navigation::map_info(scene.world, navigation::Map::TwoD).regions == 0);
    layer.navigation_enabled = true;
    layer.tile_set = "missing.tileset.json";
    info = navigation::map_info(scene.world, navigation::Map::TwoD);
    CHECK(info.regions == 0);
    REQUIRE(info.errors.size() == 1);
    CHECK(info.errors[0].id == "ground");
    CHECK(info.errors[0].message.starts_with("missing.tileset.json: "));
}

TEST_CASE("2D bakes take tile map layers' navigation outlines and collision polygons") {
    Scene scene;
    slash::canvas::add_asset_types(scene.assets);
    scene.files.write("tiles.tileset.json", std::string(kTiles));
    scene.create("ground", R"({"Transform2D": {"position": [32, 0]},
        "TileMapLayer": {"tile_set": "tiles.tileset.json",
            "legend": {"f": "floor", "w": "wall", "l": "ledge", "a": "water"},
            "rows": ["ffff", "fwfa", "ffl."]}})");
    const entt::entity yard =
        scene.create("yard", R"({"Transform2D": {"position": [-32, 0], "children": ["ground"]},
                    "NavigationRegion2D": {}})");
    navigation::NavigationPolygon settings;
    settings.agent_radius = 0.0F;
    std::vector<std::string> warnings;
    const navigation::SourceGeometry2D geometry =
        navigation::parse_source_geometry(scene.world, scene.assets, yard, settings, &warnings);
    CHECK(warnings.empty());
    // Eight floors, a ledge and a water; the wall's two polygons in both physics layers.
    CHECK(geometry.traversable_outlines.size() == 10);
    CHECK(geometry.obstruction_outlines.size() == 2);
    // In the region's space, where the layer is 32 to the right.
    CHECK(text_of(geometry.traversable_outlines[0]) == "(32, 0) (64, 0) (64, 32) (32, 32)");

    auto baked = navigation::bake(settings, geometry);
    REQUIRE(baked.ok());
    // 4 by 3 cells less the wall, the empty cell and half the ledge.
    CHECK(area(baked.value()) == doctest::Approx(32.0 * 32.0 * 9.5));

    // parsed_collision_mask picks physics layers; meshes alone take no collision polygons.
    navigation::NavigationPolygon second = settings;
    second.parsed_collision_mask = 4;
    CHECK(navigation::parse_source_geometry(scene.world, scene.assets, yard, second)
              .obstruction_outlines.size() == 1);
    navigation::NavigationPolygon meshes = settings;
    meshes.parsed_geometry_type = navigation::ParsedGeometry::MeshInstances;
    const auto outlines_only =
        navigation::parse_source_geometry(scene.world, scene.assets, yard, meshes);
    CHECK(outlines_only.traversable_outlines.size() == 10);
    CHECK(outlines_only.obstruction_outlines.empty());

    // A tile set that doesn't load is a warning.
    scene.world.ecs().get<world::TileMapLayer>(scene.world.find("ground")).tile_set =
        "gone.tileset.json";
    warnings.clear();
    CHECK(navigation::parse_source_geometry(scene.world, scene.assets, yard, settings, &warnings)
              .traversable_outlines.empty());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].starts_with("ground: gone.tileset.json: "));
}
