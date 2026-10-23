#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
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
