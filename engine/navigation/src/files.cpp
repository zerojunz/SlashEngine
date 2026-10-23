#include "slash/navigation/navigation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

json::Value file_schema(bool three_d) {
    json::Value point;
    point["type"] = "array";
    point["items"]["type"] = "number";
    point["minItems"] = three_d ? 3 : 2;
    point["maxItems"] = three_d ? 3 : 2;

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
    out["required"] = json::Array{"format", "vertices", "polygons"};
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
    return out;
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
    for (const json::Value& vertex : doc.find("vertices")->as_array()) {
        vertices.push_back(point_of(vertex, three_d));
    }
    const json::Array& polygons = doc.find("polygons")->as_array();
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
    for (const json::Value& polygon : doc.find("polygons")->as_array()) {
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
    for (const json::Value& vertex : doc.find("vertices")->as_array()) {
        mesh.vertices.push_back(point_of(vertex, true));
    }
    mesh.polygons = polygons_of(doc);
    mesh.cell_size = float_of(doc, "cell_size", mesh.cell_size);
    mesh.cell_height = float_of(doc, "cell_height", mesh.cell_height);
    return mesh;
}

Result<NavigationPolygon> parse_polygon(const json::Value& doc) {
    if (const auto errors = validate_polygon(doc); !errors.empty()) {
        return first_error(errors);
    }
    NavigationPolygon polygon;
    for (const json::Value& vertex : doc.find("vertices")->as_array()) {
        const json::Array& xy = vertex.as_array();
        polygon.vertices.push_back(
            {static_cast<float>(xy.at(0).as_double()), static_cast<float>(xy.at(1).as_double())});
    }
    polygon.polygons = polygons_of(doc);
    polygon.cell_size = float_of(doc, "cell_size", polygon.cell_size);
    return polygon;
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
