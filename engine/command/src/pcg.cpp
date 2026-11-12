#include "slash/procedural/pcg.hpp"

#include "procedural.hpp"
#include "scatter_ground.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/procedural/scatter.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/path.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// pcg.run, pcg.preview and pcg.check: follow a .pcg.json graph's nodes over the world, each
// node's points from the nodes it reads, then (pcg.run) place what its spawners ask for under one
// parent, replacing what the run before made there. Commands a graph calls run inside pcg.run,
// so one undo takes the whole run back.
namespace slash::command {
namespace {

using procedural::PcgGraph;
using procedural::PcgNode;
using procedural::PcgPoint;

constexpr int kMaxPoints = 100000;
constexpr std::uint32_t kPickSalt = 0x91C4U;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

Reply unknown_entity(const world::World& world, std::string_view id, std::string_view path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kNotFound, path, message);
}

// A number as replies give it: four decimals.
double num(real_t value) {
    return std::round(static_cast<double>(value) * 10000.0) / 10000.0;
}

json::Value point_json(Vec3 p, bool flat) {
    return flat ? json::Array{num(p.x), num(p.y)} : json::Array{num(p.x), num(p.y), num(p.z)};
}

float length(Vec3 v) {
    return to_float(std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z)));
}

Vec3 unit(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? v * (1.0F / l) : Vec3{};
}

std::vector<std::string> names_of(const json::Value& node, std::string_view key) {
    std::vector<std::string> out;
    if (const json::Value* value = node.find(key); value != nullptr) {
        for (const json::Value& name : value->as_array()) {
            out.push_back(name.as_string());
        }
    }
    return out;
}

std::optional<Vec2> range_of(const json::Value& node, std::string_view key) {
    const json::Value* value = node.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    const json::Array& items = value->as_array();
    return Vec2{static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double())};
}

std::optional<std::array<float, 4>> area_of(const json::Value& node) {
    const json::Value* value = node.find("area");
    if (value == nullptr) {
        return std::nullopt;
    }
    const json::Array& a = value->as_array();
    return std::array<float, 4>{
        static_cast<float>(a[0].as_double()), static_cast<float>(a[1].as_double()),
        static_cast<float>(a[2].as_double()), static_cast<float>(a[3].as_double())};
}

float number_of(const json::Value& node, std::string_view key, float fallback) {
    const json::Value* value = node.find(key);
    return value != nullptr ? static_cast<float>(value->as_double()) : fallback;
}

// ---- the graph ----

struct Loaded {
    json::Value doc;
    PcgGraph graph;
    bool flat = false;
    std::string nodes_path; // where errors about a node point: "/graph/nodes/" or "<file> at ..."
};

std::optional<Reply> load_graph(Session& session, const json::Value& params, Loaded& out) {
    const json::Value* path = params.find("path");
    const json::Value* inline_graph = params.find("graph");
    if ((path == nullptr) == (inline_graph == nullptr)) {
        return param_error(code::kInvalidParams, "", "give path or graph, one of them");
    }
    std::string at = "/graph";
    if (path != nullptr) {
        const std::string& file = path->as_string();
        if (!file.ends_with(procedural::kPcgSuffix)) {
            return param_error(code::kInvalidArgument, "/path", "expected a .pcg.json file");
        }
        auto loaded = session.assets().load<json::Value>(file);
        if (!loaded) {
            return param_error(loaded.error().code == ErrorCode::NotFound ? code::kNotFound
                                                                          : code::kInvalidArgument,
                               "/path", loaded.error().message);
        }
        out.doc = *loaded.value();
        at = file;
    } else {
        out.doc = *inline_graph;
    }
    std::vector<json::ValidationError> errors = procedural::validate_pcg(out.doc);
    if (!errors.empty()) {
        if (path == nullptr) {
            for (json::ValidationError& error : errors) {
                error.path = "/graph" + error.path;
            }
            return Reply::invalid_params(errors);
        }
        const json::ValidationError& first = errors.front();
        return param_error(code::kInvalidArgument, "/path",
                           quoted(at) + (first.path.empty() ? "" : " at " + first.path) + ": " +
                               first.message);
    }
    out.graph = procedural::parse_pcg(out.doc).value();
    if (const json::Value* seed = params.find("seed"); seed != nullptr) {
        out.graph.seed = static_cast<std::uint32_t>(seed->as_int());
    }
    out.flat = out.graph.space == "2d";
    out.nodes_path = path == nullptr ? "/graph/nodes/" : at + " at /nodes/";
    return std::nullopt;
}

// Where the ground under a point across it is: the highest triangle there in 3D, any cell in 2D.
class GroundIndex {
public:
    GroundIndex(std::vector<procedural::ScatterTriangle> triangles, bool flat)
        : triangles_(std::move(triangles)), flat_(flat) {
        if (triangles_.empty()) {
            return;
        }
        float min_x = std::numeric_limits<float>::max();
        float min_y = min_x;
        float max_x = -min_x;
        float max_y = -min_x;
        for (const procedural::ScatterTriangle& t : triangles_) {
            for (const Vec3 p : {t.a, t.b, t.c}) {
                const Vec2 q = across(p);
                min_x = std::min(min_x, static_cast<float>(q.x));
                min_y = std::min(min_y, static_cast<float>(q.y));
                max_x = std::max(max_x, static_cast<float>(q.x));
                max_y = std::max(max_y, static_cast<float>(q.y));
            }
        }
        cell_ = std::max({(max_x - min_x) / 64.0F, (max_y - min_y) / 64.0F, 1.0e-3F});
        for (std::size_t i = 0; i < triangles_.size(); ++i) {
            const procedural::ScatterTriangle& t = triangles_[i];
            const Vec2 a = across(t.a);
            const Vec2 b = across(t.b);
            const Vec2 c = across(t.c);
            const auto [x0, y0] = cell_of({std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y})});
            const auto [x1, y1] = cell_of({std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y})});
            for (std::int64_t y = y0; y <= y1; ++y) {
                for (std::int64_t x = x0; x <= x1; ++x) {
                    cells_[{x, y}].push_back(i);
                }
            }
        }
    }

    // The point on the ground under at, and the ground's normal there, facing up.
    [[nodiscard]] std::optional<std::pair<Vec3, Vec3>> under(Vec2 at) const {
        const auto it = cells_.find(cell_of(at));
        if (it == cells_.end()) {
            return std::nullopt;
        }
        std::optional<std::pair<Vec3, Vec3>> best;
        for (const std::size_t i : it->second) {
            const procedural::ScatterTriangle& t = triangles_[i];
            const Vec2 a = across(t.a);
            const Vec2 b = across(t.b);
            const Vec2 c = across(t.c);
            const float d = to_float(((b.y - c.y) * (a.x - c.x)) + ((c.x - b.x) * (a.y - c.y)));
            if (std::abs(d) < 1.0e-12F) {
                continue;
            }
            const float u =
                static_cast<float>(((b.y - c.y) * (at.x - c.x)) + ((c.x - b.x) * (at.y - c.y))) / d;
            const float v =
                static_cast<float>(((c.y - a.y) * (at.x - c.x)) + ((a.x - c.x) * (at.y - c.y))) / d;
            const float w = 1.0F - u - v;
            constexpr float kSlack = -1.0e-5F;
            if (u < kSlack || v < kSlack || w < kSlack) {
                continue;
            }
            if (flat_) {
                return std::pair{Vec3{at.x, at.y, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}};
            }
            const Vec3 p = (t.a * u) + (t.b * v) + (t.c * w);
            if (!best || p.y > best->first.y) {
                const Vec3 e1 = t.b - t.a;
                const Vec3 e2 = t.c - t.a;
                Vec3 n = unit({(e1.y * e2.z) - (e1.z * e2.y), (e1.z * e2.x) - (e1.x * e2.z),
                               (e1.x * e2.y) - (e1.y * e2.x)});
                if (n.y < 0.0F) {
                    n = n * -1.0F;
                }
                best = std::pair{p, n};
            }
        }
        return best;
    }

private:
    [[nodiscard]] Vec2 across(Vec3 p) const { return flat_ ? Vec2{p.x, p.y} : Vec2{p.x, p.z}; }
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> cell_of(Vec2 p) const {
        return {static_cast<std::int64_t>(std::floor(static_cast<float>(p.x) / cell_)),
                static_cast<std::int64_t>(std::floor(static_cast<float>(p.y) / cell_))};
    }
    std::vector<procedural::ScatterTriangle> triangles_;
    bool flat_;
    float cell_ = 1.0F;
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::size_t>> cells_;
};

// The world the graph runs over, gathered once.
struct Scene {
    world::World& world;
    bool flat;
    std::unique_ptr<world::Tree3D> tree3; // set in 3D
    std::unique_ptr<world::Tree2D> tree2; // set in 2D
    std::vector<navigation::EntityGeometry> geometry;
    std::vector<entt::entity> old; // what the run before made, which isn't ground
};

// An entity's place as a point.
PcgPoint entity_point(const Scene& scene, entt::entity entity) {
    PcgPoint point;
    if (scene.flat) {
        const world::Transform2D global = scene.tree2->global(entity);
        point.position = {global.position.x, global.position.y, 0.0F};
        point.rotation = {0.0F, 0.0F, global.rotation};
        point.normal = {0.0F, 0.0F, 1.0F};
    } else {
        const world::Affine3D global = scene.tree3->global(entity);
        point.position = global.origin;
        world::Affine3D rotation;
        rotation.x = unit(global.x);
        rotation.y = unit(global.y);
        rotation.z = unit(global.z);
        point.rotation = world::euler(rotation);
        point.normal = rotation.y;
    }
    return point;
}

bool has_transform(const Scene& scene, entt::entity entity) {
    return scene.flat ? scene.world.ecs().all_of<world::Transform2D>(entity)
                      : scene.world.ecs().all_of<world::Transform3D>(entity);
}

// The entities whose StoryRef names one of ids, by ID.
std::vector<entt::entity> story_entities(const world::World& world,
                                         const std::vector<std::string>& ids) {
    std::vector<std::pair<std::string, entt::entity>> found;
    for (const auto [entity, ref] : world.ecs().view<const world::StoryRef>().each()) {
        if (std::ranges::any_of(ref.ids, [&](const std::string& id) {
                return std::ranges::find(ids, id) != ids.end();
            })) {
            found.emplace_back(std::string(world.id_of(entity)), entity);
        }
    }
    std::ranges::sort(found);
    std::vector<entt::entity> out;
    out.reserve(found.size());
    for (const auto& each : found) {
        out.push_back(each.second);
    }
    return out;
}

// Positions of what difference and near measure from: earlier nodes' points, entities and
// storyboard places.
std::optional<Reply> targets_of(const Scene& scene, const Loaded& loaded, const PcgNode& node,
                                const std::map<std::string, std::size_t>& index_of,
                                const std::vector<std::vector<PcgPoint>>& done,
                                const std::string& path, std::vector<Vec3>& out) {
    for (const std::string& name : names_of(node.fields, "to")) {
        for (const PcgPoint& point : done[index_of.at(name)]) {
            out.push_back(point.position);
        }
    }
    const std::vector<std::string> targets = names_of(node.fields, "targets");
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const entt::entity entity = scene.world.find(targets[k]);
        if (entity == entt::null) {
            return unknown_entity(scene.world, targets[k], path + "/targets/" + std::to_string(k));
        }
        out.push_back(entity_point(scene, entity).position);
    }
    for (const entt::entity entity : story_entities(scene.world, names_of(node.fields, "story"))) {
        if (has_transform(scene, entity)) {
            out.push_back(entity_point(scene, entity).position);
        }
    }
    (void)loaded;
    return std::nullopt;
}

// The ground of the entities on names, or the area.
std::optional<Reply> ground_of(Session& session, Scene& scene, const PcgNode& node,
                               const std::string& path,
                               std::vector<procedural::ScatterTriangle>& out) {
    const std::vector<std::string> on_names = names_of(node.fields, "on");
    std::vector<entt::entity> on;
    for (std::size_t k = 0; k < on_names.size(); ++k) {
        const entt::entity entity = scene.world.find(on_names[k]);
        if (entity == entt::null) {
            return unknown_entity(scene.world, on_names[k], path + "/on/" + std::to_string(k));
        }
        on.push_back(entity);
    }
    if (on.empty()) {
        add_area(area_of(node.fields).value_or(std::array<float, 4>{}), scene.flat, out);
    } else if (scene.flat) {
        auto cells = ground_2d(session, names_of(node.fields, "tiles"), on, path);
        if (!cells) {
            return param_error(code::kInvalidArgument, "", cells.error().message);
        }
        out = std::move(cells).value();
    } else {
        out = ground_3d(scene.geometry, *scene.tree3, {on.begin(), on.end()});
    }
    return std::nullopt;
}

// A sampler's points.
std::optional<Reply> sample(Session& session, Scene& scene, const Loaded& loaded, std::size_t index,
                            const std::string& path, std::vector<PcgPoint>& out) {
    const PcgNode& node = loaded.graph.nodes[index];
    const json::Value& f = node.fields;
    const bool flat = loaded.flat;
    const std::uint32_t seed =
        procedural::pcg_hash(loaded.graph.seed, static_cast<std::uint32_t>(index));
    if (node.op == "surface") {
        std::vector<procedural::ScatterTriangle> ground;
        if (auto failed = ground_of(session, scene, node, path, ground)) {
            return failed;
        }
        procedural::ScatterLayer layer;
        layer.name = node.name;
        layer.area = area_of(f);
        if (const json::Value* count = f.find("count"); count != nullptr) {
            layer.count = static_cast<int>(count->as_int());
        }
        if (const json::Value* density = f.find("density"); density != nullptr) {
            layer.density = static_cast<float>(density->as_double());
        }
        layer.spacing = number_of(f, "spacing", 0.0F);
        layer.slope = range_of(f, "slope").value_or(layer.slope);
        layer.height = range_of(f, "height");
        const procedural::ScatterResult result =
            procedural::scatter(layer, index, loaded.graph.seed, flat, ground, {});
        for (std::size_t k = 0; k < result.places.size(); ++k) {
            const procedural::ScatterPlace& place = result.places[k];
            out.push_back({.position = place.position,
                           .rotation = place.rotation,
                           .scale = place.scale,
                           .density = 1.0F,
                           .normal = place.normal,
                           .seed = procedural::pcg_hash(seed, static_cast<std::uint32_t>(k))});
        }
    } else if (node.op == "grid") {
        out = procedural::pcg_grid(area_of(f).value_or(std::array<float, 4>{}),
                                   number_of(f, "spacing", 1.0F), number_of(f, "jitter", 0.0F),
                                   seed, flat);
        if (f.find("on") != nullptr) {
            std::vector<procedural::ScatterTriangle> ground;
            if (auto failed = ground_of(session, scene, node, path, ground)) {
                return failed;
            }
            const GroundIndex index_of(std::move(ground), flat);
            std::vector<PcgPoint> kept;
            for (PcgPoint& point : out) {
                const Vec2 at = flat ? Vec2{point.position.x, point.position.y}
                                     : Vec2{point.position.x, point.position.z};
                if (const auto hit = index_of.under(at); hit) {
                    point.position = hit->first;
                    point.normal = hit->second;
                    kept.push_back(point);
                }
            }
            out = std::move(kept);
        }
    } else if (node.op == "points") {
        const json::Array& list = f.find("points")->as_array();
        for (std::size_t k = 0; k < list.size(); ++k) {
            const json::Array& p = list[k].as_array();
            PcgPoint point;
            point.position = {static_cast<float>(p[0].as_double()),
                              static_cast<float>(p[1].as_double()),
                              p.size() > 2 ? static_cast<float>(p[2].as_double()) : 0.0F};
            point.normal = flat ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
            point.seed = procedural::pcg_hash(seed, static_cast<std::uint32_t>(k));
            out.push_back(point);
        }
    } else if (node.op == "path") {
        const std::string id = f.find("path")->as_string();
        const entt::entity entity = scene.world.find(id);
        if (entity == entt::null) {
            return unknown_entity(scene.world, id, path + "/path");
        }
        const float spacing = number_of(f, "spacing", 1.0F);
        const float side = number_of(f, "side", 0.0F);
        if (flat) {
            const auto curve = world::curve_2d(scene.world, entity);
            if (curve == nullptr) {
                return param_error(code::kInvalidParams, path + "/path",
                                   quoted(id) + " has no Path2D with points to follow");
            }
            const world::Affine2D global = scene.tree2->global_affine(entity);
            for (int k = 0; static_cast<float>(k) * spacing <= curve->length() && k < kMaxPoints;
                 ++k) {
                const world::Affine2D at =
                    global * curve->sample_with_rotation(static_cast<float>(k) * spacing);
                const Vec2 normal = at.y;
                const float l = to_float(std::sqrt((normal.x * normal.x) + (normal.y * normal.y)));
                const Vec2 across = l > 0.0F ? normal * (side / l) : Vec2{};
                PcgPoint point;
                point.position = {at.origin.x + across.x, at.origin.y + across.y, 0.0F};
                point.rotation = {0.0F, 0.0F, world::parts(at).rotation};
                point.normal = {0.0F, 0.0F, 1.0F};
                point.seed = procedural::pcg_hash(seed, static_cast<std::uint32_t>(k));
                out.push_back(point);
            }
        } else {
            const auto curve = world::curve_3d(scene.world, entity);
            if (curve == nullptr) {
                return param_error(code::kInvalidParams, path + "/path",
                                   quoted(id) + " has no Path3D with points to follow");
            }
            const world::Affine3D global = scene.tree3->global(entity);
            for (int k = 0; static_cast<float>(k) * spacing <= curve->length() && k < kMaxPoints;
                 ++k) {
                const world::Affine3D at =
                    global * curve->sample_with_rotation(static_cast<float>(k) * spacing);
                world::Affine3D rotation;
                rotation.x = unit(at.x);
                rotation.y = unit(at.y);
                rotation.z = unit(at.z);
                PcgPoint point;
                point.position = at.origin + (rotation.x * side);
                point.rotation = world::euler(rotation);
                point.normal = rotation.y;
                point.seed = procedural::pcg_hash(seed, static_cast<std::uint32_t>(k));
                out.push_back(point);
            }
        }
    } else if (node.op == "entities") {
        std::vector<entt::entity> chosen;
        const std::vector<std::string> ids = names_of(f, "ids");
        for (std::size_t k = 0; k < ids.size(); ++k) {
            const entt::entity entity = scene.world.find(ids[k]);
            if (entity == entt::null) {
                return unknown_entity(scene.world, ids[k], path + "/ids/" + std::to_string(k));
            }
            chosen.push_back(entity);
        }
        const std::vector<std::string> with = names_of(f, "with");
        std::vector<const world::ComponentType*> types;
        for (std::size_t k = 0; k < with.size(); ++k) {
            const world::ComponentType* type = scene.world.components().find(with[k]);
            if (type == nullptr) {
                std::string message = "no component is named " + quoted(with[k]);
                std::vector<std::string_view> known;
                for (const auto& each : scene.world.components().types()) {
                    known.push_back(each->name());
                }
                if (const std::string_view hint = closest_match(with[k], known); !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                return param_error(code::kInvalidParams, path + "/with/" + std::to_string(k),
                                   message);
            }
            types.push_back(type);
        }
        if (!types.empty()) {
            std::vector<std::string_view> all = scene.world.ids();
            std::ranges::sort(all);
            for (const std::string_view id : all) {
                const entt::entity entity = scene.world.find(id);
                if (std::ranges::all_of(types, [&](const world::ComponentType* type) {
                        return type->has(scene.world, entity);
                    })) {
                    chosen.push_back(entity);
                }
            }
        }
        for (const entt::entity entity : story_entities(scene.world, names_of(f, "story"))) {
            chosen.push_back(entity);
        }
        std::set<entt::entity> seen;
        for (const entt::entity entity : chosen) {
            if (!has_transform(scene, entity) ||
                std::ranges::find(scene.old, entity) != scene.old.end() ||
                !seen.insert(entity).second) {
                continue;
            }
            PcgPoint point = entity_point(scene, entity);
            // FNV-1a of the ID: the same seed on every machine, wherever the entity sits.
            std::uint32_t id_hash = 2166136261U;
            for (const char c : scene.world.id_of(entity)) {
                id_hash = (id_hash ^ static_cast<std::uint8_t>(c)) * 16777619U;
            }
            point.seed = procedural::pcg_hash(seed, id_hash);
            out.push_back(point);
        }
    }
    return std::nullopt;
}

// Every node's points, in order.
struct Evaluated {
    std::vector<std::vector<PcgPoint>> points;
    std::vector<std::string> warnings;
};

std::optional<Reply> evaluate(Session& session, const Loaded& loaded, entt::entity into,
                              Evaluated& out) {
    world::World& world = session.world();
    Scene scene{.world = world,
                .flat = loaded.flat,
                .tree3 = nullptr,
                .tree2 = nullptr,
                .geometry = {},
                .old = {}};
    if (into != entt::null) {
        collect_descendants(world, into, scene.old);
    }
    if (loaded.flat) {
        scene.tree2 = std::make_unique<world::Tree2D>(world);
    } else {
        scene.tree3 = std::make_unique<world::Tree3D>(world);
        const bool needs_ground = std::ranges::any_of(loaded.graph.nodes, [](const PcgNode& node) {
            return node.fields.find("on") != nullptr;
        });
        if (needs_ground) {
            const navigation::NavigationMesh settings;
            for (navigation::EntityGeometry& each :
                 navigation::world_geometry(world, session.assets(), settings, &out.warnings)) {
                if (std::ranges::find(scene.old, each.entity) == scene.old.end()) {
                    scene.geometry.push_back(std::move(each));
                }
            }
        }
    }
    std::map<std::string, std::size_t> index_of;
    const std::vector<PcgNode>& nodes = loaded.graph.nodes;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const PcgNode& node = nodes[i];
        const json::Value& f = node.fields;
        const std::string path = loaded.nodes_path + std::to_string(i);
        const auto salt =
            procedural::pcg_hash(loaded.graph.seed, static_cast<std::uint32_t>(i), 0x5A17U);
        std::vector<PcgPoint> points;
        if (procedural::pcg_is_sampler(node.op)) {
            if (auto failed = sample(session, scene, loaded, i, path, points)) {
                return failed;
            }
        } else {
            std::size_t total = 0;
            for (const std::string& name : node.from) {
                total += out.points[index_of.at(name)].size();
            }
            if (total > static_cast<std::size_t>(kMaxPoints)) {
                return param_error(code::kInvalidArgument, path + "/from",
                                   "these nodes hold " + std::to_string(total) +
                                       " points, over the 100000 a node may");
            }
            for (const std::string& name : node.from) {
                const std::vector<PcgPoint>& from = out.points[index_of.at(name)];
                points.insert(points.end(), from.begin(), from.end());
            }
        }
        if (node.op == "filter") {
            procedural::PcgFilter filter;
            filter.slope = range_of(f, "slope");
            filter.height = range_of(f, "height");
            if (const json::Value* noise = f.find("noise"); noise != nullptr) {
                filter.noise = procedural::parse_noise(*noise).value();
            }
            filter.noise_range = range_of(f, "noise_range").value_or(filter.noise_range);
            filter.density = range_of(f, "density_range");
            if (const json::Value* keep = f.find("keep"); keep != nullptr) {
                filter.keep = static_cast<float>(keep->as_double());
            }
            if (const json::Value* limit = f.find("limit"); limit != nullptr) {
                filter.limit = static_cast<int>(limit->as_int());
            }
            filter.area = area_of(f);
            points = procedural::pcg_filter(points, filter, salt, loaded.flat);
        } else if (node.op == "density") {
            procedural::pcg_density(points, procedural::parse_noise(*f.find("noise")).value(),
                                    range_of(f, "noise_range").value_or(Vec2{-1.0F, 1.0F}),
                                    f.find("multiply") != nullptr && f.find("multiply")->as_bool(),
                                    loaded.flat);
        } else if (node.op == "prune") {
            points = procedural::pcg_prune(points, number_of(f, "spacing", 0.0F), loaded.flat);
        } else if (node.op == "difference" || node.op == "near") {
            std::vector<Vec3> targets;
            if (auto failed =
                    targets_of(scene, loaded, node, index_of, out.points, path, targets)) {
                return failed;
            }
            points = node.op == "near"
                         ? procedural::pcg_near(points, targets,
                                                range_of(f, "range").value_or(Vec2{}), loaded.flat)
                         : procedural::pcg_difference(points, targets,
                                                      number_of(f, "distance", 0.0F), loaded.flat);
        } else if (node.op == "transform") {
            procedural::PcgTransform transform;
            if (const json::Value* offset = f.find("offset"); offset != nullptr) {
                const json::Array& o = offset->as_array();
                transform.offset = {static_cast<float>(o[0].as_double()),
                                    static_cast<float>(o[1].as_double()),
                                    o.size() > 2 ? static_cast<float>(o[2].as_double()) : 0.0F};
            }
            transform.offset_up = number_of(f, "offset_up", 0.0F);
            if (const json::Value* degrees = f.find("rotation_degrees"); degrees != nullptr) {
                const json::Array& d = degrees->as_array();
                constexpr double kRadians = std::numbers::pi / 180.0;
                transform.rotation = {static_cast<float>(d[0].as_double() * kRadians),
                                      static_cast<float>(d[1].as_double() * kRadians),
                                      static_cast<float>(d[2].as_double() * kRadians)};
            }
            transform.rotate_random = number_of(f, "rotate_random", 0.0F);
            transform.tilt_random = number_of(f, "tilt_random", 0.0F);
            transform.scale = number_of(f, "scale", 1.0F);
            transform.scale_random = number_of(f, "scale_random", 0.0F);
            transform.align_to_normal =
                f.find("align_to_normal") != nullptr && f.find("align_to_normal")->as_bool();
            procedural::pcg_transform(points, transform, salt, loaded.flat);
        } else if (procedural::pcg_is_spawner(node.op)) {
            if (const json::Value* keep = f.find("keep"); keep != nullptr) {
                procedural::PcgFilter filter;
                filter.keep = static_cast<float>(keep->as_double());
                points = procedural::pcg_filter(points, filter, salt, loaded.flat);
            }
            if (node.op == "route") {
                points = procedural::pcg_route_order(points, loaded.flat);
            }
        }
        index_of[node.name] = i;
        out.points.push_back(std::move(points));
    }
    return std::nullopt;
}

// What a spawner makes: an entity a point, or one.
std::size_t made_by(const PcgNode& node, std::size_t points) {
    if (node.op == "spawn" || node.op == "command") {
        return points;
    }
    return points > 0 || node.op != "route" ? 1 : 0;
}

// The budget's first breach, or nothing.
std::optional<Reply> over_budget(const Loaded& loaded, const Evaluated& done) {
    const PcgGraph& graph = loaded.graph;
    std::size_t entities = 0;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const std::size_t count = done.points[i].size();
        if (graph.budget.points && std::cmp_greater(count, *graph.budget.points)) {
            return param_error(code::kInvalidArgument, loaded.nodes_path + std::to_string(i),
                               quoted(graph.nodes[i].name) + " holds " + std::to_string(count) +
                                   " points, over the budget's " +
                                   std::to_string(*graph.budget.points));
        }
        if (procedural::pcg_is_spawner(graph.nodes[i].op)) {
            entities += made_by(graph.nodes[i], count);
        }
    }
    if (graph.budget.entities && std::cmp_greater(entities, *graph.budget.entities)) {
        return param_error(code::kInvalidArgument, "",
                           "the spawners would make " + std::to_string(entities) +
                               ", over the budget's " + std::to_string(*graph.budget.entities) +
                               " entities");
    }
    return std::nullopt;
}

// ---- placing ----

// What $ names in a spawner's values stand for at one point.
struct Bindings {
    const PcgPoint* point = nullptr;
    bool flat = false;
    Vec3 local;
    Vec3 local_rotation;
    float local_scale = 1.0F;
    std::size_t index = 0;
    std::string id;
    std::string parent;
    std::string node;
};

json::Value substitute(const json::Value& value, const Bindings& b, std::uint32_t depth = 0) {
    if (value.is_string()) {
        const std::string& s = value.as_string();
        if (s.empty() || s[0] != '$') {
            return value;
        }
        const PcgPoint& p = *b.point;
        if (s == "$position") {
            return point_json(p.position, b.flat);
        }
        if (s == "$local") {
            return point_json(b.local, b.flat);
        }
        if (s == "$rotation") {
            return b.flat ? json::Value(num(b.local_rotation.z))
                          : json::Value(point_json(b.local_rotation, false));
        }
        if (s == "$yaw") {
            return num(b.flat ? b.local_rotation.z : b.local_rotation.y);
        }
        if (s == "$scale") {
            return num(b.local_scale);
        }
        if (s == "$density") {
            return num(p.density);
        }
        if (s == "$index") {
            return static_cast<std::int64_t>(b.index + 1);
        }
        if (s == "$seed") {
            return static_cast<std::int64_t>(p.seed);
        }
        if (s == "$id") {
            return b.id;
        }
        if (s == "$parent") {
            return b.parent;
        }
        if (s == "$node") {
            return b.node;
        }
        return value;
    }
    if (value.is_array()) {
        json::Array out;
        for (const json::Value& item : value.as_array()) {
            out.push_back(substitute(item, b, depth));
        }
        return out;
    }
    if (value.is_object()) {
        const json::Object& object = value.as_object();
        if (object.size() == 1) {
            if (const json::Value* pick = value.find("$pick");
                pick != nullptr && pick->is_array() && !pick->as_array().empty()) {
                const json::Array& choices = pick->as_array();
                const std::uint32_t h = procedural::pcg_hash(b.point->seed, kPickSalt, depth);
                return substitute(choices[h % choices.size()], b, depth + 1);
            }
        }
        json::Value out = json::Object{};
        for (const auto& [key, item] : object) {
            out[key] = substitute(item, b, depth);
        }
        return out;
    }
    return value;
}

procedural::ScatterPlace place_of(const PcgPoint& point) {
    return {.position = point.position,
            .rotation = point.rotation,
            .scale = point.scale,
            .normal = point.normal};
}

// Adds a child to the parent's Transform3D or Transform2D children, once.
void adopt(world::World& world, entt::entity parent, entt::entity child, bool flat) {
    auto& ecs = world.ecs();
    std::vector<entt::entity>& children = flat ? ecs.get<world::Transform2D>(parent).children
                                               : ecs.get<world::Transform3D>(parent).children;
    if (std::ranges::find(children, child) == children.end()) {
        children.push_back(child);
    }
}

Reply pcg_run(Session& session, const json::Value& params) {
    world::World& world = session.world();
    entt::registry& ecs = world.ecs();
    Loaded loaded;
    if (auto failed = load_graph(session, params, loaded)) {
        return std::move(*failed);
    }
    const bool flat = loaded.flat;
    const PcgGraph& graph = loaded.graph;

    const std::string into = params.find("into")->as_string();
    entt::entity parent = world.find(into);
    if (parent != entt::null && (flat ? !ecs.all_of<world::Transform2D>(parent)
                                      : !ecs.all_of<world::Transform3D>(parent))) {
        return param_error(code::kInvalidParams, "/into",
                           quoted(into) + " has no " + (flat ? "Transform2D" : "Transform3D") +
                               " to place things in");
    }

    // Every node's points, before anything changes.
    Evaluated done;
    if (auto failed = evaluate(session, loaded, parent, done)) {
        return std::move(*failed);
    }
    if (auto failed = over_budget(loaded, done)) {
        return std::move(*failed);
    }
    // Names and commands checked before anything is made, so a failure changes nothing.
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const PcgNode& node = graph.nodes[i];
        if (!procedural::pcg_is_spawner(node.op)) {
            continue;
        }
        const std::string path = loaded.nodes_path + std::to_string(i);
        const bool each = node.op == "spawn" || node.op == "command";
        const std::string longest =
            into + "_" + node.name +
            (each ? "_" + std::to_string(std::max<std::size_t>(done.points[i].size(), 1))
                  : std::string());
        if (!world::World::is_valid_id(longest)) {
            return param_error(code::kInvalidParams, "/into",
                               quoted(longest) + " would be too long an id: give into or the " +
                                   "node a shorter name");
        }
        if (node.op == "command") {
            const std::string name = node.fields.find("command")->as_string();
            const Command* command = session.commands().find(name);
            if (command == nullptr) {
                std::string message = "no command is named " + quoted(name);
                std::vector<std::string_view> names;
                for (const Command& each_command : session.commands().commands()) {
                    names.push_back(each_command.name);
                }
                if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                return param_error(code::kInvalidParams, path + "/command", message);
            }
            if (command->effect != Effect::Edit) {
                return param_error(code::kInvalidParams, path + "/command",
                                   quoted(name) + " doesn't place things: a graph calls "
                                                  "commands that edit the world");
            }
        }
        if (node.op == "foliage") {
            const std::string type = node.fields.find("type")->as_string();
            if (!type.ends_with(".foliage.json")) {
                return param_error(code::kInvalidParams, path + "/type",
                                   "expected a .foliage.json file");
            }
            if (auto loaded_type = session.assets().load<json::Value>(type); !loaded_type) {
                return param_error(code::kInvalidArgument, path + "/type",
                                   loaded_type.error().message);
            }
        }
    }

    // Out with the old.
    if (parent != entt::null) {
        std::vector<entt::entity> old;
        collect_descendants(world, parent, old);
        for (const entt::entity entity : old) {
            world.destroy(entity);
        }
        if (auto* t3 = ecs.try_get<world::Transform3D>(parent); t3 != nullptr) {
            t3->children.clear();
        }
        if (auto* t2 = ecs.try_get<world::Transform2D>(parent); t2 != nullptr) {
            t2->children.clear();
        }
    } else {
        auto created = world.create(into);
        if (!created) {
            return param_error(code::kInvalidParams, "/into", created.error().message);
        }
        parent = created.value();
        if (flat) {
            ecs.emplace<world::Transform2D>(parent);
        } else {
            ecs.emplace<world::Transform3D>(parent);
        }
    }
    world::Affine3D to_local_3d;
    world::Affine2D to_local_2d;
    if (flat) {
        to_local_2d = world::Tree2D(world).global_affine(parent).inverse();
    } else {
        to_local_3d = world::Tree3D(world).global(parent).inverse();
    }
    const auto bind = [&](const PcgPoint& point, std::size_t k, const std::string& id,
                          const std::string& node) {
        Bindings b;
        b.point = &point;
        b.flat = flat;
        b.index = k;
        b.id = id;
        b.parent = into;
        b.node = node;
        if (flat) {
            const world::Transform2D local = local_2d(to_local_2d, place_of(point));
            b.local = {local.position.x, local.position.y, 0.0F};
            b.local_rotation = {0.0F, 0.0F, local.rotation};
            b.local_scale = local.scale.x;
        } else {
            const world::Transform3D local = local_3d(to_local_3d, place_of(point));
            b.local = local.position;
            b.local_rotation = local.rotation;
            b.local_scale = local.scale.x;
        }
        return b;
    };
    const auto placed = [&](const Bindings& b) {
        json::Value transform;
        transform["position"] = point_json(b.local, flat);
        if (flat) {
            transform["rotation"] = num(b.local_rotation.z);
            transform["scale"] = json::Array{num(b.local_scale), num(b.local_scale)};
        } else {
            transform["rotation"] = point_json(b.local_rotation, false);
            transform["scale"] =
                json::Array{num(b.local_scale), num(b.local_scale), num(b.local_scale)};
        }
        return transform;
    };

    // In with the new.
    json::Array report;
    std::size_t made_total = 0;
    std::vector<std::string> by_commands;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const PcgNode& node = graph.nodes[i];
        const std::vector<PcgPoint>& points = done.points[i];
        const std::string path = loaded.nodes_path + std::to_string(i);
        const std::string prefix = into + "_" + node.name;
        json::Value entry;
        entry["name"] = node.name;
        entry["points"] = static_cast<std::int64_t>(points.size());
        std::size_t made = 0;
        if (node.op == "spawn") {
            const json::Value* components = node.fields.find("components");
            const json::Value* prefab = node.fields.find("prefab");
            for (std::size_t k = 0; k < points.size(); ++k) {
                const std::string id = prefix + "_" + std::to_string(k + 1);
                const Bindings b = bind(points[k], k, id, node.name);
                json::Value create;
                create["id"] = id;
                if (prefab != nullptr) {
                    create["extends"] = *prefab;
                }
                json::Value parts = components != nullptr ? substitute(*components, b)
                                                          : json::Value(json::Object{});
                json::Value& transform = parts[flat ? "Transform2D" : "Transform3D"];
                if (!transform.is_object()) {
                    transform = json::Object{};
                }
                const json::Value place = placed(b);
                for (const auto& [key, value] : place.as_object()) {
                    transform[key] = value;
                }
                create["components"] = std::move(parts);
                Reply reply = session.call("entity.create", create);
                if (!reply.ok) {
                    return Reply::failure(reply.code, path + " (spawn): " + reply.message,
                                          std::move(reply.data));
                }
                adopt(world, parent, world.find(id), flat);
                ++made;
            }
        } else if (node.op == "multimesh") {
            const entt::entity entity = world.create(prefix).value();
            ecs.emplace<world::Transform3D>(entity);
            world::MultiMeshInstance3D multimesh;
            multimesh.mesh = node.fields.find("mesh")->as_string();
            for (const PcgPoint& point : points) {
                const world::Transform3D local = local_3d(to_local_3d, place_of(point));
                multimesh.instances.push_back(
                    {.position = local.position, .rotation = local.rotation, .scale = local.scale});
            }
            ecs.emplace<world::MultiMeshInstance3D>(entity, std::move(multimesh));
            adopt(world, parent, entity, flat);
            made = 1;
        } else if (node.op == "foliage") {
            const entt::entity entity = world.create(prefix).value();
            const std::string type = node.fields.find("type")->as_string();
            std::vector<float> instances;
            for (const PcgPoint& point : points) {
                if (flat) {
                    const world::Transform2D local = local_2d(to_local_2d, place_of(point));
                    instances.insert(instances.end(),
                                     {static_cast<float>(local.position.x),
                                      static_cast<float>(local.position.y), local.rotation,
                                      static_cast<float>(local.scale.x)});
                } else {
                    const world::Transform3D local = local_3d(to_local_3d, place_of(point));
                    instances.insert(
                        instances.end(),
                        {static_cast<float>(local.position.x), static_cast<float>(local.position.y),
                         static_cast<float>(local.position.z), static_cast<float>(local.rotation.x),
                         static_cast<float>(local.rotation.y), static_cast<float>(local.rotation.z),
                         static_cast<float>(local.scale.x)});
                }
            }
            if (flat) {
                ecs.emplace<world::Transform2D>(entity);
                world::Foliage2D foliage;
                foliage.groups.push_back({.type = type, .instances = std::move(instances)});
                ecs.emplace<world::Foliage2D>(entity, std::move(foliage));
            } else {
                ecs.emplace<world::Transform3D>(entity);
                world::Foliage3D foliage;
                foliage.groups.push_back({.type = type, .instances = std::move(instances)});
                ecs.emplace<world::Foliage3D>(entity, std::move(foliage));
            }
            adopt(world, parent, entity, flat);
            made = 1;
        } else if (node.op == "route" && !points.empty()) {
            const bool closed =
                node.fields.find("closed") != nullptr && node.fields.find("closed")->as_bool();
            json::Value create;
            create["id"] = prefix;
            json::Value parts = node.fields.find("components") != nullptr
                                    ? substitute(*node.fields.find("components"),
                                                 bind(points.front(), 0, prefix, node.name))
                                    : json::Value(json::Object{});
            json::Array curve;
            for (std::size_t k = 0; k < points.size(); ++k) {
                const Bindings b = bind(points[k], k, prefix, node.name);
                json::Value at;
                at["position"] = point_json(b.local, flat);
                curve.push_back(std::move(at));
            }
            if (flat) {
                if (closed && points.size() > 2) {
                    curve.push_back(curve.front());
                }
                parts["Transform2D"] = json::Object{};
                parts["Path2D"]["points"] = std::move(curve);
            } else {
                parts["Transform3D"] = json::Object{};
                parts["Path3D"]["points"] = std::move(curve);
                parts["Path3D"]["closed"] = closed;
            }
            create["components"] = std::move(parts);
            Reply reply = session.call("entity.create", create);
            if (!reply.ok) {
                return Reply::failure(reply.code, path + " (route): " + reply.message,
                                      std::move(reply.data));
            }
            adopt(world, parent, world.find(prefix), flat);
            made = 1;
        } else if (node.op == "command") {
            const std::string name = node.fields.find("command")->as_string();
            const json::Value& templ = *node.fields.find("params");
            for (std::size_t k = 0; k < points.size(); ++k) {
                const std::string id = prefix + "_" + std::to_string(k + 1);
                Reply reply =
                    session.call(name, substitute(templ, bind(points[k], k, id, node.name)));
                if (!reply.ok) {
                    std::string message = path;
                    message += " (";
                    message += name;
                    message += ", point ";
                    message += std::to_string(k + 1);
                    message += "): ";
                    message += reply.message;
                    return Reply::failure(reply.code, message, std::move(reply.data));
                }
                by_commands.push_back(id);
                ++made;
            }
        }
        if (procedural::pcg_is_spawner(node.op)) {
            entry["made"] = static_cast<std::int64_t>(made);
        }
        made_total += made;
        report.push_back(std::move(entry));
    }
    // What commands made with no parent of their own goes under into too, so the next run
    // replaces it.
    if (!by_commands.empty()) {
        std::vector<entt::entity> orphans;
        if (flat) {
            const world::Tree2D tree(world);
            for (const std::string& id : by_commands) {
                const entt::entity entity = world.find(id);
                if (entity != entt::null && ecs.all_of<world::Transform2D>(entity) &&
                    tree.parent(entity) == entt::null) {
                    orphans.push_back(entity);
                }
            }
        } else {
            const world::Tree3D tree(world);
            for (const std::string& id : by_commands) {
                const entt::entity entity = world.find(id);
                if (entity != entt::null && ecs.all_of<world::Transform3D>(entity) &&
                    tree.parent(entity) == entt::null) {
                    orphans.push_back(entity);
                }
            }
        }
        for (const entt::entity entity : orphans) {
            adopt(world, parent, entity, flat);
        }
    }

    json::Value out;
    out["into"] = into;
    out["nodes"] = std::move(report);
    out["made"] = static_cast<std::int64_t>(made_total);
    if (!done.warnings.empty()) {
        json::Array list;
        for (std::string& warning : done.warnings) {
            list.emplace_back(std::move(warning));
        }
        out["warnings"] = std::move(list);
    }
    return Reply::success(std::move(out));
}

// ---- looking ----

// The area points cover: their bounds, or what expect names.
float covered_area(const std::array<float, 4>& bounds) {
    return std::max(0.0F, bounds[2] - bounds[0]) * std::max(0.0F, bounds[3] - bounds[1]);
}

entt::entity into_of(const Session& session, const json::Value& params) {
    const json::Value* into = params.find("into");
    return into != nullptr ? session.world().find(into->as_string()) : entt::null;
}

constexpr std::array<std::array<std::uint8_t, 3>, 8> kPalette{{{86, 180, 233},
                                                               {230, 159, 0},
                                                               {0, 158, 115},
                                                               {240, 228, 66},
                                                               {204, 121, 167},
                                                               {213, 94, 0},
                                                               {0, 114, 178},
                                                               {255, 255, 255}}};

Reply pcg_preview(Session& session, const json::Value& params) {
    Loaded loaded;
    if (auto failed = load_graph(session, params, loaded)) {
        return std::move(*failed);
    }
    Evaluated done;
    if (auto failed = evaluate(session, loaded, into_of(session, params), done)) {
        return std::move(*failed);
    }
    const bool flat = loaded.flat;
    const json::Value* sample_param = params.find("sample");
    const std::size_t sample_count =
        sample_param != nullptr ? static_cast<std::size_t>(sample_param->as_int()) : 3;
    json::Array nodes;
    std::vector<PcgPoint> all;
    for (std::size_t i = 0; i < loaded.graph.nodes.size(); ++i) {
        const PcgNode& node = loaded.graph.nodes[i];
        const std::vector<PcgPoint>& points = done.points[i];
        json::Value entry;
        entry["name"] = node.name;
        entry["op"] = node.op;
        entry["points"] = static_cast<std::int64_t>(points.size());
        if (!points.empty()) {
            const std::array<float, 4> b = procedural::pcg_bounds(points, flat);
            entry["bounds"] = json::Array{num(b[0]), num(b[1]), num(b[2]), num(b[3])};
            json::Array sample;
            for (std::size_t k = 0; k < std::min(sample_count, points.size()); ++k) {
                sample.push_back(point_json(points[k].position, flat));
            }
            if (!sample.empty()) {
                entry["sample"] = std::move(sample);
            }
        }
        if (procedural::pcg_is_spawner(node.op)) {
            entry["makes"] = static_cast<std::int64_t>(made_by(node, points.size()));
        }
        nodes.push_back(std::move(entry));
        all.insert(all.end(), points.begin(), points.end());
    }
    json::Value out;
    out["nodes"] = std::move(nodes);

    // A top-down picture: every node's points as dots in its own color, later nodes over earlier.
    if (const json::Value* image_param = params.find("image"); image_param != nullptr) {
        const std::string file = image_param->as_string();
        if (!file.ends_with(".png")) {
            return param_error(code::kInvalidParams, "/image", quoted(file) + " must end in .png");
        }
        auto resolved = session.resolve_path(file);
        if (!resolved) {
            return param_error(code::kInvalidParams, "/image", resolved.error().message);
        }
        const json::Value* size_param = params.find("size");
        const int size = size_param != nullptr ? static_cast<int>(size_param->as_int()) : 256;
        std::array<float, 4> b = procedural::pcg_bounds(all, flat);
        const float extent = std::max({b[2] - b[0], b[3] - b[1], 1.0e-3F});
        const float margin = extent * 0.05F;
        const float scale = static_cast<float>(size - 1) / (extent + (2.0F * margin));
        render::Image image;
        image.width = static_cast<std::uint32_t>(size);
        image.height = static_cast<std::uint32_t>(size);
        image.pixels.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4U,
                            0);
        for (std::size_t p = 0; p < image.pixels.size(); p += 4) {
            image.pixels[p] = 32;
            image.pixels[p + 1] = 34;
            image.pixels[p + 2] = 38;
            image.pixels[p + 3] = 255;
        }
        for (std::size_t i = 0; i < done.points.size(); ++i) {
            const std::array<std::uint8_t, 3>& color = kPalette[i % kPalette.size()];
            for (const PcgPoint& point : done.points[i]) {
                const float u = static_cast<float>(point.position.x) - b[0] + margin;
                const float v =
                    static_cast<float>(flat ? point.position.y : point.position.z) - b[1] + margin;
                const int cx = static_cast<int>(std::lround(u * scale));
                const int cy = static_cast<int>(std::lround(v * scale));
                for (int y = cy - 1; y <= cy + 1; ++y) {
                    for (int x = cx - 1; x <= cx + 1; ++x) {
                        if (x < 0 || y < 0 || x >= size || y >= size) {
                            continue;
                        }
                        const std::size_t at =
                            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) +
                             static_cast<std::size_t>(x)) *
                            4U;
                        image.pixels[at] = color[0];
                        image.pixels[at + 1] = color[1];
                        image.pixels[at + 2] = color[2];
                    }
                }
            }
        }
        auto png = render::encode_png(image);
        if (!png) {
            return Reply::failure(png.error());
        }
        if (auto written = session.write_file(resolved.value(), png.value()); !written) {
            return Reply::failure(written.error());
        }
        out["image"] = file;
        json::Array legend;
        for (std::size_t i = 0; i < loaded.graph.nodes.size(); ++i) {
            const std::array<std::uint8_t, 3>& c = kPalette[i % kPalette.size()];
            std::array<char, 8> hex{};
            std::snprintf(hex.data(), hex.size(), "#%02x%02x%02x", c[0], c[1], c[2]);
            json::Value item;
            item["node"] = loaded.graph.nodes[i].name;
            item["color"] = std::string(hex.data());
            legend.push_back(std::move(item));
        }
        out["legend"] = std::move(legend);
    }
    if (!done.warnings.empty()) {
        json::Array list;
        for (std::string& warning : done.warnings) {
            list.emplace_back(std::move(warning));
        }
        out["warnings"] = std::move(list);
    }
    return Reply::success(std::move(out));
}

Reply pcg_check(Session& session, const json::Value& params) {
    world::World& world = session.world();
    Loaded loaded;
    if (auto failed = load_graph(session, params, loaded)) {
        return std::move(*failed);
    }
    const bool flat = loaded.flat;
    Evaluated done;
    if (auto failed = evaluate(session, loaded, into_of(session, params), done)) {
        return std::move(*failed);
    }
    const PcgGraph& graph = loaded.graph;

    // Where reachability is walked from.
    std::optional<Vec3> reach_from;
    if (const json::Value* from = params.find("reach_from"); from != nullptr) {
        if (from->is_string()) {
            const entt::entity entity = world.find(from->as_string());
            if (entity == entt::null) {
                return unknown_entity(world, from->as_string(), "/reach_from");
            }
            if (flat) {
                const world::Transform2D at = world::Tree2D(world).global(entity);
                reach_from = Vec3{at.position.x, at.position.y, 0.0F};
            } else {
                reach_from = world::Tree3D(world).global(entity).origin;
            }
        } else {
            const json::Array& p = from->as_array();
            reach_from =
                Vec3{static_cast<float>(p[0].as_double()), static_cast<float>(p[1].as_double()),
                     p.size() > 2 ? static_cast<float>(p[2].as_double()) : 0.0F};
        }
    }
    double tolerance_value = flat ? 16.0 : 0.5;
    if (const json::Value* given = params.find("reach_tolerance"); given != nullptr) {
        tolerance_value = given->as_double();
    }
    const auto tolerance = static_cast<float>(tolerance_value);

    json::Array problems;
    json::Array nodes;
    std::size_t entities = 0;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const PcgNode& node = graph.nodes[i];
        const std::vector<PcgPoint>& points = done.points[i];
        const auto count = static_cast<std::int64_t>(points.size());
        const auto problem = [&](std::string_view kind) {
            json::Value p;
            p["node"] = node.name;
            p["problem"] = kind;
            return p;
        };
        json::Value entry;
        entry["name"] = node.name;
        entry["points"] = count;
        if (procedural::pcg_is_spawner(node.op)) {
            const std::size_t makes = made_by(node, points.size());
            entities += makes;
            entry["makes"] = static_cast<std::int64_t>(makes);
            if (points.empty()) {
                json::Value p = problem("empty");
                p["message"] = "spawns nothing: no point reaches it";
                problems.push_back(std::move(p));
            }
        }
        if (graph.budget.points && count > *graph.budget.points) {
            json::Value p = problem("over_budget");
            p["budget"] = *graph.budget.points;
            p["points"] = count;
            problems.push_back(std::move(p));
        }
        if (const json::Value* expect = node.fields.find("expect"); expect != nullptr) {
            if (const json::Value* min = expect->find("min");
                min != nullptr && count < min->as_int()) {
                json::Value p = problem("too_few");
                p["points"] = count;
                p["min"] = *min;
                problems.push_back(std::move(p));
            }
            if (const json::Value* max = expect->find("max");
                max != nullptr && count > max->as_int()) {
                json::Value p = problem("too_many");
                p["points"] = count;
                p["max"] = *max;
                problems.push_back(std::move(p));
            }
            if (const std::optional<Vec2> range = range_of(*expect, "density"); range) {
                std::array<float, 4> area = procedural::pcg_bounds(points, flat);
                if (const std::optional<std::array<float, 4>> given = area_of(*expect); given) {
                    area = *given;
                }
                const float covered = covered_area(area);
                const double density =
                    covered > 0.0F ? static_cast<double>(points.size()) / covered : 0.0;
                entry["density"] = std::round(density * 1.0e6) / 1.0e6;
                if (density < range->x || density > range->y) {
                    json::Value p = problem("density");
                    p["density"] = std::round(density * 1.0e6) / 1.0e6;
                    p["range"] = json::Array{num(range->x), num(range->y)};
                    problems.push_back(std::move(p));
                }
            }
            if (const json::Value* reachable = expect->find("reachable");
                reachable != nullptr && reachable->as_bool()) {
                if (!reach_from) {
                    return param_error(code::kInvalidParams, "/reach_from",
                                       quoted(node.name) +
                                           " expects reachable points: give reach_from, an "
                                           "entity or point to walk from");
                }
                json::Array unreachable;
                std::size_t missed = 0;
                for (const PcgPoint& point : points) {
                    bool reached = false;
                    if (flat) {
                        const auto walked =
                            navigation::query_path(world, Vec2{reach_from->x, reach_from->y},
                                                   Vec2{point.position.x, point.position.y});
                        if (!walked.points.empty()) {
                            const Vec2 end = walked.points.back();
                            const float dx = to_float(end.x - point.position.x);
                            const float dy = to_float(end.y - point.position.y);
                            reached = std::sqrt((dx * dx) + (dy * dy)) <= tolerance;
                        }
                    } else {
                        const auto walked =
                            navigation::query_path(world, *reach_from, point.position);
                        if (!walked.points.empty()) {
                            const Vec3 end = walked.points.back();
                            const float dx = to_float(end.x - point.position.x);
                            const float dz = to_float(end.z - point.position.z);
                            reached = std::sqrt((dx * dx) + (dz * dz)) <= tolerance;
                        }
                    }
                    if (!reached) {
                        ++missed;
                        if (unreachable.size() < 5) {
                            unreachable.push_back(point_json(point.position, flat));
                        }
                    }
                }
                if (missed > 0) {
                    json::Value p = problem("unreachable");
                    p["count"] = static_cast<std::int64_t>(missed);
                    p["first"] = std::move(unreachable);
                    problems.push_back(std::move(p));
                }
            }
        }
        nodes.push_back(std::move(entry));
    }
    if (graph.budget.entities && std::cmp_greater(entities, *graph.budget.entities)) {
        json::Value p;
        p["problem"] = "over_entity_budget";
        p["budget"] = *graph.budget.entities;
        p["makes"] = static_cast<std::int64_t>(entities);
        problems.push_back(std::move(p));
    }
    json::Value out;
    out["passed"] = problems.empty();
    out["nodes"] = std::move(nodes);
    out["makes"] = static_cast<std::int64_t>(entities);
    if (!problems.empty()) {
        out["problems"] = std::move(problems);
    }
    if (!done.warnings.empty()) {
        json::Array list;
        for (std::string& warning : done.warnings) {
            list.emplace_back(std::move(warning));
        }
        out["warnings"] = std::move(list);
    }
    return Reply::success(std::move(out));
}

json::Value graph_params() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    json::Value& p = schema["properties"];
    p["path"] = property("string", "A .pcg.json file. $ slash-cli schema pcg.");
    p["graph"] = property("object", "The graph itself, as a .pcg.json file holds it.");
    p["seed"] = property("integer", "Run with this seed in place of the graph's.");
    p["seed"]["minimum"] = 0;
    p["seed"]["maximum"] = 4294967295LL;
    return schema;
}

} // namespace

void register_pcg_commands(Registry& registry) {
    {
        json::Value schema = graph_params();
        schema["required"] = json::Array{"into"};
        schema["properties"]["into"] =
            property("string", "ID of the entity everything is placed under, made if missing; "
                               "what an earlier run placed there is replaced.");
        registry.add({
            .name = "pcg.run",
            .description = "Follows a .pcg.json procedural graph over the world, as Unreal's "
                           "PCG: samplers make points, filters thin and move them, spawners "
                           "place prefabs, multimeshes, foliage, routes and any command's things "
                           "(creatures, characters, vehicles, lights, effects) under into. "
                           "Returns each node's points and what it made.",
            .capabilities = {Capability::Read, Capability::Edit},
            .effect = Effect::Compound,
            .params = std::move(schema),
            .handler = pcg_run,
        });
    }
    {
        json::Value schema = graph_params();
        schema["properties"]["into"] =
            property("string", "Leave what a run placed under this entity out of the ground.");
        schema["properties"]["sample"] =
            property("integer", "Points listed a node, from the first. Default 3.");
        schema["properties"]["sample"]["minimum"] = 0;
        schema["properties"]["image"] =
            property("string", "A PNG to draw: every node's points from above, in its color.");
        schema["properties"]["size"] = property("integer", "The PNG's width and height. Default "
                                                           "256.");
        schema["properties"]["size"]["minimum"] = 16;
        schema["properties"]["size"]["maximum"] = 2048;
        registry.add({
            .name = "pcg.preview",
            .description = "Runs a .pcg.json graph's nodes without placing anything: each "
                           "node's point count, bounds and first points, what its spawners "
                           "would make and, with image, a top-down PNG.",
            .capabilities = {Capability::Read, Capability::Filesystem},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = pcg_preview,
        });
    }
    {
        json::Value schema = graph_params();
        schema["properties"]["into"] =
            property("string", "Leave what a run placed under this entity out of the ground.");
        json::Value from;
        from["description"] = "Where reachable points are walked from: an entity ID, or [x, y, z] "
                              "([x, y] in 2D).";
        json::Value as_id;
        as_id["type"] = "string";
        json::Value as_point;
        as_point["type"] = "array";
        as_point["items"]["type"] = "number";
        as_point["minItems"] = 2;
        as_point["maxItems"] = 3;
        from["anyOf"] = json::Array{std::move(as_id), std::move(as_point)};
        schema["properties"]["reach_from"] = std::move(from);
        schema["properties"]["reach_tolerance"] =
            property("number", "How near a path must end to a point to reach it. Default 0.5 m, "
                               "16 px in 2D.");
        schema["properties"]["reach_tolerance"]["minimum"] = 0;
        registry.add({
            .name = "pcg.check",
            .description = "Holds a .pcg.json graph to its budget and each node's expect "
                           "(counts, density, reachable by navigation from reach_from) without "
                           "placing anything. Returns passed, problems and each node's points.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = pcg_check,
        });
    }
}

} // namespace slash::command
