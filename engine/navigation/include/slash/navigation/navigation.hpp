#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Navigation, as Godot 4.6's NavigationServer2D and NavigationServer3D: each world has a 2D and a
// 3D map, made of the regions of navigation mesh and the links that the components in
// slash/world/components.hpp place on them (NavigationRegion2D and 3D, NavigationLink2D and 3D).
// Regions join where their polygons share edges, or where edges of different regions come
// within the map's edge_connection_margin; links join the polygons closest to their ends.
//
// query_path finds a path over a map with Godot's A* and shapes it with its funnel, closest_point
// and random_point ask where the map is. A NavigationAgent2D or 3D follows a path to its
// target_position, as Godot's NavigationAgent nodes: the "navigation" system rebuilds the maps
// whose regions and links changed, then updates each agent's path and progress and emits its
// path_changed, waypoint_reached, link_reached, target_reached and navigation_finished events.
// Agents move nothing; gameplay code moves them toward next_path_position.
//
// The 2D map is the 3D one with its points at (x, 0, y), as Godot's 2D server was before 4.5, so
// both share the same code. Maps build at once rather than on a thread, and their floats are the
// same bits everywhere, so paths repeat on every machine.
namespace slash::navigation {

inline constexpr std::string_view kMeshSuffix = ".navmesh.json";
inline constexpr std::string_view kPolygonSuffix = ".navpoly.json";

// A .navmesh.json file, as Godot's NavigationMesh: convex polygons over shared vertices.
//
//   {"format": 1, "vertices": [[0, 0, 0], [4, 0, 0], [4, 0, 4], [0, 0, 4]],
//    "polygons": [[0, 1, 2, 3]], "cell_size": 0.25, "cell_height": 0.25}
struct NavigationMesh {
    std::vector<Vec3> vertices;
    std::vector<std::vector<int>> polygons; // indices into vertices
    float cell_size = 0.25F;
    float cell_height = 0.25F;
};

// A .navpoly.json file, as Godot's NavigationPolygon: the same in 2D, in pixels.
struct NavigationPolygon {
    std::vector<Vec2> vertices;
    std::vector<std::vector<int>> polygons;
    float cell_size = 1.0F;
};

// JSON Schemas for the two files.
[[nodiscard]] json::Value mesh_schema();
[[nodiscard]] json::Value polygon_schema();

// Every error in a document, each with the JSON Pointer of the bad value: those of the schema,
// indices past the vertices or used twice in a polygon, and polygons that aren't convex.
[[nodiscard]] std::vector<json::ValidationError> validate_mesh(const json::Value& doc);
[[nodiscard]] std::vector<json::ValidationError> validate_polygon(const json::Value& doc);

// The mesh or polygon in a document, or its first error.
[[nodiscard]] Result<NavigationMesh> parse_mesh(const json::Value& doc);
[[nodiscard]] Result<NavigationPolygon> parse_polygon(const json::Value& doc);

// Registers .navmesh.json and .navpoly.json files with assets.
void add_asset_types(asset::Assets& assets);

// One map's settings, as Godot's navigation/2d and navigation/3d project settings.
struct MapSettings {
    float cell_size = 0.25F;
    float cell_height = 0.25F; // 3D only
    // Edge ends closer than cell_size times this (at most 0.1, as Godot clamps it) are the same.
    float merge_rasterizer_cell_scale = 1.0F;
    bool use_edge_connections = true;
    float edge_connection_margin = 0.25F;
    float link_connection_radius = 1.0F;
    Vec3 up{0.0F, 1.0F, 0.0F}; // 3D only

    friend bool operator==(const MapSettings&, const MapSettings&) = default;
};

// The project's navigation_2d and navigation_3d settings.
struct Settings {
    MapSettings map_2d{.cell_size = 1.0F,
                       .cell_height = 1.0F,
                       .edge_connection_margin = 1.0F,
                       .link_connection_radius = 4.0F};
    MapSettings map_3d;

    friend bool operator==(const Settings&, const Settings&) = default;
};

enum class Map : std::uint8_t { TwoD, ThreeD };

// JSON Schema for project.json's navigation_2d or navigation_3d object.
[[nodiscard]] json::Value settings_schema(Map map);
// A map's settings from an object that passed the schema; missing fields keep Settings{}'s.
[[nodiscard]] MapSettings parse_settings(Map map, const json::Value& settings);

// Adds the "navigation" system to a world, which after physics rebuilds changed maps and moves
// agents along their paths. Regions' files load through assets, which must outlive the world.
void install(world::World& world, asset::Assets& assets, const Settings& settings = {});
// Changes the settings of a world the system was installed in.
void set_settings(world::World& world, const Settings& settings);

// ---- Paths, as Godot's NavigationPathQueryParameters and NavigationPathQueryResult.

enum class Postprocessing : std::uint8_t { CorridorFunnel, EdgeCentered, None };
// The name an agent's path_postprocessing uses for it, and the other way; false if unknown.
[[nodiscard]] std::string_view postprocessing_name(Postprocessing postprocessing);
[[nodiscard]] bool postprocessing_from_name(std::string_view name, Postprocessing& out);

struct PathOptions {
    std::uint32_t navigation_layers = 1;
    Postprocessing postprocessing = Postprocessing::CorridorFunnel;
    bool simplify_path = false;
    float simplify_epsilon = 0.0F;
    // Regions the path may not use, or the only ones it may (when not empty); either leaves
    // links out, as Godot's.
    std::vector<entt::entity> excluded_regions{}; // NOLINT(readability-redundant-member-init)
    std::vector<entt::entity> included_regions{}; // NOLINT(readability-redundant-member-init)
    float path_return_max_length = 0.0F;          // 0: no limit
    float path_return_max_radius = 0.0F;
    int path_search_max_polygons = 4096; // 0: no limit
    float path_search_max_distance = 0.0F;
};

// What each point of a path is on.
enum class Segment : std::uint8_t { Region, Link };

template <typename V>
struct Path {
    std::vector<V> points;
    std::vector<Segment> types;       // one per point
    std::vector<entt::entity> owners; // the region or link each point is on
    float length = 0.0F;
};

// A path over the map from start to target, as Godot's query_path: from the closest point of
// the map to start to the closest it can reach to target. Empty when the map is.
[[nodiscard]] Path<Vec2> query_path(world::World& world, Vec2 start, Vec2 target,
                                    const PathOptions& options = {});
[[nodiscard]] Path<Vec3> query_path(world::World& world, Vec3 start, Vec3 target,
                                    const PathOptions& options = {});

// The point of the map closest to a point, and the region it is in (null for none), as Godot's
// map_get_closest_point and map_get_closest_point_owner. Disabled regions count, as in Godot.
struct ClosestPoint2D {
    Vec2 point;
    entt::entity owner = entt::null;
};
struct ClosestPoint3D {
    Vec3 point;
    Vec3 normal; // its polygon's, as Godot's: the cross of its first two edges
    entt::entity owner = entt::null;
};
[[nodiscard]] ClosestPoint2D closest_point(world::World& world, Vec2 point);
[[nodiscard]] ClosestPoint3D closest_point(world::World& world, Vec3 point);

// A random point of the regions on navigation_layers, as Godot's map_get_random_point: in a
// random polygon of a random region, or with uniformly, anywhere with the same chance. The same
// seed picks the same point.
[[nodiscard]] Vec2 random_point_2d(world::World& world, std::uint32_t navigation_layers,
                                   bool uniformly, std::uint64_t seed);
[[nodiscard]] Vec3 random_point_3d(world::World& world, std::uint32_t navigation_layers,
                                   bool uniformly, std::uint64_t seed);

// What a map is made of now.
struct MapInfo {
    // Goes up each time the map is rebuilt, as Godot's get_iteration_id.
    std::uint64_t iteration_id = 0;
    std::size_t regions = 0;
    std::size_t links = 0;
    std::size_t polygons = 0;
    std::size_t edge_connections = 0; // edges of different regions joined, shared or near
    std::size_t free_edges = 0;       // edges joined to nothing
    std::size_t merge_errors = 0;     // edges three or more polygons share
    // Regions left out of the map, each with its entity's ID and why: a file that doesn't load
    // or is of the wrong kind.
    struct Error {
        std::string id;
        std::string message;
    };
    std::vector<Error> errors{}; // NOLINT(readability-redundant-member-init)
};
[[nodiscard]] MapInfo map_info(world::World& world, Map map);

// Rebuilds the maps whose regions or links changed, as the navigation system does each tick.
// Queries call it themselves.
void sync(world::World& world);

// ---- Agents, as Godot's NavigationAgent2D and NavigationAgent3D methods. Each takes an entity
// with an agent component.

// Sets target_position and asks for a path to it, even if it is the same, as Godot's
// set_target_position; changing the component's target_position asks too.
void set_target_position(world::World& world, entt::entity agent, Vec2 position);
void set_target_position(world::World& world, entt::entity agent, Vec3 position);

// Where the entity should head next: the path point it is going to, or where it is when it has
// no path. These bring the agent's path up to date first, as Godot's do.
[[nodiscard]] Vec2 next_path_position_2d(world::World& world, entt::entity agent);
[[nodiscard]] Vec3 next_path_position_3d(world::World& world, entt::entity agent);
// Whether it reached its target or the end of a path that can't reach it.
[[nodiscard]] bool is_navigation_finished(world::World& world, entt::entity agent);
// Whether it came within target_desired_distance of target_position.
[[nodiscard]] bool is_target_reached(const world::World& world, entt::entity agent);
// Whether its path ends within target_desired_distance of target_position.
[[nodiscard]] bool is_target_reachable(world::World& world, entt::entity agent);
// The last point of its path, or the origin with no path.
[[nodiscard]] Vec2 final_position_2d(world::World& world, entt::entity agent);
[[nodiscard]] Vec3 final_position_3d(world::World& world, entt::entity agent);
// The straight distance from the entity to target_position.
[[nodiscard]] float distance_to_target(const world::World& world, entt::entity agent);

// An agent's path and progress, for navigation.agent: 2D agents' points are (x, y, 0).
struct AgentInfo {
    bool target_submitted = false; // its path is being followed
    bool navigation_finished = true;
    bool target_reached = false;
    bool last_waypoint_reached = false;
    std::size_t path_index = 0; // the point it is going to
    Path<Vec3> path;
};
// An agent with no state yet, or no agent component, is all defaults.
[[nodiscard]] AgentInfo agent_info(world::World& world, entt::entity agent);

// What the navigation system keeps between ticks beside the components: agents' paths and
// progress, by entity ID. Sessions put it back with the world when a command fails or is a dry
// run.
struct Snapshot {
    std::shared_ptr<const void> state;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

// Every error in a scene's or prefab's navigation regions, each with the JSON Pointer of the
// bad value: a file that doesn't load or validate, or is of the wrong kind. Files are read
// through reader.
[[nodiscard]] std::vector<json::ValidationError> check_scene(const json::Value& doc,
                                                             const world::FileReader& reader);

} // namespace slash::navigation
