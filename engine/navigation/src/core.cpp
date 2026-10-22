#include "core.hpp"

#include <algorithm>
#include <map>

namespace slash::navigation::detail {

namespace {

// THREE_POINTS_CROSS_PRODUCT in nav_mesh_queries_3d.cpp.
Vec3 three_points_cross(Vec3 a, Vec3 b, Vec3 c) {
    return cross(c - a, b - a);
}

// Geometry3D::segment_intersects_sphere.
bool segment_intersects_sphere(Vec3 from, Vec3 to, Vec3 sphere_position, float sphere_radius,
                               Vec3& result) {
    const Vec3 sphere = sphere_position - from;
    const Vec3 rel = to - from;
    const float rel_length = length(rel);
    if (rel_length < kCmpEpsilon) {
        return false;
    }
    const Vec3 normal = divided(rel, rel_length);
    const float sphere_d = dot(normal, sphere);
    const float ray_distance = distance(sphere, normal * sphere_d);
    if (ray_distance >= sphere_radius) {
        return false;
    }
    const float inters_d2 = (sphere_radius * sphere_radius) - (ray_distance * ray_distance);
    float inters_d = sphere_d;
    if (inters_d2 >= kCmpEpsilon) {
        inters_d -= std::sqrt(inters_d2);
    }
    if (inters_d < 0.0F || inters_d > rel_length) {
        return false;
    }
    result = from + (normal * inters_d);
    return true;
}

// Plane::intersects_segment, for a plane through origin_d along normal.
bool plane_intersects_segment(Vec3 normal, float d, Vec3 begin, Vec3 end, Vec3& intersection) {
    const Vec3 segment = begin - end;
    const float den = dot(normal, segment);
    if (is_zero_approx(den)) {
        return false;
    }
    float dist = (dot(normal, begin) - d) / den;
    if (dist < -kCmpEpsilon || dist > (1.0F + kCmpEpsilon)) {
        return false;
    }
    dist = -dist;
    intersection = begin + (segment * dist);
    return true;
}

// Two polygon edges that share an edge key, as Godot's EdgeConnectionPair.
struct EdgeConnectionPair {
    Connection connections[2]; // NOLINT(modernize-avoid-c-arrays)
    int size = 0;
};

// Godot's HashMap keeps its keys in the order they went in, and the builders walk it so; this
// does the same.
class PairMap {
public:
    // The pair for a key, and whether it is new.
    std::pair<EdgeConnectionPair*, bool> find_or_insert(const EdgeKey& key) {
        const auto [found, inserted] = index_.try_emplace(key, entries_.size());
        if (inserted) {
            entries_.emplace_back(key, EdgeConnectionPair{});
        }
        return {&entries_[found->second].second, inserted};
    }
    [[nodiscard]] const std::vector<std::pair<EdgeKey, EdgeConnectionPair>>& entries() const {
        return entries_;
    }

private:
    std::unordered_map<EdgeKey, std::size_t, EdgeKeyHash> index_;
    std::vector<std::pair<EdgeKey, EdgeConnectionPair>> entries_;
};

} // namespace

Vec3 closest_point_to_segment(Vec3 point, Vec3 a, Vec3 b) {
    const Vec3 p = point - a;
    const Vec3 n = b - a;
    const float l2 = length_squared(n);
    if (l2 < 1e-20F) {
        return a;
    }
    const float d = dot(n, p) / l2;
    if (d <= 0.0F) {
        return a;
    }
    if (d >= 1.0F) {
        return b;
    }
    return a + (n * d);
}

Vec3 closest_point_on_face(Vec3 v0, Vec3 v1, Vec3 v2, Vec3 point) {
    const Vec3 edge0 = v1 - v0;
    const Vec3 edge1 = v2 - v0;
    const Vec3 to_v0 = v0 - point;

    const float a = dot(edge0, edge0);
    const float b = dot(edge0, edge1);
    const float c = dot(edge1, edge1);
    const float d = dot(edge0, to_v0);
    const float e = dot(edge1, to_v0);

    const float det = (a * c) - (b * b);
    float s = (b * e) - (c * d);
    float t = (b * d) - (a * e);

    if (s + t < det) {
        if (s < 0.0F) {
            if (t < 0.0F) {
                if (d < 0.0F) {
                    s = std::clamp(-d / a, 0.0F, 1.0F);
                    t = 0.0F;
                } else {
                    s = 0.0F;
                    t = std::clamp(-e / c, 0.0F, 1.0F);
                }
            } else {
                s = 0.0F;
                t = std::clamp(-e / c, 0.0F, 1.0F);
            }
        } else if (t < 0.0F) {
            s = std::clamp(-d / a, 0.0F, 1.0F);
            t = 0.0F;
        } else {
            const float inv_det = 1.0F / det;
            s *= inv_det;
            t *= inv_det;
        }
    } else {
        if (s < 0.0F) {
            const float tmp0 = b + d;
            const float tmp1 = c + e;
            if (tmp1 > tmp0) {
                const float numer = tmp1 - tmp0;
                const float denom = a - (2.0F * b) + c;
                s = std::clamp(numer / denom, 0.0F, 1.0F);
                t = 1.0F - s;
            } else {
                t = std::clamp(-e / c, 0.0F, 1.0F);
                s = 0.0F;
            }
        } else if (t < 0.0F) {
            if (a + d > b + e) {
                const float numer = c + e - b - d;
                const float denom = a - (2.0F * b) + c;
                s = std::clamp(numer / denom, 0.0F, 1.0F);
                t = 1.0F - s;
            } else {
                s = std::clamp(-d / a, 0.0F, 1.0F);
                t = 0.0F;
            }
        } else {
            const float numer = c + e - b - d;
            const float denom = a - (2.0F * b) + c;
            s = std::clamp(numer / denom, 0.0F, 1.0F);
            t = 1.0F - s;
        }
    }
    return v0 + (edge0 * s) + (edge1 * t);
}

float face_area(Vec3 v0, Vec3 v1, Vec3 v2) {
    return length(cross(v0 - v1, v0 - v2)) * 0.5F;
}

void Aabb::expand_to(Vec3 point) {
    Vec3 begin = position;
    Vec3 end = position + size;
    begin.x = std::min(point.x, begin.x);
    begin.y = std::min(point.y, begin.y);
    begin.z = std::min(point.z, begin.z);
    end.x = std::max(point.x, end.x);
    end.y = std::max(point.y, end.y);
    end.z = std::max(point.z, end.z);
    position = begin;
    size = end - begin;
}

Aabb Aabb::grown(float by) const {
    return {position - Vec3{by, by, by}, size + Vec3{by * 2.0F, by * 2.0F, by * 2.0F}};
}

bool Aabb::has_point(Vec3 point) const {
    return point.x >= position.x && point.y >= position.y && point.z >= position.z &&
           point.x <= position.x + size.x && point.y <= position.y + size.y &&
           point.z <= position.z + size.z;
}

Random::Random(std::uint64_t seed) : increment_((seed << 1U) | 1U) {
    next();
    state_ += seed;
    next();
}

std::uint32_t Random::next() {
    const std::uint64_t old = state_;
    state_ = (old * 6364136223846793005ULL) + increment_;
    const auto shifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
    const auto rotation = static_cast<std::uint32_t>(old >> 59U);
    return (shifted >> rotation) | (shifted << ((~rotation + 1U) & 31U));
}

float Random::unit() {
    return static_cast<float>(next() >> 8U) * (1.0F / 16777216.0F);
}

int Random::between(int from, int to) {
    if (to <= from) {
        return from;
    }
    // pcg32_boundedrand_r: no bias toward low values.
    const auto bound = static_cast<std::uint32_t>(to - from) + 1U;
    const std::uint32_t threshold = (~bound + 1U) % bound;
    while (true) {
        const std::uint32_t r = next();
        if (r >= threshold) {
            return from + static_cast<int>(r % bound);
        }
    }
}

std::size_t EdgeKeyHash::operator()(const EdgeKey& key) const noexcept {
    const std::hash<std::uint64_t> hash;
    return hash(key.a) ^ (hash(key.b) * 31U);
}

std::uint64_t point_key(Vec3 position, Vec3 cell_size) {
    const auto x = static_cast<std::int64_t>(std::floor(position.x / cell_size.x));
    const auto y = static_cast<std::int64_t>(std::floor(position.y / cell_size.y));
    const auto z = static_cast<std::int64_t>(std::floor(position.z / cell_size.z));
    const auto bits = [](std::int64_t value, unsigned width) {
        return static_cast<std::uint64_t>(value) & ((std::uint64_t{1} << width) - 1U);
    };
    return bits(x, 21) | (bits(y, 22) << 21U) | (bits(z, 21) << 43U);
}

EdgeKey edge_key(Vec3 a, Vec3 b, Vec3 cell_size) {
    EdgeKey key{point_key(a, cell_size), point_key(b, cell_size)};
    if (key.a > key.b) {
        std::swap(key.a, key.b);
    }
    return key;
}

std::shared_ptr<const Region> build_region(const RegionSource& source, Vec3 merge_cell_size) {
    auto region = std::make_shared<Region>();
    region->enabled = source.enabled;
    region->navigation_layers = source.navigation_layers;
    region->enter_cost = source.enter_cost;
    region->travel_cost = source.travel_cost;
    region->type = OwnerType::Region;
    region->owner = source.owner;
    region->use_edge_connections = source.use_edge_connections;

    // _build_step_process_navmesh_data.
    if (!source.vertices.empty() && !source.polygons.empty()) {
        const auto vertex_count = static_cast<int>(source.vertices.size());
        region->polygons.resize(source.polygons.size());
        float region_area = 0.0F;
        Aabb bounds;
        bool first_vertex = true;
        for (std::size_t i = 0; i < region->polygons.size(); ++i) {
            Polygon& polygon = region->polygons[i];
            polygon.id = static_cast<std::uint32_t>(i);
            polygon.owner = region.get();
            polygon.surface_area = 0.0F;
            const std::vector<int>& indices = source.polygons[i];
            if (indices.size() < 3 || std::ranges::any_of(indices, [&](int index) {
                    return index < 0 || index >= vertex_count;
                })) {
                continue;
            }
            const auto at = [&](std::size_t j) {
                return source.vertices[static_cast<std::size_t>(indices[j])];
            };
            float polygon_area = 0.0F;
            for (std::size_t j = 2; j < indices.size(); ++j) {
                polygon_area += face_area(at(0), at(j - 1), at(j));
            }
            polygon.surface_area = polygon_area;
            region_area += polygon_area;
            polygon.vertices.resize(indices.size());
            for (std::size_t j = 0; j < indices.size(); ++j) {
                const Vec3 point = at(j);
                polygon.vertices[j] = point;
                if (first_vertex) {
                    first_vertex = false;
                    bounds.position = point;
                } else {
                    bounds.expand_to(point);
                }
            }
        }
        region->surface_area = region_area;
        region->bounds = bounds;
    }

    // _build_step_find_edge_connection_pairs.
    PairMap pairs;
    region->internal_connections.resize(region->polygons.size());
    int free_edges = 0;
    for (const Polygon& polygon : region->polygons) {
        const std::size_t count = polygon.vertices.size();
        for (std::size_t p = 0; p < count; ++p) {
            const std::size_t next = (p + 1) % count;
            const EdgeKey key =
                edge_key(polygon.vertices[p], polygon.vertices[next], merge_cell_size);
            auto [pair, inserted] = pairs.find_or_insert(key);
            if (inserted) {
                ++region->edge_count;
                ++free_edges;
            }
            if (pair->size < 2) {
                pair->connections[pair->size] = {.polygon = &polygon,
                                                 .edge = static_cast<int>(p),
                                                 .pathway_start = polygon.vertices[p],
                                                 .pathway_end = polygon.vertices[next]};
                ++pair->size;
                if (pair->size == 2) {
                    --free_edges;
                }
            } else {
                ++region->edge_merge_errors;
            }
        }
    }
    region->edge_free_count = free_edges;

    // _build_step_merge_edge_connection_pairs.
    for (const auto& [key, pair] : pairs.entries()) {
        if (pair.size == 2) {
            const Connection& c1 = pair.connections[0];
            const Connection& c2 = pair.connections[1];
            region->internal_connections[c1.polygon->id].push_back(c2);
            region->internal_connections[c2.polygon->id].push_back(c1);
            ++region->edge_merge_count;
        } else {
            const Connection& connection = pair.connections[0];
            region->external_edges.push_back({.key = key,
                                              .polygon_index = connection.polygon->id,
                                              .edge = connection.edge,
                                              .pathway_start = connection.pathway_start,
                                              .pathway_end = connection.pathway_end});
        }
    }
    return region;
}

// ---- Heap.

bool Heap::less(const NavigationPoly* a, const NavigationPoly* b) {
    // NavPolyTravelCostGreaterThan.
    const float f_cost_a = a->total_travel_cost();
    const float f_cost_b = b->total_travel_cost();
    if (f_cost_a != f_cost_b) {
        return f_cost_a > f_cost_b;
    }
    return a->distance_to_destination > b->distance_to_destination;
}

void Heap::push(NavigationPoly* poly) {
    buffer_.push_back(poly);
    poly->traversable_poly_index = static_cast<std::uint32_t>(buffer_.size() - 1);
    shift_up(static_cast<std::uint32_t>(buffer_.size() - 1));
}

NavigationPoly* Heap::pop() {
    NavigationPoly* value = buffer_[0];
    value->traversable_poly_index = kInvalidIndex;
    if (buffer_.size() > 1) {
        buffer_[0] = buffer_.back();
        buffer_[0]->traversable_poly_index = 0;
        buffer_.pop_back();
        shift_down(0);
    } else {
        buffer_.pop_back();
    }
    return value;
}

void Heap::shift(std::uint32_t index) {
    if (!shift_up(index)) {
        shift_down(index);
    }
}

void Heap::clear() {
    for (NavigationPoly* poly : buffer_) {
        poly->traversable_poly_index = kInvalidIndex;
    }
    buffer_.clear();
}

bool Heap::shift_up(std::uint32_t index) {
    NavigationPoly* value = buffer_[index];
    std::uint32_t current = index;
    std::uint32_t parent = (current - 1) / 2;
    while (current > 0 && less(buffer_[parent], value)) {
        buffer_[current] = buffer_[parent];
        buffer_[current]->traversable_poly_index = current;
        current = parent;
        parent = (current - 1) / 2;
    }
    if (current != index) {
        buffer_[current] = value;
        value->traversable_poly_index = current;
        return true;
    }
    return false;
}

bool Heap::shift_down(std::uint32_t index) {
    NavigationPoly* value = buffer_[index];
    std::uint32_t current = index;
    std::uint32_t child = (2 * current) + 1;
    const auto size = static_cast<std::uint32_t>(buffer_.size());
    while (child < size) {
        if (child + 1 < size && less(buffer_[child], buffer_[child + 1])) {
            ++child;
        }
        if (less(buffer_[child], value)) {
            break;
        }
        buffer_[current] = buffer_[child];
        buffer_[current]->traversable_poly_index = current;
        current = child;
        child = (2 * current) + 1;
    }
    if (current != index) {
        buffer_[current] = value;
        value->traversable_poly_index = current;
        return true;
    }
    return false;
}

// ---- The map.

void build_map(Map& map, const MapBuild& build) {
    map.up = build.up;
    map.external.clear();
    map.link_polygons.clear();
    map.poly_to_id.clear();
    map.edge_connection_count = 0;
    map.edge_free_count = 0;
    map.edge_merge_errors = 0;

    // _build_step_gather_region_polygons.
    int polygon_count = 0;
    for (const auto& region : map.regions) {
        polygon_count += static_cast<int>(region->polygons.size());
        map.external[region.get()].resize(region->polygons.size());
    }

    // _build_step_find_edge_connection_pairs.
    PairMap pairs;
    for (const auto& region : map.regions) {
        for (const ConnectableEdge& edge : region->external_edges) {
            auto [pair, inserted] = pairs.find_or_insert(edge.key);
            (void)inserted;
            if (pair->size < 2) {
                pair->connections[pair->size] = {.polygon = &region->polygons[edge.polygon_index],
                                                 .edge = edge.edge,
                                                 .pathway_start = edge.pathway_start,
                                                 .pathway_end = edge.pathway_end};
                ++pair->size;
            } else {
                ++map.edge_merge_errors;
            }
        }
    }

    // _build_step_merge_edge_connection_pairs.
    std::vector<Connection> free_edges;
    for (const auto& [key, pair] : pairs.entries()) {
        if (pair.size == 2) {
            const Connection& c1 = pair.connections[0];
            const Connection& c2 = pair.connections[1];
            map.external[c1.polygon->owner][c1.polygon->id].push_back(c2);
            map.external[c2.polygon->owner][c2.polygon->id].push_back(c1);
            ++map.edge_connection_count;
        } else if (build.use_edge_connections &&
                   pair.connections[0].polygon->owner->use_edge_connections) {
            free_edges.push_back(pair.connections[0]);
        }
    }

    // _build_step_edge_connection_margin_connections: free edges of different regions close
    // enough to each other connect, through a pathway between them.
    map.edge_free_count = static_cast<int>(free_edges.size());
    const float margin_squared = build.edge_connection_margin * build.edge_connection_margin;
    for (std::size_t i = 0; i < free_edges.size(); ++i) {
        const Connection& free_edge = free_edges[i];
        const Vec3 edge_p1 = free_edge.pathway_start;
        const Vec3 edge_p2 = free_edge.pathway_end;
        for (std::size_t j = 0; j < free_edges.size(); ++j) {
            const Connection& other_edge = free_edges[j];
            if (i == j || free_edge.polygon->owner == other_edge.polygon->owner) {
                continue;
            }
            const Vec3 other_edge_p1 = other_edge.pathway_start;
            const Vec3 other_edge_p2 = other_edge.pathway_end;

            // The other edge projected on this one.
            const Vec3 edge_vector = edge_p2 - edge_p1;
            const float projected_p1_ratio =
                dot(edge_vector, other_edge_p1 - edge_p1) / length_squared(edge_vector);
            const float projected_p2_ratio =
                dot(edge_vector, other_edge_p2 - edge_p1) / length_squared(edge_vector);
            if ((projected_p1_ratio < 0.0F && projected_p2_ratio < 0.0F) ||
                (projected_p1_ratio > 1.0F && projected_p2_ratio > 1.0F)) {
                continue;
            }

            // Godot divides in double precision here, its 1.0 and 0.0 being doubles.
            const Vec3 self1 = (edge_vector * std::clamp(projected_p1_ratio, 0.0F, 1.0F)) + edge_p1;
            Vec3 other1;
            if (projected_p1_ratio >= 0.0F && projected_p1_ratio <= 1.0F) {
                other1 = other_edge_p1;
            } else {
                other1 = lerp(other_edge_p1, other_edge_p2,
                              static_cast<float>(
                                  (1.0 - static_cast<double>(projected_p1_ratio)) /
                                  static_cast<double>(projected_p2_ratio - projected_p1_ratio)));
            }
            if (distance_squared(self1, other1) > margin_squared) {
                continue;
            }

            const Vec3 self2 = (edge_vector * std::clamp(projected_p2_ratio, 0.0F, 1.0F)) + edge_p1;
            Vec3 other2;
            if (projected_p2_ratio >= 0.0F && projected_p2_ratio <= 1.0F) {
                other2 = other_edge_p2;
            } else {
                other2 = lerp(other_edge_p1, other_edge_p2,
                              static_cast<float>(
                                  (0.0 - static_cast<double>(projected_p1_ratio)) /
                                  static_cast<double>(projected_p2_ratio - projected_p1_ratio)));
            }
            if (distance_squared(self2, other2) > margin_squared) {
                continue;
            }

            Connection connection = other_edge;
            connection.pathway_start = divided(self1 + other1, 2.0F);
            connection.pathway_end = divided(self2 + other2, 2.0F);
            map.external[free_edge.polygon->owner][free_edge.polygon->id].push_back(connection);
            ++map.edge_connection_count;
        }
    }

    // _build_step_navlink_connections: each link becomes a thin polygon from the closest point
    // within reach of its start to the closest within reach of its end.
    const float radius = build.link_connection_radius;
    const float radius_squared = radius * radius;
    map.link_polygons.resize(map.links.size());
    std::size_t link_index = 0;
    for (const auto& link : map.links) {
        ++polygon_count;
        Polygon& link_polygon = map.link_polygons[link_index++];
        link_polygon.id = 0;
        link_polygon.owner = link.get();

        const Vec3 start = link->start_position;
        const Vec3 end = link->end_position;
        const Polygon* closest_start_polygon = nullptr;
        float closest_start_squared = radius_squared;
        Vec3 closest_start_point;
        const Polygon* closest_end_polygon = nullptr;
        float closest_end_squared = radius_squared;
        Vec3 closest_end_point;

        for (const auto& region : map.regions) {
            const Aabb bounds = region->bounds.grown(radius);
            if (!bounds.has_point(start) && !bounds.has_point(end)) {
                continue;
            }
            for (const Polygon& polygon : region->polygons) {
                for (std::size_t point = 2; point < polygon.vertices.size(); ++point) {
                    const Vec3 v0 = polygon.vertices[0];
                    const Vec3 v1 = polygon.vertices[point - 1];
                    const Vec3 v2 = polygon.vertices[point];
                    const Vec3 start_point = closest_point_on_face(v0, v1, v2, start);
                    if (const float squared = distance_squared(start_point, start);
                        squared < closest_start_squared) {
                        closest_start_squared = squared;
                        closest_start_point = start_point;
                        closest_start_polygon = &polygon;
                    }
                    const Vec3 end_point = closest_point_on_face(v0, v1, v2, end);
                    if (const float squared = distance_squared(end_point, end);
                        squared < closest_end_squared) {
                        closest_end_squared = squared;
                        closest_end_point = end_point;
                        closest_end_polygon = &polygon;
                    }
                }
            }
        }

        if (closest_start_polygon == nullptr || closest_end_polygon == nullptr) {
            continue;
        }
        link_polygon.vertices = {closest_start_point, closest_start_point, closest_end_point,
                                 closest_end_point};
        std::vector<std::vector<Connection>>& exits = map.external[link.get()];
        // Forward through the link.
        map.external[closest_start_polygon->owner][closest_start_polygon->id].push_back(
            {.polygon = &link_polygon,
             .edge = -1,
             .pathway_start = link_polygon.vertices[0],
             .pathway_end = link_polygon.vertices[1]});
        exits.emplace_back();
        exits[0].push_back({.polygon = closest_end_polygon,
                            .edge = -1,
                            .pathway_start = link_polygon.vertices[2],
                            .pathway_end = link_polygon.vertices[3]});
        // And back, for a bidirectional link.
        if (link->bidirectional) {
            map.external[closest_end_polygon->owner][closest_end_polygon->id].push_back(
                {.polygon = &link_polygon,
                 .edge = -1,
                 .pathway_start = link_polygon.vertices[2],
                 .pathway_end = link_polygon.vertices[3]});
            exits.emplace_back();
            exits[0].push_back({.polygon = closest_start_polygon,
                                .edge = -1,
                                .pathway_start = link_polygon.vertices[0],
                                .pathway_end = link_polygon.vertices[1]});
        }
    }
    map.polygon_count = polygon_count;

    // _build_update_map_iteration: every polygon's place in a search.
    map.path_corridor.assign(static_cast<std::size_t>(polygon_count), NavigationPoly{});
    map.traversable_polys.clear();
    std::uint32_t polygon_id = 0;
    for (const auto& region : map.regions) {
        for (const Polygon& polygon : region->polygons) {
            map.poly_to_id[&polygon] = polygon_id++;
        }
    }
    for (const Polygon& polygon : map.link_polygons) {
        map.poly_to_id[&polygon] = polygon_id++;
    }
}

// ---- Queries.

namespace {

bool owner_usable(const PathTask& task, const Base* owner) {
    if (!owner->enabled || (task.navigation_layers & owner->navigation_layers) == 0) {
        return false;
    }
    const bool exclude = !task.excluded_regions.empty();
    const bool include = !task.included_regions.empty();
    if (!exclude && !include) {
        return true;
    }
    if (owner->type == OwnerType::Link) {
        // Godot looks for the regions a link joins among the link's own polygons, which it never
        // has, so a link is never usable while regions are excluded or included.
        return false;
    }
    if (exclude &&
        std::ranges::find(task.excluded_regions, owner->owner) != task.excluded_regions.end()) {
        return false;
    }
    return !include ||
           std::ranges::find(task.included_regions, owner->owner) != task.included_regions.end();
}

void push_point(PathTask& task, Vec3 point, const Polygon* polygon) {
    task.path_types.push_back(polygon->owner->type);
    task.path_owners.push_back(polygon->owner->owner);
    task.path_points.push_back(point);
}

void clear_path(PathTask& task) {
    task.path_points.clear();
    task.path_types.clear();
    task.path_owners.clear();
}

void reverse_path(PathTask& task) {
    std::ranges::reverse(task.path_points);
    std::ranges::reverse(task.path_types);
    std::ranges::reverse(task.path_owners);
}

std::uint32_t id_of(const Map& map, const Polygon* polygon) {
    return map.poly_to_id.at(polygon);
}

// _query_task_find_start_end_positions.
void find_start_end_positions(const Map& map, PathTask& task) {
    float begin_d = kFloatMax;
    float end_d = kFloatMax;
    for (const auto& region : map.regions) {
        if (!owner_usable(task, region.get())) {
            continue;
        }
        for (const Polygon& p : region->polygons) {
            if ((task.navigation_layers & p.owner->navigation_layers) == 0) {
                continue;
            }
            for (std::size_t point = 2; point < p.vertices.size(); ++point) {
                const Vec3 v0 = p.vertices[0];
                const Vec3 v1 = p.vertices[point - 1];
                const Vec3 v2 = p.vertices[point];
                Vec3 closest = closest_point_on_face(v0, v1, v2, task.start_position);
                float distance_to_point = distance(closest, task.start_position);
                if (distance_to_point < begin_d) {
                    begin_d = distance_to_point;
                    task.begin_polygon = &p;
                    task.begin_position = closest;
                }
                closest = closest_point_on_face(v0, v1, v2, task.target_position);
                distance_to_point = distance(closest, task.target_position);
                if (distance_to_point < end_d) {
                    end_d = distance_to_point;
                    task.end_polygon = &p;
                    task.end_position = closest;
                }
            }
        }
    }
}

// _query_task_search_polygon_connections.
void search_connection(const Map& map, PathTask& task, const Connection& connection,
                       std::uint32_t least_cost_id, const NavigationPoly& least_cost_poly,
                       float poly_enter_cost, Vec3 end_point) {
    const Base* connection_owner = connection.polygon->owner;
    if (!owner_usable(task, connection_owner)) {
        return;
    }
    const float poly_travel_cost = least_cost_poly.poly->owner->travel_cost;
    const Vec3 new_entry = closest_point_to_segment(least_cost_poly.entry, connection.pathway_start,
                                                    connection.pathway_end);
    const float new_traveled_distance =
        (distance(least_cost_poly.entry, new_entry) * poly_travel_cost) + poly_enter_cost +
        least_cost_poly.traveled_distance;

    NavigationPoly& neighbor_poly = map.path_corridor[id_of(map, connection.polygon)];
    if (new_traveled_distance < neighbor_poly.traveled_distance) {
        neighbor_poly.back_navigation_poly_id = static_cast<int>(least_cost_id);
        neighbor_poly.back_navigation_edge = connection.edge;
        neighbor_poly.back_navigation_edge_pathway_start = connection.pathway_start;
        neighbor_poly.back_navigation_edge_pathway_end = connection.pathway_end;
        neighbor_poly.traveled_distance = new_traveled_distance;
        neighbor_poly.distance_to_destination =
            distance(new_entry, end_point) * connection_owner->travel_cost;
        neighbor_poly.entry = new_entry;
        if (neighbor_poly.traversable_poly_index != Heap::kInvalidIndex) {
            map.traversable_polys.shift(neighbor_poly.traversable_poly_index);
        } else {
            neighbor_poly.poly = connection.polygon;
            map.traversable_polys.push(&neighbor_poly);
        }
    }
}

// The point of a polygon's faces closest to target, if closer than best_squared.
void closest_on_polygon(const Polygon& polygon, Vec3 target, Vec3& point, float& best_squared,
                        bool* found = nullptr) {
    for (std::size_t i = 2; i < polygon.vertices.size(); ++i) {
        const Vec3 candidate = closest_point_on_face(polygon.vertices[0], polygon.vertices[i - 1],
                                                     polygon.vertices[i], target);
        const float squared = distance_squared(candidate, target);
        if (squared < best_squared) {
            point = candidate;
            best_squared = squared;
            if (found != nullptr) {
                *found = true;
            }
        }
    }
}

// _query_task_build_path_corridor: A* from the begin polygon to the end polygon, or, if it
// can't be reached, to the reachable polygon closest to the target.
void build_path_corridor(const Map& map, PathTask& task) {
    const Vec3 target_position = task.target_position;
    const Polygon* begin_poly = task.begin_polygon;
    const Polygon* end_poly = task.end_polygon;
    const Vec3 begin_point = task.begin_position;
    Vec3 end_point = task.end_position;

    Heap& traversable_polys = map.traversable_polys;
    traversable_polys.clear();
    std::vector<NavigationPoly>& navigation_polys = map.path_corridor;
    for (NavigationPoly& polygon : navigation_polys) {
        polygon.reset();
    }

    const std::uint32_t begin_id = id_of(map, begin_poly);
    NavigationPoly& begin_navigation_poly = navigation_polys[begin_id];
    begin_navigation_poly.poly = begin_poly;
    begin_navigation_poly.entry = begin_point;
    begin_navigation_poly.back_navigation_edge_pathway_start = begin_point;
    begin_navigation_poly.back_navigation_edge_pathway_end = begin_point;
    begin_navigation_poly.traveled_distance = 0.0F;

    std::uint32_t least_cost_id = begin_id;
    bool found_route = false;
    const Polygon* reachable_end = nullptr;
    float distance_to_reachable_end = kFloatMax;
    bool is_reachable = true;
    float poly_enter_cost = 0.0F;

    bool path_search_max_reached = false;
    const float max_distance_squared =
        task.path_search_max_distance * task.path_search_max_distance;
    const bool has_max_distance = max_distance_squared > 0.0F;
    int processed_polygon_count = 0;
    const bool has_max_polygons = task.path_search_max_polygons > 0;
    const bool has_max = has_max_polygons || has_max_distance;

    while (true) {
        const NavigationPoly& least_cost_poly = navigation_polys[least_cost_id];
        const Base* least_cost_navbase = least_cost_poly.poly->owner;
        ++processed_polygon_count;

        const std::uint32_t local_id = least_cost_poly.poly->id;
        if (!least_cost_navbase->internal_connections.empty()) {
            for (const Connection& connection :
                 least_cost_navbase->internal_connections[local_id]) {
                search_connection(map, task, connection, least_cost_id, least_cost_poly,
                                  poly_enter_cost, end_point);
            }
        }
        // Connections to other regions by shared and nearby edges, and links.
        if (const auto external = map.external.find(least_cost_navbase);
            external != map.external.end() && local_id < external->second.size()) {
            for (const Connection& connection : external->second[local_id]) {
                search_connection(map, task, connection, least_cost_id, least_cost_poly,
                                  poly_enter_cost, end_point);
            }
        }

        if (has_max && !path_search_max_reached &&
            ((has_max_polygons && processed_polygon_count >= task.path_search_max_polygons) ||
             (has_max_distance &&
              distance_squared(least_cost_poly.entry, begin_point) > max_distance_squared))) {
            path_search_max_reached = true;
            traversable_polys.clear();
        }

        poly_enter_cost = 0.0F;
        if (traversable_polys.empty()) {
            // The end polygon can't be reached: aim for the reachable one closest to the target.
            if (!is_reachable) {
                break;
            }
            is_reachable = false;
            if (reachable_end == nullptr) {
                break;
            }
            end_poly = reachable_end;
            float end_d = kFloatMax;
            closest_on_polygon(*end_poly, target_position, end_point, end_d);
            bool closest_point_on_start_poly = false;
            closest_on_polygon(*begin_poly, target_position, end_point, end_d,
                               &closest_point_on_start_poly);
            if (closest_point_on_start_poly) {
                // No point post-processing a path within one convex polygon.
                clear_path(task);
                push_point(task, begin_point, begin_poly);
                push_point(task, end_point, begin_poly);
                task.finished = true;
                return;
            }
            for (NavigationPoly& nav_poly : navigation_polys) {
                nav_poly.poly = nullptr;
                nav_poly.traveled_distance = kFloatMax;
            }
            navigation_polys[begin_id].poly = begin_poly;
            navigation_polys[begin_id].traveled_distance = 0.0F;
            least_cost_id = begin_id;
            reachable_end = nullptr;
        } else {
            least_cost_id = id_of(map, traversable_polys.pop()->poly);
            if (is_reachable) {
                const float distance_squared_to_target =
                    distance_squared(navigation_polys[least_cost_id].entry, target_position);
                if (distance_to_reachable_end > distance_squared_to_target) {
                    distance_to_reachable_end = distance_squared_to_target;
                    reachable_end = navigation_polys[least_cost_id].poly;
                }
            }
            if (navigation_polys[least_cost_id].poly == end_poly) {
                found_route = true;
                break;
            }
            if (navigation_polys[least_cost_id].poly->owner->owner !=
                least_cost_poly.poly->owner->owner) {
                poly_enter_cost = least_cost_poly.poly->owner->enter_cost;
            }
        }
    }

    if (!found_route) {
        // Usually the begin polygon is a single polygon with no connected edges.
        float end_d = kFloatMax;
        closest_on_polygon(*begin_poly, target_position, end_point, end_d);
        clear_path(task);
        push_point(task, begin_point, begin_poly);
        push_point(task, end_point, begin_poly);
        task.finished = true;
        return;
    }
    task.end_position = end_point;
    task.end_polygon = end_poly;
    task.begin_position = begin_point;
    task.begin_polygon = begin_poly;
    task.least_cost_id = least_cost_id;
}

// _query_task_clip_path: where a straight line from the last point to to_point crosses the
// pathways between from_poly and to_poly.
void clip_path(const Map& map, PathTask& task, const NavigationPoly* from_poly, Vec3 to_point,
               const NavigationPoly* to_poly) {
    const Vec3 from = task.path_points.back();
    if (is_equal_approx(from, to_point)) {
        return;
    }
    Vec3 normal = cross(from - to_point, map.up);
    if (normal == Vec3{}) {
        return;
    }
    normal = normalized(normal);
    const float d = dot(normal, from);

    while (from_poly != to_poly) {
        const Vec3 pathway_start = from_poly->back_navigation_edge_pathway_start;
        const Vec3 pathway_end = from_poly->back_navigation_edge_pathway_end;
        if (from_poly->back_navigation_poly_id == -1) {
            return;
        }
        from_poly =
            &map.path_corridor[static_cast<std::size_t>(from_poly->back_navigation_poly_id)];
        if (!is_equal_approx(pathway_start, pathway_end)) {
            Vec3 inters;
            if (plane_intersects_segment(normal, d, pathway_start, pathway_end, inters) &&
                !is_equal_approx(inters, to_point) &&
                !is_equal_approx(inters, task.path_points.back())) {
                push_point(task, inters, from_poly->poly);
            }
        }
    }
}

// _query_task_post_process_corridorfunnel: the shortest path through the corridor's pathways,
// built from the end.
void post_process_corridor_funnel(const Map& map, PathTask& task) {
    const Vec3 end_point = task.end_position;
    const Vec3 begin_point = task.begin_position;
    std::vector<NavigationPoly>& navigation_polys = map.path_corridor;
    const Vec3 up = map.up;

    NavigationPoly* apex_poly = &navigation_polys[task.least_cost_id];
    const Vec3 back_edge_closest_point =
        closest_point_to_segment(end_point, apex_poly->back_navigation_edge_pathway_start,
                                 apex_poly->back_navigation_edge_pathway_end);
    if (is_equal_approx(end_point, back_edge_closest_point)) {
        // The end point is on the last pathway: funneling would at best do nothing.
        if (apex_poly->back_navigation_poly_id != -1) {
            apex_poly =
                &navigation_polys[static_cast<std::size_t>(apex_poly->back_navigation_poly_id)];
        }
    }

    Vec3 apex_point = end_point;
    NavigationPoly* left_poly = apex_poly;
    Vec3 left_portal = apex_point;
    NavigationPoly* right_poly = apex_poly;
    Vec3 right_portal = apex_point;
    NavigationPoly* p = apex_poly;

    push_point(task, end_point, task.end_polygon);

    while (p != nullptr) {
        Vec3 left = p->back_navigation_edge_pathway_start;
        Vec3 right = p->back_navigation_edge_pathway_end;
        if (dot(three_points_cross(apex_point, left, right), up) < 0.0F) {
            std::swap(left, right);
        }

        bool skip = false;
        if (dot(three_points_cross(apex_point, left_portal, left), up) >= 0.0F) {
            if (left_portal == apex_point ||
                dot(three_points_cross(apex_point, left, right_portal), up) > 0.0F) {
                left_poly = p;
                left_portal = left;
            } else {
                clip_path(map, task, apex_poly, right_portal, right_poly);
                apex_point = right_portal;
                p = right_poly;
                left_poly = p;
                apex_poly = p;
                left_portal = apex_point;
                right_portal = apex_point;
                push_point(task, apex_point, apex_poly->poly);
                skip = true;
            }
        }

        if (!skip && dot(three_points_cross(apex_point, right_portal, right), up) <= 0.0F) {
            if (right_portal == apex_point ||
                dot(three_points_cross(apex_point, right, left_portal), up) < 0.0F) {
                right_poly = p;
                right_portal = right;
            } else {
                clip_path(map, task, apex_poly, left_portal, left_poly);
                apex_point = left_portal;
                p = left_poly;
                right_poly = p;
                apex_poly = p;
                right_portal = apex_point;
                left_portal = apex_point;
                push_point(task, apex_point, apex_poly->poly);
            }
        }

        if (p->back_navigation_poly_id != -1) {
            p = &navigation_polys[static_cast<std::size_t>(p->back_navigation_poly_id)];
        } else {
            p = nullptr;
        }
    }

    if (task.path_points.back() != begin_point) {
        push_point(task, begin_point, task.begin_polygon);
    }
}

// _query_task_post_process_edgecentered: through the middle of each edge crossed.
void post_process_edge_centered(const Map& map, PathTask& task) {
    const std::vector<NavigationPoly>& navigation_polys = map.path_corridor;
    push_point(task, task.end_position, task.end_polygon);
    int np_id = static_cast<int>(task.least_cost_id);
    while (np_id != -1 &&
           navigation_polys[static_cast<std::size_t>(np_id)].back_navigation_poly_id != -1) {
        const NavigationPoly& np = navigation_polys[static_cast<std::size_t>(np_id)];
        if (np.back_navigation_edge != -1) {
            const auto prev = static_cast<std::size_t>(np.back_navigation_edge);
            const std::size_t prev_n = (prev + 1) % np.poly->vertices.size();
            const Vec3 point = (np.poly->vertices[prev] + np.poly->vertices[prev_n]) * 0.5F;
            push_point(task, point, np.poly);
        } else {
            push_point(task, np.entry, np.poly);
        }
        np_id = np.back_navigation_poly_id;
    }
    push_point(task, task.begin_position, task.begin_polygon);
}

// _query_task_post_process_nopostprocessing: through where the search entered each polygon.
void post_process_none(const Map& map, PathTask& task) {
    const std::vector<NavigationPoly>& navigation_polys = map.path_corridor;
    push_point(task, task.end_position, task.end_polygon);
    int np_id = static_cast<int>(task.least_cost_id);
    while (np_id != -1 &&
           navigation_polys[static_cast<std::size_t>(np_id)].back_navigation_poly_id != -1) {
        const NavigationPoly& np = navigation_polys[static_cast<std::size_t>(np_id)];
        push_point(task, np.entry, np.poly);
        np_id = np.back_navigation_poly_id;
    }
    push_point(task, task.begin_position, task.begin_polygon);
}

// simplify_path_segment: Ramer-Douglas-Peucker.
void simplify_segment(std::size_t start, std::size_t end, const std::vector<Vec3>& points,
                      float epsilon_squared, std::vector<std::size_t>& indices) {
    const Vec3 a = points[start];
    const Vec3 b = points[end];
    float max_distance = 0.0F;
    std::size_t max_index = 0;
    for (std::size_t i = start; i < end; ++i) {
        const Vec3 closest = closest_point_to_segment(points[i], a, b);
        const float squared = distance_squared(closest, points[i]);
        if (squared > max_distance) {
            max_index = i;
            max_distance = squared;
        }
    }
    if (max_distance > epsilon_squared) {
        simplify_segment(start, max_index, points, epsilon_squared, indices);
        indices.push_back(max_index);
        simplify_segment(max_index, end, points, epsilon_squared, indices);
    }
}

// _query_task_simplified_path_points.
void simplify_path(PathTask& task) {
    if (!task.simplify_path || task.path_points.size() <= 2) {
        return;
    }
    const float epsilon = std::max(0.0F, task.simplify_epsilon);
    std::vector<std::size_t> indices;
    indices.reserve(task.path_points.size());
    indices.push_back(0);
    simplify_segment(0, task.path_points.size() - 1, task.path_points, epsilon * epsilon, indices);
    indices.push_back(task.path_points.size() - 1);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        task.path_points[i] = task.path_points[indices[i]];
        task.path_types[i] = task.path_types[indices[i]];
        task.path_owners[i] = task.path_owners[indices[i]];
    }
    task.path_points.resize(indices.size());
    task.path_types.resize(indices.size());
    task.path_owners.resize(indices.size());
}

// _query_task_process_path_result_limits: the path's length, cut short at
// path_return_max_length or where it leaves path_return_max_radius around its start.
void process_path_result_limits(PathTask& task) {
    std::vector<Vec3>& path = task.path_points;
    if (path.size() < 2) {
        return;
    }
    const bool check_max_length = task.path_return_max_length > 0.0F;
    const bool check_max_radius = task.path_return_max_radius > 0.0F;
    if (!check_max_length && !check_max_radius) {
        float path_length = 0.0F;
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            path_length += distance(path[i], path[i + 1]);
        }
        task.path_length = path_length;
        return;
    }

    const float max_length = task.path_return_max_length;
    const float max_radius = task.path_return_max_radius;
    const float max_radius_squared = max_radius * max_radius;
    const Vec3 start = path[0];
    float accumulated = 0.0F;
    std::size_t path_max_size = path.size();
    bool path_max_reached = false;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const std::size_t next = i + 1;
        const Vec3 vertex1 = path[i];
        float edge_length = length(path[next] - vertex1);
        if (check_max_radius && distance_squared(start, path[next]) > max_radius_squared) {
            Vec3 intersection;
            if (segment_intersects_sphere(path[next], vertex1, start, max_radius, intersection)) {
                edge_length = length(intersection - vertex1);
                path[next] = intersection;
                path_max_size = next + 1;
                path_max_reached = true;
            }
        }
        if (check_max_length && accumulated + edge_length > max_length) {
            edge_length = max_length - accumulated;
            const Vec3 direction = normalized(path[next] - vertex1);
            path[next] = vertex1 + (direction * edge_length);
            path_max_size = next + 1;
            path_max_reached = true;
        }
        accumulated += edge_length;
        if (path_max_reached) {
            break;
        }
    }
    task.path_length = accumulated;
    if (path_max_size < path.size()) {
        path.resize(path_max_size);
        task.path_types.resize(path_max_size);
        task.path_owners.resize(path_max_size);
    }
}

} // namespace

void query_path(const Map& map, PathTask& task) {
    clear_path(task);
    task.path_length = 0.0F;
    task.finished = false;
    task.begin_polygon = nullptr;
    task.end_polygon = nullptr;
    find_start_end_positions(map, task);
    if (task.begin_polygon == nullptr || task.end_polygon == nullptr) {
        task.finished = true;
        return;
    }
    if (task.begin_polygon == task.end_polygon) {
        push_point(task, task.begin_position, task.begin_polygon);
        push_point(task, task.end_position, task.end_polygon);
        process_path_result_limits(task);
        task.finished = true;
        return;
    }

    build_path_corridor(map, task);
    if (task.finished) {
        process_path_result_limits(task);
        return;
    }
    switch (task.path_postprocessing) {
    case PostProcessing::CorridorFunnel:
        post_process_corridor_funnel(map, task);
        break;
    case PostProcessing::EdgeCentered:
        post_process_edge_centered(map, task);
        break;
    case PostProcessing::None:
        post_process_none(map, task);
        break;
    }
    reverse_path(task);
    simplify_path(task);
    process_path_result_limits(task);
    task.finished = true;
}

ClosestPoint closest_point(const Map& map, Vec3 point) {
    ClosestPoint result;
    float closest_squared = kFloatMax;
    for (const auto& region : map.regions) {
        for (const Polygon& polygon : region->polygons) {
            if (polygon.vertices.size() < 3) {
                continue;
            }
            const Vec3 plane_normal = cross(polygon.vertices[1] - polygon.vertices[0],
                                            polygon.vertices[2] - polygon.vertices[0]);
            Vec3 closest_on_polygon;
            float closest = kFloatMax;
            bool inside = true;
            Vec3 previous = polygon.vertices.back();
            for (const Vec3 vertex : polygon.vertices) {
                const Vec3 edge = vertex - previous;
                const Vec3 to_point = point - previous;
                const bool clockwise = dot(cross(edge, to_point), plane_normal) > 0.0F;
                // Outside one edge, the closest point is on an edge.
                if (!clockwise) {
                    inside = false;
                    const float projected = dot(edge, to_point);
                    const float edge_square = length_squared(edge);
                    if (projected > edge_square) {
                        if (const float squared = distance_squared(vertex, point);
                            squared < closest) {
                            closest_on_polygon = vertex;
                            closest = squared;
                        }
                    } else if (projected < 0.0F) {
                        if (const float squared = distance_squared(previous, point);
                            squared < closest) {
                            closest_on_polygon = previous;
                            closest = squared;
                        }
                    } else {
                        closest_on_polygon = previous + (edge * (projected / edge_square));
                        break;
                    }
                }
                previous = vertex;
            }

            if (inside) {
                const Vec3 unit_normal = normalized(plane_normal);
                const float plane_distance = dot(unit_normal, point - polygon.vertices[0]);
                const float squared = plane_distance * plane_distance;
                if (squared < closest_squared) {
                    closest_squared = squared;
                    result.point = point - (unit_normal * plane_distance);
                    result.normal = plane_normal;
                    result.owner = polygon.owner->owner;
                    if (is_zero_approx(plane_distance)) {
                        break;
                    }
                }
            } else if (const float squared = distance_squared(closest_on_polygon, point);
                       squared < closest_squared) {
                closest_squared = squared;
                result.point = closest_on_polygon;
                result.normal = plane_normal;
                result.owner = polygon.owner->owner;
            }
        }
    }
    return result;
}

namespace {

// RBMap::find_closest over a map of accumulated areas: the entry at or before position.
std::uint32_t find_closest(const std::map<float, std::uint32_t>& areas, float position) {
    auto found = areas.upper_bound(position);
    if (found != areas.begin()) {
        --found;
    }
    return found->second;
}

// NavMeshQueries3D::polygons_get_random_point.
Vec3 polygons_random_point(const std::vector<Polygon>& polygons, bool uniformly, Random& random) {
    if (polygons.empty()) {
        return {};
    }
    const Polygon* polygon = nullptr;
    std::size_t face = 0;
    if (uniformly) {
        float accumulated = 0.0F;
        std::map<float, std::uint32_t> areas;
        for (std::size_t i = 0; i < polygons.size(); ++i) {
            if (polygons[i].surface_area == 0.0F) {
                continue;
            }
            areas[accumulated] = static_cast<std::uint32_t>(i);
            accumulated += polygons[i].surface_area;
        }
        if (areas.empty() || accumulated == 0.0F) {
            return {};
        }
        polygon = &polygons[find_closest(areas, random.unit() * accumulated)];

        float accumulated_faces = 0.0F;
        std::map<float, std::uint32_t> faces;
        for (std::size_t i = 2; i < polygon->vertices.size(); ++i) {
            const float area =
                face_area(polygon->vertices[0], polygon->vertices[i - 1], polygon->vertices[i]);
            if (area == 0.0F) {
                continue;
            }
            faces[accumulated_faces] = static_cast<std::uint32_t>(i);
            accumulated_faces += area;
        }
        if (faces.empty() || accumulated_faces == 0.0F) {
            return {};
        }
        face = find_closest(faces, random.unit() * accumulated_faces);
    } else {
        polygon = &polygons[static_cast<std::size_t>(
            random.between(0, static_cast<int>(polygons.size()) - 1))];
        if (polygon->vertices.size() < 3) {
            return {};
        }
        face = static_cast<std::size_t>(
            random.between(2, static_cast<int>(polygon->vertices.size()) - 1));
    }
    // Face3::get_random_point_inside.
    float a = random.unit();
    float b = random.unit();
    if (a > b) {
        std::swap(a, b);
    }
    return (polygon->vertices[0] * a) + (polygon->vertices[face - 1] * (b - a)) +
           (polygon->vertices[face] * (1.0F - b));
}

} // namespace

Vec3 random_point(const Map& map, std::uint32_t navigation_layers, bool uniformly, Random& random) {
    std::vector<const Region*> accessible;
    for (const auto& region : map.regions) {
        if (region->enabled && (navigation_layers & region->navigation_layers) != 0) {
            accessible.push_back(region.get());
        }
    }
    if (accessible.empty()) {
        return {};
    }
    if (uniformly) {
        float accumulated = 0.0F;
        std::map<float, std::uint32_t> areas;
        for (std::size_t i = 0; i < accessible.size(); ++i) {
            if (accessible[i]->surface_area == 0.0F) {
                continue;
            }
            areas[accumulated] = static_cast<std::uint32_t>(i);
            accumulated += accessible[i]->surface_area;
        }
        if (areas.empty() || accumulated == 0.0F) {
            return {};
        }
        const Region* region = accessible[find_closest(areas, random.unit() * accumulated)];
        return polygons_random_point(region->polygons, uniformly, random);
    }
    const Region* region = accessible[static_cast<std::size_t>(
        random.between(0, static_cast<int>(accessible.size()) - 1))];
    return polygons_random_point(region->polygons, uniformly, random);
}

} // namespace slash::navigation::detail
