#include "slash/procedural/scatter.hpp"

#include "procedural.hpp"

#include "slash/canvas/tilemap.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

} // namespace

void register_scatter_commands(Registry& registry) {
    using enum Capability;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"into"};
    json::Value& p = schema["properties"];
    p["path"] = property("string", "A .scatter.json file.");
    p["rules"] = property("object", "A .scatter.json document, to try rules without a file.");
    p["into"] = property("string", "ID of the parent the places go under, made if missing; what "
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

} // namespace slash::command
