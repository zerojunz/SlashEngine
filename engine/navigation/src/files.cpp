#include "slash/navigation/navigation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::navigation {

namespace {

json::Value number_property(std::string_view description, double fallback, double minimum) {
    json::Value out;
    out["type"] = "number";
    out["exclusiveMinimum"] = minimum;
    out["default"] = fallback;
    out["description"] = description;
    return out;
}

// A setting of zero or more.
json::Value amount_property(std::string_view description, double fallback) {
    json::Value out;
    out["type"] = "number";
    out["minimum"] = 0;
    out["default"] = fallback;
    out["description"] = description;
    return out;
}

json::Value flag_property(std::string_view description) {
    json::Value out;
    out["type"] = "boolean";
    out["default"] = false;
    out["description"] = description;
    return out;
}

json::Value mask_property(std::string_view description) {
    json::Value out;
    out["type"] = "integer";
    out["minimum"] = 0;
    out["maximum"] = 4294967295LL;
    out["default"] = 4294967295LL;
    out["description"] = description;
    return out;
}

json::Value names_property(json::Array names, std::string_view fallback,
                           std::string_view description) {
    json::Value out;
    out["default"] = fallback;
    out["enum"] = std::move(names);
    out["description"] = description;
    return out;
}

json::Value point_schema(bool three_d) {
    json::Value point;
    point["type"] = "array";
    point["items"]["type"] = "number";
    point["minItems"] = three_d ? 3 : 2;
    point["maxItems"] = three_d ? 3 : 2;
    return point;
}

// A box or rect as {"position": ..., "size": ...}.
json::Value area_property(bool three_d, std::string_view description) {
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = json::Array{"position", "size"};
    out["properties"]["position"] = point_schema(three_d);
    out["properties"]["size"] = point_schema(three_d);
    out["description"] = description;
    return out;
}

constexpr std::array<std::string_view, 3> kParsedGeometryNames = {"mesh_instances",
                                                                  "static_colliders", "both"};
constexpr std::array<std::string_view, 3> kPartition3DNames = {"watershed", "monotone", "layers"};
constexpr std::array<std::string_view, 2> kPartition2DNames = {"convex_partition", "triangulate"};

template <std::size_t N>
json::Array names_of(const std::array<std::string_view, N>& names) {
    json::Array out;
    for (const std::string_view name : names) {
        out.emplace_back(name);
    }
    return out;
}

void add_bake_properties(json::Value& properties, bool three_d) {
    if (three_d) {
        properties["sample_partition_type"] = names_property(
            names_of(kPartition3DNames), "watershed", "How Recast splits the walkable area.");
        properties["geometry_parsed_geometry_type"] =
            names_property(names_of(kParsedGeometryNames), "both",
                           "What under the region bakes: MeshInstance3D meshes, static bodies' "
                           "shapes, or both.");
        properties["geometry_collision_mask"] =
            mask_property("Static bodies on these physics layers bake.");
        properties["border_size"] =
            amount_property("Meters kept clear at the edge of filter_baking_aabb.", 0.0);
        properties["agent_height"] = amount_property("How tall agents are.", 1.5);
        properties["agent_radius"] = amount_property("How far agents keep from walls.", 0.5);
        properties["agent_max_climb"] = amount_property("The tallest step agents climb.", 0.25);
        properties["agent_max_slope"] =
            amount_property("The steepest slope agents walk, in degrees.", 45.0);
        properties["agent_max_slope"]["maximum"] = 90;
        properties["region_min_size"] =
            amount_property("Islands smaller than this many cells squared are dropped.", 2.0);
        properties["region_merge_size"] = amount_property(
            "Areas smaller than this many cells squared merge into their neighbors.", 20.0);
        properties["edge_max_length"] =
            amount_property("The longest an outline's edge may be; 0 for any.", 0.0);
        properties["edge_max_error"] = amount_property(
            "How far, in cells, simplified outlines may stray from the walkable area.", 1.3);
        properties["vertices_per_polygon"] =
            amount_property("The most corners a polygon may have.", 6.0);
        properties["vertices_per_polygon"]["minimum"] = 3;
        properties["detail_sample_distance"] = amount_property(
            "How far apart, in cells, heights are sampled for the detail mesh.", 6.0);
        properties["detail_sample_max_error"] = amount_property(
            "How far, in cells of cell_height, the detail mesh may stray from the ground.", 1.0);
        properties["filter_low_hanging_obstacles"] =
            flag_property("Walk over obstacles no taller than agent_max_climb.");
        properties["filter_ledge_spans"] = flag_property("Leave out ledges agents would fall off.");
        properties["filter_walkable_low_height_spans"] =
            flag_property("Leave out places lower than agent_height.");
        properties["filter_baking_aabb"] =
            area_property(true, "Bake only what is in this box, when its size has volume.");
        properties["filter_baking_aabb_offset"] = point_schema(true);
        properties["filter_baking_aabb_offset"]["description"] = "Moves filter_baking_aabb.";
        return;
    }
    json::Value outline;
    outline["type"] = "array";
    outline["items"] = point_schema(false);
    outline["minItems"] = 3;
    properties["outlines"]["type"] = "array";
    properties["outlines"]["items"] = std::move(outline);
    properties["outlines"]["description"] =
        "Closed outlines of where agents can go, in pixels, which baking merges.";
    properties["sample_partition_type"] =
        names_property(names_of(kPartition2DNames), "convex_partition",
                       "Whether baking makes convex polygons or triangles.");
    properties["parsed_geometry_type"] =
        names_property(names_of(kParsedGeometryNames), "both",
                       "What under the region bakes; static_colliders and both take static "
                       "bodies' shapes as obstructions.");
    properties["parsed_collision_mask"] =
        mask_property("Static bodies on these physics layers bake.");
    properties["border_size"] =
        amount_property("Pixels kept clear at the edge of baking_rect.", 0.0);
    properties["agent_radius"] = amount_property("How far agents keep from walls.", 10.0);
    properties["baking_rect"] =
        area_property(false, "Bake only what is in this rect, when it has area.");
    properties["baking_rect_offset"] = point_schema(false);
    properties["baking_rect_offset"]["description"] = "Moves baking_rect.";
}

json::Value file_schema(bool three_d) {
    json::Value point = point_schema(three_d);

    json::Value polygon;
    polygon["type"] = "array";
    polygon["items"]["type"] = "integer";
    polygon["items"]["minimum"] = 0;
    polygon["minItems"] = 3;
    polygon["description"] = "Indices into vertices, going round a convex polygon.";

    json::Value out;
    out["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    out["title"] = three_d ? "SlashEngine navigation mesh" : "SlashEngine navigation polygon";
    out["description"] =
        three_d ? "Convex polygons a 3D map's paths go over, as Godot's NavigationMesh; a "
                  "NavigationRegion3D places it."
                : "Convex polygons a 2D map's paths go over, as Godot's NavigationPolygon; a "
                  "NavigationRegion2D places it.";
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = json::Array{"format"};
    json::Value& properties = out["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["vertices"]["type"] = "array";
    properties["vertices"]["items"] = std::move(point);
    properties["vertices"]["description"] =
        three_d ? "Points as [x, y, z] meters in the region's space."
                : "Points as [x, y] pixels in the region's space.";
    properties["polygons"]["type"] = "array";
    properties["polygons"]["items"] = std::move(polygon);
    if (three_d) {
        properties["cell_size"] =
            number_property("The cell size it was made for, as the 3D map's cell_size.", 0.25, 0.0);
        properties["cell_height"] = number_property(
            "The cell height it was made for, as the 3D map's cell_height.", 0.25, 0.0);
    } else {
        properties["cell_size"] =
            number_property("The cell size it was made for, as the 2D map's cell_size.", 1.0, 0.0);
    }
    add_bake_properties(properties, three_d);
    return out;
}

// A document's array, or an empty one when it leaves it out.
const json::Array& array_of(const json::Value& doc, std::string_view key) {
    static const json::Array kNone;
    const json::Value* value = doc.find(key);
    return value != nullptr ? value->as_array() : kNone;
}

Vec3 point_of(const json::Value& value, bool three_d) {
    const json::Array& xyz = value.as_array();
    if (three_d) {
        return {static_cast<float>(xyz.at(0).as_double()),
                static_cast<float>(xyz.at(1).as_double()),
                static_cast<float>(xyz.at(2).as_double())};
    }
    // A 2D point is on the plane y = 0, as the 2D map holds it.
    return {static_cast<float>(xyz.at(0).as_double()), 0.0F,
            static_cast<float>(xyz.at(1).as_double())};
}

// The errors past the schema: indices out of range or used twice, and polygons that turn both
// ways round. Polygons can go either way round, as Godot's, but all their corners the same way.
std::vector<json::ValidationError> check_polygons(const json::Value& doc, bool three_d) {
    std::vector<json::ValidationError> errors;
    std::vector<Vec3> vertices;
    for (const json::Value& vertex : array_of(doc, "vertices")) {
        vertices.push_back(point_of(vertex, three_d));
    }
    const json::Array& polygons = array_of(doc, "polygons");
    for (std::size_t p = 0; p < polygons.size(); ++p) {
        const std::string at = "/polygons/" + std::to_string(p);
        const json::Array& indices = polygons[p].as_array();
        std::vector<Vec3> points;
        std::set<std::int64_t> seen;
        bool bad = false;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const std::int64_t index = indices[i].as_int();
            if (index >= static_cast<std::int64_t>(vertices.size())) {
                errors.push_back({at + "/" + std::to_string(i),
                                  "vertex " + std::to_string(index) + " is past the last of " +
                                      std::to_string(vertices.size()) + " vertices"});
                bad = true;
            } else if (!seen.insert(index).second) {
                errors.push_back({at + "/" + std::to_string(i),
                                  "vertex " + std::to_string(index) + " is in the polygon twice"});
                bad = true;
            } else {
                points.push_back(vertices[static_cast<std::size_t>(index)]);
            }
        }
        if (bad) {
            continue;
        }
        // Each corner's turn about the polygon's normal (Newell's) must have the same sign; a
        // straight corner has none. Small turns are let through, as float noise.
        Vec3 normal;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Vec3 a = points[i];
            const Vec3 b = points[(i + 1) % points.size()];
            normal.x += (a.y - b.y) * (a.z + b.z);
            normal.y += (a.z - b.z) * (a.x + b.x);
            normal.z += (a.x - b.x) * (a.y + b.y);
        }
        const float normal_length =
            std::sqrt((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));
        if (normal_length == 0.0F) {
            errors.push_back({at, "the polygon has no area"});
            continue;
        }
        const float tolerance = normal_length * 1e-5F;
        bool positive = false;
        bool negative = false;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Vec3 a = points[i];
            const Vec3 b = points[(i + 1) % points.size()];
            const Vec3 c = points[(i + 2) % points.size()];
            const Vec3 ab = b - a;
            const Vec3 bc = c - b;
            const Vec3 turn{(ab.y * bc.z) - (ab.z * bc.y), (ab.z * bc.x) - (ab.x * bc.z),
                            (ab.x * bc.y) - (ab.y * bc.x)};
            const float along = (turn.x * normal.x) + (turn.y * normal.y) + (turn.z * normal.z);
            positive = positive || along > tolerance;
            negative = negative || along < -tolerance;
        }
        if (positive && negative) {
            errors.push_back({at, "the polygon isn't convex; split it into convex polygons"});
        }
    }
    return errors;
}

std::vector<json::ValidationError> validate(const json::Value& doc, bool three_d) {
    std::vector<json::ValidationError> errors = json::validate(doc, file_schema(three_d));
    if (!errors.empty()) {
        return errors;
    }
    return check_polygons(doc, three_d);
}

Error first_error(const std::vector<json::ValidationError>& errors) {
    const json::ValidationError& first = errors.front();
    return Error{ErrorCode::InvalidArgument,
                 (first.path.empty() ? std::string("root") : first.path) + ": " + first.message};
}

std::vector<std::vector<int>> polygons_of(const json::Value& doc) {
    std::vector<std::vector<int>> out;
    for (const json::Value& polygon : array_of(doc, "polygons")) {
        std::vector<int>& indices = out.emplace_back();
        for (const json::Value& index : polygon.as_array()) {
            indices.push_back(static_cast<int>(index.as_int()));
        }
    }
    return out;
}

float float_of(const json::Value& doc, std::string_view key, float fallback) {
    const json::Value* value = doc.find(key);
    return value != nullptr ? static_cast<float>(value->as_double()) : fallback;
}

bool bool_of(const json::Value& doc, std::string_view key) {
    const json::Value* value = doc.find(key);
    return value != nullptr && value->as_bool();
}

std::uint32_t mask_of(const json::Value& doc, std::string_view key) {
    const json::Value* value = doc.find(key);
    return value != nullptr ? static_cast<std::uint32_t>(value->as_int()) : 0xFFFFFFFFU;
}

// The value a name the schema let through stands for, by its index, or fallback when the
// document leaves it out.
template <typename Enum, std::size_t N>
Enum enum_of(const json::Value& doc, std::string_view key,
             const std::array<std::string_view, N>& names, Enum fallback) {
    const json::Value* value = doc.find(key);
    if (value == nullptr) {
        return fallback;
    }
    const auto it = std::find(names.begin(), names.end(), value->as_string());
    return static_cast<Enum>(it - names.begin());
}

Vec2 vec2_of(const json::Value& value) {
    const json::Array& xy = value.as_array();
    return {static_cast<float>(xy.at(0).as_double()), static_cast<float>(xy.at(1).as_double())};
}

json::Value point_value(Vec3 point) {
    return json::Array{point.x, point.y, point.z};
}

json::Value point_value(Vec2 point) {
    return json::Array{point.x, point.y};
}

json::Value polygons_value(const std::vector<std::vector<int>>& polygons) {
    json::Array out;
    for (const std::vector<int>& polygon : polygons) {
        json::Array& indices = out.emplace_back(json::Array{}).as_array();
        for (const int index : polygon) {
            indices.emplace_back(index);
        }
    }
    return out;
}

json::Value base_document(json::Value doc) {
    if (!doc.is_object()) {
        doc = json::Object{};
        doc["format"] = 1;
    }
    return doc;
}

} // namespace

json::Value mesh_schema() {
    return file_schema(true);
}
json::Value polygon_schema() {
    return file_schema(false);
}

std::vector<json::ValidationError> validate_mesh(const json::Value& doc) {
    return validate(doc, true);
}
std::vector<json::ValidationError> validate_polygon(const json::Value& doc) {
    return validate(doc, false);
}

Result<NavigationMesh> parse_mesh(const json::Value& doc) {
    if (const auto errors = validate_mesh(doc); !errors.empty()) {
        return first_error(errors);
    }
    NavigationMesh mesh;
    for (const json::Value& vertex : array_of(doc, "vertices")) {
        mesh.vertices.push_back(point_of(vertex, true));
    }
    mesh.polygons = polygons_of(doc);
    mesh.cell_size = float_of(doc, "cell_size", mesh.cell_size);
    mesh.cell_height = float_of(doc, "cell_height", mesh.cell_height);
    mesh.sample_partition_type =
        enum_of(doc, "sample_partition_type", kPartition3DNames, mesh.sample_partition_type);
    mesh.geometry_parsed_geometry_type =
        enum_of(doc, "geometry_parsed_geometry_type", kParsedGeometryNames,
                mesh.geometry_parsed_geometry_type);
    mesh.geometry_collision_mask = mask_of(doc, "geometry_collision_mask");
    mesh.border_size = float_of(doc, "border_size", mesh.border_size);
    mesh.agent_height = float_of(doc, "agent_height", mesh.agent_height);
    mesh.agent_radius = float_of(doc, "agent_radius", mesh.agent_radius);
    mesh.agent_max_climb = float_of(doc, "agent_max_climb", mesh.agent_max_climb);
    mesh.agent_max_slope = float_of(doc, "agent_max_slope", mesh.agent_max_slope);
    mesh.region_min_size = float_of(doc, "region_min_size", mesh.region_min_size);
    mesh.region_merge_size = float_of(doc, "region_merge_size", mesh.region_merge_size);
    mesh.edge_max_length = float_of(doc, "edge_max_length", mesh.edge_max_length);
    mesh.edge_max_error = float_of(doc, "edge_max_error", mesh.edge_max_error);
    mesh.vertices_per_polygon = float_of(doc, "vertices_per_polygon", mesh.vertices_per_polygon);
    mesh.detail_sample_distance =
        float_of(doc, "detail_sample_distance", mesh.detail_sample_distance);
    mesh.detail_sample_max_error =
        float_of(doc, "detail_sample_max_error", mesh.detail_sample_max_error);
    mesh.filter_low_hanging_obstacles = bool_of(doc, "filter_low_hanging_obstacles");
    mesh.filter_ledge_spans = bool_of(doc, "filter_ledge_spans");
    mesh.filter_walkable_low_height_spans = bool_of(doc, "filter_walkable_low_height_spans");
    if (const json::Value* box = doc.find("filter_baking_aabb"); box != nullptr) {
        mesh.filter_baking_aabb_position = point_of(*box->find("position"), true);
        mesh.filter_baking_aabb_size = point_of(*box->find("size"), true);
    }
    if (const json::Value* offset = doc.find("filter_baking_aabb_offset"); offset != nullptr) {
        mesh.filter_baking_aabb_offset = point_of(*offset, true);
    }
    return mesh;
}

Result<NavigationPolygon> parse_polygon(const json::Value& doc) {
    if (const auto errors = validate_polygon(doc); !errors.empty()) {
        return first_error(errors);
    }
    NavigationPolygon polygon;
    for (const json::Value& vertex : array_of(doc, "vertices")) {
        polygon.vertices.push_back(vec2_of(vertex));
    }
    polygon.polygons = polygons_of(doc);
    for (const json::Value& outline : array_of(doc, "outlines")) {
        std::vector<Vec2>& points = polygon.outlines.emplace_back();
        for (const json::Value& point : outline.as_array()) {
            points.push_back(vec2_of(point));
        }
    }
    polygon.cell_size = float_of(doc, "cell_size", polygon.cell_size);
    polygon.sample_partition_type =
        enum_of(doc, "sample_partition_type", kPartition2DNames, polygon.sample_partition_type);
    polygon.parsed_geometry_type =
        enum_of(doc, "parsed_geometry_type", kParsedGeometryNames, polygon.parsed_geometry_type);
    polygon.parsed_collision_mask = mask_of(doc, "parsed_collision_mask");
    polygon.border_size = float_of(doc, "border_size", polygon.border_size);
    polygon.agent_radius = float_of(doc, "agent_radius", polygon.agent_radius);
    if (const json::Value* rect = doc.find("baking_rect"); rect != nullptr) {
        polygon.baking_rect_position = vec2_of(*rect->find("position"));
        polygon.baking_rect_size = vec2_of(*rect->find("size"));
    }
    if (const json::Value* offset = doc.find("baking_rect_offset"); offset != nullptr) {
        polygon.baking_rect_offset = vec2_of(*offset);
    }
    return polygon;
}

json::Value with_polygons(json::Value doc, const NavigationMesh& mesh) {
    doc = base_document(std::move(doc));
    json::Array vertices;
    for (const Vec3 vertex : mesh.vertices) {
        vertices.push_back(point_value(vertex));
    }
    doc["vertices"] = std::move(vertices);
    doc["polygons"] = polygons_value(mesh.polygons);
    return doc;
}

json::Value with_polygons(json::Value doc, const NavigationPolygon& polygon) {
    doc = base_document(std::move(doc));
    json::Array vertices;
    for (const Vec2 vertex : polygon.vertices) {
        vertices.push_back(point_value(vertex));
    }
    doc["vertices"] = std::move(vertices);
    doc["polygons"] = polygons_value(polygon.polygons);
    return doc;
}

void add_asset_types(asset::Assets& assets) {
    const auto polygons_bytes = [](const std::vector<std::vector<int>>& polygons) {
        std::size_t bytes = 0;
        for (const std::vector<int>& polygon : polygons) {
            bytes += sizeof(std::vector<int>) + (polygon.size() * sizeof(int));
        }
        return bytes;
    };
    assets.add_type<NavigationMesh>(
        "navigation_mesh", {std::string(kMeshSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<NavigationMesh> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_mesh(doc.value());
        },
        [polygons_bytes](const NavigationMesh& mesh) {
            return sizeof(mesh) + (mesh.vertices.size() * sizeof(Vec3)) +
                   polygons_bytes(mesh.polygons);
        });
    assets.add_type<NavigationPolygon>(
        "navigation_polygon", {std::string(kPolygonSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<NavigationPolygon> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_polygon(doc.value());
        },
        [polygons_bytes](const NavigationPolygon& polygon) {
            return sizeof(polygon) + (polygon.vertices.size() * sizeof(Vec2)) +
                   polygons_bytes(polygon.polygons);
        });
}

} // namespace slash::navigation
