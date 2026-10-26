#include "slash/scene3d/recipe.hpp"

#include "model_internal.hpp"

#include "slash/core/expression.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <manifold/manifold.h>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

// The most copies an array makes, and the most shapes a recipe makes in all.
constexpr std::int64_t kMaxCopies = 1024;
constexpr std::size_t kMaxShapes = 4096;
constexpr float kPi = std::numbers::pi_v<float>;

const world::ComponentRegistry& components() {
    static const std::unique_ptr<const world::ComponentRegistry> registry = [] {
        auto made = std::make_unique<world::ComponentRegistry>();
        world::register_builtin_components(*made);
        return std::unique_ptr<const world::ComponentRegistry>(std::move(made));
    }();
    return *registry;
}

// ---- Fields ----

enum class Kind : std::uint8_t { Number, Integer, Boolean, Vector, Polygon, Choice, Path };

struct Field {
    std::string_view name;
    Kind kind;
    std::string_view doc;
    // The component whose field of the same name gives its limits and default, or "".
    std::string_view component;
    std::vector<std::string_view> choices{}; // NOLINT(readability-redundant-member-init)
};

// The fields a part's shape takes, beyond those every part has.
const std::vector<Field>& shape_fields() {
    static const std::vector<Field> fields{
        {"size", Kind::Vector, "box and prism: width, height and depth in meters, as [x, y, z].",
         "CSGBox3D"},
        {"radius", Kind::Number, "sphere, cylinder and capsule: meters.", "CSGSphere3D"},
        {"height", Kind::Number, "cylinder and capsule: meters along y, end to end.",
         "CSGCylinder3D"},
        {"radial_segments", Kind::Integer,
         "sphere and capsule: corners around y; 12 for a sphere, 64 for a capsule.", "CSGSphere3D"},
        {"rings", Kind::Integer,
         "sphere and capsule: rows of corners from top to bottom; 6 for a sphere, 8 for a capsule.",
         "CSGSphere3D"},
        {"sides", Kind::Integer, "cylinder and torus: corners around y.", "CSGCylinder3D"},
        {"cone", Kind::Boolean, "cylinder: a cone with its point up.", "CSGCylinder3D"},
        {"inner_radius", Kind::Number, "torus: meters from y to the ring's inside.", "CSGTorus3D"},
        {"outer_radius", Kind::Number, "torus: meters from y to the ring's outside.", "CSGTorus3D"},
        {"ring_sides", Kind::Integer, "torus: corners around the ring.", "CSGTorus3D"},
        {"smooth_faces", Kind::Boolean,
         "sphere, cylinder, torus and polygon: shade across faces as one smooth surface; on but "
         "for a polygon.",
         "CSGSphere3D"},
        {"polygon", Kind::Polygon,
         "polygon: at least 3 points [x, y] in the part's xy plane, in meters.", "CSGPolygon3D"},
        {"mode", Kind::Choice,
         "polygon: pushed depth along -z (\"depth\"), spun around y (\"spin\", a lathe) or swept "
         "along path (\"path\").",
         "CSGPolygon3D", std::vector<std::string_view>{"depth", "spin", "path"}},
        {"depth", Kind::Number, "polygon, depth mode: meters along -z.", "CSGPolygon3D"},
        {"spin_degrees", Kind::Number, "polygon, spin mode: degrees around y.", "CSGPolygon3D"},
        {"spin_sides", Kind::Integer, "polygon, spin mode: steps around y.", "CSGPolygon3D"},
        {"path", Kind::Path,
         "polygon, path mode: the curve it is swept along, as a Path3D's fields, in the part's "
         "space.",
         ""},
        {"path_interval_type", Kind::Choice,
         "polygon, path mode: path_interval is \"distance\" in meters or \"subdivide\" each "
         "curve segment.",
         "CSGPolygon3D", std::vector<std::string_view>{"distance", "subdivide"}},
        {"path_interval", Kind::Number, "polygon, path mode: how often a ring is placed.",
         "CSGPolygon3D"},
        {"path_simplify_angle", Kind::Number,
         "polygon, path mode: degrees of bend under which rings are left out.", "CSGPolygon3D"},
        {"path_rotation", Kind::Choice,
         "polygon, path mode: how the polygon turns along the path, as Godot's.", "CSGPolygon3D",
         std::vector<std::string_view>{"polygon", "path", "path_follow"}},
        {"path_rotation_accurate", Kind::Boolean,
         "polygon, path mode: turn by the curve's exact direction.", "CSGPolygon3D"},
        {"path_continuous_u", Kind::Boolean,
         "polygon, path mode: texture coordinates run on along the path.", "CSGPolygon3D"},
        {"path_u_distance", Kind::Number,
         "polygon, path mode: meters along the path a texture spans.", "CSGPolygon3D"},
        {"path_joined", Kind::Boolean, "polygon, path mode: join the path's ends.", "CSGPolygon3D"},
        {"left_to_right", Kind::Number, "prism: where its top edge is, 0 left to 1 right.",
         "PrismMesh"},
        {"subdivide_width", Kind::Integer, "prism: cuts across x.", "PrismMesh"},
        {"subdivide_height", Kind::Integer, "prism: cuts across y.", "PrismMesh"},
        {"subdivide_depth", Kind::Integer, "prism: cuts across z.", "PrismMesh"},
        {"bevel", Kind::Number,
         "box and cylinder: meters its edges are rounded by; 0 leaves them sharp.", ""},
        {"bevel_segments", Kind::Integer,
         "box and cylinder: steps round each bevel; 1 cuts edges flat (a chamfer).", ""},
    };
    return fields;
}

const Field* find_field(std::string_view name) {
    const auto& fields = shape_fields();
    const auto found =
        std::ranges::find_if(fields, [&](const Field& field) { return field.name == name; });
    return found == fields.end() ? nullptr : &*found;
}

// Each shape and the fields it takes.
struct ShapeKind {
    std::string_view name;
    std::vector<std::string_view> fields;
};

const std::vector<ShapeKind>& shape_kinds() {
    static const std::vector<ShapeKind> kinds{
        {"combiner", {}},
        {"box", {"size", "bevel", "bevel_segments"}},
        {"sphere", {"radius", "radial_segments", "rings", "smooth_faces"}},
        {"cylinder",
         {"radius", "height", "sides", "cone", "smooth_faces", "bevel", "bevel_segments"}},
        {"torus", {"inner_radius", "outer_radius", "sides", "ring_sides", "smooth_faces"}},
        {"polygon",
         {"polygon", "mode", "depth", "spin_degrees", "spin_sides", "path", "path_interval_type",
          "path_interval", "path_simplify_angle", "path_rotation", "path_rotation_accurate",
          "path_continuous_u", "path_u_distance", "path_joined", "smooth_faces"}},
        {"capsule", {"radius", "height", "radial_segments", "rings"}},
        {"prism",
         {"left_to_right", "size", "subdivide_width", "subdivide_height", "subdivide_depth"}},
    };
    return kinds;
}

const ShapeKind& shape_kind(std::string_view name) {
    const auto& kinds = shape_kinds();
    const auto found =
        std::ranges::find_if(kinds, [&](const ShapeKind& kind) { return kind.name == name; });
    return found == kinds.end() ? kinds.front() : *found;
}

// ---- Schema ----

// The limits a component gives its field of the same name.
json::Value component_limits(std::string_view component, std::string_view field) {
    json::Value out;
    if (component.empty()) {
        return out;
    }
    world::SchemaBuilder builder;
    (void)components().find(component)->schema(builder);
    const json::Value* properties = nullptr;
    if (const auto found = builder.definitions().find(component);
        found != builder.definitions().end()) {
        properties = found->second.find("properties");
    }
    const json::Value* property = properties != nullptr ? properties->find(field) : nullptr;
    if (property == nullptr) {
        return out;
    }
    for (const std::string_view key : {"minimum", "maximum", "default"}) {
        if (const json::Value* value = property->find(key); value != nullptr) {
            out[key] = *value;
        }
    }
    return out;
}

// A number, or with loose an expression in its place.
json::Value number_schema(std::string_view type, std::string_view doc, bool loose,
                          const json::Value& limits = {}) {
    json::Value number;
    number["type"] = type;
    if (limits.is_object()) {
        for (const auto& [key, value] : limits.as_object()) {
            if (key != "default") {
                number[key] = value;
            }
        }
    }
    json::Value out;
    if (loose) {
        json::Value expression;
        expression["$ref"] = "#/$defs/Expression";
        out["anyOf"] = json::Array{std::move(number), std::move(expression)};
    } else {
        out = std::move(number);
    }
    if (!doc.empty()) {
        out["description"] = doc;
    }
    if (const json::Value* fallback = limits.is_object() ? limits.find("default") : nullptr;
        fallback != nullptr) {
        out["default"] = *fallback;
    }
    return out;
}

json::Value vector_schema(std::size_t count, std::string_view doc, bool loose) {
    json::Value out;
    out["type"] = "array";
    out["minItems"] = static_cast<std::int64_t>(count);
    out["maxItems"] = static_cast<std::int64_t>(count);
    out["items"] = number_schema("number", "", loose);
    out["description"] = doc;
    return out;
}

json::Value choice_schema(const std::vector<std::string_view>& choices, std::string_view doc,
                          std::string_view fallback) {
    json::Value out;
    out["type"] = "string";
    json::Array values;
    for (const std::string_view choice : choices) {
        values.emplace_back(std::string(choice));
    }
    out["enum"] = std::move(values);
    out["default"] = fallback;
    out["description"] = doc;
    return out;
}

json::Value field_schema(const Field& field, bool loose, world::SchemaBuilder& builder) {
    json::Value limits = component_limits(field.component, field.name);
    // Shapes differ in these defaults, which the fields' docs give.
    if (limits.is_object() && (field.name == "radial_segments" || field.name == "rings" ||
                               field.name == "smooth_faces")) {
        limits.as_object().erase("default");
    }
    switch (field.kind) {
    case Kind::Number:
        return number_schema("number", field.doc, loose, limits);
    case Kind::Integer: {
        json::Value bounded = limits.is_object() ? limits : json::Value(json::Object{});
        if (field.name == "bevel_segments") {
            bounded["minimum"] = 1;
            bounded["maximum"] = 16;
            bounded["default"] = 4;
        }
        return number_schema("integer", field.doc, loose, bounded);
    }
    case Kind::Boolean:
        return number_schema("boolean", field.doc, loose, limits);
    case Kind::Vector:
        return vector_schema(3, field.doc, loose);
    case Kind::Polygon: {
        json::Value out;
        out["type"] = "array";
        out["items"] = vector_schema(2, "[x, y] in meters.", loose);
        out["description"] = field.doc;
        return out;
    }
    case Kind::Choice: {
        const json::Value* fallback = limits.find("default");
        return choice_schema(field.choices, field.doc,
                             fallback != nullptr ? fallback->as_string() : field.choices.front());
    }
    case Kind::Path: {
        json::Value out = components().find("Path3D")->schema(builder);
        out["description"] = field.doc;
        return out;
    }
    }
    return {};
}

void transform_properties(json::Value& properties, bool loose) {
    properties["position"] = vector_schema(3, "Meters, as [x, y, z].", loose);
    properties["rotation"] =
        vector_schema(3, "Radians about each axis, as Transform3D's (YXZ order).", loose);
    properties["scale"] = vector_schema(3, "As [x, y, z].", loose);
}

json::Value make_schema(bool loose) {
    json::Value part;
    part["type"] = "object";
    part["additionalProperties"] = false;
    part["description"] =
        "A shape, as Godot's CSG node of its kind, combined with the shapes before it under the "
        "same parent by its operation; or at the top level, a named part of the model.";
    json::Value& properties = part["properties"];
    properties["name"]["type"] = "string";
    properties["name"]["minLength"] = 1;
    properties["name"]["description"] =
        "A top-level part's node name, by which animations and Skeleton3D poses move it.";
    json::Array shapes;
    for (const ShapeKind& kind : shape_kinds()) {
        shapes.emplace_back(std::string(kind.name));
    }
    properties["shape"]["type"] = "string";
    properties["shape"]["enum"] = std::move(shapes);
    properties["shape"]["default"] = "combiner";
    properties["shape"]["description"] =
        "combiner (only its children, as CSGCombiner3D), box, sphere, cylinder, torus, polygon "
        "(CSGPolygon3D), capsule or prism (CSGMesh3D of that primitive mesh).";
    properties["operation"] =
        choice_schema({"union", "intersection", "subtraction"},
                      "How it combines with the shapes before it under the same parent.", "union");
    properties["material"]["type"] = "string";
    properties["material"]["description"] =
        "A name in materials, for this shape and the shapes under it that name none; Godot's "
        "default material without.";
    transform_properties(properties, loose);
    properties["flip_faces"] = number_schema("boolean", "Turns its faces inside out.", loose);
    world::SchemaBuilder builder;
    for (const Field& field : shape_fields()) {
        properties[field.name] = field_schema(field, loose, builder);
    }
    json::Value array;
    array["type"] = "object";
    array["additionalProperties"] = false;
    array["required"] = json::Array{"count"};
    array["description"] =
        "Copies of the part: each placed by this step's transform after the one before it, so "
        "position lines them up and rotation turns them round the parent's origin.";
    array["properties"]["count"] =
        number_schema("integer", "How many, the part itself first.", loose,
                      json::Object{{"minimum", 1}, {"maximum", kMaxCopies}});
    transform_properties(array["properties"], loose);
    properties["array"] = std::move(array);
    properties["children"]["type"] = "array";
    properties["children"]["items"]["$ref"] = "#/$defs/Part";
    properties["children"]["description"] =
        "Shapes in its space, combined with it in order, as a CSG shape's children.";

    json::Value material = components().find("StandardMaterial3D")->schema(builder);

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine model recipe";
    schema["description"] =
        "A model as named parameters, materials and parts made of CSG shapes, which the engine "
        "builds when it loads the file.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "parts"};
    schema["properties"]["format"]["const"] = 1;
    schema["properties"]["format"]["description"] = "Format version. Must be 1.";
    json::Value parameter;
    parameter["description"] = "A number, or an expression over the other parameters.";
    parameter["anyOf"] =
        json::Array{json::Object{{"type", "number"}}, json::Object{{"$ref", "#/$defs/Expression"}}};
    schema["properties"]["parameters"]["type"] = "object";
    schema["properties"]["parameters"]["description"] =
        "Numbers by name (letters, digits and '_', not starting with a digit), which parts' "
        "expressions read.";
    schema["properties"]["parameters"]["additionalProperties"] = std::move(parameter);
    schema["properties"]["materials"]["type"] = "object";
    schema["properties"]["materials"]["description"] =
        "StandardMaterial3D fields by name, which parts' material names.";
    schema["properties"]["materials"]["additionalProperties"] = std::move(material);
    schema["properties"]["parts"]["type"] = "array";
    schema["properties"]["parts"]["minItems"] = 1;
    schema["properties"]["parts"]["items"]["$ref"] = "#/$defs/Part";
    schema["properties"]["parts"]["description"] =
        "The model's parts: each a node of its own, drawn as one mesh.";
    json::Value point;
    point["type"] = "object";
    point["additionalProperties"] = false;
    point["required"] = json::Array{"name"};
    point["description"] = "An attachment point: a node with no mesh, as Godot's Marker3D, which a "
                           "BoneAttachment3D's bone_name follows.";
    point["properties"]["name"]["type"] = "string";
    point["properties"]["name"]["minLength"] = 1;
    point["properties"]["name"]["description"] = "Its node name, unique in the model.";
    point["properties"]["part"]["type"] = "string";
    point["properties"]["part"]["minLength"] = 1;
    point["properties"]["part"]["description"] =
        "A top-level part's node name (a copy's numbered one), to place it in and move with. "
        "Default: the model's origin.";
    point["properties"]["position"] = vector_schema(3, "Meters, as [x, y, z].", loose);
    point["properties"]["rotation"] =
        vector_schema(3, "Radians about each axis, as Transform3D's (YXZ order).", loose);
    schema["properties"]["points"]["type"] = "array";
    schema["properties"]["points"]["items"] = std::move(point);
    schema["properties"]["points"]["description"] =
        "Named places on the model where other things attach: a seat, a muzzle, a door's handle.";
    json::Value definitions = builder.definitions();
    definitions["Part"] = std::move(part);
    json::Value expression;
    expression["type"] = "string";
    expression["description"] =
        "\"=\" then an expression in Godot's Expression syntax over the parameters, such as "
        "\"=width / 2 - 0.05\"; its value takes the string's place.";
    definitions["Expression"] = std::move(expression);
    schema["$defs"] = std::move(definitions);
    return schema;
}

// ---- Expressions ----

bool is_expression(const json::Value& value) {
    return value.is_string() && value.as_string().starts_with('=');
}

bool is_identifier(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

// Works out every parameter and every expression in the parts, in place, reporting each
// failure at its pointer and leaving 0 where it was.
class Evaluator {
public:
    Evaluator(json::Value& doc, std::vector<json::ValidationError>& errors)
        : doc_(doc), errors_(errors) {}

    void run() {
        if (json::Value* parameters = doc_.find("parameters");
            parameters != nullptr && parameters->is_object()) {
            for (const auto& [name, value] : parameters->as_object()) {
                names_.push_back(name);
                if (!is_identifier(name) || kReserved.contains(name)) {
                    errors_.push_back(
                        {"/parameters/" + json::pointer_escape(name),
                         "isn't a parameter name: use letters, digits and '_', not starting with "
                         "a digit, and none of PI, TAU, INF, NAN, true, false, null, and, or and "
                         "not"});
                }
            }
            for (const std::string& name : names_) {
                (void)parameter(name);
            }
            for (const std::string& name : names_) {
                const auto found = values_.find(name);
                (*parameters)[name] =
                    found != values_.end() && found->second ? *found->second : json::Value(0.0);
            }
        }
        if (json::Value* parts = doc_.find("parts"); parts != nullptr) {
            walk(*parts, "/parts");
        }
        if (json::Value* points = doc_.find("points"); points != nullptr) {
            walk(*points, "/points");
        }
    }

private:
    static inline const std::set<std::string, std::less<>> kReserved = {
        "PI", "TAU", "INF", "NAN", "true", "false", "null", "and", "or", "not"};

    // A parameter's value, working it out the first time; nullopt when it fails.
    std::optional<json::Value> parameter(const std::string& name) {
        if (const auto found = values_.find(name); found != values_.end()) {
            return found->second;
        }
        const std::string at = "/parameters/" + json::pointer_escape(name);
        if (std::ranges::find(stack_, name) != stack_.end()) {
            std::string chain;
            for (const std::string& step : stack_) {
                chain += step + " -> ";
            }
            errors_.push_back({at, "depends on itself: " + chain + name});
            values_[name] = std::nullopt;
            return std::nullopt;
        }
        const json::Value& value = doc_.find("parameters")->as_object().at(name);
        std::optional<json::Value> out;
        if (value.is_number()) {
            out = json::Value(value.as_double());
        } else if (is_expression(value)) {
            stack_.push_back(name);
            out = evaluate(value.as_string(), at);
            stack_.pop_back();
        } else {
            errors_.push_back({at, "expected a number or an expression starting with '='"});
        }
        values_[name] = out;
        return out;
    }

    std::optional<json::Value> evaluate(const std::string& text, const std::string& at) {
        auto parsed = Expression::parse(std::string_view(text).substr(1));
        if (!parsed) {
            errors_.push_back({at, "expression " + quoted(text) + ": " + parsed.error().message});
            return std::nullopt;
        }
        bool failed = false;
        for (const std::string& name : parsed.value().names()) {
            if (std::ranges::find(names_, name) != names_.end()) {
                if (!parameter(name)) {
                    failed = true;
                }
                continue;
            }
            std::string message = "expression " + quoted(text) + " reads " + quoted(name) +
                                  ", which isn't a parameter";
            if (const std::string_view near = closest_match(name, names_); !near.empty()) {
                message += " (did you mean " + quoted(near) + "?)";
            }
            errors_.push_back({at, std::move(message)});
            failed = true;
        }
        if (failed) {
            return std::nullopt;
        }
        auto result =
            parsed.value().execute([&](std::string_view name) -> std::optional<json::Value> {
                const auto found = values_.find(name);
                if (found == values_.end()) {
                    return std::nullopt;
                }
                return found->second;
            });
        if (!result) {
            errors_.push_back({at, "expression " + quoted(text) + ": " + result.error().message});
            return std::nullopt;
        }
        if (!result.value().is_number() && !result.value().is_bool()) {
            errors_.push_back(
                {at, "expression " + quoted(text) +
                         " gives neither a number nor a boolean: " + json::write(result.value())});
            return std::nullopt;
        }
        if (result.value().is_number() && !std::isfinite(result.value().as_double())) {
            errors_.push_back({at, "expression " + quoted(text) + " gives " +
                                       json::write(result.value()) + ", not a finite number"});
            return std::nullopt;
        }
        return std::move(result).value();
    }

    void walk(json::Value& value, const std::string& at) {
        if (is_expression(value)) {
            std::optional<json::Value> result = evaluate(value.as_string(), at);
            value = result ? std::move(*result) : json::Value(0.0);
        } else if (value.is_array()) {
            json::Array& items = value.as_array();
            for (std::size_t i = 0; i < items.size(); ++i) {
                walk(items[i], at + "/" + std::to_string(i));
            }
        } else if (value.is_object()) {
            for (auto& [key, item] : value.as_object()) {
                walk(item, at + "/" + json::pointer_escape(key));
            }
        }
    }

    json::Value& doc_;
    std::vector<json::ValidationError>& errors_;
    std::vector<std::string> names_;
    std::map<std::string, std::optional<json::Value>, std::less<>> values_;
    std::vector<std::string> stack_;
};

// ---- Bevels ----

Vec3 normalized(Vec3 v) {
    const float length = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
    return length > 0.0F ? v * (1.0F / length) : Vec3{};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

// Planar texture coordinates in meters, across the axis a face mostly faces.
std::array<float, 2> planar_uv(Vec3 p, Vec3 n) {
    const float ax = std::abs(n.x);
    const float ay = std::abs(n.y);
    const float az = std::abs(n.z);
    if (ax >= ay && ax >= az) {
        return {p.z, -p.y};
    }
    if (ay >= az) {
        return {p.x, p.z};
    }
    return {p.x, -p.y};
}

// The convex hull of points as a model of one surface in Godot's winding, each corner's normal
// pointing away from the nearest point of the shape inside (inner), or with flat each face's.
std::shared_ptr<const Model> hull_model(const std::vector<Vec3>& points,
                                        const std::function<Vec3(Vec3)>& inner, bool flat) {
    std::vector<manifold::vec3> corners;
    corners.reserve(points.size());
    for (const Vec3 p : points) {
        corners.emplace_back(p.x, p.y, p.z);
    }
    const manifold::MeshGL mesh = manifold::Manifold::Hull(corners).GetMeshGL();
    const std::size_t stride = mesh.numProp;
    const auto at = [&](std::uint32_t index) {
        const std::size_t base = static_cast<std::size_t>(index) * stride;
        return Vec3{mesh.vertProperties[base], mesh.vertProperties[base + 1],
                    mesh.vertProperties[base + 2]};
    };
    ModelSurface surface;
    const auto vertex = [](Vec3 p, Vec3 n) {
        render::Vertex3D v;
        v.position = {p.x, p.y, p.z};
        v.normal = {n.x, n.y, n.z};
        v.uv = planar_uv(p, n);
        return v;
    };
    if (!flat) {
        for (std::size_t i = 0; i < mesh.vertProperties.size() / stride; ++i) {
            const Vec3 p = at(static_cast<std::uint32_t>(i));
            surface.vertices.push_back(vertex(p, normalized(p - inner(p))));
        }
    }
    for (std::size_t t = 0; t + 2 < mesh.triVerts.size(); t += 3) {
        // Manifold's faces are counter-clockwise from outside; Godot's are clockwise.
        const std::array<std::uint32_t, 3> corner{mesh.triVerts[t], mesh.triVerts[t + 2],
                                                  mesh.triVerts[t + 1]};
        if (!flat) {
            surface.indices.insert(surface.indices.end(), corner.begin(), corner.end());
            continue;
        }
        const Vec3 a = at(corner[0]);
        const Vec3 b = at(corner[1]);
        const Vec3 c = at(corner[2]);
        const Vec3 n = normalized(cross(c - a, b - a));
        for (const Vec3 p : {a, b, c}) {
            surface.indices.push_back(static_cast<std::uint32_t>(surface.vertices.size()));
            surface.vertices.push_back(vertex(p, n));
        }
    }
    auto model = std::make_shared<Model>();
    model->meshes.push_back({.name = "bevel", .surfaces = {std::move(surface)}});
    model->parts.push_back({.node = "bevel", .mesh = 0, .transform = {}});
    return model;
}

// A box of size with its edges rounded by radius in segments steps: the hull of an eighth of a
// sphere at each corner.
std::shared_ptr<const Model> beveled_box(Vec3 size, float radius, int segments) {
    const Vec3 half{(size.x * 0.5F) - radius, (size.y * 0.5F) - radius, (size.z * 0.5F) - radius};
    std::vector<Vec3> points;
    for (int i = 0; i <= segments; ++i) {
        for (int j = 0; i + j <= segments; ++j) {
            const int k = segments - i - j;
            const Vec3 d =
                normalized({static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)});
            for (const float sx : {-1.0F, 1.0F}) {
                for (const float sy : {-1.0F, 1.0F}) {
                    for (const float sz : {-1.0F, 1.0F}) {
                        points.push_back({sx * (half.x + (d.x * radius)),
                                          sy * (half.y + (d.y * radius)),
                                          sz * (half.z + (d.z * radius))});
                    }
                }
            }
        }
    }
    const auto inner = [half](Vec3 p) {
        return Vec3{std::clamp(p.x, -half.x, half.x), std::clamp(p.y, -half.y, half.y),
                    std::clamp(p.z, -half.z, half.z)};
    };
    return hull_model(points, inner, segments == 1);
}

// A cylinder of radius and height with its rims rounded by bevel in segments steps.
std::shared_ptr<const Model> beveled_cylinder(float radius, float height, int sides, float bevel,
                                              int segments, bool smooth) {
    const float core = radius - bevel;
    const float half = (height * 0.5F) - bevel;
    std::vector<Vec3> points;
    for (int s = 0; s < sides; ++s) {
        const Vec2 around = cos_sin(static_cast<float>(s) * 2.0F * kPi / static_cast<float>(sides));
        for (int t = 0; t <= segments; ++t) {
            const Vec2 arc =
                cos_sin(static_cast<float>(t) * 0.5F * kPi / static_cast<float>(segments));
            const float out = core + (bevel * arc.y);
            const float up = half + (bevel * arc.x);
            points.push_back({around.y * out, up, around.x * out});
            points.push_back({around.y * out, -up, around.x * out});
        }
    }
    const auto inner = [core, half](Vec3 p) {
        const float out = std::sqrt((p.x * p.x) + (p.z * p.z));
        const float scale = out > core && out > 0.0F ? core / out : 1.0F;
        return Vec3{p.x * scale, std::clamp(p.y, -half, half), p.z * scale};
    };
    return hull_model(points, inner, segments == 1 || !smooth);
}

// ---- Reading parts ----

world::Affine3D transform_of(const json::Value& object) {
    world::Transform3D transform;
    const auto vec = [](const json::Value& xyz) {
        const json::Array& v = xyz.as_array();
        return Vec3{static_cast<float>(v.at(0).as_double()),
                    static_cast<float>(v.at(1).as_double()),
                    static_cast<float>(v.at(2).as_double())};
    };
    if (const json::Value* position = object.find("position"); position != nullptr) {
        transform.position = vec(*position);
    }
    if (const json::Value* rotation = object.find("rotation"); rotation != nullptr) {
        transform.rotation = vec(*rotation);
    }
    if (const json::Value* scale = object.find("scale"); scale != nullptr) {
        transform.scale = vec(*scale);
    }
    return world::affine(transform);
}

float number_of(const json::Value& object, std::string_view field, float fallback) {
    const json::Value* value = object.find(field);
    return value != nullptr ? static_cast<float>(value->as_double()) : fallback;
}

// A copy of a top-level part: its name, where it is and its shapes.
struct Root {
    std::string name;
    world::Affine3D place;
    CsgTree tree;
};

class Reader {
public:
    explicit Reader(std::vector<json::ValidationError>& errors) : errors_(errors) {}

    void read_materials(const json::Value& doc) {
        const json::Value* materials = doc.find("materials");
        if (materials == nullptr) {
            return;
        }
        for (const auto& [name, fields] : materials->as_object()) {
            world::StandardMaterial3D standard;
            std::string path;
            if (auto read = world::from_json(scratch_, fields, standard, path); !read) {
                errors_.push_back(
                    {"/materials/" + json::pointer_escape(name) + path, read.error().message});
                continue;
            }
            materials_.emplace(name, std::move(standard));
        }
    }

    // The copies a part makes, each with its shapes.
    std::vector<CsgTree> read_part(const json::Value& part, const std::string& at,
                                   const std::optional<world::StandardMaterial3D>& inherited) {
        std::vector<CsgTree> out;
        const std::string shape =
            part.find("shape") != nullptr ? part.find("shape")->as_string() : "combiner";
        const ShapeKind& kind = shape_kind(shape);
        bool fits = true;
        for (const auto& [key, value] : part.as_object()) {
            const Field* field = find_field(key);
            if (field == nullptr || std::ranges::find(kind.fields, key) != kind.fields.end()) {
                continue;
            }
            std::string message = "isn't a field of a " + shape;
            if (!kind.fields.empty()) {
                message += ": its fields are";
                for (std::size_t i = 0; i < kind.fields.size(); ++i) {
                    message += (i == 0 ? " " : ", ") + std::string(kind.fields[i]);
                }
            }
            std::string where = at;
            where += '/';
            where += key;
            errors_.push_back({std::move(where), std::move(message)});
            fits = false;
        }
        if (shape == "combiner" && part.find("flip_faces") != nullptr) {
            errors_.push_back({at + "/flip_faces", "a combiner has no faces of its own to flip"});
            fits = false;
        }
        std::optional<world::StandardMaterial3D> material = inherited;
        if (const json::Value* name = part.find("material"); name != nullptr) {
            if (const auto found = materials_.find(name->as_string()); found != materials_.end()) {
                material = found->second;
            } else {
                std::string message = quoted(name->as_string()) + " isn't in materials";
                std::vector<std::string_view> names;
                names.reserve(materials_.size());
                for (const auto& entry : materials_) {
                    names.push_back(entry.first);
                }
                if (const std::string_view near = closest_match(name->as_string(), names);
                    !near.empty()) {
                    message += " (did you mean " + quoted(near) + "?)";
                }
                errors_.push_back({at + "/material", std::move(message)});
            }
        }
        std::int64_t count = 1;
        world::Affine3D step;
        if (const json::Value* array = part.find("array"); array != nullptr) {
            count = static_cast<std::int64_t>(array->find("count")->as_double());
            step = transform_of(*array);
        }
        if (!fits) {
            return out;
        }
        std::optional<CsgTree> made = make_shape(part, shape, at, material);
        if (!made) {
            return out;
        }
        if (const json::Value* children = part.find("children"); children != nullptr) {
            const json::Array& list = children->as_array();
            for (std::size_t i = 0; i < list.size(); ++i) {
                std::vector<CsgTree> copies =
                    read_part(list[i], at + "/children/" + std::to_string(i), material);
                made->children.insert(made->children.end(), std::make_move_iterator(copies.begin()),
                                      std::make_move_iterator(copies.end()));
            }
        }
        world::Affine3D place = transform_of(part);
        for (std::int64_t i = 0; i < count; ++i) {
            if (shapes_ >= kMaxShapes) {
                if (!too_many_) {
                    errors_.push_back({at, "makes more than " + std::to_string(kMaxShapes) +
                                               " shapes in all, the most a recipe may"});
                    too_many_ = true;
                }
                break;
            }
            // The first copy's children counted themselves as they were read.
            shapes_ += i == 0 ? 1 : 1 + count_shapes(*made);
            CsgTree copy = *made;
            copy.local = place;
            out.push_back(std::move(copy));
            place = step * place;
        }
        return out;
    }

    [[nodiscard]] const std::map<std::string, world::StandardMaterial3D, std::less<>>&
    materials() const {
        return materials_;
    }

private:
    static std::size_t count_shapes(const CsgTree& tree) {
        std::size_t total = 0;
        for (const CsgTree& child : tree.children) {
            total += 1 + count_shapes(child);
        }
        return total;
    }

    // The part's component, read from its fields as the component's.
    template <typename T>
    std::optional<T> component(const json::Value& part, std::string_view shape,
                               const std::string& at, bool flips = true) {
        json::Value fields = json::Object{};
        for (const std::string_view name : shape_kind(shape).fields) {
            if (name == "bevel" || name == "bevel_segments" || name == "path") {
                continue;
            }
            if (const json::Value* value = part.find(name); value != nullptr) {
                fields[name] = *value;
            }
        }
        if (flips) {
            if (const json::Value* flip = part.find("flip_faces"); flip != nullptr) {
                fields["flip_faces"] = *flip;
            }
        }
        T out;
        std::string path;
        if (auto read = world::from_json(scratch_, fields, out, path); !read) {
            errors_.push_back({at + path, read.error().message});
            return std::nullopt;
        }
        return out;
    }

    // A CSGMesh3D of a primitive mesh or a model.
    static world::CSGMesh3D mesh_shape(const json::Value& part, const std::string& operation) {
        world::CSGMesh3D mesh;
        mesh.operation = operation;
        if (const json::Value* flip = part.find("flip_faces"); flip != nullptr) {
            mesh.flip_faces = flip->as_bool();
        }
        return mesh;
    }

    std::optional<CsgTree> make_shape(const json::Value& part, const std::string& shape,
                                      const std::string& at,
                                      const std::optional<world::StandardMaterial3D>& material) {
        const std::string operation =
            part.find("operation") != nullptr ? part.find("operation")->as_string() : "union";
        CsgTree tree;
        tree.where = at;
        tree.material = material;
        const float bevel = number_of(part, "bevel", 0.0F);
        const int segments = static_cast<int>(number_of(part, "bevel_segments", 4.0F));
        if (shape == "combiner") {
            world::CSGCombiner3D combiner;
            combiner.operation = operation;
            tree.shape = combiner;
        } else if (shape == "box") {
            auto box = component<world::CSGBox3D>(part, shape, at);
            if (!box) {
                return std::nullopt;
            }
            box->operation = operation;
            const float smallest = std::min({box->size.x, box->size.y, box->size.z});
            if (bevel > 0.0F && bevel * 2.0F > smallest) {
                errors_.push_back({at + "/bevel", "is more than half the box's smallest side (" +
                                                      json::write(json::Value(smallest)) +
                                                      " meters)"});
                return std::nullopt;
            }
            if (bevel > 0.0F) {
                tree.shape = mesh_shape(part, operation);
                tree.model = beveled_box(box->size, bevel, segments);
            } else {
                tree.shape = *box;
            }
        } else if (shape == "sphere") {
            auto sphere = component<world::CSGSphere3D>(part, shape, at);
            if (!sphere) {
                return std::nullopt;
            }
            sphere->operation = operation;
            tree.shape = *sphere;
        } else if (shape == "cylinder") {
            auto cylinder = component<world::CSGCylinder3D>(part, shape, at);
            if (!cylinder) {
                return std::nullopt;
            }
            cylinder->operation = operation;
            if (bevel > 0.0F) {
                std::string problem;
                if (cylinder->cone) {
                    problem = "a cone's rim can't be beveled: leave bevel out or cone off";
                } else if (bevel > cylinder->radius) {
                    problem = "is more than the cylinder's radius";
                } else if (bevel * 2.0F > cylinder->height) {
                    problem = "is more than half the cylinder's height";
                } else if (cylinder->sides < 3) {
                    problem = "needs a cylinder of at least 3 sides";
                }
                if (!problem.empty()) {
                    errors_.push_back({at + "/bevel", std::move(problem)});
                    return std::nullopt;
                }
                tree.shape = mesh_shape(part, operation);
                tree.model = beveled_cylinder(cylinder->radius, cylinder->height, cylinder->sides,
                                              bevel, segments, cylinder->smooth_faces);
            } else {
                tree.shape = *cylinder;
            }
        } else if (shape == "torus") {
            auto torus = component<world::CSGTorus3D>(part, shape, at);
            if (!torus) {
                return std::nullopt;
            }
            torus->operation = operation;
            tree.shape = *torus;
        } else if (shape == "polygon") {
            auto polygon = component<world::CSGPolygon3D>(part, shape, at);
            if (!polygon) {
                return std::nullopt;
            }
            polygon->operation = operation;
            polygon->path_local = true;
            if (const json::Value* path = part.find("path"); path != nullptr) {
                world::Path3D curve;
                std::string field;
                if (auto read = world::from_json(scratch_, *path, curve, field); !read) {
                    errors_.push_back({at + "/path" + field, read.error().message});
                    return std::nullopt;
                }
                if (polygon->mode == "path" && curve.points.size() < 2) {
                    errors_.push_back({at + "/path/points", "has fewer than 2 points: the "
                                                            "polygon needs a curve to follow"});
                    return std::nullopt;
                }
                tree.path = std::move(curve);
            } else if (polygon->mode == "path") {
                errors_.push_back({at, "is in path mode with no path: give it one"});
                return std::nullopt;
            }
            tree.shape = *polygon;
        } else if (shape == "capsule" || shape == "prism") {
            Primitive primitive;
            if (shape == "capsule") {
                auto capsule = component<world::CapsuleMesh>(part, shape, at, false);
                if (!capsule) {
                    return std::nullopt;
                }
                primitive = *capsule;
            } else {
                auto prism = component<world::PrismMesh>(part, shape, at, false);
                if (!prism) {
                    return std::nullopt;
                }
                primitive = *prism;
            }
            if (auto checked = check_primitive(primitive); !checked) {
                errors_.push_back({at, checked.error().message});
                return std::nullopt;
            }
            tree.shape = mesh_shape(part, operation);
            tree.primitive = primitive;
        }
        return tree;
    }

    world::World scratch_{components()};
    std::vector<json::ValidationError>& errors_;
    std::map<std::string, world::StandardMaterial3D, std::less<>> materials_;
    std::size_t shapes_ = 0;
    bool too_many_ = false;
};

// An attachment point: its name, the root it is placed in (into Read::roots, or -1 for the
// model's origin) and where.
struct Point {
    std::string name;
    int root = -1;
    world::Affine3D place;
};

// The recipe with its expressions worked out and its parts read, and every error on the way.
struct Read {
    json::Value doc;
    std::vector<Root> roots;
    std::vector<Point> points;
    std::map<std::string, world::StandardMaterial3D, std::less<>> materials;
};

Read read_recipe(const json::Value& doc, std::vector<json::ValidationError>& errors) {
    Read out;
    out.doc = doc;
    if (auto loose = json::validate(doc, model_recipe_schema()); !loose.empty()) {
        errors = std::move(loose);
        return out;
    }
    Evaluator(out.doc, errors).run();
    static const json::Value strict = make_schema(false);
    if (auto checked = json::validate(out.doc, strict); !checked.empty()) {
        errors.insert(errors.end(), checked.begin(), checked.end());
        return out;
    }
    Reader reader(errors);
    reader.read_materials(out.doc);
    out.materials = reader.materials();
    NameSet taken = node_names();
    const json::Array& parts = out.doc.find("parts")->as_array();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const json::Value& part = parts[i];
        const std::string at = "/parts/" + std::to_string(i);
        std::string name = "combiner";
        if (const json::Value* given = part.find("name"); given != nullptr) {
            name = given->as_string();
        } else if (const json::Value* shape = part.find("shape"); shape != nullptr) {
            name = shape->as_string();
        }
        for (CsgTree& tree : reader.read_part(part, at, std::nullopt)) {
            Root root;
            root.name = unique_name(taken, name);
            root.place = tree.local;
            tree.local = {};
            root.tree = std::move(tree);
            out.roots.push_back(std::move(root));
        }
    }
    const json::Value* points = out.doc.find("points");
    if (points == nullptr) {
        return out;
    }
    std::vector<std::string> roots;
    roots.reserve(out.roots.size());
    for (const Root& root : out.roots) {
        roots.push_back(root.name);
    }
    for (std::size_t i = 0; i < points->as_array().size(); ++i) {
        const json::Value& given = points->as_array()[i];
        const std::string at = "/points/" + std::to_string(i);
        Point point;
        point.name = given.find("name")->as_string();
        // Kept as given, so the name a BoneAttachment3D gives is the one the node has.
        if (taken.contains(point.name)) {
            errors.push_back({at + "/name", "a part or point is already named " +
                                                quoted(point.name) +
                                                "; give each point a name of its own"});
            continue;
        }
        if (unique_name(taken, point.name) != point.name) {
            errors.push_back({at + "/name", "a node name can't hold . : @ / \" or %"});
            continue;
        }
        if (const json::Value* part = given.find("part"); part != nullptr) {
            const std::string& name = part->as_string();
            const auto found = std::ranges::find(roots, name);
            if (found == roots.end()) {
                std::string message = "names no part: the parts are";
                for (std::size_t r = 0; r < roots.size(); ++r) {
                    message += (r == 0 ? " " : ", ") + roots[r];
                }
                if (const std::string_view near = closest_match(name, roots); !near.empty()) {
                    message = "names no part (did you mean " + quoted(near) + "?)";
                }
                errors.push_back({at + "/part", std::move(message)});
                continue;
            }
            point.root = static_cast<int>(found - roots.begin());
        }
        point.place = transform_of(given);
        out.points.push_back(std::move(point));
    }
    return out;
}

// A placed node's position, rotation and scale, as a ModelNode keeps them.
void decompose(const world::Affine3D& place, ModelNode& node) {
    const auto length = [](Vec3 v) { return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z)); };
    node.position = place.origin;
    node.scale = {length(place.x), length(place.y), length(place.z)};
    const Vec3 c = cross(place.x, place.y);
    if ((c.x * place.z.x) + (c.y * place.z.y) + (c.z * place.z.z) < 0.0F) {
        node.scale.x = -node.scale.x;
    }
    world::Affine3D rotation;
    rotation.x = node.scale.x != 0.0F ? place.x * (1.0F / node.scale.x) : Vec3{1.0F, 0.0F, 0.0F};
    rotation.y = node.scale.y != 0.0F ? place.y * (1.0F / node.scale.y) : Vec3{0.0F, 1.0F, 0.0F};
    rotation.z = node.scale.z != 0.0F ? place.z * (1.0F / node.scale.z) : Vec3{0.0F, 0.0F, 1.0F};
    node.rotation = world::quaternion(world::euler(rotation));
}

// The image index of a texture path, adding it to the model's images.
int image_of(Model& model, std::string& path) {
    if (path.empty()) {
        return -1;
    }
    const auto found = std::ranges::find_if(
        model.images, [&](const ModelImage& image) { return image.path == path; });
    const int index = found != model.images.end() ? static_cast<int>(found - model.images.begin())
                                                  : static_cast<int>(model.images.size());
    if (found == model.images.end()) {
        model.images.push_back({.path = path, .image = {}});
    }
    path.clear();
    return index;
}

Model build(const Read& read, std::vector<json::ValidationError>* problems) {
    Model model;
    std::vector<world::StandardMaterial3D> made;
    const auto material_index = [&](const SurfaceMaterial& material) {
        if (!material.standard) {
            return -1;
        }
        const auto found = std::ranges::find(made, *material.standard);
        if (found != made.end()) {
            return static_cast<int>(found - made.begin());
        }
        made.push_back(*material.standard);
        ModelMaterial out;
        for (const auto& [name, standard] : read.materials) {
            if (standard == *material.standard) {
                out.name = name;
                break;
            }
        }
        out.standard = *material.standard;
        out.albedo_image = image_of(model, out.standard.albedo_texture);
        out.metallic_image = image_of(model, out.standard.metallic_texture);
        out.roughness_image = image_of(model, out.standard.roughness_texture);
        out.normal_image = image_of(model, out.standard.normal_texture);
        out.emission_image = image_of(model, out.standard.emission_texture);
        model.materials.push_back(std::move(out));
        return static_cast<int>(model.materials.size() - 1);
    };
    for (const Root& root : read.roots) {
        const std::shared_ptr<const CsgMesh> mesh = csg_mesh(root.tree);
        if (problems != nullptr) {
            for (const Problem& problem : mesh->problems) {
                problems->push_back({problem.path, problem.message});
            }
        }
        ModelMesh out;
        out.name = root.name;
        for (const MeshSurface& surface : mesh->surfaces) {
            ModelSurface made_surface;
            made_surface.vertices = surface.vertices;
            made_surface.indices = surface.indices;
            made_surface.material = material_index(surface.material);
            out.surfaces.push_back(std::move(made_surface));
        }
        ModelNode node;
        node.name = root.name;
        decompose(root.place, node);
        const int index = static_cast<int>(model.nodes.size());
        model.nodes.push_back(node);
        world::Transform3D rest;
        rest.position = node.position;
        rest.rotation = world::euler(world::rotation_basis(node.rotation));
        rest.scale = node.scale;
        model.parts.push_back({.node = root.name,
                               .mesh = model.meshes.size(),
                               .transform = world::affine(rest),
                               .node_index = index});
        model.meshes.push_back(std::move(out));
    }
    // Points after the parts, each under its part's node, so the parts keep their indices.
    for (const Point& point : read.points) {
        ModelNode node;
        node.name = point.name;
        node.parent =
            point.root >= 0 ? model.parts[static_cast<std::size_t>(point.root)].node_index : -1;
        decompose(point.place, node);
        model.nodes.push_back(node);
    }
    return model;
}

} // namespace

json::Value model_recipe_schema() {
    static const json::Value schema = make_schema(true);
    return schema;
}

std::vector<json::ValidationError> validate_model_recipe(const json::Value& doc) {
    std::vector<json::ValidationError> errors;
    const Read read = read_recipe(doc, errors);
    if (!errors.empty()) {
        return errors;
    }
    (void)build(read, &errors);
    return errors;
}

Result<Model> build_model_recipe(const json::Value& doc) {
    std::vector<json::ValidationError> errors;
    const Read read = read_recipe(doc, errors);
    if (!errors.empty()) {
        return make_error(
            ErrorCode::InvalidArgument,
            (errors.front().path.empty() ? std::string() : errors.front().path + ": ") +
                errors.front().message);
    }
    return build(read, nullptr);
}

} // namespace slash::scene3d
