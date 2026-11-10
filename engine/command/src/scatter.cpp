#include "slash/procedural/scatter.hpp"

#include "procedural.hpp"

#include "slash/canvas/tilemap.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/procedural/foliage.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// scatter.run: places what .scatter.json rules ask for, as entities under one parent or as a
// MultiMeshInstance3D a layer. The ground is what navigation bakes read in 3D
// (navigation::world_geometry) and tile map cells in 2D; procedural::scatter picks the places.
namespace slash::command {
namespace {

using procedural::ScatterLayer;
using procedural::ScatterPlace;
using procedural::ScatterTriangle;

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

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

// A place in the parent's space, as a Transform3D's fields: the parent's inverse after it.
world::Transform3D local_3d(const world::Affine3D& to_local, const ScatterPlace& place) {
    world::Transform3D placed;
    placed.position = place.position;
    placed.rotation = place.rotation;
    placed.scale = {place.scale, place.scale, place.scale};
    const world::Affine3D local = to_local * world::affine(placed);
    world::Transform3D out;
    out.position = local.origin;
    out.scale = {length(local.x), length(local.y), length(local.z)};
    world::Affine3D rotation;
    rotation.x = out.scale.x > 0.0F ? local.x * (1.0F / out.scale.x) : Vec3{1.0F, 0.0F, 0.0F};
    rotation.y = out.scale.y > 0.0F ? local.y * (1.0F / out.scale.y) : Vec3{0.0F, 1.0F, 0.0F};
    rotation.z = out.scale.z > 0.0F ? local.z * (1.0F / out.scale.z) : Vec3{0.0F, 0.0F, 1.0F};
    out.rotation = world::euler(rotation);
    return out;
}

// The same in 2D.
world::Transform2D local_2d(const world::Affine2D& to_local, const ScatterPlace& place) {
    world::Transform2D placed;
    placed.position = {place.position.x, place.position.y};
    placed.rotation = place.rotation.z;
    placed.scale = {place.scale, place.scale};
    return world::parts(to_local * world::affine(placed));
}

// Whether the entity is one of on or under one of them.
bool under(const std::set<entt::entity>& on, entt::entity entity,
           const std::function<entt::entity(entt::entity)>& parent) {
    for (entt::entity at = entity; at != entt::null; at = parent(at)) {
        if (on.contains(at)) {
            return true;
        }
    }
    return false;
}

// The rectangle of an area as two triangles, at z 0 in 2D and on y 0 in 3D.
void add_area(const std::array<float, 4>& area, bool flat, std::vector<ScatterTriangle>& out) {
    const auto at = [flat](float u, float v) { return flat ? Vec3{u, v, 0.0F} : Vec3{u, 0.0F, v}; };
    const Vec3 a = at(area[0], area[1]);
    const Vec3 b = at(area[2], area[1]);
    const Vec3 c = at(area[2], area[3]);
    const Vec3 d = at(area[0], area[3]);
    out.push_back({a, b, c});
    out.push_back({a, c, d});
}

// A layer's 3D ground: the geometry of the entities on names and those under them.
std::vector<ScatterTriangle> ground_3d(const std::vector<navigation::EntityGeometry>& geometry,
                                       const world::Tree3D& tree,
                                       const std::set<entt::entity>& on) {
    std::vector<ScatterTriangle> out;
    const auto parent = [&tree](entt::entity e) { return tree.parent(e); };
    for (const navigation::EntityGeometry& each : geometry) {
        if (!under(on, each.entity, parent)) {
            continue;
        }
        const std::vector<float>& v = each.geometry.vertices;
        const auto vertex = [&v](int i) {
            const auto at = static_cast<std::size_t>(i) * 3;
            return Vec3{v[at], v[at + 1], v[at + 2]};
        };
        const std::vector<int>& indices = each.geometry.indices;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            out.push_back({vertex(indices[i]), vertex(indices[i + 1]), vertex(indices[i + 2])});
        }
    }
    return out;
}

// A layer's 2D ground: the rectangles of the cells of the TileMapLayers on names whose key or
// tile is in tiles (any, when it is empty), in world space.
Result<std::vector<ScatterTriangle>> ground_2d(Session& session, const ScatterLayer& layer,
                                               const std::vector<entt::entity>& on,
                                               const std::string& path) {
    world::World& world = session.world();
    const world::Tree2D tree(world);
    std::vector<ScatterTriangle> out;
    for (std::size_t i = 0; i < on.size(); ++i) {
        const auto* map = world.ecs().try_get<world::TileMapLayer>(on[i]);
        if (map == nullptr) {
            return make_error(ErrorCode::InvalidArgument,
                              path + "/on/" + std::to_string(i) + ": " +
                                  quoted(world.id_of(on[i])) +
                                  " has no TileMapLayer: 2D layers scatter over tile maps' cells");
        }
        auto set = session.assets().load<canvas::TileSet>(map->tile_set);
        if (!set) {
            return make_error(ErrorCode::InvalidArgument,
                              path + "/on/" + std::to_string(i) + ": " + set.error().message);
        }
        if (map->legend.empty()) {
            continue;
        }
        const std::size_t width = map->legend.begin()->first.size();
        const world::Affine2D place = tree.global_affine(on[i]);
        const Vec2 half{static_cast<float>(set.value()->tile_size.x) * 0.5F,
                        static_cast<float>(set.value()->tile_size.y) * 0.5F};
        for (std::size_t row = 0; row < map->rows.size(); ++row) {
            const std::string& keys = map->rows[row];
            for (std::size_t column = 0; column * width + width <= keys.size(); ++column) {
                const std::string key = keys.substr(column * width, width);
                const auto tile = map->legend.find(key);
                if (tile == map->legend.end()) {
                    continue; // empty, or reported by scene validate
                }
                if (!layer.tiles.empty() &&
                    std::ranges::find(layer.tiles, key) == layer.tiles.end() &&
                    std::ranges::find(layer.tiles, tile->second) == layer.tiles.end()) {
                    continue;
                }
                const Vec2i cell{map->origin.x + static_cast<int>(column),
                                 map->origin.y + static_cast<int>(row)};
                const Vec2 center = canvas::map_to_local(*set.value(), cell);
                const auto corner = [&](float sx, float sy) {
                    const Vec2 p =
                        place.apply({center.x + (sx * half.x), center.y + (sy * half.y)});
                    return Vec3{p.x, p.y, 0.0F};
                };
                const Vec3 a = corner(-1.0F, -1.0F);
                const Vec3 b = corner(1.0F, -1.0F);
                const Vec3 c = corner(1.0F, 1.0F);
                const Vec3 d = corner(-1.0F, 1.0F);
                out.push_back({a, b, c});
                out.push_back({a, c, d});
            }
        }
    }
    return out;
}

// The entities under the parent, and theirs, so a run replaces what the one before made.
void collect_descendants(const world::World& world, entt::entity entity,
                         std::vector<entt::entity>& out) {
    const auto& ecs = world.ecs();
    std::vector<entt::entity> children;
    if (const auto* t3 = ecs.try_get<const world::Transform3D>(entity); t3 != nullptr) {
        children = t3->children;
    } else if (const auto* t2 = ecs.try_get<const world::Transform2D>(entity); t2 != nullptr) {
        children = t2->children;
    }
    for (const entt::entity child : children) {
        if (world.valid(child) && std::ranges::find(out, child) == out.end()) {
            out.push_back(child);
            collect_descendants(world, child, out);
        }
    }
}

Reply scatter_run(Session& session, const json::Value& params) {
    world::World& world = session.world();
    entt::registry& ecs = world.ecs();

    // The rules, from a file or given inline.
    const json::Value* path = params.find("path");
    const json::Value* inline_rules = params.find("rules");
    if ((path == nullptr) == (inline_rules == nullptr)) {
        return param_error(code::kInvalidParams, "", "give path or rules, one of them");
    }
    json::Value loaded_doc;
    const json::Value* doc = inline_rules;
    std::string at = "/rules";
    if (path != nullptr) {
        const std::string& file = path->as_string();
        if (!file.ends_with(procedural::kScatterSuffix)) {
            return param_error(code::kInvalidArgument, "/path", "expected a .scatter.json file");
        }
        auto loaded = session.assets().load<json::Value>(file);
        if (!loaded) {
            return param_error(loaded.error().code == ErrorCode::NotFound ? code::kNotFound
                                                                          : code::kInvalidArgument,
                               "/path", loaded.error().message);
        }
        loaded_doc = *loaded.value();
        doc = &loaded_doc;
        at = file;
    }
    std::vector<json::ValidationError> errors = procedural::validate_scatter(*doc);
    if (!errors.empty()) {
        if (path == nullptr) {
            for (json::ValidationError& error : errors) {
                error.path = "/rules" + error.path;
            }
            return Reply::invalid_params(errors);
        }
        const json::ValidationError& first = errors.front();
        return param_error(code::kInvalidArgument, "/path",
                           quoted(at) + (first.path.empty() ? "" : " at " + first.path) + ": " +
                               first.message);
    }
    const procedural::ScatterRules rules = procedural::parse_scatter(*doc).value();
    const bool flat = rules.space == "2d";
    const std::string layers_path = path == nullptr ? "/rules/layers/" : at + " at /layers/";

    // The parent: made if missing, else what it holds is replaced.
    const std::string into = params.find("into")->as_string();
    entt::entity parent = world.find(into);
    if (parent != entt::null) {
        if (flat ? !ecs.all_of<world::Transform2D>(parent)
                 : !ecs.all_of<world::Transform3D>(parent)) {
            return param_error(code::kInvalidParams, "/into",
                               quoted(into) + " has no " + (flat ? "Transform2D" : "Transform3D") +
                                   " to place things in");
        }
    }

    // Every layer's ground and places, before anything changes.
    struct Layer {
        procedural::ScatterResult result;
    };
    std::vector<Layer> done;
    std::vector<navigation::EntityGeometry> geometry;
    std::vector<std::string> warnings;
    std::optional<world::Tree3D> tree3d;
    if (!flat) {
        // What the previous run made isn't ground for this one.
        std::vector<entt::entity> old;
        if (parent != entt::null) {
            collect_descendants(world, parent, old);
        }
        tree3d.emplace(world);
        const navigation::NavigationMesh settings;
        for (navigation::EntityGeometry& each :
             navigation::world_geometry(world, session.assets(), settings, &warnings)) {
            if (std::ranges::find(old, each.entity) == old.end()) {
                geometry.push_back(std::move(each));
            }
        }
    }
    for (std::size_t i = 0; i < rules.layers.size(); ++i) {
        const ScatterLayer& layer = rules.layers[i];
        const std::string layer_path = layers_path + std::to_string(i);
        std::vector<entt::entity> on;
        for (std::size_t k = 0; k < layer.on.size(); ++k) {
            const entt::entity entity = world.find(layer.on[k]);
            if (entity == entt::null) {
                return unknown_entity(world, layer.on[k], layer_path + "/on/" + std::to_string(k));
            }
            on.push_back(entity);
        }
        std::vector<ScatterTriangle> ground;
        if (on.empty()) {
            add_area(layer.area.value_or(std::array<float, 4>{}), flat, ground);
        } else if (flat) {
            auto cells = ground_2d(session, layer, on, layer_path);
            if (!cells) {
                return param_error(code::kInvalidArgument, "", cells.error().message);
            }
            ground = std::move(cells).value();
        } else {
            ground = ground_3d(geometry, *tree3d, {on.begin(), on.end()});
        }
        std::vector<Vec3> avoid;
        for (const std::string& name : layer.avoid) {
            for (std::size_t k = 0; k < i; ++k) {
                if (rules.layers[k].name == name) {
                    for (const ScatterPlace& place : done[k].result.places) {
                        avoid.push_back(place.position);
                    }
                }
            }
        }
        done.push_back({procedural::scatter(layer, i, rules.seed, flat, ground, avoid)});
    }

    // Names checked before anything is made, so a failure changes nothing.
    for (std::size_t i = 0; i < rules.layers.size(); ++i) {
        const std::string longest =
            into + "_" + rules.layers[i].name +
            (rules.layers[i].as == "multimesh"
                 ? std::string()
                 : "_" + std::to_string(std::max<std::size_t>(done[i].result.places.size(), 1)));
        if (!world::World::is_valid_id(longest)) {
            return param_error(code::kInvalidParams, "/into",
                               quoted(longest) + " would be too long an id: give into or the " +
                                   "layer a shorter name");
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

    // In with the new.
    std::vector<entt::entity> children;
    json::Array report;
    for (std::size_t i = 0; i < rules.layers.size(); ++i) {
        const ScatterLayer& layer = rules.layers[i];
        const procedural::ScatterResult& result = done[i].result;
        const std::string layer_path = layers_path + std::to_string(i);
        const std::string prefix = into + "_" + layer.name;
        if (layer.as == "multimesh") {
            const entt::entity entity = world.create(prefix).value();
            ecs.emplace<world::Transform3D>(entity);
            world::MultiMeshInstance3D multimesh;
            multimesh.mesh = layer.mesh;
            for (const ScatterPlace& place : result.places) {
                const world::Transform3D local = local_3d(to_local_3d, place);
                multimesh.instances.push_back(
                    {.position = local.position, .rotation = local.rotation, .scale = local.scale});
            }
            ecs.emplace<world::MultiMeshInstance3D>(entity, std::move(multimesh));
            children.push_back(entity);
        } else {
            for (std::size_t k = 0; k < result.places.size(); ++k) {
                const entt::entity entity =
                    world.create(prefix + "_" + std::to_string(k + 1)).value();
                if (!layer.prefab.empty()) {
                    if (auto made = world::instantiate_prefab(world, entity, layer.prefab); !made) {
                        return param_error(code::kInvalidArgument, layer_path + "/prefab",
                                           made.error().message);
                    }
                }
                if (flat) {
                    const world::Transform2D local = local_2d(to_local_2d, result.places[k]);
                    auto& transform = ecs.get_or_emplace<world::Transform2D>(entity);
                    transform.position = local.position;
                    transform.rotation = local.rotation;
                    transform.scale = local.scale;
                } else {
                    const world::Transform3D local = local_3d(to_local_3d, result.places[k]);
                    auto& transform = ecs.get_or_emplace<world::Transform3D>(entity);
                    transform.position = local.position;
                    transform.rotation = local.rotation;
                    transform.scale = local.scale;
                    if (!layer.mesh.empty()) {
                        ecs.emplace_or_replace<world::MeshInstance3D>(entity).mesh = layer.mesh;
                    }
                }
                children.push_back(entity);
            }
        }
        json::Value entry;
        entry["name"] = layer.name;
        entry["placed"] = static_cast<std::int64_t>(result.places.size());
        if (std::cmp_less(result.places.size(), result.wanted)) {
            entry["wanted"] = result.wanted;
            json::Value rejected;
            const auto add = [&rejected](std::string_view key, int count) {
                if (count > 0) {
                    rejected[key] = count;
                }
            };
            add("outside", result.outside);
            add("slope", result.slope);
            add("height", result.height);
            add("noise", result.noise);
            add("spacing", result.spacing);
            if (result.ground_area <= 0.0F) {
                entry["problem"] = "no ground to scatter over: on gave no geometry there";
            } else {
                entry["rejected"] = std::move(rejected);
            }
        }
        report.push_back(std::move(entry));
    }
    if (flat) {
        ecs.get<world::Transform2D>(parent).children = std::move(children);
    } else {
        ecs.get<world::Transform3D>(parent).children = std::move(children);
    }

    json::Value out;
    out["into"] = into;
    out["layers"] = std::move(report);
    if (!warnings.empty()) {
        json::Array list;
        for (std::string& warning : warnings) {
            list.emplace_back(std::move(warning));
        }
        out["warnings"] = std::move(list);
    }
    return Reply::success(std::move(out));
}

std::string short_number(float value) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%g", static_cast<double>(value));
    return text.data();
}

// The foliage commands' common params: the entity, its type and the brush.
struct Brush {
    entt::entity entity = entt::null;
    bool flat = false;
    Vec3 center;
    float radius = 1.0F;
};

Vec3 point_of(const json::Value& value) {
    const json::Array& at = value.as_array();
    return {static_cast<float>(at[0].as_double()), static_cast<float>(at[1].as_double()),
            at.size() > 2 ? static_cast<float>(at[2].as_double()) : 0.0F};
}

float number_or(const json::Value& params, std::string_view key, float fallback) {
    const json::Value* value = params.find(key);
    return value != nullptr ? static_cast<float>(value->as_double()) : fallback;
}

// A Foliage3D's or Foliage2D's instance in world space, and its index in the group.
struct Found {
    std::size_t group = 0;
    std::size_t index = 0;
    Vec3 position; // world space; 2D in x and y
    float distance = 0.0F;
};

// The instances within the brush, nearest first (ties by group and index), of the type given
// or every type.
std::vector<Found> instances_in(world::World& world, const Brush& brush, const std::string& type) {
    std::vector<Found> out;
    const auto take = [&](std::size_t g, std::size_t i, Vec3 at) {
        const float dx = at.x - brush.center.x;
        const float dy = brush.flat ? at.y - brush.center.y : at.z - brush.center.z;
        const float distance = std::sqrt((dx * dx) + (dy * dy));
        if (distance <= brush.radius) {
            out.push_back({.group = g, .index = i, .position = at, .distance = distance});
        }
    };
    if (brush.flat) {
        const auto* held = world.ecs().try_get<const world::Foliage2D>(brush.entity);
        if (held == nullptr) {
            return out;
        }
        const world::Foliage2D& foliage = *held;
        const world::Affine2D place = world::Tree2D(world).global_affine(brush.entity);
        for (std::size_t g = 0; g < foliage.groups.size(); ++g) {
            if (!type.empty() && foliage.groups[g].type != type) {
                continue;
            }
            for (std::size_t i = 0; i < procedural::foliage_count(foliage.groups[g]); ++i) {
                const Vec2 at =
                    place.apply(procedural::foliage_place(foliage.groups[g], i).position);
                take(g, i, {at.x, at.y, 0.0F});
            }
        }
    } else {
        const auto* held = world.ecs().try_get<const world::Foliage3D>(brush.entity);
        if (held == nullptr) {
            return out;
        }
        const world::Foliage3D& foliage = *held;
        const world::Affine3D place = world::Tree3D(world).global(brush.entity);
        for (std::size_t g = 0; g < foliage.groups.size(); ++g) {
            if (!type.empty() && foliage.groups[g].type != type) {
                continue;
            }
            for (std::size_t i = 0; i < procedural::foliage_count(foliage.groups[g]); ++i) {
                take(g, i, place.apply(procedural::foliage_place(foliage.groups[g], i).position));
            }
        }
    }
    std::ranges::stable_sort(
        out, [](const Found& a, const Found& b) { return a.distance < b.distance; });
    return out;
}

// Reads entity, at and radius; the entity must hold a Foliage3D or Foliage2D unless make names
// the space to give it one in (made with its transform if missing).
Result<Brush> read_brush(Session& session, const json::Value& params,
                         const std::optional<bool>& make_flat) {
    world::World& world = session.world();
    entt::registry& ecs = world.ecs();
    Brush brush;
    const std::string id = params.find("entity")->as_string();
    brush.entity = world.find(id);
    if (brush.entity == entt::null && !make_flat) {
        std::string message = "/entity: no entity has the id " + quoted(id);
        if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
            message += "; did you mean " + quoted(hint) + "?";
        }
        return make_error(ErrorCode::NotFound, message);
    }
    if (brush.entity != entt::null) {
        if (ecs.all_of<world::Foliage2D>(brush.entity)) {
            brush.flat = true;
        } else if (ecs.all_of<world::Foliage3D>(brush.entity)) {
            brush.flat = false;
        } else if (make_flat) {
            brush.flat = *make_flat;
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              "/entity: " + quoted(id) + " has no Foliage3D or Foliage2D");
        }
        if (make_flat && *make_flat != brush.flat) {
            return make_error(ErrorCode::InvalidArgument,
                              "/type: a " + std::string(*make_flat ? "2D" : "3D") +
                                  " type can't be painted into " + quoted(id) + "'s " +
                                  (brush.flat ? "Foliage2D" : "Foliage3D"));
        }
    } else {
        brush.flat = *make_flat;
    }
    const json::Value* at = params.find("at");
    if (at != nullptr) {
        if ((at->as_array().size() == 2) != brush.flat) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string("/at: give ") + (brush.flat ? "[x, y]" : "[x, y, z]") +
                                  " for a " + (brush.flat ? "Foliage2D" : "Foliage3D"));
        }
        brush.center = point_of(*at);
    }
    brush.radius = number_or(params, "radius", brush.flat ? 32.0F : 1.0F);
    return brush;
}

Reply brush_error(const Error& error) {
    const std::string& message = error.message;
    const std::size_t colon = message.find(':');
    const std::string path = colon != std::string::npos ? message.substr(0, colon) : "";
    return param_error(error.code == ErrorCode::NotFound ? code::kNotFound : code::kInvalidParams,
                       path, colon != std::string::npos ? message.substr(colon + 2) : message);
}

Reply foliage_paint(Session& session, const json::Value& params) {
    world::World& world = session.world();
    entt::registry& ecs = world.ecs();
    const std::string type_path = params.find("type")->as_string();
    std::string problem;
    const auto type = procedural::foliage_type(world, type_path, &problem);
    if (type == nullptr) {
        return param_error(code::kInvalidArgument, "/type", problem);
    }
    const bool flat = type->space == "2d";
    auto read = read_brush(session, params, flat);
    if (!read) {
        return brush_error(read.error());
    }
    Brush brush = read.value();
    const float strength = std::clamp(number_or(params, "strength", 1.0F), 0.0F, 1.0F);

    // The ground under the brush.
    std::vector<ScatterTriangle> ground;
    std::vector<std::string> warnings;
    const json::Value* on = params.find("on");
    std::vector<entt::entity> on_entities;
    if (on != nullptr) {
        const json::Array& names = on->as_array();
        for (std::size_t k = 0; k < names.size(); ++k) {
            const entt::entity entity = world.find(names[k].as_string());
            if (entity == entt::null) {
                return unknown_entity(world, names[k].as_string(), "/on/" + std::to_string(k));
            }
            on_entities.push_back(entity);
        }
    }
    const std::array<float, 4> square =
        flat ? std::array<float, 4>{static_cast<float>(brush.center.x - brush.radius),
                                    static_cast<float>(brush.center.y - brush.radius),
                                    static_cast<float>(brush.center.x + brush.radius),
                                    static_cast<float>(brush.center.y + brush.radius)}
             : std::array<float, 4>{static_cast<float>(brush.center.x - brush.radius),
                                    static_cast<float>(brush.center.z - brush.radius),
                                    static_cast<float>(brush.center.x + brush.radius),
                                    static_cast<float>(brush.center.z + brush.radius)};
    if (flat) {
        if (on_entities.empty()) {
            add_area(square, true, ground);
        } else {
            ScatterLayer layer;
            auto cells = ground_2d(session, layer, on_entities, "");
            if (!cells) {
                return param_error(code::kInvalidArgument, "/on", cells.error().message);
            }
            ground = std::move(cells).value();
        }
    } else {
        const world::Tree3D tree(world);
        const navigation::NavigationMesh settings;
        std::vector<navigation::EntityGeometry> geometry =
            navigation::world_geometry(world, session.assets(), settings, &warnings);
        std::set<entt::entity> under_names(on_entities.begin(), on_entities.end());
        if (on_entities.empty()) {
            for (const navigation::EntityGeometry& each : geometry) {
                under_names.insert(each.entity);
            }
        }
        ground = ground_3d(geometry, tree, under_names);
        if (ground.empty() && on_entities.empty()) {
            add_area(square, false, ground); // nothing solid: the ground plane
        }
    }

    // Kept apart from this type's instances, and from those that block, nearby.
    std::vector<Vec3> avoid;
    if (brush.entity != entt::null) {
        Brush near = brush;
        near.radius = brush.radius + std::max(type->spacing, 0.0F) + 1.0F;
        for (const Found& found : instances_in(world, near, "")) {
            const std::string& other =
                flat ? ecs.get<world::Foliage2D>(brush.entity).groups[found.group].type
                     : ecs.get<world::Foliage3D>(brush.entity).groups[found.group].type;
            const auto other_type = procedural::foliage_type(world, other);
            if (other == type_path ||
                (other_type != nullptr && other_type->collision.shape != "none")) {
                avoid.push_back(flat ? Vec3{found.position.x, found.position.y, 0.0F}
                                     : found.position);
            }
        }
    }
    procedural::FoliageStroke stroke{
        .center = brush.center, .radius = brush.radius, .strength = strength, .seed = 0};
    if (const json::Value* seed = params.find("seed"); seed != nullptr) {
        stroke.seed = static_cast<std::uint32_t>(seed->as_int());
    } else {
        // Strokes in other places differ; the same stroke again places the same.
        std::uint32_t hash = 2166136261U;
        for (const float v : {brush.center.x, brush.center.y, brush.center.z}) {
            hash = (hash ^ static_cast<std::uint32_t>(std::lround(v * 1000.0F))) * 16777619U;
        }
        stroke.seed = hash;
    }
    const procedural::FoliagePainted painted =
        procedural::paint_foliage(*type, stroke, ground, avoid);

    // The entity, made if missing, and the group.
    if (brush.entity == entt::null) {
        auto created = world.create(params.find("entity")->as_string());
        if (!created) {
            return param_error(code::kInvalidParams, "/entity", created.error().message);
        }
        brush.entity = created.value();
    }
    std::size_t total = 0;
    if (flat) {
        static_cast<void>(ecs.get_or_emplace<world::Transform2D>(brush.entity));
        auto& foliage = ecs.get_or_emplace<world::Foliage2D>(brush.entity);
        const world::Affine2D to_local = world::Tree2D(world).global_affine(brush.entity).inverse();
        auto group = std::ranges::find(foliage.groups, type_path, &world::FoliageGroup2D::type);
        if (group == foliage.groups.end()) {
            foliage.groups.push_back({.type = type_path, .instances = {}});
            group = foliage.groups.end() - 1;
        }
        for (const procedural::FoliagePlace3D& place : painted.places) {
            procedural::add_foliage(
                *group, {.position = to_local.apply({place.position.x, place.position.y}),
                         .rotation = 0.0F,
                         .scale = place.scale});
        }
        total = procedural::foliage_count(*group);
    } else {
        static_cast<void>(ecs.get_or_emplace<world::Transform3D>(brush.entity));
        auto& foliage = ecs.get_or_emplace<world::Foliage3D>(brush.entity);
        const world::Affine3D to_local = world::Tree3D(world).global(brush.entity).inverse();
        auto group = std::ranges::find(foliage.groups, type_path, &world::FoliageGroup3D::type);
        if (group == foliage.groups.end()) {
            foliage.groups.push_back({.type = type_path, .instances = {}});
            group = foliage.groups.end() - 1;
        }
        for (const procedural::FoliagePlace3D& place : painted.places) {
            const world::Transform3D local = local_3d(
                to_local,
                {.position = place.position, .rotation = place.rotation, .scale = place.scale});
            procedural::add_foliage(*group, {.position = local.position,
                                             .rotation = local.rotation,
                                             .scale = static_cast<float>(local.scale.y)});
        }
        total = procedural::foliage_count(*group);
    }
    json::Value out;
    out["added"] = static_cast<std::int64_t>(painted.places.size());
    out["count"] = static_cast<std::int64_t>(total);
    if (std::cmp_less(painted.places.size(), painted.wanted)) {
        json::Value rejected;
        if (painted.slope > 0) {
            rejected["slope"] = painted.slope;
        }
        if (painted.height > 0) {
            rejected["height"] = painted.height;
        }
        if (painted.spacing > 0) {
            rejected["spacing"] = painted.spacing;
        }
        if (rejected.is_object() && !rejected.as_object().empty()) {
            out["rejected"] = std::move(rejected);
        }
    }
    if (ground.empty()) {
        out["problem"] = "no ground under the brush: on gave no geometry there";
    }
    return Reply::success(std::move(out));
}

// Removes the instances found, last first so indices stay good.
void remove_found(world::World& world, const Brush& brush, std::vector<Found> found) {
    std::ranges::sort(found, [](const Found& a, const Found& b) {
        return std::tie(a.group, a.index) > std::tie(b.group, b.index);
    });
    for (const Found& each : found) {
        if (brush.flat) {
            procedural::remove_foliage(
                world.ecs().get<world::Foliage2D>(brush.entity).groups[each.group], each.index);
        } else {
            procedural::remove_foliage(
                world.ecs().get<world::Foliage3D>(brush.entity).groups[each.group], each.index);
        }
    }
}

std::string type_of(world::World& world, const Brush& brush, std::size_t group) {
    return brush.flat ? world.ecs().get<world::Foliage2D>(brush.entity).groups[group].type
                      : world.ecs().get<world::Foliage3D>(brush.entity).groups[group].type;
}

Reply foliage_erase(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto read = read_brush(session, params, std::nullopt);
    if (!read) {
        return brush_error(read.error());
    }
    const Brush brush = read.value();
    const json::Value* type = params.find("type");
    std::vector<Found> found = instances_in(world, brush, type != nullptr ? type->as_string() : "");
    if (const json::Value* count = params.find("count"); count != nullptr) {
        found.resize(std::min(found.size(), static_cast<std::size_t>(count->as_int())));
    }
    const std::size_t removed = found.size();
    remove_found(world, brush, std::move(found));
    json::Value out;
    out["removed"] = static_cast<std::int64_t>(removed);
    return Reply::success(std::move(out));
}

json::Value point_json(Vec3 at, bool flat) {
    // Rounded in double, so a thousandth prints as one.
    const auto round = [](float v) { return std::round(static_cast<double>(v) * 1000.0) / 1000.0; };
    json::Array point{round(at.x), round(at.y)};
    if (!flat) {
        point.emplace_back(round(at.z));
    }
    return point;
}

Reply foliage_get(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto read = read_brush(session, params, std::nullopt);
    if (!read) {
        return brush_error(read.error());
    }
    const Brush brush = read.value();
    json::Array groups;
    const std::size_t group_count =
        brush.flat ? world.ecs().get<world::Foliage2D>(brush.entity).groups.size()
                   : world.ecs().get<world::Foliage3D>(brush.entity).groups.size();
    for (std::size_t g = 0; g < group_count; ++g) {
        const std::string path = type_of(world, brush, g);
        json::Value entry;
        entry["type"] = path;
        entry["count"] = static_cast<std::int64_t>(
            brush.flat ? procedural::foliage_count(
                             world.ecs().get<world::Foliage2D>(brush.entity).groups[g])
                       : procedural::foliage_count(
                             world.ecs().get<world::Foliage3D>(brush.entity).groups[g]));
        std::string problem;
        if (const auto type = procedural::foliage_type(world, path, &problem); type != nullptr) {
            entry["kind"] = type->kind;
            if (type->collision.shape != "none") {
                entry["blocks"] = true;
            }
            if (type->climbable) {
                entry["climbable"] = true;
            }
            if (!type->harvest_item.empty()) {
                entry["harvest"] = type->harvest_item;
            }
        } else {
            entry["problem"] = problem;
        }
        groups.push_back(std::move(entry));
    }
    json::Value out;
    out["groups"] = std::move(groups);
    // Pushed over by characters now.
    std::int64_t pushed = 0;
    for (const procedural::FoliagePush& push : procedural::foliage_pushes(world)) {
        pushed += push.entity == brush.entity ? 1 : 0;
    }
    if (pushed > 0) {
        out["pushed"] = pushed;
    }
    if (params.find("at") != nullptr) {
        const json::Value* type = params.find("type");
        const std::vector<Found> found =
            instances_in(world, brush, type != nullptr ? type->as_string() : "");
        const auto limit = static_cast<std::size_t>(
            params.find("limit") != nullptr ? params.find("limit")->as_int() : 20);
        json::Array near;
        for (std::size_t k = 0; k < found.size() && k < limit; ++k) {
            json::Value each;
            each["group"] = static_cast<std::int64_t>(found[k].group);
            each["index"] = static_cast<std::int64_t>(found[k].index);
            each["position"] = point_json(found[k].position, brush.flat);
            near.push_back(std::move(each));
        }
        out["near"] = std::move(near);
        out["within"] = static_cast<std::int64_t>(found.size());
    }
    return Reply::success(std::move(out));
}

Reply foliage_harvest(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto read = read_brush(session, params, std::nullopt);
    if (!read) {
        return brush_error(read.error());
    }
    const Brush brush = read.value();
    const json::Value* type_param = params.find("type");
    const std::vector<Found> found =
        instances_in(world, brush, type_param != nullptr ? type_param->as_string() : "");
    for (const Found& each : found) {
        const std::string path = type_of(world, brush, each.group);
        const auto type = procedural::foliage_type(world, path);
        if (type == nullptr || type->harvest_item.empty()) {
            continue;
        }
        json::Value data;
        data["type"] = path;
        data["item"] = type->harvest_item;
        data["amount"] = type->harvest_amount;
        data["position"] = point_json(each.position, brush.flat);
        remove_found(world, brush, {each});
        world.emit("foliage_harvested", world.id_of(brush.entity), data);
        return Reply::success(std::move(data));
    }
    return param_error(code::kNotFound, "/at",
                       "nothing that can be harvested within " + short_number(brush.radius) +
                           " of there");
}

} // namespace

void register_scatter_commands(Registry& registry) {
    using enum Capability;
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"into"};
        json::Value& p = schema["properties"];
        p["path"] = property("string", "A .scatter.json file.");
        p["rules"] = property("object", "A .scatter.json document, to try rules without a file.");
        p["into"] =
            property("string", "ID of the parent the places go under, made if missing; what "
                               "it held is replaced.");
        registry.add({
            .name = "scatter.run",
            .description =
                "Places what scatter rules (a .scatter.json \"path\" or inline \"rules\") ask for "
                "over the ground: entities <into>_<layer>_<n>, or a MultiMeshInstance3D "
                "<into>_<layer>, under \"into\", replacing what it held. Returns each layer's "
                "count placed and, when short, why tries failed. $ slash-cli schema scatter.",
            .capabilities = {Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = scatter_run,
        });
    }

    const auto point = [](std::string_view description) {
        json::Value schema;
        schema["type"] = "array";
        schema["items"]["type"] = "number";
        schema["minItems"] = 2;
        schema["maxItems"] = 3;
        schema["description"] = description;
        return schema;
    };
    const auto brush_schema = [&](bool at_required) {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = at_required ? json::Array{"entity", "at"} : json::Array{"entity"};
        json::Value& p = schema["properties"];
        p["entity"] = property("string", "ID of the entity with the Foliage3D or Foliage2D.");
        p["at"] = point("The brush's middle in world space: [x, y, z], or [x, y] in 2D.");
        p["radius"] = property("number", "The brush's radius: meters, or pixels in 2D "
                                         "(default 1 m, 32 px).");
        p["radius"]["exclusiveMinimum"] = 0;
        return schema;
    };
    {
        json::Value schema = brush_schema(true);
        schema["required"] = json::Array{"entity", "type", "at"};
        json::Value& p = schema["properties"];
        p["entity"]["description"] = "ID of the entity to paint into, made with a Foliage3D or "
                                     "Foliage2D if missing.";
        p["type"] = property("string", "The .foliage.json file to paint.");
        p["strength"] = property("number", "Share of the type's density, 0 to 1 (default 1).");
        p["strength"]["minimum"] = 0;
        p["strength"]["maximum"] = 1;
        p["on"] = property("array", "IDs of what to paint on, with what is under them: 3D "
                                    "geometry, 2D tile maps. Default: everything solid in 3D, "
                                    "the brush's area in 2D.");
        p["on"]["items"]["type"] = "string";
        p["seed"] = property("integer", "Seed for where instances go (default: from at).");
        registry.add({
            .name = "foliage.paint",
            .description = "Paints instances of a foliage type (.foliage.json) within a brush "
                           "onto the ground, by the type's density, spacing, slope and scale. "
                           "Returns added and the group's count. $ slash-cli schema foliage.",
            .capabilities = {Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = foliage_paint,
        });
    }
    {
        json::Value schema = brush_schema(true);
        json::Value& p = schema["properties"];
        p["type"] = property("string", "Only this .foliage.json type's instances.");
        p["count"] = property("integer", "At most this many, nearest first.");
        p["count"]["minimum"] = 0;
        registry.add({
            .name = "foliage.erase",
            .description = "Removes foliage instances within a brush. Returns removed.",
            .capabilities = {Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = foliage_erase,
        });
    }
    {
        json::Value schema = brush_schema(false);
        json::Value& p = schema["properties"];
        p["type"] = property("string", "With at: only this .foliage.json type's instances.");
        p["limit"] = property("integer", "With at: at most this many listed (default 20).");
        p["limit"]["minimum"] = 0;
        registry.add({
            .name = "foliage.get",
            .description = "A Foliage3D's or Foliage2D's groups: type, kind, count and what "
                           "they do, how many characters push over now and, with at, the "
                           "instances within radius, nearest first.",
            .capabilities = {Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = foliage_get,
        });
    }
    {
        json::Value schema = brush_schema(true);
        json::Value& p = schema["properties"];
        p["type"] = property("string", "Only this .foliage.json type's instances.");
        registry.add({
            .name = "foliage.harvest",
            .description = "Harvests the nearest instance within radius whose type gives an "
                           "item: removes it, emits foliage_harvested and returns its type, "
                           "item, amount and position.",
            .capabilities = {Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = foliage_harvest,
        });
    }
}

} // namespace slash::command
