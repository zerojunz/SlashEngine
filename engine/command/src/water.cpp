// water.place and water.sample, over .water.json recipes (slash/world/water.hpp): put a lake,
// ocean or river in a scene, and read its surface where things float.
#include "slash/world/water.hpp"

#include "procedural.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/world/components.hpp"
#include "slash/world/path.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {

namespace {

constexpr std::size_t kMaxPoints = 64;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value numbers_property(std::string_view description, int least, int most) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "number";
    schema["minItems"] = least;
    schema["maxItems"] = most;
    return schema;
}

Reply param_error(std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(code::kInvalidParams, std::string(path) + ": " + message,
                          std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

Reply unknown_id(const world::World& world, std::string_view id, const std::string& path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(path, message);
}

double rounded(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

json::Value vector_value(Vec3f v, bool flat) {
    json::Array out{rounded(v.x), rounded(v.y)};
    if (!flat) {
        out.emplace_back(rounded(v.z));
    }
    return out;
}

std::string stem_of(std::string_view path) {
    if (const std::size_t slash = path.rfind('/'); slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    if (path.ends_with(world::kWaterSuffix)) {
        path.remove_suffix(world::kWaterSuffix.size());
    }
    std::string out;
    for (const char c : path) {
        const bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(word ? c : '_');
    }
    return out.empty() ? "water" : out;
}

std::string free_id(const world::World& world, const std::string& name) {
    if (world.find(name) == entt::null) {
        return name;
    }
    for (int n = 2;; ++n) {
        std::string id = name + "_" + std::to_string(n);
        if (world.find(id) == entt::null) {
            return id;
        }
    }
}

std::string_view kind_name(world::WaterRecipe::Kind kind) {
    switch (kind) {
    case world::WaterRecipe::Kind::Ocean:
        return "ocean";
    case world::WaterRecipe::Kind::River:
        return "river";
    case world::WaterRecipe::Kind::Lake:
        break;
    }
    return "lake";
}

// ---- water.place ----

Reply water_place(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto& ecs = world.ecs();
    const std::string& path = params.find("recipe")->as_string();
    json::Value overrides;
    if (const json::Value* given = params.find("overrides"); given != nullptr) {
        overrides = *given;
    }
    if (!world::is_water_path(path)) {
        return param_error("/recipe", quoted(path) + " is not a .water.json file");
    }
    auto text = session.read_project_file(path);
    if (!text) {
        return param_error("/recipe", text.error().message);
    }
    if (!text.value()) {
        return param_error("/recipe", quoted(path) + " doesn't exist");
    }
    auto loaded = world::load_water(world, path, overrides);
    if (!loaded) {
        return param_error("/recipe", loaded.error().message);
    }
    const world::WaterRecipe& recipe = *loaded.value();
    const bool flat = recipe.flat;
    Vec3 at;
    if (const json::Value* given = params.find("at"); given != nullptr) {
        const json::Array& a = given->as_array();
        if (a.size() != (flat ? 2U : 3U)) {
            return param_error("/at", flat ? "2d water takes [x, y] pixels"
                                           : "3d water takes [x, y, z] meters");
        }
        at = {static_cast<real_t>(a[0].as_double()), static_cast<real_t>(a[1].as_double()),
              flat ? 0.0F : static_cast<real_t>(a[2].as_double())};
    }
    std::string river_path;
    if (const json::Value* given = params.find("path"); given != nullptr) {
        if (flat || recipe.kind != world::WaterRecipe::Kind::River) {
            return param_error("/path", "only a 3d river runs along a path");
        }
        river_path = given->as_string();
        const entt::entity found = world.find(river_path);
        if (found == entt::null) {
            return unknown_id(world, river_path, "/path");
        }
        if (!ecs.all_of<world::Spline3D>(found)) {
            return param_error("/path", quoted(river_path) + " has no Spline3D");
        }
    } else if (!flat && recipe.kind == world::WaterRecipe::Kind::River) {
        return param_error("/path", "a river needs the Spline3D it runs along");
    }
    entt::entity parent = entt::null;
    if (const json::Value* given = params.find("parent"); given != nullptr) {
        parent = world.find(given->as_string());
        if (parent == entt::null) {
            return unknown_id(world, given->as_string(), "/parent");
        }
        if (flat ? !ecs.all_of<world::Transform2D>(parent)
                 : !ecs.all_of<world::Transform3D>(parent)) {
            return param_error("/parent", quoted(given->as_string()) + " has no " +
                                              (flat ? "Transform2D" : "Transform3D"));
        }
    }
    std::string id;
    if (const json::Value* given = params.find("id"); given != nullptr) {
        id = given->as_string();
        if (world.find(id) != entt::null) {
            return param_error("/id", quoted(id) + " is taken");
        }
    } else {
        id = free_id(world, stem_of(path));
    }
    auto made = world.create(id);
    if (!made) {
        return param_error("/id", made.error().message);
    }
    const entt::entity entity = made.value();
    if (flat) {
        world::Transform2D place;
        place.position = {at.x, at.y};
        ecs.emplace<world::Transform2D>(entity, place);
        world::Water2D water;
        water.recipe = path;
        water.overrides = overrides;
        if (const json::Value* size = params.find("size"); size != nullptr) {
            const json::Array& a = size->as_array();
            water.width = static_cast<float>(a[0].as_double());
            if (a.size() > 1) {
                water.depth = static_cast<float>(a[1].as_double());
            }
        }
        ecs.emplace<world::Water2D>(entity, std::move(water));
        if (parent != entt::null) {
            ecs.get<world::Transform2D>(parent).children.push_back(entity);
        }
    } else {
        world::Transform3D place;
        place.position = at;
        ecs.emplace<world::Transform3D>(entity, place);
        world::Water3D water;
        water.recipe = path;
        water.overrides = overrides;
        water.path = river_path;
        if (const json::Value* size = params.find("size"); size != nullptr) {
            const json::Array& a = size->as_array();
            water.size = {static_cast<real_t>(a[0].as_double()),
                          static_cast<real_t>(a.size() > 1 ? a[1].as_double() : a[0].as_double())};
        }
        if (const json::Value* width = params.find("width"); width != nullptr) {
            water.width = static_cast<float>(width->as_double());
        }
        ecs.emplace<world::Water3D>(entity, std::move(water));
        if (parent != entt::null) {
            ecs.get<world::Transform3D>(parent).children.push_back(entity);
        }
    }
    // What its waves do: the highest crest they can stack to, and the longest wave's period.
    float crest = 0.0F;
    float period = 0.0F;
    for (const world::WaterWave& wave : recipe.waves) {
        crest += wave.amplitude;
        const float omega = world::water_wave_frequency(recipe, wave);
        if (omega > 0.0F) {
            period = std::max(period, 2.0F * std::numbers::pi_v<float> / omega);
        }
    }
    json::Value result;
    result["id"] = id;
    result["kind"] = kind_name(recipe.kind);
    result["space"] = flat ? "2d" : "3d";
    result["waves"] = recipe.waves.size();
    result["crest"] = rounded(crest);
    result["period"] = rounded(period);
    return Reply::success(std::move(result));
}

// ---- water.sample ----

Reply water_sample(Session& session, const json::Value& params) {
    world::World& world = session.world();
    std::vector<world::WaterBody> bodies;
    std::optional<bool> flat;
    if (const json::Value* given = params.find("water"); given != nullptr) {
        const entt::entity found = world.find(given->as_string());
        if (found == entt::null) {
            return unknown_id(world, given->as_string(), "/water");
        }
        auto body = world::water_body(world, found, true);
        if (!body) {
            if (!world.ecs().any_of<world::Water3D, world::Water2D>(found)) {
                return param_error("/water",
                                   quoted(given->as_string()) + " has no Water3D or Water2D");
            }
            return param_error("/water", quoted(given->as_string()) +
                                             " can't be made; its recipe or path is wrong");
        }
        flat = body->flat;
        bodies.push_back(std::move(*body));
    } else {
        bodies = world::water_bodies(world, false, true);
        std::vector<world::WaterBody> flats = world::water_bodies(world, true, true);
        if (bodies.empty() && flats.empty()) {
            return param_error("", "the scene has no Water3D or Water2D");
        }
        flat = bodies.empty();
        if (*flat) {
            bodies = std::move(flats);
        }
    }
    double time = world::water_time(world);
    if (const json::Value* ahead = params.find("ahead"); ahead != nullptr) {
        time += ahead->as_double();
    }
    const json::Array& points = params.find("points")->as_array();
    if (points.size() > kMaxPoints) {
        return param_error("/points", "at most 64 points a call");
    }
    json::Array out;
    for (const json::Value& point : points) {
        const json::Array& p = point.as_array();
        Vec3f at;
        if (*flat) {
            at = {static_cast<float>(p[0].as_double()),
                  p.size() > 1 ? static_cast<float>(p[1].as_double()) : 0.0F, 0.0F};
        } else if (p.size() == 2) {
            at = {static_cast<float>(p[0].as_double()), 0.0F, static_cast<float>(p[1].as_double())};
        } else {
            at = {static_cast<float>(p[0].as_double()), static_cast<float>(p[1].as_double()),
                  static_cast<float>(p[2].as_double())};
        }
        json::Value item;
        const world::WaterBody* over = nullptr;
        world::WaterSample sample;
        for (const world::WaterBody& body : bodies) {
            sample = world::sample_water(body, at, time);
            if (sample.inside) {
                over = &body;
                break;
            }
        }
        if (over == nullptr) {
            item["water"] = json::Value{};
            out.push_back(std::move(item));
            continue;
        }
        item["water"] = over->id;
        item["height"] = rounded(sample.height);
        item["normal"] = vector_value(sample.normal, *flat);
        item["velocity"] = vector_value(sample.velocity, *flat);
        item["foam"] = rounded(sample.foam);
        if (std::isfinite(sample.depth)) {
            item["depth"] = rounded(sample.depth);
        }
        // How far under the surface the point is: above water is below 0.
        const float under = *flat ? at.y - sample.height : sample.height - at.y;
        item["submerged"] = rounded(under);
        out.push_back(std::move(item));
    }
    json::Value result;
    result["time"] = rounded(time);
    result["points"] = std::move(out);
    return Reply::success(std::move(result));
}

} // namespace

void register_water_commands(Registry& registry) {
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"recipe"};
        json::Value& p = schema["properties"];
        p["recipe"] = property("string", "A .water.json file.");
        p["id"] = property("string", "The new entity's ID; default the file's name.");
        p["at"] = numbers_property(
            "Where its surface rests: [x, y, z] meters, or [x, y] pixels for 2d water.", 2, 3);
        p["size"] = numbers_property(
            "A lake's or ocean's area [x, z] in meters (default 20 by 20), or 2d water's "
            "[width, depth] in pixels.",
            1, 2);
        p["path"] = property("string", "A river's Spline3D entity, which it runs along.");
        p["width"] = property("number", "A river's width in meters. Default 4.");
        p["parent"] = property("string", "An entity to place it under, moving with it.");
        p["overrides"] = property("object", "Fields over the recipe's, as a JSON Merge Patch.");
        registry.add({
            .name = "water.place",
            .description = "Places a .water.json lake, ocean or river as a new Water3D or "
                           "Water2D, whose waves float and drag bodies.",
            .capabilities = {Capability::Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = water_place,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"points"};
        json::Value& p = schema["properties"];
        p["water"] = property("string", "The Water3D or Water2D entity; default the first "
                                        "that holds each point.");
        json::Value points = property("array", "Up to 64 points: [x, y, z] or [x, z] meters, or "
                                               "[x, y] pixels for 2d water.");
        points["items"] = numbers_property("A point.", 1, 3);
        points["maxItems"] = 64;
        p["points"] = std::move(points);
        p["ahead"] = property("number", "Seconds after now to sample at; default 0.");
        registry.add({
            .name = "water.sample",
            .description = "The water at points: surface height, normal, velocity, foam, depth "
                           "to the ground and how far under it each point is.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = water_sample,
        });
    }
}

} // namespace slash::command
