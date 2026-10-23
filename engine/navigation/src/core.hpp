#pragma once

#include "slash/core/math.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

// The navigation map and its path queries, ported from Godot 4.6's navigation module
// (modules/navigation_3d: nav_utils_3d.h, 3d/nav_region_builder_3d.cpp,
// 3d/nav_map_builder_3d.cpp, 3d/nav_mesh_queries_3d.cpp and servers/nav_heap.h). Everything is in
// 3D: a 2D map's points are (x, 0, y), as Godot's 2D server mapped them before 4.5, so the two
// share this code. Floats only, built without fused multiply-adds, so paths are the same bits on
// every platform.
namespace slash::navigation::detail {

inline constexpr float kCmpEpsilon = 0.00001F;
inline constexpr float kFloatMax = std::numeric_limits<float>::max();

// ---- Godot's Vector3 math, in the same order of operations.

inline float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}
inline float length_squared(Vec3 v) {
    return dot(v, v);
}
inline float length(Vec3 v) {
    return std::sqrt(length_squared(v));
}
inline float distance(Vec3 a, Vec3 b) {
    return length(b - a);
}
inline float distance_squared(Vec3 a, Vec3 b) {
    return length_squared(b - a);
}
inline Vec3 divided(Vec3 v, float s) {
    return {v.x / s, v.y / s, v.z / s};
}
inline Vec3 normalized(Vec3 v) {
    const float squared = length_squared(v);
    if (squared == 0.0F) {
        return {};
    }
    return divided(v, std::sqrt(squared));
}
inline float lerp(float a, float b, float weight) {
    return a + (weight * (b - a));
}
inline Vec3 lerp(Vec3 a, Vec3 b, float weight) {
    return {lerp(a.x, b.x, weight), lerp(a.y, b.y, weight), lerp(a.z, b.z, weight)};
}
inline bool is_zero_approx(float value) {
    return std::abs(value) < kCmpEpsilon;
}
inline bool is_equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    float tolerance = kCmpEpsilon * std::abs(a);
    if (tolerance < kCmpEpsilon) {
        tolerance = kCmpEpsilon;
    }
    return std::abs(a - b) < tolerance;
}
inline bool is_equal_approx(Vec3 a, Vec3 b) {
    return is_equal_approx(a.x, b.x) && is_equal_approx(a.y, b.y) && is_equal_approx(a.z, b.z);
}

// Geometry3D::get_closest_point_to_segment.
Vec3 closest_point_to_segment(Vec3 point, Vec3 a, Vec3 b);
// Face3::get_closest_point_to.
Vec3 closest_point_on_face(Vec3 v0, Vec3 v1, Vec3 v2, Vec3 point);
// Face3::get_area.
float face_area(Vec3 v0, Vec3 v1, Vec3 v2);

// Godot's AABB, for a region's bounds.
struct Aabb {
    Vec3 position;
    Vec3 size;

    void expand_to(Vec3 point);
    [[nodiscard]] Aabb grown(float by) const;
    [[nodiscard]] bool has_point(Vec3 point) const;
};

// RandomPCG's generator (PCG32), for random points that repeat from a seed.
class Random {
public:
    explicit Random(std::uint64_t seed);
    std::uint32_t next();
    // From 0 to 1.
    float unit();
    // From from to to, both included.
    int between(int from, int to);

private:
    std::uint64_t state_ = 0;
    std::uint64_t increment_ = 0;
};

// ---- The map's parts, as Nav3D's.

enum class OwnerType : std::uint8_t { Region, Link };

struct Base;

struct Polygon {
    std::uint32_t id = std::numeric_limits<std::uint32_t>::max();
    const Base* owner = nullptr; // the region or link it is part of
    std::vector<Vec3> vertices;
    float surface_area = 0.0F;
};

struct Connection {
    const Polygon* polygon = nullptr; // where it leads
    int edge = -1;                    // the edge of that polygon it crosses, or -1 for a link
    Vec3 pathway_start;
    Vec3 pathway_end;
};

// An edge's ends rounded to the map's cells, as Godot's PointKey (21, 22 and 21 bits of x, y and
// z, which wrap past them) and EdgeKey (the smaller key first).
struct EdgeKey {
    std::uint64_t a = 0;
    std::uint64_t b = 0;

    friend bool operator==(const EdgeKey&, const EdgeKey&) = default;
};
struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& key) const noexcept;
};
std::uint64_t point_key(Vec3 position, Vec3 cell_size);
EdgeKey edge_key(Vec3 a, Vec3 b, Vec3 cell_size);

struct ConnectableEdge {
    EdgeKey key;
    std::uint32_t polygon_index = 0;
    int edge = -1;
    Vec3 pathway_start;
    Vec3 pathway_end;
};

// A region or link as a map sees it, as Godot's NavBaseIteration3D. Its polygons point back at
// it, so it never moves.
struct Base {
    Base() = default;
    Base(const Base&) = delete;
    Base& operator=(const Base&) = delete;
    Base(Base&&) = delete;
    Base& operator=(Base&&) = delete;
    ~Base() = default;

    bool enabled = true;
    std::uint32_t navigation_layers = 1;
    float enter_cost = 0.0F;
    float travel_cost = 1.0F;
    OwnerType type = OwnerType::Region;
    entt::entity owner = entt::null;
    bool use_edge_connections = false;
    std::vector<Polygon> polygons;
    std::vector<std::vector<Connection>> internal_connections;
};

// What builds a region: its owner's settings and its mesh in the map's space.
struct RegionSource {
    entt::entity owner = entt::null;
    bool enabled = true;
    bool use_edge_connections = true;
    std::uint32_t navigation_layers = 1;
    float enter_cost = 0.0F;
    float travel_cost = 1.0F;
    std::vector<Vec3> vertices; // placed by the region's transform
    std::vector<std::vector<int>> polygons;
};

// A region's polygons joined where they share edges, as Godot's NavRegionIteration3D.
struct Region : Base {
    float surface_area = 0.0F;
    Aabb bounds;
    std::vector<ConnectableEdge> external_edges; // edges no polygon of its own shares
    int edge_count = 0;
    int edge_merge_count = 0;
    int edge_free_count = 0;
    int edge_merge_errors = 0; // edges a third polygon tried to share
};

// NavRegionBuilder3D::build_iteration. Polygons with fewer than 3 vertices or indices out of
// range are left empty.
std::shared_ptr<const Region> build_region(const RegionSource& source, Vec3 merge_cell_size);

// A link between two points, as Godot's NavLinkIteration3D.
struct Link : Base {
    bool bidirectional = true;
    Vec3 start_position;
    Vec3 end_position;
};

// ---- The map, as Godot's NavMapIteration3D and NavMapBuilder3D.

struct MapBuild {
    bool use_edge_connections = true;
    float edge_connection_margin = 0.25F;
    float link_connection_radius = 1.0F;
    Vec3 up{0.0F, 1.0F, 0.0F};
};

// One polygon's place in an A* search, as Godot's NavigationPoly.
struct NavigationPoly {
    const Polygon* poly = nullptr;
    std::uint32_t traversable_poly_index = std::numeric_limits<std::uint32_t>::max();
    int back_navigation_poly_id = -1;
    int back_navigation_edge = -1;
    Vec3 back_navigation_edge_pathway_start;
    Vec3 back_navigation_edge_pathway_end;
    Vec3 entry;
    float traveled_distance = 0.0F;       // g cost
    float distance_to_destination = 0.0F; // h cost

    [[nodiscard]] float total_travel_cost() const {
        return traveled_distance + distance_to_destination;
    }
    void reset() {
        poly = nullptr;
        traversable_poly_index = std::numeric_limits<std::uint32_t>::max();
        back_navigation_poly_id = -1;
        back_navigation_edge = -1;
        traveled_distance = kFloatMax;
        distance_to_destination = 0.0F;
    }
};

// Godot's Heap (servers/nav_heap.h): a max-heap by NavPolyTravelCostGreaterThan, so its top is
// the polygon cheapest to reach, telling each polygon where it is.
class Heap {
public:
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] bool empty() const { return buffer_.empty(); }
    void push(NavigationPoly* poly);
    NavigationPoly* pop();
    void shift(std::uint32_t index);
    void clear();

private:
    static bool less(const NavigationPoly* a, const NavigationPoly* b);
    bool shift_up(std::uint32_t index);
    bool shift_down(std::uint32_t index);

    std::vector<NavigationPoly*> buffer_;
};

struct Map {
    Vec3 up{0.0F, 1.0F, 0.0F};
    std::vector<std::shared_ptr<const Region>> regions;
    std::vector<std::shared_ptr<const Link>> links;
    // Each region's and link's polygons' connections to other regions and links: shared edges,
    // edges near each other and links.
    std::unordered_map<const Base*, std::vector<std::vector<Connection>>> external;
    std::vector<Polygon> link_polygons;
    std::unordered_map<const Polygon*, std::uint32_t> poly_to_id;
    int polygon_count = 0;
    int edge_connection_count = 0;
    int edge_free_count = 0;
    int edge_merge_errors = 0;

    // A path query's working space, as Godot's PathQuerySlot.
    mutable std::vector<NavigationPoly> path_corridor;
    mutable Heap traversable_polys;
};

// NavMapBuilder3D::build_navmap_iteration over map.regions and map.links.
void build_map(Map& map, const MapBuild& build);

// ---- Queries, as Godot's NavMeshQueries3D.

enum class PostProcessing : std::uint8_t { CorridorFunnel, EdgeCentered, None };

struct PathTask {
    Vec3 start_position;
    Vec3 target_position;
    std::uint32_t navigation_layers = 1;
    PostProcessing path_postprocessing = PostProcessing::CorridorFunnel;
    bool simplify_path = false;
    float simplify_epsilon = 0.0F;
    std::vector<entt::entity> excluded_regions;
    std::vector<entt::entity> included_regions;
    float path_return_max_length = 0.0F;
    float path_return_max_radius = 0.0F;
    int path_search_max_polygons = 4096;
    float path_search_max_distance = 0.0F;

    // The path.
    std::vector<Vec3> path_points;
    std::vector<OwnerType> path_types;
    std::vector<entt::entity> path_owners;
    float path_length = 0.0F;

    // Building it.
    Vec3 begin_position;
    Vec3 end_position;
    const Polygon* begin_polygon = nullptr;
    const Polygon* end_polygon = nullptr;
    std::uint32_t least_cost_id = 0;
    bool finished = false;
};

// NavMeshQueries3D::query_task_map_iteration_get_path.
void query_path(const Map& map, PathTask& task);

struct ClosestPoint {
    Vec3 point;
    Vec3 normal;
    entt::entity owner = entt::null;
};
// NavMeshQueries3D::map_iteration_get_closest_point_info.
ClosestPoint closest_point(const Map& map, Vec3 point);
// NavMeshQueries3D::map_iteration_get_random_point.
Vec3 random_point(const Map& map, std::uint32_t navigation_layers, bool uniformly, Random& random);

} // namespace slash::navigation::detail
