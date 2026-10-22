#include "slash/command/navigation.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/navigation/navigation.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::int64_t kMaxPolygons = 1'000'000;
constexpr double kMaxSeed = 9'007'199'254'740'991.0; // the largest integer JSON keeps exactly

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value point_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "number";
    schema["minItems"] = 2;
    schema["maxItems"] = 3;
    return schema;
}

json::Value integer_property(std::string_view description, std::int64_t minimum,
                             std::int64_t maximum) {
    json::Value schema = property("integer", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    return schema;
}

json::Value number_property(std::string_view description) {
    json::Value schema = property("number", description);
    schema["minimum"] = 0;
    return schema;
}

json::Value enum_property(json::Array values, std::string_view description) {
    json::Value schema;
    schema["enum"] = std::move(values);
    schema["description"] = description;
    return schema;
}

json::Value ids_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "string";
    schema["items"]["minLength"] = 1;
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
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

// A number rounded to four decimals, with no ".0" on whole ones, so replies stay short.
json::Value short_number(float value) {
    constexpr double kScale = 10000.0;
    const double out = std::round(static_cast<double>(value) * kScale) / kScale;
    if (out == std::floor(out) && std::abs(out) < 1.0e15) {
        return static_cast<std::int64_t>(out);
    }
    return out;
}

json::Value point_value(Vec2 point) {
    return json::Array{short_number(point.x), short_number(point.y)};
}

json::Value point_value(Vec3 point) {
    return json::Array{short_number(point.x), short_number(point.y), short_number(point.z)};
}

Vec3 vec3_of(const json::Value& value) {
    const json::Array& xyz = value.as_array();
    return {static_cast<float>(xyz[0].as_double()), static_cast<float>(xyz[1].as_double()),
            static_cast<float>(xyz[2].as_double())};
}

Vec2 vec2_of(const json::Value& value) {
    const json::Array& xy = value.as_array();
    return {static_cast<float>(xy[0].as_double()), static_cast<float>(xy[1].as_double())};
}

json::Value id_or_null(const world::World& world, entt::entity entity) {
    if (entity == entt::null || !world.valid(entity)) {
        return nullptr;
    }
    const std::string_view id = world.id_of(entity);
    return id.empty() ? json::Value() : json::Value(id);
}

std::optional<Reply> read_regions(const world::World& world, const json::Value& params,
                                  std::string_view key, std::vector<entt::entity>& out) {
    const json::Value* ids = params.find(key);
    if (ids == nullptr) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < ids->size(); ++i) {
        const std::string& id = ids->as_array()[i].as_string();
        const entt::entity entity = world.find(id);
        const std::string path = "/" + std::string(key) + "/" + std::to_string(i);
        if (entity == entt::null) {
            return unknown_entity(world, id, path);
        }
        if (!world.ecs().any_of<world::NavigationRegion2D, world::NavigationRegion3D>(entity)) {
            return param_error(code::kInvalidParams, path,
                               quoted(id) + " has no NavigationRegion2D or NavigationRegion3D");
        }
        out.push_back(entity);
    }
    return std::nullopt;
}

template <typename V>
json::Value path_value(const world::World& world, const navigation::Path<V>& path, bool metadata) {
    json::Value result;
    json::Array points;
    for (const V& point : path.points) {
        points.push_back(point_value(point));
    }
    result["points"] = std::move(points);
    result["length"] = short_number(path.length);
    if (metadata) {
        json::Array owners;
        json::Array types;
        for (std::size_t i = 0; i < path.points.size(); ++i) {
            owners.push_back(id_or_null(world, path.owners[i]));
            types.emplace_back(path.types[i] == navigation::Segment::Link ? "link" : "region");
        }
        result["owners"] = std::move(owners);
        result["types"] = std::move(types);
    }
    return result;
}

Reply navigation_path(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const json::Value& from = *params.find("from");
    const json::Value& to = *params.find("to");
    if (from.size() != to.size()) {
        return param_error(code::kInvalidParams, "/to",
                           "from and to need the same number of coordinates");
    }
    navigation::PathOptions options;
    if (const json::Value* layers = params.find("navigation_layers"); layers != nullptr) {
        options.navigation_layers = static_cast<std::uint32_t>(layers->as_int());
    }
    if (const json::Value* name = params.find("postprocessing"); name != nullptr) {
        (void)navigation::postprocessing_from_name(name->as_string(), options.postprocessing);
    }
    if (const json::Value* simplify = params.find("simplify_epsilon"); simplify != nullptr) {
        options.simplify_path = true;
        options.simplify_epsilon = static_cast<float>(simplify->as_double());
    }
    if (auto failed = read_regions(world, params, "excluded_regions", options.excluded_regions)) {
        return std::move(*failed);
    }
    if (auto failed = read_regions(world, params, "included_regions", options.included_regions)) {
        return std::move(*failed);
    }
    if (const json::Value* most = params.find("max_polygons"); most != nullptr) {
        options.path_search_max_polygons = static_cast<int>(most->as_int());
    }
    if (const json::Value* most = params.find("max_distance"); most != nullptr) {
        options.path_search_max_distance = static_cast<float>(most->as_double());
    }
    if (const json::Value* most = params.find("return_max_length"); most != nullptr) {
        options.path_return_max_length = static_cast<float>(most->as_double());
    }
    if (const json::Value* most = params.find("return_max_radius"); most != nullptr) {
        options.path_return_max_radius = static_cast<float>(most->as_double());
    }
    const json::Value* metadata = params.find("metadata");
    const bool with_metadata = metadata != nullptr && metadata->as_bool();
    if (from.size() == 3) {
        return Reply::success(
            path_value(world, navigation::query_path(world, vec3_of(from), vec3_of(to), options),
                       with_metadata));
    }
    return Reply::success(path_value(
        world, navigation::query_path(world, vec2_of(from), vec2_of(to), options), with_metadata));
}

Reply navigation_closest(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const json::Value& point = *params.find("point");
    json::Value result;
    if (point.size() == 3) {
        const navigation::ClosestPoint3D closest = navigation::closest_point(world, vec3_of(point));
        if (closest.owner == entt::null) {
            result["point"] = nullptr;
            return Reply::success(std::move(result));
        }
        result["point"] = point_value(closest.point);
        result["normal"] = point_value(closest.normal);
        result["owner"] = id_or_null(world, closest.owner);
        return Reply::success(std::move(result));
    }
    const navigation::ClosestPoint2D closest = navigation::closest_point(world, vec2_of(point));
    if (closest.owner == entt::null) {
        result["point"] = nullptr;
        return Reply::success(std::move(result));
    }
    result["point"] = point_value(closest.point);
    result["owner"] = id_or_null(world, closest.owner);
    return Reply::success(std::move(result));
}

// The map a command names, or the 3D one if it has regions and the 2D one otherwise.
navigation::Map map_of(world::World& world, const json::Value& params) {
    if (const json::Value* map = params.find("map"); map != nullptr) {
        return map->as_string() == "2d" ? navigation::Map::TwoD : navigation::Map::ThreeD;
    }
    return navigation::map_info(world, navigation::Map::ThreeD).regions > 0
               ? navigation::Map::ThreeD
               : navigation::Map::TwoD;
}

Reply navigation_random(Session& session, const json::Value& params) {
    world::World& world = session.world();
    std::uint32_t layers = 1;
    if (const json::Value* value = params.find("navigation_layers"); value != nullptr) {
        layers = static_cast<std::uint32_t>(value->as_int());
    }
    const json::Value* uniformly = params.find("uniformly");
    const bool uniform = uniformly != nullptr && uniformly->as_bool();
    std::uint64_t seed = 0;
    if (const json::Value* value = params.find("seed"); value != nullptr) {
        seed = static_cast<std::uint64_t>(value->as_int());
    }
    json::Value result;
    if (map_of(world, params) == navigation::Map::ThreeD) {
        result["point"] = point_value(navigation::random_point_3d(world, layers, uniform, seed));
    } else {
        result["point"] = point_value(navigation::random_point_2d(world, layers, uniform, seed));
    }
    return Reply::success(std::move(result));
}

json::Value map_value(const navigation::MapInfo& info) {
    json::Value out;
    out["iteration_id"] = static_cast<std::int64_t>(info.iteration_id);
    out["regions"] = static_cast<std::int64_t>(info.regions);
    out["links"] = static_cast<std::int64_t>(info.links);
    out["polygons"] = static_cast<std::int64_t>(info.polygons);
    out["edge_connections"] = static_cast<std::int64_t>(info.edge_connections);
    out["free_edges"] = static_cast<std::int64_t>(info.free_edges);
    if (info.merge_errors > 0) {
        out["merge_errors"] = static_cast<std::int64_t>(info.merge_errors);
    }
    if (!info.errors.empty()) {
        json::Array errors;
        for (const navigation::MapInfo::Error& error : info.errors) {
            json::Value item;
            item["id"] = error.id;
            item["message"] = error.message;
            errors.push_back(std::move(item));
        }
        out["errors"] = std::move(errors);
    }
    return out;
}

Reply navigation_map(Session& session, const json::Value& params) {
    world::World& world = session.world();
    json::Value result = json::Object{};
    const json::Value* map = params.find("map");
    for (const auto& [name, kind] :
         {std::pair{"2d", navigation::Map::TwoD}, std::pair{"3d", navigation::Map::ThreeD}}) {
        if (map == nullptr || map->as_string() == name) {
            result[name] = map_value(navigation::map_info(world, kind));
        }
    }
    return Reply::success(std::move(result));
}

Reply navigation_agent(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string& id = params.find("id")->as_string();
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    const bool three_d = world.ecs().all_of<world::NavigationAgent3D>(entity);
    if (!three_d && !world.ecs().all_of<world::NavigationAgent2D>(entity)) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(id) + " has no NavigationAgent2D or NavigationAgent3D");
    }
    json::Value result;
    if (three_d) {
        result["next_position"] = point_value(navigation::next_path_position_3d(world, entity));
    } else {
        result["next_position"] = point_value(navigation::next_path_position_2d(world, entity));
    }
    const navigation::AgentInfo info = navigation::agent_info(world, entity);
    result["navigation_finished"] = info.navigation_finished;
    result["target_reached"] = info.target_reached;
    result["target_reachable"] = navigation::is_target_reachable(world, entity);
    result["distance_to_target"] = short_number(navigation::distance_to_target(world, entity));
    json::Array points;
    for (const Vec3 point : info.path.points) {
        points.push_back(three_d ? point_value(point) : point_value(Vec2{point.x, point.y}));
    }
    result["path"] = std::move(points);
    result["path_index"] = static_cast<std::int64_t>(info.path_index);
    return Reply::success(std::move(result));
}

} // namespace

void register_navigation_commands(Registry& registry) {
    using enum Capability;
    json::Value postprocessing = enum_property(
        {"corridorfunnel", "edgecentered", "none"},
        "corridorfunnel pulls the path tight, edgecentered goes through edges' middles, none "
        "where it first touches each. Default corridorfunnel.");
    registry.add({
        .name = "navigation.path",
        .description = "Returns a path over the navigation map from from to to, as Godot's "
                       "query_path: [x, y] points on the 2D map, [x, y, z] on the 3D one, and "
                       "its length. It ends as close to to as the map reaches.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"from", point_property("Where it starts, [x, y] or [x, y, z].")},
                {"to", point_property("Where it goes.")},
                {"navigation_layers",
                 integer_property("Regions and links it may use, as bits. Default 1.", 0,
                                  0xFFFFFFFF)},
                {"postprocessing", std::move(postprocessing)},
                {"simplify_epsilon",
                 number_property("Drops points closer than this to the line through their "
                                 "neighbors, as Godot's simplify_path.")},
                {"excluded_regions", ids_property("Region entities it may not use.")},
                {"included_regions", ids_property("The only region entities it may use.")},
                {"max_polygons",
                 integer_property("Stop the search after this many polygons; 0 for no limit. "
                                  "Default 4096.",
                                  0, kMaxPolygons)},
                {"max_distance", number_property("Stop the search this far from from.")},
                {"return_max_length", number_property("Cut the path to this length.")},
                {"return_max_radius", number_property("Cut the path this far from from.")},
                {"metadata", property("boolean", "Also return each point's owner (a region or "
                                                 "link ID) and type. Default false.")},
            },
            {"from", "to"}),
        .handler = navigation_path,
    });
    registry.add({
        .name = "navigation.closest",
        .description = "Returns the point of the navigation map closest to a point, the region "
                       "it is on and, in 3D, its polygon's normal (not normalized, as Godot's). "
                       "point is null if the map is empty.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"point", point_property("[x, y] or [x, y, z].")}}, {"point"}),
        .handler = navigation_closest,
    });
    json::Value seed = property("integer", "The same seed picks the same point. Default 0.");
    seed["minimum"] = 0;
    seed["maximum"] = kMaxSeed;
    registry.add({
        .name = "navigation.random",
        .description = "Returns a random point of the navigation map's regions on "
                       "navigation_layers, as Godot's map_get_random_point.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"map", enum_property({"2d", "3d"}, "Default: 3d if it has regions, else 2d.")},
            {"navigation_layers", integer_property("Default 1.", 0, 0xFFFFFFFF)},
            {"uniformly", property("boolean", "Any point with the same chance, rather than a "
                                              "random polygon of a random region. Default "
                                              "false.")},
            {"seed", std::move(seed)},
        }),
        .handler = navigation_random,
    });
    registry.add({
        .name = "navigation.map",
        .description = "Returns what each navigation map is made of: its iteration, regions, "
                       "links, polygons, edges joined and free, and regions left out with why.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"map", enum_property({"2d", "3d"}, "Default: both.")}}),
        .handler = navigation_map,
    });
    json::Value agent_id = property("string", "An entity with a NavigationAgent2D or 3D.");
    agent_id["minLength"] = 1;
    registry.add({
        .name = "navigation.agent",
        .description = "Returns an agent's next position, whether it finished, reached or can "
                       "reach its target, the distance to it and its path, as Godot's "
                       "NavigationAgent getters.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"id", std::move(agent_id)}}, {"id"}),
        .handler = navigation_agent,
    });
}

} // namespace slash::command
