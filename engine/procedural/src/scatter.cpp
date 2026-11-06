#include "slash/procedural/scatter.hpp"

#include "json_fields.hpp"

#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <set>
#include <tuple>
#include <utility>

namespace slash::procedural {
namespace {

constexpr int kTriesPerPlace = 30;     // tries for each place wanted before giving up
constexpr int kMaxPlaces = 10000;      // places a layer makes at most
constexpr float kMinArea = 1.0e-6F;    // triangles smaller than this are left out
constexpr float kSlopeSlack = 1.0e-5F; // so a flat floor is within [0, 0] degrees
constexpr float kPi = std::numbers::pi_v<float>;

// PCG32 (O'Neill, pcg-random.org, Apache 2.0): the same numbers on every machine.
class Random {
public:
    Random(std::uint64_t seed, std::uint64_t stream) : increment_((stream << 1U) | 1U) {
        next();
        state_ += seed;
        next();
    }
    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = (old * 6364136223846793005ULL) + increment_;
        const auto shifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
        const auto rotation = static_cast<std::uint32_t>(old >> 59U);
        return (shifted >> rotation) | (shifted << ((32U - rotation) & 31U));
    }
    // From 0 up to 1, in steps of 2^-24.
    float unit() { return static_cast<float>(next() >> 8U) * (1.0F / 16777216.0F); }
    // As Godot's Math::random(from, to).
    float range(float from, float to) { return (unit() * (to - from)) + from; }

private:
    std::uint64_t state_ = 0;
    std::uint64_t increment_;
};

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? v * (1.0F / l) : Vec3{};
}

// A rotation basis by rows, as Godot's Basis.
struct Basis {
    std::array<Vec3, 3> rows{Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                             Vec3{0.0F, 0.0F, 1.0F}};

    [[nodiscard]] Vec3 column(int i) const {
        const auto at = [i](Vec3 row) {
            const std::array<float, 3> values{static_cast<float>(row.x), static_cast<float>(row.y),
                                              static_cast<float>(row.z)};
            return values[static_cast<std::size_t>(i)];
        };
        return {at(rows[0]), at(rows[1]), at(rows[2])};
    }
    [[nodiscard]] Basis operator*(const Basis& o) const {
        Basis out;
        for (int r = 0; r < 3; ++r) {
            const Vec3 row = rows[static_cast<std::size_t>(r)];
            out.rows[static_cast<std::size_t>(r)] = {dot(row, o.column(0)), dot(row, o.column(1)),
                                                     dot(row, o.column(2))};
        }
        return out;
    }
};

// Godot's Basis(axis, angle), set_axis_angle: a turn of angle radians about a unit axis.
Basis axis_angle(Vec3 axis, float angle) {
    const Vec3 sq{axis.x * axis.x, axis.y * axis.y, axis.z * axis.z};
    const Vec2 cs = cos_sin(angle);
    const float cosine = cs.x;
    const float sine = cs.y;
    Basis b;
    b.rows[0].x = sq.x + (cosine * (1.0F - sq.x));
    b.rows[1].y = sq.y + (cosine * (1.0F - sq.y));
    b.rows[2].z = sq.z + (cosine * (1.0F - sq.z));
    const float t = 1.0F - cosine;
    float xyzt = axis.x * axis.y * t;
    float zyxs = axis.z * sine;
    b.rows[0].y = xyzt - zyxs;
    b.rows[1].x = xyzt + zyxs;
    xyzt = axis.x * axis.z * t;
    zyxs = axis.y * sine;
    b.rows[0].z = xyzt + zyxs;
    b.rows[2].x = xyzt - zyxs;
    xyzt = axis.y * axis.z * t;
    zyxs = axis.x * sine;
    b.rows[1].z = xyzt - zyxs;
    b.rows[2].y = xyzt + zyxs;
    return b;
}

// A basis whose y axis is up and whose x and z lie across it, the identity for straight up.
Basis standing_on(Vec3 up) {
    const Vec3 forward = std::abs(up.z) < 0.9F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 x = normalized(cross(up, forward));
    const Vec3 z = cross(x, up);
    Basis b;
    b.rows = {Vec3{x.x, up.x, z.x}, Vec3{x.y, up.y, z.y}, Vec3{x.z, up.z, z.z}};
    return b;
}

// Godot's Plane(a, b, c).normal, turned to face up.
Vec3 up_normal(const ScatterTriangle& t) {
    Vec3 n = normalized(cross(t.a - t.c, t.a - t.b));
    if (n.y < 0.0F) {
        n = n * -1.0F;
    }
    return n;
}

// The triangle's area: half its edges' cross product.
float area_of(const ScatterTriangle& t) {
    return 0.5F * length(cross(t.b - t.a, t.c - t.a));
}

// Across the ground: x and z in 3D, x and y in 2D.
Vec2 across(Vec3 p, bool flat) {
    return flat ? Vec2{p.x, p.y} : Vec2{p.x, p.z};
}

// The parts of a triangle inside the area, as triangles: the polygon clipped against each side
// (Sutherland-Hodgman), then fanned. Points stay on the triangle's plane.
void clip(const ScatterTriangle& t, const std::array<float, 4>& area, bool flat,
          std::vector<ScatterTriangle>& out) {
    std::vector<Vec3> polygon{t.a, t.b, t.c};
    // Each side as (axis across the ground, limit, whether inside is above it).
    const std::array<std::tuple<int, float, bool>, 4> sides{
        std::tuple{0, area[0], true}, std::tuple{1, area[1], true}, std::tuple{0, area[2], false},
        std::tuple{1, area[3], false}};
    for (const auto& [axis, limit, above] : sides) {
        const auto signed_distance = [&, side_axis = axis, side_limit = limit,
                                      side_above = above](Vec3 p) {
            const Vec2 g = across(p, flat);
            const float v = side_axis == 0 ? g.x : g.y;
            return side_above ? v - side_limit : side_limit - v;
        };
        std::vector<Vec3> kept;
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const Vec3 from = polygon[i];
            const Vec3 to = polygon[(i + 1) % polygon.size()];
            const float df = signed_distance(from);
            const float dt = signed_distance(to);
            if (df >= 0.0F) {
                kept.push_back(from);
            }
            if ((df >= 0.0F) != (dt >= 0.0F)) {
                const float s = df / (df - dt);
                kept.push_back(from + ((to - from) * s));
            }
        }
        polygon = std::move(kept);
        if (polygon.size() < 3) {
            return;
        }
    }
    for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
        out.push_back({polygon[0], polygon[i], polygon[i + 1]});
    }
}

// Places already made, in cells as wide as the spacing, so a new one checks a few neighbors.
class Spacing {
public:
    explicit Spacing(float distance) : distance_(distance) {}
    void add(Vec2 p) {
        if (distance_ > 0.0F) {
            cells_[cell_of(p)].push_back(p);
        }
    }
    [[nodiscard]] bool clear(Vec2 p) const {
        if (distance_ <= 0.0F) {
            return true;
        }
        const auto [cx, cy] = cell_of(p);
        const float limit = distance_ * distance_;
        for (std::int64_t y = cy - 1; y <= cy + 1; ++y) {
            for (std::int64_t x = cx - 1; x <= cx + 1; ++x) {
                const auto it = cells_.find({x, y});
                if (it == cells_.end()) {
                    continue;
                }
                for (const Vec2 q : it->second) {
                    const float dx = q.x - p.x;
                    const float dy = q.y - p.y;
                    if ((dx * dx) + (dy * dy) < limit) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

private:
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> cell_of(Vec2 p) const {
        return {static_cast<std::int64_t>(std::floor(p.x / distance_)),
                static_cast<std::int64_t>(std::floor(p.y / distance_))};
    }
    float distance_;
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<Vec2>> cells_;
};

// ---- the file format ----

json::Value number(float fallback, std::string_view description) {
    return detail::number_field(fallback, description);
}

json::Value range_field(Vec2 fallback, std::string_view description) {
    json::Value field;
    field["type"] = "array";
    field["items"]["type"] = "number";
    field["minItems"] = 2;
    field["maxItems"] = 2;
    field["default"] = json::Array{fallback.x, fallback.y};
    field["description"] = description;
    return field;
}

json::Value names_field(std::string_view description) {
    json::Value field;
    field["type"] = "array";
    field["items"]["type"] = "string";
    field["description"] = description;
    return field;
}

json::Value layer_schema() {
    const ScatterLayer d;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"name"};
    json::Value& p = schema["properties"];
    p["name"]["type"] = "string";
    p["name"]["pattern"] = "^[A-Za-z0-9_-]{1,32}$";
    p["name"]["description"] = R"(The layer's name: letters, digits, "_" and "-".)";
    p["mesh"]["type"] = "string";
    p["mesh"]["description"] = "Project path of the model each place draws (3D).";
    p["prefab"]["type"] = "string";
    p["prefab"]["description"] = "Project path of the .prefab.json each place's entity extends.";
    p["as"]["type"] = "string";
    p["as"]["enum"] = json::Array{"entities", "multimesh"};
    p["as"]["default"] = d.as;
    p["as"]["description"] = "An entity a place, or one MultiMeshInstance3D drawing mesh at "
                             "every place (3D, with mesh).";
    p["on"] = names_field("IDs of the entities to scatter over, with those under them: in 3D "
                          "their meshes, CSG, terrains, grid maps and static bodies' shapes; in 2D "
                          "TileMapLayers' cells. Without, the area (on y = 0 in 3D).");
    p["area"]["type"] = "array";
    p["area"]["items"]["type"] = "number";
    p["area"]["minItems"] = 4;
    p["area"]["maxItems"] = 4;
    p["area"]["description"] =
        "[min x, min z, max x, max z] in 3D or [min x, min y, max x, max y] in 2D, in world space: "
        "only within it.";
    p["tiles"] = names_field("2D: only cells whose legend key or tile is one of these.");
    p["count"]["type"] = "integer";
    p["count"]["minimum"] = 0;
    p["count"]["maximum"] = kMaxPlaces;
    p["count"]["description"] = "How many places.";
    p["density"]["type"] = "number";
    p["density"]["minimum"] = 0;
    p["density"]["description"] = "Places per square unit of ground (meter in 3D, pixel in 2D).";
    p["spacing"] = number(d.spacing, "Least distance across the ground between two places of the "
                                     "layer, and from those of the layers it avoids.");
    p["spacing"]["minimum"] = 0;
    p["avoid"] = names_field("Earlier layers whose places keep spacing away.");
    p["slope"] = range_field(d.slope, "3D: [min, max] degrees of the ground's slope from flat.");
    p["height"]["type"] = "array";
    p["height"]["items"]["type"] = "number";
    p["height"]["minItems"] = 2;
    p["height"]["maxItems"] = 2;
    p["height"]["description"] = "3D: [min, max] world height of the ground.";
    p["noise"] = noise_schema();
    p["noise"]["description"] = "Noise sampled at (x, z) in 3D or (x, y) in 2D: only where it is "
                                "within noise_range.";
    p["noise_range"] =
        range_field(d.noise_range, "[min, max] of the noise, which runs about -1 to 1.");
    p["align_to_normal"] = detail::boolean_field(
        d.align_to_normal, "3D: stand along the ground's normal, as Godot's populate; else "
                           "upright.");
    p["rotate_random"] = number(d.rotate_random, "Turn about the up axis by up to this many half "
                                                 "turns either way: 1 is any heading.");
    p["tilt_random"] = number(d.tilt_random, "3D: tilt by up to this many half turns either way.");
    p["scale"] = number(d.scale, "Uniform scale.");
    p["scale_random"] = number(d.scale_random, "Add up to this much to the scale either way.");
    p["offset"] = number(d.offset, "Move along the up axis by this much; negative sinks it.");
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

} // namespace

json::Value scatter_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine scatter rules";
    schema["description"] = "What scatter.run places over terrains, meshes, tile maps and areas, "
                            "a layer at a time, as Unreal's PCG graphs and Godot's MultiMesh "
                            "populate.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "layers"};
    json::Value& p = schema["properties"];
    p["format"]["const"] = 1;
    p["format"]["description"] = "Format version. Must be 1.";
    p["space"]["type"] = "string";
    p["space"]["enum"] = json::Array{"3d", "2d"};
    p["space"]["default"] = "3d";
    p["space"]["description"] = "Whether places are 3D (Transform3D) or 2D (Transform2D).";
    p["seed"] = detail::integer_field(0,
                                      "Seed of the random places: the same seed places the same "
                                      "things.",
                                      0, 4294967295);
    p["layers"]["type"] = "array";
    p["layers"]["items"] = layer_schema();
    p["layers"]["description"] = "What to scatter, in order: later layers can avoid earlier ones.";
    return schema;
}

std::vector<json::ValidationError> validate_scatter(const json::Value& doc, const std::string& at) {
    std::vector<json::ValidationError> errors = json::validate(doc, scatter_schema());
    if (errors.empty()) {
        const json::Value* space = doc.find("space");
        const bool flat = space != nullptr && space->as_string() == "2d";
        std::set<std::string> earlier;
        const json::Array& layers = doc.find("layers")->as_array();
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const json::Value& layer = layers[i];
            const std::string path = "/layers/" + std::to_string(i);
            const std::string name = layer.find("name")->as_string();
            if (!world::World::is_valid_id(name) || name.size() > 32) {
                errors.push_back({path + "/name", "\"" + name +
                                                      "\" isn't a name: up to 32 letters, "
                                                      "digits, \"_\" and \"-\""});
            }
            const bool mesh = layer.find("mesh") != nullptr;
            const bool prefab = layer.find("prefab") != nullptr;
            const json::Value* as = layer.find("as");
            const bool multimesh = as != nullptr && as->as_string() == "multimesh";
            if (mesh == prefab) {
                errors.push_back({path, "give mesh or prefab, one of them"});
            } else if (flat && mesh) {
                errors.push_back({path + "/mesh", "2D places are entities: give a prefab"});
            } else if (multimesh && !mesh) {
                errors.push_back({path + "/as", "a multimesh draws a mesh: give mesh"});
            }
            if ((layer.find("count") != nullptr) == (layer.find("density") != nullptr)) {
                errors.push_back({path, "give count or density, one of them"});
            }
            if (layer.find("on") == nullptr && layer.find("area") == nullptr) {
                errors.push_back({path, "say where: give on, area or both"});
            }
            if (const json::Value* area = layer.find("area"); area != nullptr) {
                const json::Array& a = area->as_array();
                if (a[0].as_double() >= a[2].as_double() || a[1].as_double() >= a[3].as_double()) {
                    errors.push_back({path + "/area", "expected min below max on each axis"});
                }
            }
            for (const std::string_view key : {"slope", "height", "noise_range"}) {
                if (const std::optional<Vec2> range = read_range(layer, key);
                    range && range->x > range->y) {
                    errors.push_back({path + "/" + std::string(key), "expected [min, max]"});
                }
            }
            if (!flat && layer.find("tiles") != nullptr) {
                errors.push_back({path + "/tiles", "tiles pick tile map cells, in 2D"});
            }
            const std::vector<std::string> avoid = read_names(layer, "avoid");
            for (std::size_t k = 0; k < avoid.size(); ++k) {
                if (!earlier.contains(avoid[k])) {
                    errors.push_back({path + "/avoid/" + std::to_string(k),
                                      "no earlier layer is named \"" + avoid[k] + "\""});
                }
            }
            if (!earlier.insert(name).second) {
                errors.push_back({path + "/name", "\"" + name + "\" names an earlier layer too"});
            }
        }
    }
    for (json::ValidationError& error : errors) {
        error.path = at + error.path;
    }
    return errors;
}

Result<ScatterRules> parse_scatter(const json::Value& doc) {
    if (const auto errors = validate_scatter(doc); !errors.empty()) {
        return detail::first_error(errors);
    }
    ScatterRules rules;
    if (const json::Value* space = doc.find("space"); space != nullptr) {
        rules.space = space->as_string();
    }
    if (const json::Value* seed = doc.find("seed"); seed != nullptr) {
        rules.seed = static_cast<std::uint32_t>(seed->as_int());
    }
    for (const json::Value& entry : doc.find("layers")->as_array()) {
        ScatterLayer layer;
        layer.name = entry.find("name")->as_string();
        if (const json::Value* mesh = entry.find("mesh"); mesh != nullptr) {
            layer.mesh = mesh->as_string();
        }
        if (const json::Value* prefab = entry.find("prefab"); prefab != nullptr) {
            layer.prefab = prefab->as_string();
        }
        if (const json::Value* as = entry.find("as"); as != nullptr) {
            layer.as = as->as_string();
        }
        layer.on = read_names(entry, "on");
        layer.tiles = read_names(entry, "tiles");
        layer.avoid = read_names(entry, "avoid");
        if (const json::Value* area = entry.find("area"); area != nullptr) {
            const json::Array& a = area->as_array();
            layer.area = std::array<float, 4>{
                static_cast<float>(a[0].as_double()), static_cast<float>(a[1].as_double()),
                static_cast<float>(a[2].as_double()), static_cast<float>(a[3].as_double())};
        }
        if (const json::Value* count = entry.find("count"); count != nullptr) {
            layer.count = static_cast<int>(count->as_int());
        }
        if (const json::Value* density = entry.find("density"); density != nullptr) {
            layer.density = static_cast<float>(density->as_double());
        }
        detail::read(entry, "spacing", layer.spacing);
        layer.slope = read_range(entry, "slope").value_or(layer.slope);
        layer.height = read_range(entry, "height");
        if (const json::Value* noise = entry.find("noise"); noise != nullptr) {
            layer.noise = parse_noise(*noise).value();
        }
        layer.noise_range = read_range(entry, "noise_range").value_or(layer.noise_range);
        detail::read(entry, "align_to_normal", layer.align_to_normal);
        detail::read(entry, "rotate_random", layer.rotate_random);
        detail::read(entry, "tilt_random", layer.tilt_random);
        detail::read(entry, "scale", layer.scale);
        detail::read(entry, "scale_random", layer.scale_random);
        detail::read(entry, "offset", layer.offset);
        rules.layers.push_back(std::move(layer));
    }
    return rules;
}

ScatterResult scatter(const ScatterLayer& layer, std::size_t index, std::uint32_t seed, bool flat,
                      std::span<const ScatterTriangle> ground, std::span<const Vec3> avoid) {
    ScatterResult result;
    // The ground within the area, and each triangle's share of it.
    std::vector<ScatterTriangle> triangles;
    for (const ScatterTriangle& t : ground) {
        if (layer.area) {
            clip(t, *layer.area, flat, triangles);
        } else {
            triangles.push_back(t);
        }
    }
    std::vector<float> cumulative;
    std::vector<std::size_t> picked;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const float area = area_of(triangles[i]);
        if (area < kMinArea) {
            continue;
        }
        result.ground_area += area;
        cumulative.push_back(result.ground_area);
        picked.push_back(i);
    }
    const double wanted = layer.count
                              ? *layer.count
                              : std::round(static_cast<double>(layer.density.value_or(0.0F)) *
                                           static_cast<double>(result.ground_area));
    result.wanted = static_cast<int>(std::min(wanted, static_cast<double>(kMaxPlaces)));
    if (cumulative.empty() || result.wanted == 0) {
        return result;
    }

    Spacing spacing(layer.spacing);
    for (const Vec3 other : avoid) {
        spacing.add(across(other, flat));
    }
    std::optional<Noise> noise;
    if (layer.noise) {
        noise.emplace(*layer.noise);
    }
    float slope_min = 1.0F; // the cosines bounding the normal's height
    float slope_max = -1.0F;
    if (!flat) {
        constexpr float kDegrees = kPi / 180.0F;
        slope_min = cos_sin(layer.slope.x * kDegrees).x + kSlopeSlack;
        slope_max = cos_sin(layer.slope.y * kDegrees).x - kSlopeSlack;
    }

    Random random(seed, (static_cast<std::uint64_t>(index) * 2U) + 1U);
    const int max_tries = result.wanted * kTriesPerPlace;
    while (std::cmp_less(result.places.size(), result.wanted) && result.tries < max_tries) {
        ++result.tries;
        // A triangle by area, then a point inside it as Godot's Face3::get_random_point_inside.
        const float at = random.range(0.0F, result.ground_area);
        const auto it = std::ranges::upper_bound(cumulative, at);
        const std::size_t slot =
            std::min(static_cast<std::size_t>(it - cumulative.begin()), cumulative.size() - 1);
        const ScatterTriangle& t = triangles[picked[slot]];
        float a = random.range(0.0F, 1.0F);
        float b = random.range(0.0F, 1.0F);
        if (a > b) {
            std::swap(a, b);
        }
        const Vec3 point = (t.a * a) + (t.b * (b - a)) + (t.c * (1.0F - b));
        const Vec2 ground_point = across(point, flat);
        if (layer.area &&
            (ground_point.x < (*layer.area)[0] || ground_point.y < (*layer.area)[1] ||
             ground_point.x > (*layer.area)[2] || ground_point.y > (*layer.area)[3])) {
            ++result.outside;
            continue;
        }
        const Vec3 normal = flat ? Vec3{0.0F, 0.0F, 1.0F} : up_normal(t);
        if (!flat && (normal.y > slope_min || normal.y < slope_max)) {
            ++result.slope;
            continue;
        }
        if (!flat && layer.height && (point.y < layer.height->x || point.y > layer.height->y)) {
            ++result.height;
            continue;
        }
        if (noise) {
            const float value = noise->get_noise_2d(ground_point.x, ground_point.y);
            if (value < layer.noise_range.x || value > layer.noise_range.y) {
                ++result.noise;
                continue;
            }
        }
        if (!spacing.clear(ground_point)) {
            ++result.spacing;
            continue;
        }
        spacing.add(ground_point);

        // Turned and scaled as Godot's MultiMeshEditor::_populate does.
        ScatterPlace place;
        const float heading = -random.range(-layer.rotate_random, layer.rotate_random) * kPi;
        const float tilt_z = -random.range(-layer.tilt_random, layer.tilt_random) * kPi;
        const float tilt_x = -random.range(-layer.tilt_random, layer.tilt_random) * kPi;
        place.scale = layer.scale + random.range(-layer.scale_random, layer.scale_random);
        if (flat) {
            place.position = point;
            place.rotation = {0.0F, 0.0F, -heading};
        } else {
            const Vec3 up = layer.align_to_normal ? normal : Vec3{0.0F, 1.0F, 0.0F};
            const Basis stand = standing_on(up);
            Basis post = axis_angle(normalized(stand.column(1)), heading);
            post = axis_angle(normalized(stand.column(2)), tilt_z) * post;
            post = axis_angle(normalized(stand.column(0)), tilt_x) * post;
            const Basis turned = post * stand;
            world::Affine3D rotation;
            rotation.x = turned.column(0);
            rotation.y = turned.column(1);
            rotation.z = turned.column(2);
            place.rotation = world::euler(rotation);
            place.position = point + (turned.column(1) * layer.offset);
        }
        result.places.push_back(place);
    }
    return result;
}

} // namespace slash::procedural
