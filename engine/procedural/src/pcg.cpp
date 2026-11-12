#include "slash/procedural/pcg.hpp"

#include "json_fields.hpp"

#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace slash::procedural {
namespace {

// A JSON Pointer to key under path.
std::string child(const std::string& path, std::string_view key) {
    std::string out = path;
    out += '/';
    out += key;
    return out;
}

constexpr int kMaxPoints = 100000;     // points a node holds at most
constexpr float kSlopeSlack = 1.0e-5F; // so a flat floor is within [0, 0] degrees
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegrees = kPi / 180.0F;

// ---- the ops ----

struct OpInfo {
    std::string_view name;
    std::string_view kind; // "sampler", "modifier" or "spawner"
    std::vector<std::string_view> fields;
};

const std::vector<OpInfo>& ops() {
    static const std::vector<OpInfo> table{
        {"surface",
         "sampler",
         {"on", "area", "tiles", "count", "density", "spacing", "slope", "height"}},
        {"grid", "sampler", {"area", "spacing", "jitter", "on", "tiles"}},
        {"points", "sampler", {"points"}},
        {"path", "sampler", {"path", "spacing", "side"}},
        {"entities", "sampler", {"ids", "with", "story"}},
        {"filter",
         "modifier",
         {"slope", "height", "noise", "noise_range", "density_range", "keep", "limit", "area"}},
        {"density", "modifier", {"noise", "noise_range", "multiply"}},
        {"prune", "modifier", {"spacing"}},
        {"difference", "modifier", {"to", "targets", "story", "distance"}},
        {"near", "modifier", {"to", "targets", "story", "range"}},
        {"transform",
         "modifier",
         {"offset", "offset_up", "rotation_degrees", "rotate_random", "tilt_random", "scale",
          "scale_random", "align_to_normal"}},
        {"merge", "modifier", {}},
        {"spawn", "spawner", {"prefab", "components", "keep"}},
        {"multimesh", "spawner", {"mesh", "keep"}},
        {"foliage", "spawner", {"type", "keep"}},
        {"command", "spawner", {"command", "params", "keep"}},
        {"route", "spawner", {"closed", "components", "keep"}},
    };
    return table;
}

const OpInfo* op_info(std::string_view op) {
    for (const OpInfo& info : ops()) {
        if (info.name == op) {
            return &info;
        }
    }
    return nullptr;
}

// ---- small vector math ----

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 normalized(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0.0F ? v * (1.0F / l) : Vec3{};
}

// Where a point lies across the ground: (x, z) in 3D, (x, y) in 2D.
Vec2 across(Vec3 p, bool flat) {
    return flat ? Vec2{p.x, p.y} : Vec2{p.x, p.z};
}

float distance_squared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

// Godot's Basis(axis, angle) as a transform's columns: a turn of angle radians about a unit axis.
world::Affine3D axis_angle(Vec3 axis, float angle) {
    const Vec2 cs = cos_sin(angle);
    const float c = cs.x;
    const float s = cs.y;
    const float t = 1.0F - c;
    // Rows of Godot's basis, then put as columns.
    const Vec3 r0{(axis.x * axis.x * t) + c, (axis.x * axis.y * t) - (axis.z * s),
                  (axis.x * axis.z * t) + (axis.y * s)};
    const Vec3 r1{(axis.x * axis.y * t) + (axis.z * s), (axis.y * axis.y * t) + c,
                  (axis.y * axis.z * t) - (axis.x * s)};
    const Vec3 r2{(axis.x * axis.z * t) - (axis.y * s), (axis.y * axis.z * t) + (axis.x * s),
                  (axis.z * axis.z * t) + c};
    world::Affine3D out;
    out.x = {r0.x, r1.x, r2.x};
    out.y = {r0.y, r1.y, r2.y};
    out.z = {r0.z, r1.z, r2.z};
    return out;
}

// A rotation whose y axis is up and whose x and z lie across it, none for straight up.
world::Affine3D standing_on(Vec3 up) {
    const Vec3 forward = std::abs(up.z) < 0.9F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 x = normalized(cross(up, forward));
    world::Affine3D out;
    out.x = x;
    out.y = up;
    out.z = cross(x, up);
    return out;
}

world::Affine3D rotation_of(Vec3 euler_angles) {
    world::Transform3D t;
    t.rotation = euler_angles;
    return world::affine(t);
}

// A grid of points by cell, for spacing and nearest tests.
class Cells {
public:
    explicit Cells(float size) : size_(std::max(size, 1.0e-4F)) {}
    void add(Vec2 p) { cells_[cell_of(p)].push_back(p); }
    // Whether no point added is closer than limit (limit at most the cell size).
    [[nodiscard]] bool clear(Vec2 p, float limit) const {
        const auto [cx, cy] = cell_of(p);
        const float limit2 = limit * limit;
        for (std::int64_t y = cy - 1; y <= cy + 1; ++y) {
            for (std::int64_t x = cx - 1; x <= cx + 1; ++x) {
                const auto it = cells_.find({x, y});
                if (it == cells_.end()) {
                    continue;
                }
                if (std::ranges::any_of(it->second,
                                        [&](Vec2 q) { return distance_squared(p, q) < limit2; })) {
                    return false;
                }
            }
        }
        return true;
    }
    // The distance to the nearest point added within the cell size, else infinity.
    [[nodiscard]] float nearest(Vec2 p) const {
        const auto [cx, cy] = cell_of(p);
        float best = std::numeric_limits<float>::infinity();
        for (std::int64_t y = cy - 1; y <= cy + 1; ++y) {
            for (std::int64_t x = cx - 1; x <= cx + 1; ++x) {
                const auto it = cells_.find({x, y});
                if (it == cells_.end()) {
                    continue;
                }
                for (const Vec2 q : it->second) {
                    best = std::min(best, distance_squared(p, q));
                }
            }
        }
        return std::sqrt(best);
    }
    [[nodiscard]] bool empty() const { return cells_.empty(); }

private:
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> cell_of(Vec2 p) const {
        return {static_cast<std::int64_t>(std::floor(p.x / size_)),
                static_cast<std::int64_t>(std::floor(p.y / size_))};
    }
    float size_;
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<Vec2>> cells_;
};

// ---- the file format ----

json::Value number(std::string_view description) {
    json::Value field;
    field["type"] = "number";
    field["description"] = description;
    return field;
}

json::Value numbers(int count, std::string_view description) {
    json::Value field;
    field["type"] = "array";
    field["items"]["type"] = "number";
    field["minItems"] = count;
    field["maxItems"] = count;
    field["description"] = description;
    return field;
}

json::Value names(std::string_view description) {
    json::Value field;
    field["type"] = "array";
    field["items"]["type"] = "string";
    field["description"] = description;
    return field;
}

json::Value text(std::string_view description) {
    json::Value field;
    field["type"] = "string";
    field["description"] = description;
    return field;
}

json::Value node_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"name", "op"};
    json::Value& p = schema["properties"];
    p["name"]["type"] = "string";
    p["name"]["pattern"] = "^[A-Za-z0-9_-]{1,32}$";
    p["name"]["description"] = R"(The node's name: letters, digits, "_" and "-".)";
    p["op"]["type"] = "string";
    json::Array op_names;
    for (const OpInfo& info : ops()) {
        op_names.emplace_back(info.name);
    }
    p["op"]["enum"] = std::move(op_names);
    p["op"]["description"] =
        "What the node does. Samplers make points: surface (over the ground), grid, points, path "
        "(along a Path3D or Path2D) and entities (at entities, by ID, component or storyboard "
        "entry). Modifiers keep, thin, move and turn the points of the nodes in from: filter, "
        "density, prune, difference, near, transform and merge. Spawners place things at them: "
        "spawn (an entity a point), multimesh, foliage (instances of a .foliage.json type), "
        "command (any command, its params written per point) and route (a path through them).";
    p["from"] = names("Earlier nodes whose points this one takes, one after another. Every op but "
                      "the samplers needs it.");
    p["description"] = text("What the node is for, for whoever reads the graph.");
    json::Value& expect = p["expect"];
    expect["type"] = "object";
    expect["additionalProperties"] = false;
    expect["description"] = "What pcg.check holds the node's points to.";
    expect["properties"]["min"]["type"] = "integer";
    expect["properties"]["min"]["minimum"] = 0;
    expect["properties"]["min"]["description"] = "The fewest points.";
    expect["properties"]["max"]["type"] = "integer";
    expect["properties"]["max"]["minimum"] = 0;
    expect["properties"]["max"]["description"] = "The most points.";
    expect["properties"]["density"] =
        numbers(2, "[min, max] points per square unit of the area they cover (expect's area, "
                   "else their bounds).");
    expect["properties"]["area"] = numbers(4, "The area density is measured over.");
    expect["properties"]["reachable"]["type"] = "boolean";
    expect["properties"]["reachable"]["description"] =
        "Every point can be walked to from pcg.check's reach_from, by the navigation map.";

    // Samplers.
    p["on"] = names("surface, grid: IDs of the entities whose ground to sample, with those under "
                    "them: in 3D their meshes, CSG, terrains, grid maps and static bodies' "
                    "shapes; in 2D TileMapLayers' cells.");
    p["area"] = numbers(4, "[min x, min z, max x, max z] in 3D or [min x, min y, max x, max y] in "
                           "2D, in world space: a grid's cells, or only points within it.");
    p["tiles"] = names("surface, grid (2D): only cells whose legend key or tile is one of these.");
    p["count"]["type"] = "integer";
    p["count"]["minimum"] = 0;
    p["count"]["maximum"] = kMaxPoints;
    p["count"]["description"] = "surface: how many points.";
    p["density"]["type"] = "number";
    p["density"]["minimum"] = 0;
    p["density"]["description"] = "surface: points per square unit of ground.";
    p["spacing"]["type"] = "number";
    p["spacing"]["minimum"] = 0;
    p["spacing"]["description"] = "surface, prune: the least distance between two points; grid, "
                                  "path: the distance between them.";
    p["slope"] = numbers(2, "surface, filter (3D): [min, max] degrees of the ground from flat.");
    p["height"] = numbers(2, "surface, filter: [min, max] height (y).");
    p["jitter"] = detail::number_field(0.0F,
                                       "grid: move each point by up to this share of "
                                       "spacing either way.",
                                       0.0, 1.0);
    json::Value& point_list = p["points"];
    point_list["type"] = "array";
    point_list["items"]["type"] = "array";
    point_list["items"]["items"]["type"] = "number";
    point_list["items"]["minItems"] = 2;
    point_list["items"]["maxItems"] = 3;
    point_list["maxItems"] = kMaxPoints;
    point_list["description"] = "points: [x, y, z] in 3D or [x, y] in 2D, in world space.";
    p["path"] = text("path: ID of the Path3D or Path2D entity whose curve to follow.");
    p["side"] = number("path: move each point this far to the curve's side (right in 3D, the "
                       "curve's left-hand normal in 2D).");
    p["ids"] = names("entities: these entities.");
    p["with"] = names("entities: entities that have all these components.");
    p["story"] = names("entities, difference, near: entities whose StoryRef names one of these "
                       "storyboard entries, such as \"place/harbor\".");

    // Modifiers.
    p["noise"] = noise_schema();
    p["noise"]["description"] = "filter, density: noise sampled at (x, z) in 3D or (x, y) in 2D.";
    p["noise_range"] = numbers(2, "filter: keep where the noise is within [min, max]; density: "
                                  "the noise mapped from [min, max] to 0..1.");
    p["density_range"] = numbers(2, "filter: keep the points whose density is within [min, max].");
    p["keep"] = detail::number_field(1.0F,
                                     "filter, spawners: keep this share of the points, "
                                     "chosen at random.",
                                     0.0, 1.0);
    p["limit"]["type"] = "integer";
    p["limit"]["minimum"] = 0;
    p["limit"]["description"] = "filter: keep at most this many, the first in order.";
    p["multiply"] = detail::boolean_field(false, "density: multiply the density the points have "
                                                 "instead of setting it.");
    p["to"] = names("difference, near: earlier nodes whose points to measure from.");
    p["targets"] = names("difference, near: entities to measure from.");
    p["distance"]["type"] = "number";
    p["distance"]["minimum"] = 0;
    p["distance"]["description"] = "difference: drop the points closer than this to any.";
    p["range"] = numbers(2, "near: keep the points whose nearest is within [min, max] of them.");
    json::Value& offset = p["offset"];
    offset["type"] = "array";
    offset["items"]["type"] = "number";
    offset["minItems"] = 2;
    offset["maxItems"] = 3;
    offset["description"] = "transform: move by [x, y, z] in 3D or [x, y] in 2D, in world space.";
    p["offset_up"] = number("transform: move along each point's up by this much; negative sinks "
                            "it.");
    p["rotation_degrees"] = numbers(3, "transform: turn each point by these Euler angles in "
                                       "degrees (Godot's YXZ; 2D turns by z).");
    p["rotate_random"] = detail::number_field(0.0F, "transform: turn about the up axis by up to "
                                                    "this many half turns either way: 1 is any "
                                                    "heading.");
    p["tilt_random"] = detail::number_field(0.0F, "transform (3D): tilt by up to this many half "
                                                  "turns either way.");
    p["scale"] = detail::number_field(1.0F, "transform: multiply each point's scale by this.");
    p["scale_random"] =
        detail::number_field(0.0F, "transform: add up to this much to scale either way.");
    p["align_to_normal"] = detail::boolean_field(false, "transform (3D): stand each point along "
                                                        "the ground's normal first.");

    // Spawners.
    p["prefab"] = text("spawn: project path of the .prefab.json each entity extends.");
    p["components"]["type"] = "object";
    p["components"]["description"] =
        "spawn, route: components each entity has, by name, over the prefab's; their values may "
        "use the same $ names as command params.";
    p["mesh"] = text("multimesh (3D): project path of the model drawn at every point.");
    p["type"] = text("foliage: project path of the .foliage.json type painted at the points.");
    p["command"] = text("command: the command called once a point, such as \"creature.place\".");
    p["params"]["type"] = "object";
    p["params"]["description"] =
        "command: its params. Strings that are a $ name become the point's: $position, $local "
        "(in into's space), $rotation, $yaw (radians about up, or the 2D rotation), $scale, "
        "$density, $index, $seed, $id (<into>_<node>_<index>) and $parent (into); "
        "{\"$pick\": [..]} picks one value by the point's seed.";
    p["closed"] = detail::boolean_field(false, "route: join the last point back to the first.");
    return schema;
}

std::optional<Vec2> read_range(const json::Value& doc, std::string_view key) {
    const json::Value* value = doc.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    const json::Array& items = value->as_array();
    return Vec2{static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double())};
}

std::vector<std::string> read_names(const json::Value& doc, std::string_view key) {
    std::vector<std::string> out;
    if (const json::Value* value = doc.find(key); value != nullptr) {
        for (const json::Value& name : value->as_array()) {
            out.push_back(name.as_string());
        }
    }
    return out;
}

bool has(const json::Value& node, std::string_view key) {
    return node.find(key) != nullptr;
}

} // namespace

bool pcg_is_sampler(std::string_view op) {
    const OpInfo* info = op_info(op);
    return info != nullptr && info->kind == "sampler";
}

bool pcg_is_spawner(std::string_view op) {
    const OpInfo* info = op_info(op);
    return info != nullptr && info->kind == "spawner";
}

json::Value pcg_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine procedural graph";
    schema["description"] =
        "Steps over points that pcg.run follows to fill a world, as Unreal's PCG graphs: "
        "samplers make points, modifiers keep, thin, move and turn them, and spawners place "
        "things at them.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "nodes"};
    json::Value& p = schema["properties"];
    p["format"]["const"] = 1;
    p["format"]["description"] = "Format version. Must be 1.";
    p["description"] = text("What the graph makes, for whoever reads it.");
    p["space"]["type"] = "string";
    p["space"]["enum"] = json::Array{"3d", "2d"};
    p["space"]["default"] = "3d";
    p["space"]["description"] = "Whether points are 3D (Transform3D) or 2D (Transform2D).";
    p["seed"] = detail::integer_field(0,
                                      "Seed of every random choice: the same seed over the same "
                                      "world makes the same points.",
                                      0, 4294967295);
    json::Value& budget = p["budget"];
    budget["type"] = "object";
    budget["additionalProperties"] = false;
    budget["description"] = "Limits pcg.run keeps to and pcg.check reports.";
    budget["properties"]["points"]["type"] = "integer";
    budget["properties"]["points"]["minimum"] = 0;
    budget["properties"]["points"]["description"] = "The most points any node may hold.";
    budget["properties"]["entities"]["type"] = "integer";
    budget["properties"]["entities"]["minimum"] = 0;
    budget["properties"]["entities"]["description"] =
        "The most entities the spawners may make in all (a foliage, multimesh or route node "
        "makes one).";
    p["nodes"]["type"] = "array";
    p["nodes"]["minItems"] = 1;
    p["nodes"]["items"] = node_schema();
    p["nodes"]["description"] = "The steps, in order: a node reads only nodes before it.";
    return schema;
}

std::vector<json::ValidationError> validate_pcg(const json::Value& doc, const std::string& at) {
    std::vector<json::ValidationError> errors = json::validate(doc, pcg_schema());
    if (errors.empty()) {
        const json::Value* space = doc.find("space");
        const bool flat = space != nullptr && space->as_string() == "2d";
        std::set<std::string> earlier;
        const json::Array& nodes = doc.find("nodes")->as_array();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const json::Value& node = nodes[i];
            const std::string path = "/nodes/" + std::to_string(i);
            const std::string name = node.find("name")->as_string();
            const std::string op = node.find("op")->as_string();
            const OpInfo& info = *op_info(op);
            if (!world::World::is_valid_id(name)) {
                errors.push_back({path + "/name", "\"" + name +
                                                      "\" isn't a name: up to 32 letters, "
                                                      "digits, \"_\" and \"-\""});
            }
            // Fields the op doesn't take.
            for (const auto& entry : node.as_object()) {
                const std::string& key = entry.first;
                static constexpr std::array<std::string_view, 5> kCommon{"name", "op", "from",
                                                                         "expect", "description"};
                if (std::ranges::find(kCommon, key) == kCommon.end() &&
                    std::ranges::find(info.fields, key) == info.fields.end()) {
                    std::string allowed;
                    for (const std::string_view field : info.fields) {
                        allowed += (allowed.empty() ? "" : ", ") + std::string(field);
                    }
                    std::string message = op;
                    message += " doesn't take ";
                    message += key;
                    if (allowed.empty()) {
                        message += ": it takes only from";
                    } else {
                        message += "; it takes ";
                        message += allowed;
                    }
                    errors.push_back({child(path, key), std::move(message)});
                }
            }
            // from: none for samplers, earlier nodes for the rest.
            const std::vector<std::string> from = read_names(node, "from");
            if (info.kind == "sampler" && has(node, "from")) {
                errors.push_back({path + "/from", op + " makes its own points: leave out from"});
            } else if (info.kind != "sampler" && from.empty()) {
                errors.push_back({path, op + " needs from: the earlier nodes whose points it "
                                             "takes"});
            }
            const auto check_names = [&](std::string_view key,
                                         const std::vector<std::string>& list) {
                for (std::size_t k = 0; k < list.size(); ++k) {
                    if (!earlier.contains(list[k])) {
                        errors.push_back({path + "/" + std::string(key) + "/" + std::to_string(k),
                                          "no earlier node is named \"" + list[k] + "\""});
                    }
                }
            };
            check_names("from", from);
            check_names("to", read_names(node, "to"));

            // What each op needs.
            const auto need = [&](std::string_view key, std::string_view why) {
                if (!has(node, key)) {
                    errors.push_back(
                        {path, op + " needs " + std::string(key) + ": " + std::string(why)});
                }
            };
            if (op == "surface") {
                if (!has(node, "on") && !has(node, "area")) {
                    errors.push_back({path, "say where: give on, area or both"});
                }
                if (has(node, "count") == has(node, "density")) {
                    errors.push_back({path, "give count or density, one of them"});
                }
            } else if (op == "grid") {
                need("area", "the rect its cells cover");
                need("spacing", "the distance between points");
            } else if (op == "points") {
                need("points", "where they are");
            } else if (op == "path") {
                need("path", "the Path3D or Path2D entity to follow");
                need("spacing", "the distance between points along the curve");
            } else if (op == "entities") {
                if (!has(node, "ids") && !has(node, "with") && !has(node, "story")) {
                    errors.push_back({path, "say which: give ids, with, story or several"});
                }
            } else if (op == "density") {
                need("noise", "the noise that sets the density");
            } else if (op == "prune") {
                need("spacing", "the least distance between points kept");
            } else if (op == "difference" || op == "near") {
                if (!has(node, "to") && !has(node, "targets") && !has(node, "story")) {
                    errors.push_back({path, "say from what: give to, targets, story or several"});
                }
                need(op == "near" ? "range" : "distance",
                     op == "near" ? "[min, max] from the nearest" : "how far to stay away");
            } else if (op == "spawn") {
                if (!has(node, "prefab") && !has(node, "components")) {
                    errors.push_back({path, "spawn needs prefab, components or both"});
                }
            } else if (op == "multimesh") {
                need("mesh", "the model drawn at every point");
                if (flat) {
                    errors.push_back({path + "/op", "a multimesh is 3D: use spawn in 2D"});
                }
            } else if (op == "foliage") {
                need("type", "the .foliage.json type to paint");
            } else if (op == "command") {
                need("command", "the command to call at each point");
                need("params", "the command's params");
            }

            // Values.
            if (flat) {
                for (const std::string_view key : {"slope", "tilt_random", "align_to_normal"}) {
                    if (has(node, key)) {
                        errors.push_back(
                            {path + "/" + std::string(key), std::string(key) + " is 3D only"});
                    }
                }
            }
            for (const std::string_view key :
                 {"slope", "height", "noise_range", "density_range", "range"}) {
                if (const std::optional<Vec2> range = read_range(node, key);
                    range && range->x > range->y) {
                    errors.push_back({path + "/" + std::string(key), "expected [min, max]"});
                }
            }
            if (op == "density") {
                if (const std::optional<Vec2> range = read_range(node, "noise_range");
                    range && range->x >= range->y) {
                    errors.push_back({path + "/noise_range", "expected min below max"});
                }
            }
            for (const std::string& key : {std::string("area"), std::string("expect/area")}) {
                const json::Value* area = node.find("area");
                if (key != "area") {
                    const json::Value* expect = node.find("expect");
                    area = expect != nullptr ? expect->find("area") : nullptr;
                }
                if (area != nullptr) {
                    const json::Array& a = area->as_array();
                    if (a[0].as_double() >= a[2].as_double() ||
                        a[1].as_double() >= a[3].as_double()) {
                        errors.push_back({child(path, key), "expected min below max on each axis"});
                    }
                }
            }
            if (op == "grid" || op == "path") {
                if (const json::Value* spacing = node.find("spacing");
                    spacing != nullptr && spacing->as_double() <= 0.0) {
                    errors.push_back({path + "/spacing", "expected more than 0"});
                }
            }
            if (const json::Value* points = node.find("points"); points != nullptr) {
                const json::Array& list = points->as_array();
                for (std::size_t k = 0; k < list.size(); ++k) {
                    if (list[k].as_array().size() != (flat ? 2U : 3U)) {
                        errors.push_back({path + "/points/" + std::to_string(k),
                                          flat ? "expected [x, y]" : "expected [x, y, z]"});
                    }
                }
            }
            if (const json::Value* offset = node.find("offset");
                offset != nullptr && offset->as_array().size() != (flat ? 2U : 3U)) {
                errors.push_back(
                    {path + "/offset", flat ? "expected [x, y]" : "expected [x, y, z]"});
            }
            if (const json::Value* noise = node.find("noise"); noise != nullptr) {
                for (json::ValidationError& error : validate_noise(*noise, path + "/noise")) {
                    errors.push_back(std::move(error));
                }
            }
            if (const json::Value* expect = node.find("expect"); expect != nullptr) {
                const json::Value* min = expect->find("min");
                const json::Value* max = expect->find("max");
                if (min != nullptr && max != nullptr && min->as_int() > max->as_int()) {
                    errors.push_back({path + "/expect/max", "expected max at least min"});
                }
                if (const std::optional<Vec2> range = read_range(*expect, "density");
                    range && range->x > range->y) {
                    errors.push_back({path + "/expect/density", "expected [min, max]"});
                }
            }
            if (!earlier.insert(name).second) {
                errors.push_back({path + "/name", "\"" + name + "\" names an earlier node too"});
            }
        }
    }
    for (json::ValidationError& error : errors) {
        error.path = at + error.path;
    }
    return errors;
}

Result<PcgGraph> parse_pcg(const json::Value& doc) {
    if (const auto errors = validate_pcg(doc); !errors.empty()) {
        return detail::first_error(errors);
    }
    PcgGraph graph;
    if (const json::Value* space = doc.find("space"); space != nullptr) {
        graph.space = space->as_string();
    }
    if (const json::Value* seed = doc.find("seed"); seed != nullptr) {
        graph.seed = static_cast<std::uint32_t>(seed->as_int());
    }
    if (const json::Value* budget = doc.find("budget"); budget != nullptr) {
        if (const json::Value* points = budget->find("points"); points != nullptr) {
            graph.budget.points = static_cast<int>(points->as_int());
        }
        if (const json::Value* entities = budget->find("entities"); entities != nullptr) {
            graph.budget.entities = static_cast<int>(entities->as_int());
        }
    }
    for (const json::Value& entry : doc.find("nodes")->as_array()) {
        PcgNode node;
        node.name = entry.find("name")->as_string();
        node.op = entry.find("op")->as_string();
        node.from = read_names(entry, "from");
        node.fields = entry;
        graph.nodes.push_back(std::move(node));
    }
    return graph;
}

// ---- randomness ----

std::uint32_t pcg_hash(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    // Chris Wellons' lowbias32 finalizer, folded over each value.
    const auto mix = [](std::uint32_t h) {
        h ^= h >> 16U;
        h *= 0x7FEB352DU;
        h ^= h >> 15U;
        h *= 0x846CA68BU;
        h ^= h >> 16U;
        return h;
    };
    std::uint32_t h = mix(a + 0x9E3779B9U);
    h = mix(h ^ b);
    return mix(h ^ (c + 0x85EBCA6BU));
}

float pcg_unit(std::uint32_t seed, std::uint32_t salt) {
    return static_cast<float>(pcg_hash(seed, salt) >> 8U) * (1.0F / 16777216.0F);
}

// ---- the point steps ----

std::vector<PcgPoint> pcg_grid(const std::array<float, 4>& area, float spacing, float jitter,
                               std::uint32_t seed, bool flat) {
    std::vector<PcgPoint> points;
    if (spacing <= 0.0F) {
        return points;
    }
    const auto columns = static_cast<std::int64_t>(std::floor((area[2] - area[0]) / spacing));
    const auto rows = static_cast<std::int64_t>(std::floor((area[3] - area[1]) / spacing));
    const float start_x =
        area[0] + (((area[2] - area[0]) - (static_cast<float>(columns - 1) * spacing)) * 0.5F);
    const float start_y =
        area[1] + (((area[3] - area[1]) - (static_cast<float>(rows - 1) * spacing)) * 0.5F);
    for (std::int64_t j = 0; j < rows; ++j) {
        for (std::int64_t i = 0; i < columns; ++i) {
            if (std::cmp_greater_equal(points.size(), kMaxPoints)) {
                return points;
            }
            PcgPoint point;
            point.seed =
                pcg_hash(seed, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
            const float jx = ((pcg_unit(point.seed, 1U) * 2.0F) - 1.0F) * jitter * spacing;
            const float jy = ((pcg_unit(point.seed, 2U) * 2.0F) - 1.0F) * jitter * spacing;
            const float x = start_x + (static_cast<float>(i) * spacing) + jx;
            const float y = start_y + (static_cast<float>(j) * spacing) + jy;
            point.position = flat ? Vec3{x, y, 0.0F} : Vec3{x, 0.0F, y};
            point.normal = flat ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
            points.push_back(point);
        }
    }
    return points;
}

std::vector<PcgPoint> pcg_filter(std::span<const PcgPoint> points, const PcgFilter& filter,
                                 std::uint32_t salt, bool flat) {
    std::vector<PcgPoint> kept;
    std::optional<Noise> noise;
    if (filter.noise) {
        noise.emplace(*filter.noise);
    }
    float slope_min = 1.0F; // the cosines bounding the normal's height
    float slope_max = -1.0F;
    if (filter.slope && !flat) {
        slope_min = cos_sin(filter.slope->x * kDegrees).x + kSlopeSlack;
        slope_max = cos_sin(filter.slope->y * kDegrees).x - kSlopeSlack;
    }
    for (const PcgPoint& point : points) {
        if (filter.limit && std::cmp_greater_equal(kept.size(), *filter.limit)) {
            break;
        }
        const Vec2 at = across(point.position, flat);
        if (filter.area && (at.x < (*filter.area)[0] || at.y < (*filter.area)[1] ||
                            at.x > (*filter.area)[2] || at.y > (*filter.area)[3])) {
            continue;
        }
        if (filter.slope && !flat && (point.normal.y > slope_min || point.normal.y < slope_max)) {
            continue;
        }
        if (filter.height &&
            (point.position.y < filter.height->x || point.position.y > filter.height->y)) {
            continue;
        }
        if (noise) {
            const float value = noise->get_noise_2d(at.x, at.y);
            if (value < filter.noise_range.x || value > filter.noise_range.y) {
                continue;
            }
        }
        if (filter.density &&
            (point.density < filter.density->x || point.density > filter.density->y)) {
            continue;
        }
        if (filter.keep && pcg_unit(point.seed, salt) >= *filter.keep) {
            continue;
        }
        kept.push_back(point);
    }
    return kept;
}

void pcg_density(std::vector<PcgPoint>& points, const NoiseSettings& noise_settings, Vec2 range,
                 bool multiply, bool flat) {
    const Noise noise(noise_settings);
    const float span = to_float(range.y - range.x);
    for (PcgPoint& point : points) {
        const Vec2 at = across(point.position, flat);
        const float value = noise.get_noise_2d(at.x, at.y);
        const float density =
            span > 0.0F ? std::clamp((value - to_float(range.x)) / span, 0.0F, 1.0F) : 0.0F;
        point.density = multiply ? point.density * density : density;
    }
}

std::vector<PcgPoint> pcg_prune(std::span<const PcgPoint> points, float spacing, bool flat) {
    if (spacing <= 0.0F) {
        return {points.begin(), points.end()};
    }
    std::vector<std::size_t> order(points.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(
        order, [&](std::size_t a, std::size_t b) { return points[a].density > points[b].density; });
    Cells cells(spacing);
    std::vector<bool> kept(points.size(), false);
    for (const std::size_t i : order) {
        const Vec2 at = across(points[i].position, flat);
        if (cells.clear(at, spacing)) {
            cells.add(at);
            kept[i] = true;
        }
    }
    std::vector<PcgPoint> out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (kept[i]) {
            out.push_back(points[i]);
        }
    }
    return out;
}

std::vector<PcgPoint> pcg_difference(std::span<const PcgPoint> points, std::span<const Vec3> others,
                                     float distance, bool flat) {
    if (distance <= 0.0F || others.empty()) {
        return {points.begin(), points.end()};
    }
    Cells cells(distance);
    for (const Vec3 other : others) {
        cells.add(across(other, flat));
    }
    std::vector<PcgPoint> out;
    for (const PcgPoint& point : points) {
        if (cells.clear(across(point.position, flat), distance)) {
            out.push_back(point);
        }
    }
    return out;
}

std::vector<PcgPoint> pcg_near(std::span<const PcgPoint> points, std::span<const Vec3> targets,
                               Vec2 range, bool flat) {
    std::vector<PcgPoint> out;
    if (targets.empty()) {
        return out;
    }
    // Cells as large as the farthest kept, so the nearest within reach is in the 3 by 3 around.
    Cells cells(std::max(to_float(range.y), 1.0e-3F));
    for (const Vec3 target : targets) {
        cells.add(across(target, flat));
    }
    for (const PcgPoint& point : points) {
        const float d = cells.nearest(across(point.position, flat));
        if (d >= range.x && d <= range.y) {
            out.push_back(point);
        }
    }
    return out;
}

void pcg_transform(std::vector<PcgPoint>& points, const PcgTransform& transform, std::uint32_t salt,
                   bool flat) {
    for (PcgPoint& point : points) {
        // Each random value from its own draw, in a fixed order.
        const float heading_unit = pcg_unit(point.seed, salt);
        const float tilt_z_unit = pcg_unit(point.seed, salt + 1U);
        const float tilt_x_unit = pcg_unit(point.seed, salt + 2U);
        const float scale_unit = pcg_unit(point.seed, salt + 3U);
        const float heading = ((heading_unit * 2.0F) - 1.0F) * transform.rotate_random * kPi;
        const float tilt_z = ((tilt_z_unit * 2.0F) - 1.0F) * transform.tilt_random * kPi;
        const float tilt_x = ((tilt_x_unit * 2.0F) - 1.0F) * transform.tilt_random * kPi;
        point.scale *= transform.scale + (((scale_unit * 2.0F) - 1.0F) * transform.scale_random);
        point.position = point.position + transform.offset;
        if (flat) {
            point.rotation.z += transform.rotation.z + heading;
            const Vec2 cs = cos_sin(point.rotation.z);
            // A 2D point's up is its -y: y grows down the screen, as Godot's.
            point.position = point.position + (Vec3{cs.y, -cs.x, 0.0F} * transform.offset_up);
            continue;
        }
        world::Affine3D basis = transform.align_to_normal ? standing_on(normalized(point.normal))
                                                          : rotation_of(point.rotation);
        basis = basis * rotation_of(transform.rotation);
        const Vec3 up = normalized(basis.y);
        basis = axis_angle(up, heading) * basis;
        basis = axis_angle(normalized(basis.z), tilt_z) * basis;
        basis = axis_angle(normalized(basis.x), tilt_x) * basis;
        point.rotation = world::euler(basis);
        point.position = point.position + (normalized(basis.y) * transform.offset_up);
    }
}

std::vector<PcgPoint> pcg_route_order(std::span<const PcgPoint> points, bool flat) {
    std::vector<PcgPoint> out;
    if (points.empty()) {
        return out;
    }
    std::vector<bool> visited(points.size(), false);
    std::size_t current = 0;
    visited[0] = true;
    out.push_back(points[0]);
    for (std::size_t step = 1; step < points.size(); ++step) {
        std::size_t best = points.size();
        float best_distance = std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (visited[i]) {
                continue;
            }
            const Vec3 d = points[i].position - points[current].position;
            const float distance = flat ? (d.x * d.x) + (d.y * d.y) : dot(d, d);
            if (distance < best_distance) {
                best_distance = distance;
                best = i;
            }
        }
        visited[best] = true;
        out.push_back(points[best]);
        current = best;
    }
    return out;
}

std::array<float, 4> pcg_bounds(std::span<const PcgPoint> points, bool flat) {
    if (points.empty()) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    const Vec2 first = across(points.front().position, flat);
    const float x = to_float(first.x);
    const float y = to_float(first.y);
    std::array<float, 4> out{x, y, x, y};
    for (const PcgPoint& point : points) {
        const Vec2 at = across(point.position, flat);
        out[0] = std::min(out[0], static_cast<float>(at.x));
        out[1] = std::min(out[1], static_cast<float>(at.y));
        out[2] = std::max(out[2], static_cast<float>(at.x));
        out[3] = std::max(out[3], static_cast<float>(at.y));
    }
    return out;
}

} // namespace slash::procedural
