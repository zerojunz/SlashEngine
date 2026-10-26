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
// A TileMapLayer with navigation_enabled puts a region on the 2D map for each cell, in each of its
// tile set's navigation layers that the cell's tile has outlines in, as Godot's TileMapLayer does
// (slash/canvas/tilemap.hpp).
//
// Agents with avoidance_enabled and NavigationObstacle2D and 3D avoid each other as Godot's do,
// with ORCA from RVO2 (src/rvo2.hpp): last in each tick, each agent's velocity becomes a safe
// velocity, emitted as velocity_computed and returned by safe_velocity_2d and 3d, which gameplay
// code moves it at. Obstacles with affect_navigation_mesh are cut out of bakes.
//
// The 2D map is the 3D one with its points at (x, 0, y), as Godot's 2D server was before 4.5, so
// both share the same code. Maps build at once rather than on a thread, and their floats are the
// same bits everywhere, so paths repeat on every machine.
namespace slash::navigation {

inline constexpr std::string_view kMeshSuffix = ".navmesh.json";
inline constexpr std::string_view kPolygonSuffix = ".navpoly.json";

// What geometry under a region bakes into its file, as Godot's ParsedGeometryType.
enum class ParsedGeometry : std::uint8_t { MeshInstances, StaticColliders, Both };
// How Recast splits a 3D bake's walkable area, as Godot's NavigationMesh SamplePartitionType.
enum class Partition3D : std::uint8_t { Watershed, Monotone, Layers };
// How a 2D bake's area is split, as Godot's NavigationPolygon SamplePartitionType.
enum class Partition2D : std::uint8_t { ConvexPartition, Triangulate };

// A .navmesh.json file, as Godot's NavigationMesh: convex polygons over shared vertices, and the
// settings that bake them (navigation.bake), with Godot's names and defaults.
//
//   {"format": 1, "vertices": [[0, 0, 0], [4, 0, 0], [4, 0, 4], [0, 0, 4]],
//    "polygons": [[0, 1, 2, 3]], "cell_size": 0.25, "agent_radius": 0.4}
struct NavigationMesh {
    std::vector<Vec3> vertices;
    std::vector<std::vector<int>> polygons; // indices into vertices
    float cell_size = 0.25F;
    float cell_height = 0.25F;
    // Baking, in meters (agent_max_slope in degrees).
    Partition3D sample_partition_type = Partition3D::Watershed;
    ParsedGeometry geometry_parsed_geometry_type = ParsedGeometry::Both;
    std::uint32_t geometry_collision_mask = 0xFFFFFFFFU;
    float border_size = 0.0F;
    float agent_height = 1.5F;
    float agent_radius = 0.5F;
    float agent_max_climb = 0.25F;
    float agent_max_slope = 45.0F;
    float region_min_size = 2.0F;
    float region_merge_size = 20.0F;
    float edge_max_length = 0.0F;
    float edge_max_error = 1.3F;
    float vertices_per_polygon = 6.0F;
    float detail_sample_distance = 6.0F;
    float detail_sample_max_error = 1.0F;
    bool filter_low_hanging_obstacles = false;
    bool filter_ledge_spans = false;
    bool filter_walkable_low_height_spans = false;
    // Bakes only what is in this box, when its size has volume.
    Vec3 filter_baking_aabb_position;
    Vec3 filter_baking_aabb_size;
    Vec3 filter_baking_aabb_offset;
};

// A .navpoly.json file, as Godot's NavigationPolygon: the same in 2D, in pixels, with outlines
// that baking fills.
struct NavigationPolygon {
    std::vector<Vec2> vertices;
    std::vector<std::vector<int>> polygons;
    // Closed outlines of the walkable area that baking merges, as Godot's outlines.
    std::vector<std::vector<Vec2>> outlines;
    float cell_size = 1.0F;
    // Baking, in pixels.
    Partition2D sample_partition_type = Partition2D::ConvexPartition;
    ParsedGeometry parsed_geometry_type = ParsedGeometry::Both;
    std::uint32_t parsed_collision_mask = 0xFFFFFFFFU;
    float border_size = 0.0F;
    float agent_radius = 10.0F;
    // Bakes only what is in this rect, when it has area.
    Vec2 baking_rect_position;
    Vec2 baking_rect_size;
    Vec2 baking_rect_offset;
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

// ---- Baking, as Godot 4.6's NavigationServer3D and 2D: parse the geometry under a region, then
// bake it into the region's polygons with the settings in its file.

// What a 3D bake works from, as Godot's NavigationMeshSourceGeometryData3D: triangles in the
// region's space, wound counterclockwise as Recast takes them.
struct SourceGeometry3D {
    std::vector<float> vertices; // x, y and z of each point
    std::vector<int> indices;    // three per triangle
    // Outlines of obstacles with affect_navigation_mesh, on the ground from elevation up to
    // elevation + height, as Godot's projected obstructions: taken out before the walkable area
    // shrinks by agent_radius, or after it with carve.
    struct Obstruction {
        std::vector<Vec3> vertices; // y is not used
        float elevation = 0.0F;
        float height = 0.0F;
        bool carve = false;
    };
    std::vector<Obstruction> projected_obstructions{}; // NOLINT(readability-redundant-member-init)
};

// What a 2D bake works from, as Godot's NavigationMeshSourceGeometryData2D: outlines in the
// region's space of where agents can go and of what is in their way.
struct SourceGeometry2D {
    std::vector<std::vector<Vec2>> traversable_outlines;
    std::vector<std::vector<Vec2>> obstruction_outlines;
    struct Obstruction {
        std::vector<Vec2> vertices;
        bool carve = false;
    };
    std::vector<Obstruction> projected_obstructions{}; // NOLINT(readability-redundant-member-init)
};

// The geometry under a region entity, as Godot's parse_source_geometry_data with the region as
// its root node: the entity and those under it in Transform3D children, each MeshInstance3D's
// primitive mesh or model file (with geometry_parsed_geometry_type mesh_instances or both) and
// the shapes of each StaticBody3D and AnimatableBody3D whose collision_layer shares a bit with
// geometry_collision_mask (static_colliders or both), and each NavigationObstacle3D with
// affect_navigation_mesh as a projected obstruction. Model files load through assets; a mesh
// that doesn't load is left out with a warning naming its entity.
[[nodiscard]] SourceGeometry3D parse_source_geometry(const world::World& world,
                                                     asset::Assets& assets, entt::entity root,
                                                     const NavigationMesh& settings,
                                                     std::vector<std::string>* warnings = nullptr);
// One entity's own geometry, as parse_source_geometry takes it, in the world's space.
struct EntityGeometry {
    entt::entity entity = entt::null;
    SourceGeometry3D geometry;
};
// Each entity's own geometry, in world space and tree order (world::Tree3D::order), those with
// none left out: what parse_source_geometry takes of each, without those under it. What scene
// assembly stands things on and keeps them apart from.
[[nodiscard]] std::vector<EntityGeometry>
world_geometry(const world::World& world, asset::Assets& assets, const NavigationMesh& settings,
               std::vector<std::string>* warnings = nullptr);
// The same in 2D: the shapes of each StaticBody2D and AnimatableBody2D under the region whose
// collision_layer shares a bit with parsed_collision_mask, as obstructions, each
// NavigationObstacle2D with affect_navigation_mesh as a projected obstruction, and each
// TileMapLayer's tiles: their navigation outlines as traversable outlines, and their collision
// polygons in physics layers whose collision_layer shares a bit with parsed_collision_mask as
// obstructions (with parsed_geometry_type static_colliders or both). Tile sets load through
// assets; a layer whose set doesn't load, or whose cells don't decode, is left out with a warning
// naming its entity.
[[nodiscard]] SourceGeometry2D parse_source_geometry(const world::World& world,
                                                     asset::Assets& assets, entt::entity root,
                                                     const NavigationPolygon& settings,
                                                     std::vector<std::string>* warnings = nullptr);

// The settings with their polygons baked from the geometry, as Godot's
// bake_from_source_geometry_data: Recast's walkable area, its detail mesh's triangles as the
// polygons. With no geometry the polygons are empty. Warnings say which settings don't fit
// whole cells, as Godot's do. Fails when Recast does, or when the geometry spans more than
// 30 million cells, as Godot refuses to bake.
[[nodiscard]] Result<NavigationMesh> bake(const NavigationMesh& settings,
                                          const SourceGeometry3D& geometry,
                                          std::vector<std::string>* warnings = nullptr);
// The 2D one: the outlines (the file's and the geometry's) merged, the obstructions taken out,
// shrunk by agent_radius, carving obstructions taken out, and split into convex polygons or
// triangles.
[[nodiscard]] Result<NavigationPolygon> bake(const NavigationPolygon& settings,
                                             const SourceGeometry2D& geometry);

// A file's document with its vertices and polygons replaced by a bake's, keeping the rest as
// written. doc is the file's, or null for a new one.
[[nodiscard]] json::Value with_polygons(json::Value doc, const NavigationMesh& mesh);
[[nodiscard]] json::Value with_polygons(json::Value doc, const NavigationPolygon& polygon);

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

// Avoidance, as Godot's NavigationAgent avoidance: the safe velocity the last tick gave an agent
// with avoidance_enabled (its last velocity_computed event's), or its velocity without.
[[nodiscard]] Vec2 safe_velocity_2d(const world::World& world, entt::entity agent);
[[nodiscard]] Vec3 safe_velocity_3d(const world::World& world, entt::entity agent);
// Replaces the velocity avoidance starts the next step from, as Godot's set_velocity_forced, so
// an agent that was moved to a new place doesn't carry its old velocity there. It takes effect
// on the next tick the agent follows a path. Called often, it can get agents stuck.
void set_velocity_forced(world::World& world, entt::entity agent, Vec2 velocity);
void set_velocity_forced(world::World& world, entt::entity agent, Vec3 velocity);

// An agent's path and progress, for navigation.agent: 2D agents' points are (x, y, 0).
struct AgentInfo {
    bool target_submitted = false; // its path is being followed
    bool navigation_finished = true;
    bool target_reached = false;
    bool last_waypoint_reached = false;
    std::size_t path_index = 0; // the point it is going to
    Path<Vec3> path;
    Vec3 safe_velocity; // the last tick's, with avoidance_enabled
};
// An agent with no state yet, or no agent component, is all defaults.
[[nodiscard]] AgentInfo agent_info(world::World& world, entt::entity agent);

// What the navigation system keeps between ticks beside the components: agents' paths, progress
// and avoidance velocities and obstacles' avoidance velocities, by entity ID. Sessions put it
// back with the world when a command fails or is a dry run.
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
