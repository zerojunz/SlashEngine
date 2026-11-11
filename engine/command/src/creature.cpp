#include "slash/command/creature.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/npc/fauna.hpp"
#include "slash/world/components.hpp"
#include "slash/world/creature.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// creature.place, creature.populate, creature.interact and creature.state, over .creature.json
// recipes (slash/world/creature.hpp) and the fauna system that runs them (slash/npc/fauna.hpp).
// Built with -ffp-contract=off, so a populated herd stands in the same places everywhere.
namespace slash::command {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value text_property(std::string_view description) {
    json::Value out = property("string", description);
    out["minLength"] = 1;
    return out;
}

json::Value schema_of(json::Object properties, json::Array required) {
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

double rounded(double value, double places = 1000.0) {
    return std::round(value * places) / places;
}

Reply unknown_entity(const world::World& world, std::string_view id, std::string_view path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kNotFound, path, message);
}

// The recipe a param names, or the reply that says why it can't be read.
Result<std::shared_ptr<const world::CreatureRecipe>> recipe_param(world::World& world,
                                                                  const std::string& path) {
    if (!world::is_creature_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          quoted(path) + " must end in " + std::string(world::kCreatureSuffix));
    }
    auto recipe = world::load_creature(world, path);
    if (!recipe) {
        std::string message = recipe.error().message;
        if (!message.starts_with(quoted(path))) {
            message = quoted(path) + ": " + message;
        }
        return make_error(ErrorCode::InvalidArgument, message);
    }
    return recipe;
}

// A number from 0 to 1, the same everywhere: a hash of the seed, the try and which number.
float random(std::uint64_t seed, std::uint64_t index, std::uint64_t salt) {
    std::uint64_t h = (seed * 0x9E3779B97F4A7C15ULL) ^ (index * 0xBF58476D1CE4E5B9ULL) ^
                      (salt * 0x94D049BB133111EBULL);
    h = (h ^ (h >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27U)) * 0x94D049BB133111EBULL;
    h ^= h >> 31U;
    return static_cast<float>(h >> 40U) / static_cast<float>(1U << 24U);
}

// The height of the highest triangle of the world's geometry over (x, z), if any is.
std::optional<real_t> ground_at(const std::vector<navigation::EntityGeometry>& geometry, real_t x,
                                real_t z) {
    std::optional<real_t> best;
    for (const navigation::EntityGeometry& each : geometry) {
        const std::vector<float>& v = each.geometry.vertices;
        const std::vector<int>& indices = each.geometry.indices;
        const auto vertex = [&v](int i) {
            const auto at = static_cast<std::size_t>(i) * 3;
            return Vec3{v[at], v[at + 1], v[at + 2]};
        };
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const Vec3 a = vertex(indices[i]);
            const Vec3 b = vertex(indices[i + 1]);
            const Vec3 c = vertex(indices[i + 2]);
            // Where (x, z) is across the triangle seen from above.
            const real_t d = ((b.z - c.z) * (a.x - c.x)) + ((c.x - b.x) * (a.z - c.z));
            if (std::abs(d) < 1e-9F) {
                continue;
            }
            const real_t u = (((b.z - c.z) * (x - c.x)) + ((c.x - b.x) * (z - c.z))) / d;
            const real_t w = (((c.z - a.z) * (x - c.x)) + ((a.x - c.x) * (z - c.z))) / d;
            if (u < 0.0F || w < 0.0F || u + w > 1.0F) {
                continue;
            }
            const real_t y = (u * a.y) + (w * b.y) + ((1.0F - u - w) * c.y);
            if (!best || y > *best) {
                best = y;
            }
        }
    }
    return best;
}

struct Placing {
    std::string recipe;
    std::string id;
    Vec3 at; // 3D meters, or 2D pixels in x and y
    bool flat = false;
    real_t yaw = 0.0F;
    float pixels_per_meter = 100.0F;
    bool flip_h = false;
    entt::entity parent{entt::null};
};

// Makes a creature's entity: in 3D a model, its animations and its Creature3D; in 2D its
// Creature2D. Returns the components it has.
json::Array make_creature(world::World& world, entt::entity entity, const Placing& placing) {
    auto& ecs = world.ecs();
    if (placing.flat) {
        world::Transform2D transform;
        transform.position = {placing.at.x, placing.at.y};
        ecs.emplace<world::Transform2D>(entity, transform);
        world::Creature2D creature;
        creature.recipe = placing.recipe;
        creature.pixels_per_meter = placing.pixels_per_meter;
        creature.flip_h = placing.flip_h;
        ecs.emplace<world::Creature2D>(entity, creature);
        if (placing.parent != entt::null) {
            if (auto* tree = ecs.try_get<world::Transform2D>(placing.parent); tree != nullptr) {
                tree->children.push_back(entity);
            }
        }
        return {"Transform2D", "Creature2D"};
    }
    world::Transform3D transform;
    transform.position = placing.at;
    transform.rotation.y = placing.yaw;
    ecs.emplace<world::Transform3D>(entity, transform);
    world::MeshInstance3D instance;
    instance.mesh = placing.recipe;
    ecs.emplace<world::MeshInstance3D>(entity, instance);
    world::AnimationPlayer player;
    player.library = placing.recipe;
    player.animation = "idle";
    ecs.emplace<world::AnimationPlayer>(entity, player);
    world::Creature3D creature;
    creature.recipe = placing.recipe;
    ecs.emplace<world::Creature3D>(entity, creature);
    if (placing.parent != entt::null) {
        if (auto* tree = ecs.try_get<world::Transform3D>(placing.parent); tree != nullptr) {
            tree->children.push_back(entity);
        }
    }
    return {"Transform3D", "MeshInstance3D", "AnimationPlayer", "Creature3D"};
}

Result<entt::entity> parent_param(world::World& world, const json::Value& params) {
    const json::Value* given = params.find("parent");
    if (given == nullptr) {
        return entt::entity{entt::null};
    }
    const entt::entity parent = world.find(given->as_string());
    if (parent == entt::null) {
        return make_error(ErrorCode::NotFound, given->as_string());
    }
    return parent;
}

Reply creature_place(Session& session, const json::Value& params) {
    world::World& world = session.world();
    Placing placing;
    placing.recipe = params.find("recipe")->as_string();
    auto recipe = recipe_param(world, placing.recipe);
    if (!recipe) {
        return param_error(code::kInvalidParams, "/recipe", recipe.error().message);
    }
    placing.id = params.find("id")->as_string();
    if (world.find(placing.id) != entt::null) {
        return param_error(code::kInvalidParams, "/id", quoted(placing.id) + " already exists");
    }
    const json::Array& at = params.find("at")->as_array();
    placing.flat = at.size() == 2;
    placing.at = {static_cast<real_t>(at[0].as_double()), static_cast<real_t>(at[1].as_double()),
                  placing.flat ? 0.0F : static_cast<real_t>(at[2].as_double())};
    if (const json::Value* yaw = params.find("yaw"); yaw != nullptr) {
        placing.yaw = static_cast<real_t>(yaw->as_double());
    }
    if (const json::Value* ppm = params.find("pixels_per_meter"); ppm != nullptr) {
        placing.pixels_per_meter = static_cast<float>(ppm->as_double());
    }
    if (const json::Value* flip = params.find("flip_h"); flip != nullptr) {
        placing.flip_h = flip->as_bool();
    }
    auto parent = parent_param(world, params);
    if (!parent) {
        return unknown_entity(world, parent.error().message, "/parent");
    }
    placing.parent = parent.value();
    auto created = world.create(placing.id);
    if (!created) {
        return param_error(code::kInvalidParams, "/id", created.error().message);
    }
    json::Value result;
    result["id"] = placing.id;
    result["components"] = make_creature(world, created.value(), placing);
    const world::CreatureRecipe& made = *recipe.value();
    result["flies"] = world::creature_flies(made);
    result["swims"] = world::creature_swims(made);
    return Reply::success(std::move(result));
}

std::string slug(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out.empty() ? std::string("creature") : out;
}

Reply creature_populate(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string path = params.find("recipe")->as_string();
    auto loaded = recipe_param(world, path);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/recipe", loaded.error().message);
    }
    const world::CreatureRecipe& recipe = *loaded.value();
    const json::Array& at = params.find("at")->as_array();
    const bool flat = at.size() == 2;
    const Vec3 center{static_cast<real_t>(at[0].as_double()),
                      static_cast<real_t>(at[1].as_double()),
                      flat ? 0.0F : static_cast<real_t>(at[2].as_double())};
    const json::Value* count_value = params.find("count");
    const int count = count_value != nullptr ? static_cast<int>(count_value->as_int()) : 5;
    const float ppm = params.find("pixels_per_meter") != nullptr
                          ? static_cast<float>(params.find("pixels_per_meter")->as_double())
                          : 100.0F;
    const real_t scale = flat ? ppm : 1.0F;
    const json::Value* radius_value = params.find("radius");
    const real_t radius = radius_value != nullptr ? static_cast<real_t>(radius_value->as_double())
                                                  : recipe.behavior.herd_radius * scale;
    const json::Value* seed_value = params.find("seed");
    const auto seed = seed_value != nullptr ? static_cast<std::uint64_t>(seed_value->as_int()) : 0U;
    std::string prefix = slug(recipe.species.empty() ? recipe.name : recipe.species);
    if (const json::Value* given = params.find("prefix"); given != nullptr) {
        prefix = given->as_string();
    }
    auto parent = parent_param(world, params);
    if (!parent) {
        return unknown_entity(world, parent.error().message, "/parent");
    }
    // The ground, which walkers stand on and fliers fly above.
    std::vector<std::string> warnings;
    std::vector<navigation::EntityGeometry> geometry;
    if (!flat) {
        const navigation::NavigationMesh settings;
        geometry = navigation::world_geometry(world, session.assets(), settings, &warnings);
    }
    const bool flies = world::creature_flies(recipe);
    const bool swims = world::creature_swims(recipe);
    const real_t spacing = std::max<real_t>(recipe.body.length * 1.5F, 0.3F) * scale;
    std::vector<Vec3> placed;
    json::Array ids;
    int index = 1;
    for (std::uint64_t attempt = 0;
         std::cmp_less(placed.size(), count) && attempt < static_cast<std::uint64_t>(count) * 30U;
         ++attempt) {
        const auto [c, s] = cos_sin(random(seed, attempt, 1) * 6.2831853F);
        const real_t r = radius * std::sqrt(random(seed, attempt, 2));
        Vec3 spot;
        if (flat) {
            // Pixels, y down: walkers along the line at y, fliers and swimmers about it.
            spot = {center.x + (r * c), center.y, 0.0F};
            if (flies || swims) {
                spot.y = center.y + (r * s * 0.4F);
            }
        } else {
            spot = {center.x + (r * c), center.y, center.z + (r * s)};
            if (const auto ground = ground_at(geometry, spot.x, spot.z)) {
                spot.y = *ground;
            }
            if (flies) {
                spot.y = spot.y + recipe.behavior.altitude;
            } else if (swims) {
                spot.y = center.y - recipe.behavior.altitude;
            }
        }
        const bool crowded = std::ranges::any_of(placed, [&](Vec3 other) {
            const real_t dx = other.x - spot.x;
            const real_t dy = flat ? other.y - spot.y : other.z - spot.z;
            return std::sqrt((dx * dx) + (dy * dy)) < spacing;
        });
        if (crowded) {
            continue;
        }
        std::string id;
        do {
            id = prefix + "_" + std::to_string(index++);
        } while (world.find(id) != entt::null);
        auto created = world.create(id);
        if (!created) {
            return param_error(code::kInvalidParams, "/prefix", created.error().message);
        }
        Placing placing;
        placing.recipe = path;
        placing.id = id;
        placing.at = spot;
        placing.flat = flat;
        placing.yaw = random(seed, attempt, 3) * 6.2831853F;
        placing.pixels_per_meter = ppm;
        placing.flip_h = random(seed, attempt, 4) < 0.5F;
        placing.parent = parent.value();
        (void)make_creature(world, created.value(), placing);
        placed.push_back(spot);
        ids.emplace_back(id);
    }
    json::Value result;
    result["placed"] = std::move(ids);
    if (std::cmp_less(placed.size(), count)) {
        result["short"] = count - static_cast<int>(placed.size());
    }
    return Reply::success(std::move(result));
}

Result<entt::entity> creature_param(world::World& world, const json::Value& params) {
    const std::string& id = params.find("creature")->as_string();
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return make_error(ErrorCode::NotFound, id);
    }
    if (npc::creature_mind(world, entity) == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          quoted(id) + " has no Creature3D or Creature2D");
    }
    return entity;
}

Reply creature_interact(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto creature = creature_param(world, params);
    if (!creature) {
        if (creature.error().code == ErrorCode::NotFound) {
            return unknown_entity(world, creature.error().message, "/creature");
        }
        return param_error(code::kInvalidParams, "/creature", creature.error().message);
    }
    const std::string& action = params.find("action")->as_string();
    entt::entity by{entt::null};
    if (const json::Value* given = params.find("by"); given != nullptr) {
        by = world.find(given->as_string());
        if (by == entt::null) {
            return unknown_entity(world, given->as_string(), "/by");
        }
    }
    const bool needs_by = action == "feed" || action == "ride" || action == "scare";
    if (needs_by && by == entt::null) {
        return param_error(code::kInvalidParams, "/by", action + " needs who does it");
    }
    json::Value result;
    Result<void> done;
    if (action == "feed") {
        const json::Value* food = params.find("food");
        auto fed = npc::feed(world, creature.value(), by,
                             food != nullptr ? food->as_string() : std::string());
        if (!fed) {
            return param_error(code::kInvalidParams, food != nullptr ? "/food" : "/creature",
                               fed.error().message);
        }
        result["tamed"] = fed.value().tamed;
    } else if (action == "ride") {
        done = npc::ride(world, creature.value(), by);
    } else if (action == "dismount") {
        done = npc::dismount(world, creature.value());
    } else if (action == "steer") {
        const json::Value* direction = params.find("direction");
        if (direction == nullptr) {
            return param_error(code::kInvalidParams, "/direction", "steer needs a direction");
        }
        const json::Array& d = direction->as_array();
        const bool flat = world.ecs().all_of<world::Creature2D>(creature.value());
        if ((d.size() == 2) != flat) {
            return param_error(code::kInvalidParams, "/direction",
                               flat ? "a 2D creature steers by [x, y]"
                                    : "a 3D creature steers by [x, y, z]");
        }
        // 2D directions come as the screen goes, y down.
        const Vec3 way = flat ? Vec3{static_cast<real_t>(d[0].as_double()),
                                     -static_cast<real_t>(d[1].as_double()), 0.0F}
                              : Vec3{static_cast<real_t>(d[0].as_double()),
                                     static_cast<real_t>(d[1].as_double()),
                                     static_cast<real_t>(d[2].as_double())};
        done = npc::steer(world, creature.value(), way);
    } else if (action == "release") {
        done = npc::release(world, creature.value());
    } else {
        done = npc::scare(world, creature.value(), by);
    }
    if (!done) {
        return param_error(code::kInvalidParams, "/action", done.error().message);
    }
    const world::CreatureMind& mind = *npc::creature_mind(world, creature.value());
    result["mode"] = mind.mode;
    result["fed"] = mind.fed;
    return Reply::success(std::move(result));
}

std::string id_or_null(const world::World& world, entt::entity entity) {
    return entity != entt::null && world.valid(entity) ? std::string(world.id_of(entity)) : "";
}

json::Value state_of(world::World& world, entt::entity entity, const world::Tree3D& tree3,
                     const world::Tree2D& tree2) {
    const world::CreatureMind& mind = *npc::creature_mind(world, entity);
    json::Value out;
    out["id"] = std::string(world.id_of(entity));
    const bool flat = world.ecs().all_of<world::Creature2D>(entity);
    out["recipe"] = flat ? world.ecs().get<const world::Creature2D>(entity).recipe
                         : world.ecs().get<const world::Creature3D>(entity).recipe;
    out["mode"] = mind.mode;
    if (const std::string target = id_or_null(world, mind.target); !target.empty()) {
        out["target"] = target;
    }
    if (const std::string owner = id_or_null(world, mind.owner); !owner.empty()) {
        out["owner"] = owner;
    }
    if (const std::string rider = id_or_null(world, mind.rider); !rider.empty()) {
        out["rider"] = rider;
    }
    if (mind.fed > 0) {
        out["fed"] = mind.fed;
    }
    out["hunger"] = rounded(mind.hunger, 100.0);
    if (flat) {
        const Vec2 p = tree2.global(entity).position;
        out["position"] = json::Array{rounded(p.x, 10.0), rounded(p.y, 10.0)};
    } else {
        const Vec3 p = tree3.global(entity).origin;
        out["position"] =
            json::Array{rounded(p.x, 100.0), rounded(p.y, 100.0), rounded(p.z, 100.0)};
    }
    const Vec3 v = mind.velocity;
    out["speed"] = rounded(std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z)), 100.0);
    return out;
}

Reply creature_state(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const world::Tree3D tree3(world);
    const world::Tree2D tree2(world);
    json::Value result;
    if (params.find("creature") != nullptr) {
        auto creature = creature_param(world, params);
        if (!creature) {
            if (creature.error().code == ErrorCode::NotFound) {
                return unknown_entity(world, creature.error().message, "/creature");
            }
            return param_error(code::kInvalidParams, "/creature", creature.error().message);
        }
        result["creatures"] = json::Array{state_of(world, creature.value(), tree3, tree2)};
        return Reply::success(std::move(result));
    }
    // Every creature, in the order they were made.
    std::vector<entt::entity> all;
    for (const entt::entity entity : world.ecs().view<const world::Creature3D>()) {
        all.push_back(entity);
    }
    for (const entt::entity entity : world.ecs().view<const world::Creature2D>()) {
        all.push_back(entity);
    }
    std::ranges::sort(
        all, [&](entt::entity a, entt::entity b) { return world.id_of(a) < world.id_of(b); });
    json::Array out;
    for (const entt::entity entity : all) {
        out.push_back(state_of(world, entity, tree3, tree2));
    }
    result["creatures"] = std::move(out);
    return Reply::success(std::move(result));
}

json::Value point_property(std::string_view description) {
    json::Value at = property("array", description);
    at["items"]["type"] = "number";
    at["minItems"] = 2;
    at["maxItems"] = 3;
    return at;
}

} // namespace

void register_creature_commands(Registry& registry) {
    using enum Capability;
    json::Value ppm = property("number", "2D: pixels a meter. Default 100.");
    ppm["exclusiveMinimum"] = 0;
    registry.add({
        .name = "creature.place",
        .description = "Makes an animal of a .creature.json recipe: in 3D a MeshInstance3D "
                       "drawing its rigged model, an AnimationPlayer of its animations and a "
                       "Creature3D; in 2D a Creature2D, its cut-out kept as <id>_<bone> "
                       "children. The fauna system then runs it.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of({{"recipe", text_property("A .creature.json file.")},
                             {"id", text_property("The new entity's ID.")},
                             {"at", point_property("Where: [x, y] pixels in 2D, [x, y, z] "
                                                   "meters in 3D.")},
                             {"parent", text_property("An entity to place it under.")},
                             {"yaw", property("number", "3D: radians it is turned about y.")},
                             {"pixels_per_meter", ppm},
                             {"flip_h", property("boolean", "2D: face left.")}},
                            {"recipe", "id", "at"}),
        .handler = creature_place,
    });
    json::Value count = property("integer", "How many. Default 5.");
    count["minimum"] = 1;
    count["maximum"] = 200;
    json::Value radius = property("number", "How far from at, in meters (2D: pixels). Default "
                                            "the recipe's herd_radius.");
    radius["exclusiveMinimum"] = 0;
    json::Value seed = property("integer", "Picks other places. Default 0.");
    seed["minimum"] = 0;
    registry.add({
        .name = "creature.populate",
        .description = "Places a herd, flock or school of a .creature.json recipe around a "
                       "point, apart from each other: in 3D walkers on the ground below, fliers "
                       "their altitude above it and fish their depth below at; in 2D walkers "
                       "along at's line. IDs are <prefix>_<n>.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of({{"recipe", text_property("A .creature.json file.")},
                             {"at", point_property("The middle: [x, y] pixels in 2D, [x, y, z] "
                                                   "meters in 3D.")},
                             {"count", std::move(count)},
                             {"radius", std::move(radius)},
                             {"prefix", text_property("IDs start with it. Default the species.")},
                             {"parent", text_property("An entity to place them under.")},
                             {"pixels_per_meter", ppm},
                             {"seed", std::move(seed)}},
                            {"recipe", "at"}),
        .handler = creature_populate,
    });
    json::Value action = property("string", "feed (by, food; tames it once fed enough), ride "
                                            "(by its owner), dismount, steer (direction), "
                                            "release (its owner lets it go) or scare (by).");
    action["enum"] = json::Array{"feed", "ride", "dismount", "steer", "release", "scare"};
    registry.add({
        .name = "creature.interact",
        .description = "Feeds, tames, rides, steers, releases or scares a creature, as "
                       "gameplay code does. Returns its mode and feedings, and whether a "
                       "feeding tamed it.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of(
            {{"creature", text_property("The creature's ID.")},
             {"action", std::move(action)},
             {"by", text_property("Who does it.")},
             {"food", text_property("What it is fed.")},
             {"direction", point_property("steer: [x, y] in 2D (y down), [x, y, z] in 3D, as a "
                                          "share of its run speed.")}},
            {"creature", "action"}),
        .handler = creature_interact,
    });
    registry.add({
        .name = "creature.state",
        .description = "What creatures are doing: mode, target, owner, rider, feedings, "
                       "hunger, position and speed, of one or every creature by ID.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = schema_of({{"creature", text_property("One creature's ID. Default all.")}}, {}),
        .handler = creature_state,
    });
}

} // namespace slash::command
