#include "slash/command/creature.hpp"

#include "animation_layers.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/npc/fauna.hpp"
#include "slash/render/image.hpp"
#include "slash/world/components.hpp"
#include "slash/world/creature.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
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
    // Adding zero turns a rounded -0 into 0.
    return (std::round(value * places) / places) + 0.0;
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
    json::Value motion; // a merge patch over the recipe's motion block, for this one
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
        creature.motion = placing.motion;
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
    world::Model3D instance;
    instance.mesh = placing.recipe;
    ecs.emplace<world::Model3D>(entity, instance);
    world::Animator player;
    player.library = placing.recipe;
    player.animation = "idle";
    ecs.emplace<world::Animator>(entity, player);
    world::Creature3D creature;
    creature.recipe = placing.recipe;
    creature.motion = placing.motion;
    ecs.emplace<world::Creature3D>(entity, creature);
    if (placing.parent != entt::null) {
        if (auto* tree = ecs.try_get<world::Transform3D>(placing.parent); tree != nullptr) {
            tree->children.push_back(entity);
        }
    }
    return {"Transform3D", "Model3D", "Animator", "Creature3D"};
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
    if (const json::Value* motion = params.find("motion"); motion != nullptr) {
        if (auto tuned = world::load_creature(world, placing.recipe, *motion); !tuned) {
            return param_error(code::kInvalidParams, "/motion", tuned.error().message);
        }
        placing.motion = *motion;
    }
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

// ---- animation.preview and animation.tune ----

Quat turn_about(Vec3 axis, float angle) {
    const auto [c, s] = cos_sin(angle * 0.5F);
    return {axis.x * s, axis.y * s, axis.z * s, c};
}

Quat compose(Quat a, Quat b) {
    return {(a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
            (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
            (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w),
            (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z)};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0F;
    return v + (t * q.w) + cross(u, t);
}

// Where each leg's foot is in the model's space at a moment, turned as the 3D model's keys turn
// its bones (the leg a straight bone from its head to the ground at rest).
struct Feet {
    std::vector<std::string> legs;
    std::vector<Vec3> at;
};

Feet feet_at(const std::vector<world::CreatureBone>& bones,
             const std::vector<world::CreatureTurn>& turns) {
    std::vector<Quat> local(bones.size());
    for (const world::CreatureTurn& turn : turns) {
        for (std::size_t b = 0; b < bones.size(); ++b) {
            if (bones[b].name == turn.bone) {
                local[b] = compose(local[b], turn_about(turn.axis, turn.angle));
            }
        }
    }
    // Parents come before their children.
    std::vector<Quat> global(bones.size());
    std::vector<Vec3> head(bones.size());
    for (std::size_t b = 0; b < bones.size(); ++b) {
        const int parent = bones[b].parent;
        if (parent < 0) {
            global[b] = local[b];
            head[b] = bones[b].head;
            continue;
        }
        const auto p = static_cast<std::size_t>(parent);
        global[b] = compose(global[p], local[b]);
        head[b] = head[p] + rotate(global[p], bones[b].head - bones[p].head);
    }
    Feet out;
    for (std::size_t b = 0; b < bones.size(); ++b) {
        if (!bones[b].name.starts_with("leg")) {
            continue;
        }
        out.legs.push_back(bones[b].name);
        out.at.push_back(head[b] + rotate(global[b], Vec3{0.0F, -bones[b].head.y, 0.0F}));
    }
    return out;
}

// How an animation moves, sampled through one cycle.
struct Motion {
    double length = 0.0;
    bool loop = true;
    // Each bone's turns about pitch, yaw and roll: least and most, in radians.
    std::map<std::string, std::array<std::array<float, 2>, 3>, std::less<>> ranges;
    // The gait: how far feet sweep fore and aft, how fast they sweep back while planted (the half
    // of the cycle each moves back), and how fast the body moves while it plays.
    std::size_t legs = 0;
    double stride = 0.0;
    double foot_speed = 0.0;
    double move_speed = 0.0;
};

constexpr std::array<std::string_view, 3> kAxisNames{"pitch", "yaw", "roll"};

// The built-in animation a recipe's animation plays as: its own name, or its motion's from.
std::string base_animation(const world::CreatureRecipe& recipe, std::string_view animation) {
    if (const auto found = recipe.motion.find(animation);
        found != recipe.motion.end() && !found->second.from.empty()) {
        return found->second.from;
    }
    return std::string(animation);
}

// How fast the fauna system moves a creature while the animation plays at its own length: walk
// at its walk speed, run at its run speed, the rest standing.
double move_speed_of(const world::CreatureRecipe& recipe, std::string_view animation) {
    const std::string base = base_animation(recipe, animation);
    if (base == "walk") {
        return recipe.behavior.walk_speed;
    }
    if (base == "run") {
        return recipe.behavior.run_speed;
    }
    return 0.0;
}

Motion measure_motion(const world::CreatureRecipe& recipe, std::string_view animation, int samples,
                      std::optional<double> move_speed) {
    Motion out;
    for (const world::CreatureAnimation& each : world::creature_animations(recipe)) {
        if (each.name == animation) {
            out.length = each.length;
            out.loop = each.loop;
        }
    }
    out.move_speed = move_speed.value_or(move_speed_of(recipe, animation));
    const std::vector<world::CreatureBone> bones = world::creature_skeleton(recipe);
    std::vector<Feet> feet;
    for (int i = 0; i < samples; ++i) {
        const double time = out.length * static_cast<double>(i) / static_cast<double>(samples);
        const std::vector<world::CreatureTurn> turns =
            world::creature_pose(recipe, animation, time);
        // Each bone's angle about each axis at this moment.
        std::map<std::string, std::array<float, 3>, std::less<>> angles;
        for (const world::CreatureTurn& turn : turns) {
            std::array<float, 3>& a = angles[turn.bone];
            a[0] += turn.axis.x * turn.angle;
            a[1] += turn.axis.y * turn.angle;
            a[2] += turn.axis.z * turn.angle;
        }
        for (const world::CreatureBone& bone : bones) {
            const auto found = angles.find(bone.name);
            const std::array<float, 3> a =
                found != angles.end() ? found->second : std::array<float, 3>{};
            auto [range, made] = out.ranges.try_emplace(bone.name);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (made || a[axis] < range->second[axis][0]) {
                    range->second[axis][0] = a[axis];
                }
                if (made || a[axis] > range->second[axis][1]) {
                    range->second[axis][1] = a[axis];
                }
            }
        }
        feet.push_back(feet_at(bones, turns));
    }
    if (feet.empty() || feet.front().legs.empty() || out.length <= 0.0) {
        return out;
    }
    out.legs = feet.front().legs.size();
    const double step = out.length / static_cast<double>(samples);
    double stride = 0.0;
    double speed = 0.0;
    for (std::size_t leg = 0; leg < out.legs; ++leg) {
        double low = feet[0].at[leg].z;
        double high = low;
        double back = 0.0;
        int backward = 0;
        for (std::size_t i = 0; i < feet.size(); ++i) {
            low = std::min<double>(low, feet[i].at[leg].z);
            high = std::max<double>(high, feet[i].at[leg].z);
            if (i + 1 == feet.size() && !out.loop) {
                break;
            }
            // Its nose is toward +z, so a planted foot moves toward -z under the body.
            const double dz = feet[(i + 1) % feet.size()].at[leg].z - feet[i].at[leg].z;
            if (dz < 0.0) {
                back -= dz;
                ++backward;
            }
        }
        stride += high - low;
        if (backward > 0) {
            speed += back / (static_cast<double>(backward) * step);
        }
    }
    out.stride = stride / static_cast<double>(out.legs);
    out.foot_speed = speed / static_cast<double>(out.legs);
    return out;
}

json::Value motion_json(const Motion& m, const std::vector<std::string>& only) {
    json::Value out;
    out["length"] = rounded(m.length);
    out["loop"] = m.loop;
    json::Value bones = json::Object{};
    for (const auto& [bone, axes] : m.ranges) {
        if (!only.empty() && std::ranges::find(only, bone) == only.end()) {
            continue;
        }
        json::Value each = json::Object{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (axes[axis][1] - axes[axis][0] > 1e-4F || std::abs(axes[axis][0]) > 1e-4F) {
                each[kAxisNames[axis]] =
                    json::Array{rounded(axes[axis][0]), rounded(axes[axis][1])};
            }
        }
        if (!each.as_object().empty()) {
            bones[bone] = std::move(each);
        }
    }
    out["bones"] = std::move(bones);
    if (m.legs > 0) {
        json::Value gait;
        gait["stride"] = rounded(m.stride);
        gait["foot_speed"] = rounded(m.foot_speed);
        gait["move_speed"] = rounded(m.move_speed);
        if (m.move_speed > 0.0) {
            // Positive: the body outruns its feet and they skate forward; negative: they slip
            // back.
            gait["slide"] = rounded(m.move_speed - m.foot_speed);
        }
        out["gait"] = std::move(gait);
    }
    return out;
}

// The swing of a bone: the most any of its axes turns through, in radians.
float swing_of(const Motion& m, std::string_view bone) {
    float most = 0.0F;
    if (const auto found = m.ranges.find(bone); found != m.ranges.end()) {
        for (const auto& axis : found->second) {
            most = std::max(most, axis[1] - axis[0]);
        }
    }
    return most;
}

// What a preview or tune works on: the recipe file, the creature (if one was named) and the
// motion patched over the recipe's (the creature's own, for an instance).
struct MotionTarget {
    std::string path;
    entt::entity creature{entt::null};
    bool flat = false;
    json::Value prior; // the creature's motion, or null
    std::shared_ptr<const world::CreatureRecipe> file;
};

Result<MotionTarget> motion_target(world::World& world, const json::Value& params,
                                   std::string& error_path) {
    MotionTarget out;
    if (params.find("creature") != nullptr) {
        error_path = "/creature";
        auto creature = creature_param(world, params);
        if (!creature) {
            return creature.error();
        }
        out.creature = creature.value();
        const auto& ecs = world.ecs();
        if (const auto* c3 = ecs.try_get<const world::Creature3D>(out.creature); c3 != nullptr) {
            out.path = c3->recipe;
            out.prior = c3->motion;
        } else {
            const auto& c2 = ecs.get<const world::Creature2D>(out.creature);
            out.path = c2.recipe;
            out.prior = c2.motion;
            out.flat = true;
        }
    } else if (const json::Value* recipe = params.find("recipe"); recipe != nullptr) {
        error_path = "/recipe";
        out.path = recipe->as_string();
    } else {
        error_path = "";
        return make_error(ErrorCode::InvalidArgument, "name a creature or a recipe");
    }
    auto file = recipe_param(world, out.path);
    if (!file) {
        return file.error();
    }
    out.file = file.value();
    return out;
}

Reply target_error(const world::World& world, const Error& error, const json::Value& params,
                   const std::string& path) {
    if (error.code == ErrorCode::NotFound && path == "/creature") {
        return unknown_entity(world, params.find("creature")->as_string(), "/creature");
    }
    if (path.empty()) {
        return Reply::failure(code::kInvalidParams, error.message);
    }
    return param_error(code::kInvalidParams, path, error.message);
}

// The animation a param names, or why it can't be measured.
Result<std::string> animation_param(const world::CreatureRecipe& recipe,
                                    const json::Value& params) {
    std::string name = params.find("animation")->as_string();
    std::vector<std::string> names;
    for (const world::CreatureAnimation& each : world::creature_animations(recipe)) {
        if (each.name == name) {
            return name;
        }
        names.push_back(each.name);
    }
    std::string message = quoted(name) + " is not one of its animations";
    if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return make_error(ErrorCode::InvalidArgument, message);
}

int int_or(const json::Value& params, std::string_view key, int fallback) {
    const json::Value* given = params.find(key);
    return given != nullptr ? static_cast<int>(given->as_int()) : fallback;
}

// Draws frames of the animation through one cycle side by side on a stage of its own, as the
// creature is drawn in a scene: its 3D model from its near side, or its 2D cut-out.
Result<json::Value> filmstrip(Session& session, const MotionTarget& target,
                              const json::Value& motion, std::string_view animation,
                              const world::CreatureRecipe& tuned, const json::Value& options) {
    std::string out = "previews/" + std::string(animation) + ".png";
    if (const json::Value* given = options.find("out"); given != nullptr) {
        out = given->as_string();
    }
    if (!out.ends_with(".png")) {
        return make_error(ErrorCode::InvalidArgument, quoted(out) + " must end in .png");
    }
    auto full = session.resolve_path(out);
    if (!full) {
        return full.error();
    }
    const int frames = int_or(options, "frames", 6);
    std::array<std::uint32_t, 2> size{160, 120};
    if (const json::Value* given = options.find("size"); given != nullptr) {
        size = {static_cast<std::uint32_t>(given->as_array()[0].as_int()),
                static_cast<std::uint32_t>(given->as_array()[1].as_int())};
    }
    bool flat = target.flat;
    if (const json::Value* given = options.find("two_d"); given != nullptr) {
        flat = given->as_bool();
    }
    SessionOptions stage_options = session.options();
    stage_options.capture_logs = false;
    stage_options.write_files = false;
    std::erase(stage_options.capabilities, Capability::Build);
    std::erase(stage_options.capabilities, Capability::Network);
    Session stage(session.components(), session.commands(), stage_options);
    world::World& world = stage.world();
    auto& ecs = world.ecs();
    const world::CreatureMeasures m = world::creature_measures(tuned);
    const real_t extent = std::max(m.length + m.tail, m.height + m.wing);
    Placing placing;
    placing.recipe = target.path;
    placing.id = "preview";
    placing.flat = flat;
    // 2D: as many pixels a meter as fit it in a frame with room around it.
    placing.pixels_per_meter = std::min(static_cast<float>(size[0]), static_cast<float>(size[1])) *
                               0.7F / std::max(static_cast<float>(extent), 0.05F);
    auto made = world.create(placing.id);
    if (!made) {
        return made.error();
    }
    const entt::entity creature = made.value();
    make_creature(world, creature, placing);
    const entt::entity camera = world.create();
    if (flat) {
        auto& c2 = ecs.get<world::Creature2D>(creature);
        c2.motion = motion;
        c2.enabled = false;
        world::Transform2D place;
        place.position = {static_cast<real_t>(m.body_z) * placing.pixels_per_meter,
                          -static_cast<real_t>(m.height) * 0.5F * placing.pixels_per_meter};
        ecs.emplace<world::Transform2D>(camera, place);
        ecs.emplace<world::Camera2D>(camera);
    } else {
        auto& c3 = ecs.get<world::Creature3D>(creature);
        c3.motion = motion;
        c3.enabled = false;
        // From its near side (+x), looking across at its middle.
        world::Transform3D place;
        place.position = {(extent * 1.1F) + 0.3F, m.height * 0.5F, m.body_z};
        place.rotation = {0.0F, std::numbers::pi_v<real_t> * 0.5F, 0.0F};
        ecs.emplace<world::Transform3D>(camera, place);
        world::Camera3D c;
        c.current = true;
        ecs.emplace<world::Camera3D>(camera, c);
        const entt::entity sun = world.create();
        world::Transform3D sun_place;
        sun_place.rotation = {-0.9F, 0.6F, 0.0F};
        ecs.emplace<world::Transform3D>(sun, sun_place);
        ecs.emplace<world::SunLight3D>(sun);
    }
    // One step makes the 2D cut-out's bones.
    world.step(1.0 / 60.0);
    const double length = world::creature_length(tuned, animation);
    double library = length;
    if (!flat) {
        // The model's animations are the recipe's; the creature's motion retimes them.
        library = world::creature_length(*target.file, animation);
        if (library <= 0.0) {
            library = length;
        }
    }
    render::Image strip;
    strip.width = size[0] * static_cast<std::uint32_t>(frames);
    strip.height = size[1];
    strip.pixels.assign(static_cast<std::size_t>(strip.width) * strip.height * 4, 0);
    for (int f = 0; f < frames; ++f) {
        const double share = static_cast<double>(f) / static_cast<double>(frames);
        if (flat) {
            npc::pose_creature_cutout(world, creature, tuned, animation, share * length);
        } else {
            auto& player = ecs.get<world::Animator>(creature);
            player.animation = std::string(animation);
            player.playing = false;
            player.time = share * library;
        }
        canvas::CaptureOptions capture;
        capture.viewport = Vec2{static_cast<real_t>(size[0]), static_cast<real_t>(size[1])};
        auto image = canvas::capture(world, stage.assets(), capture);
        if (!image) {
            return image.error();
        }
        const render::Image& frame = image.value().image;
        const std::uint32_t w = std::min(frame.width, size[0]);
        const std::uint32_t h = std::min(frame.height, size[1]);
        for (std::uint32_t y = 0; y < h; ++y) {
            const auto from = static_cast<std::size_t>(y) * frame.width * 4;
            const auto to = ((static_cast<std::size_t>(y) * strip.width) +
                             (static_cast<std::size_t>(f) * size[0])) *
                            4;
            std::copy_n(frame.pixels.begin() + static_cast<std::ptrdiff_t>(from),
                        static_cast<std::ptrdiff_t>(w) * 4,
                        strip.pixels.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
    auto png = render::encode_png(strip);
    if (!png) {
        return png.error();
    }
    if (auto written = session.write_file(full.value(), png.value()); !written) {
        return written.error();
    }
    json::Value result;
    result["out"] = out;
    result["frames"] = frames;
    result["size"] = json::Array{static_cast<std::int64_t>(strip.width),
                                 static_cast<std::int64_t>(strip.height)};
    return result;
}

std::vector<std::string> names_in(const json::Value* list) {
    std::vector<std::string> out;
    if (list != nullptr) {
        for (const json::Value& each : list->as_array()) {
            out.push_back(each.as_string());
        }
    }
    return out;
}

std::optional<double> number_in(const json::Value& params, std::string_view key) {
    if (const json::Value* given = params.find(key); given != nullptr) {
        return given->as_double();
    }
    return std::nullopt;
}

Reply animation_preview(Session& session, const json::Value& params) {
    if (params.find("library") != nullptr) {
        return detail::library_preview(session, params);
    }
    for (const std::string_view key : {"set", "layer"}) {
        if (params.find(key) != nullptr) {
            return param_error(code::kInvalidParams, "/" + std::string(key),
                               "a library's animation takes " + std::string(key) +
                                   "; a creature's takes motion");
        }
    }
    world::World& world = session.world();
    std::string error_path;
    auto target = motion_target(world, params, error_path);
    if (!target) {
        return target_error(world, target.error(), params, error_path);
    }
    json::Value motion = target.value().prior;
    if (const json::Value* given = params.find("motion"); given != nullptr) {
        motion = motion.is_null() ? *given : world::merge_patch(motion, *given);
    }
    auto tuned = world::load_creature(world, target.value().path, motion);
    if (!tuned) {
        return param_error(code::kInvalidParams,
                           params.find("motion") != nullptr ? "/motion" : "/creature",
                           tuned.error().message);
    }
    auto animation = animation_param(*tuned.value(), params);
    if (!animation) {
        return param_error(code::kInvalidParams, "/animation", animation.error().message);
    }
    const Motion measured =
        measure_motion(*tuned.value(), animation.value(), int_or(params, "samples", 16),
                       number_in(params, "move_speed"));
    json::Value result = motion_json(measured, names_in(params.find("bones")));
    if (const json::Value* options = params.find("filmstrip"); options != nullptr) {
        auto strip =
            filmstrip(session, target.value(), motion, animation.value(), *tuned.value(), *options);
        if (!strip) {
            return param_error(code::kInvalidParams, "/filmstrip", strip.error().message);
        }
        result["filmstrip"] = std::move(strip).value();
    }
    return Reply::success(std::move(result));
}

// Tunes one animation's motion entry toward goals, trying each change on the recipe as it
// would play.
struct Tuner {
    const MotionTarget& target;
    bool into_recipe = false;
    std::string animation;
    int samples = 16;
    std::optional<double> move_speed;
    // An object, not null: a null patch would remove the animation's entry.
    json::Value change = json::Object{}; // NOLINT(readability-redundant-member-init)

    // The motion patch the change makes: over the creature's own, or over the recipe's.
    [[nodiscard]] json::Value patch() const {
        json::Value wrapped = json::Object{};
        wrapped[animation] = change;
        if (into_recipe || target.prior.is_null()) {
            return wrapped;
        }
        return world::merge_patch(target.prior, wrapped);
    }

    [[nodiscard]] Result<world::CreatureRecipe> recipe() const {
        return world::with_motion(*target.file, patch());
    }

    [[nodiscard]] Result<Motion> measure() const {
        auto made = recipe();
        if (!made) {
            return made.error();
        }
        return measure_motion(made.value(), animation, samples, move_speed);
    }

    // The value a motion entry's field has in the recipe as tuned so far, or its default.
    [[nodiscard]] float field(const world::CreatureRecipe& made, std::string_view bone,
                              float fallback) const {
        const auto found = made.motion.find(animation);
        if (found == made.motion.end()) {
            return fallback;
        }
        if (bone.empty()) {
            return static_cast<float>(found->second.speed);
        }
        const auto b = found->second.bones.find(bone);
        return b != found->second.bones.end() ? b->second.scale : fallback;
    }
};

Reply animation_tune(Session& session, const json::Value& params) {
    if (params.find("library") != nullptr) {
        return detail::library_tune(session, params);
    }
    if (params.find("layer") != nullptr) {
        return param_error(code::kInvalidParams, "/layer",
                           "a library's animation takes layer; a creature's takes set");
    }
    world::World& world = session.world();
    std::string error_path;
    auto target = motion_target(world, params, error_path);
    if (!target) {
        return target_error(world, target.error(), params, error_path);
    }
    const MotionTarget& t = target.value();
    Tuner tuner{.target = t,
                .into_recipe = t.creature == entt::null,
                .animation = {},
                .samples = 16,
                .move_speed = std::nullopt,
                .change = json::Object{}};
    if (const json::Value* into = params.find("into"); into != nullptr) {
        tuner.into_recipe = into->as_string() == "recipe";
        if (!tuner.into_recipe && t.creature == entt::null) {
            return param_error(code::kInvalidParams, "/into",
                               "\"instance\" needs a creature; name one, or tune the recipe");
        }
    }
    tuner.samples = int_or(params, "samples", 16);
    tuner.move_speed = number_in(params, "move_speed");
    if (const json::Value* set = params.find("set"); set != nullptr) {
        tuner.change = *set;
    }
    auto start = world::with_motion(
        *t.file, tuner.into_recipe || t.prior.is_null() ? json::Value(json::Object{}) : t.prior);
    if (!start) {
        return param_error(code::kInvalidParams, "/creature", start.error().message);
    }
    auto animation = animation_param(start.value(), params);
    const bool added = !animation;
    if (added) {
        // A new animation the change itself adds, which only the recipe can.
        tuner.animation = params.find("animation")->as_string();
        auto with_set = tuner.recipe();
        if (!with_set || !with_set.value().motion.contains(tuner.animation)) {
            return param_error(code::kInvalidParams, "/animation", animation.error().message);
        }
        if (!tuner.into_recipe) {
            return param_error(code::kInvalidParams, "/set",
                               "an instance's motion can't add an animation; add " +
                                   quoted(tuner.animation) +
                                   R"x( to the recipe ("into": "recipe"))x");
        }
    } else {
        tuner.animation = animation.value();
    }
    const Motion before =
        added ? Motion{}
              : measure_motion(start.value(), tuner.animation, tuner.samples, tuner.move_speed);
    auto measured = tuner.measure();
    if (!measured) {
        return param_error(code::kInvalidParams, "/set", measured.error().message);
    }
    std::vector<std::string> bone_names;
    for (const world::CreatureBone& bone : world::creature_skeleton(*t.file)) {
        bone_names.push_back(bone.name);
    }
    // Bones' swings toward what ranges asks, a few times over, as added waves don't scale.
    if (const json::Value* ranges = params.find("ranges"); ranges != nullptr) {
        for (const auto& [bone, want] : ranges->as_object()) {
            if (std::ranges::find(bone_names, bone) == bone_names.end()) {
                std::string message = quoted(bone) + " is not one of its bones";
                if (const std::string_view hint = closest_match(bone, bone_names); !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                return param_error(code::kInvalidParams, "/ranges/" + json::pointer_escape(bone),
                                   message);
            }
        }
        for (int round = 0; round < 4; ++round) {
            auto made = tuner.recipe();
            if (!made) {
                return param_error(code::kInvalidParams, "/ranges", made.error().message);
            }
            const Motion now =
                measure_motion(made.value(), tuner.animation, tuner.samples, tuner.move_speed);
            for (const auto& [bone, want] : ranges->as_object()) {
                const float swing = swing_of(now, bone);
                if (swing <= 1e-5F) {
                    continue; // nothing to scale; add a wave with set instead
                }
                const float scale = std::clamp(tuner.field(made.value(), bone, 1.0F) *
                                                   static_cast<float>(want.as_double()) / swing,
                                               0.0F, 4.0F);
                tuner.change["bones"][bone]["scale"] = rounded(scale, 10000.0);
            }
        }
    }
    // The feet planted as fast as the body moves: by cadence (speed) or by swing (the legs'
    // scale).
    if (const json::Value* slide = params.find("slide"); slide != nullptr) {
        const bool by_swing = slide->as_string() == "swing";
        for (int round = 0; round < (by_swing ? 8 : 3); ++round) {
            auto made = tuner.recipe();
            if (!made) {
                return param_error(code::kInvalidParams, "/slide", made.error().message);
            }
            const Motion now =
                measure_motion(made.value(), tuner.animation, tuner.samples, tuner.move_speed);
            if (now.legs == 0 || now.move_speed <= 0.0 || now.foot_speed <= 1e-6) {
                return param_error(code::kInvalidParams, "/slide",
                                   now.legs == 0 ? "it has no legs to plant"
                                                 : "its feet don't sweep, or it stands still; "
                                                   "give move_speed");
            }
            const double ratio = now.move_speed / now.foot_speed;
            if (std::abs(ratio - 1.0) < 1e-4) {
                break;
            }
            if (by_swing) {
                for (const std::string& bone : bone_names) {
                    if (!bone.starts_with("leg")) {
                        continue;
                    }
                    const float scale = std::clamp(tuner.field(made.value(), bone, 1.0F) *
                                                       static_cast<float>(ratio),
                                                   0.0F, 4.0F);
                    tuner.change["bones"][bone]["scale"] = rounded(scale, 10000.0);
                }
            } else {
                // The feet sweep back as fast as the animation plays.
                const double speed = std::clamp(
                    static_cast<double>(tuner.field(made.value(), "", 1.0F)) * ratio, 0.01, 10.0);
                tuner.change["speed"] = rounded(speed, 10000.0);
            }
        }
    }
    auto after = tuner.measure();
    if (!after) {
        return param_error(code::kInvalidParams, "/set", after.error().message);
    }
    // Writes the change where it goes.
    const json::Value wrapped = [&] {
        json::Value w = json::Object{};
        w[tuner.animation] = tuner.change;
        return w;
    }();
    json::Value result;
    if (tuner.into_recipe) {
        auto text = session.read_project_file(t.path);
        if (!text) {
            return Reply::failure(text.error());
        }
        const std::optional<std::string>& existing = text.value();
        if (!existing) {
            return param_error(code::kNotFound, "/recipe", quoted(t.path) + " doesn't exist");
        }
        auto doc = json::parse(*existing);
        if (!doc) {
            return param_error(code::kInvalidParams, "/recipe", doc.error().message);
        }
        json::Value file = std::move(doc).value();
        const json::Value* own = file.find("motion");
        file["motion"] =
            world::merge_patch(own != nullptr ? *own : json::Value(json::Object{}), wrapped);
        std::string written = json::write(file);
        if (written != *existing) {
            if (auto staged = session.stage_file(t.path, written); !staged) {
                return Reply::failure(staged.error());
            }
            session.add_undo_file(t.path, existing, std::move(written));
        } else {
            session.mark_unchanged();
        }
        result["into"] = "recipe";
        result["recipe"] = t.path;
    } else {
        auto& ecs = world.ecs();
        json::Value motion = tuner.patch();
        if (auto* c3 = ecs.try_get<world::Creature3D>(t.creature); c3 != nullptr) {
            c3->motion = std::move(motion);
        } else {
            ecs.get<world::Creature2D>(t.creature).motion = std::move(motion);
        }
        result["into"] = "instance";
        result["creature"] = params.find("creature")->as_string();
    }
    result["animation"] = tuner.animation;
    result["change"] = tuner.change;
    const std::vector<std::string> shown = names_in(params.find("bones"));
    result["before"] = added ? json::Value() : motion_json(before, shown);
    result["after"] = motion_json(after.value(), shown);
    return Reply::success(std::move(result));
}

json::Value motion_property(std::string_view description) {
    json::Value out = property("object", description);
    out["maxProperties"] = 32;
    return out;
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
        .description = "Makes an animal of a .creature.json recipe: in 3D a Model3D "
                       "drawing its rigged model, an Animator of its animations and a "
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
                             {"flip_h", property("boolean", "2D: face left.")},
                             {"motion", motion_property("Its own motion: a merge patch over the "
                                                        "recipe's motion block.")}},
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
    json::Value samples = property("integer", "Moments measured through a cycle. Default 16.");
    samples["minimum"] = 4;
    samples["maximum"] = 64;
    json::Value move = property("number", "m/s the body moves while it plays, for the gait's "
                                          "slide. Default walk_speed for walk, run_speed for "
                                          "run, else 0.");
    move["minimum"] = 0;
    json::Value bones = property("array", "Only these bones' ranges. Default every bone.");
    bones["items"] = text_property("A bone.");
    json::Value frames = property("integer", "Frames through a cycle. Default 6.");
    frames["minimum"] = 1;
    frames["maximum"] = 16;
    json::Value size = property("array", "Each frame's [width, height] in pixels. Default "
                                         "[160, 120].");
    size["items"]["type"] = "integer";
    size["items"]["minimum"] = 16;
    size["items"]["maximum"] = 512;
    size["minItems"] = 2;
    size["maxItems"] = 2;
    json::Value strip =
        schema_of({{"out", text_property("A .png file. Default previews/<animation>.png.")},
                   {"frames", std::move(frames)},
                   {"size", std::move(size)},
                   {"two_d", property("boolean", "Draw the 2D cut-out. Default: a Creature2D's is, "
                                                 "a recipe's 3D model.")}},
                  {});
    strip["description"] = "Draws frames side by side on a stage of its own and writes a PNG.";
    const auto who = [](json::Object more) {
        more.emplace("creature", text_property("A creature's ID: its recipe with its own motion."));
        more.emplace("recipe", text_property("A .creature.json file, when no creature is named."));
        more.emplace("library", text_property("A .anim.json file, in place of a creature: its "
                                              "animation from a model's (a character's, any "
                                              "model's), with set and layer as animation.edit's."));
        more.emplace("animation", text_property("idle, walk, run, eat, attack, fly, swim or one "
                                                "its motion adds; a library's by name."));
        return more;
    };
    json::Value library_set = property("object", "A library's animation: a merge patch over its "
                                                 "entry to try or make first.");
    json::Value library_layer = property("object", "A library's animation: a layer to merge in by "
                                                   "name first (tune puts its goals there; "
                                                   "default \"tune\").");
    library_layer["required"] = json::Array{"name"};
    registry.add({
        .name = "animation.preview",
        .description = "Measures a creature's animation through a cycle as numbers: each "
                       "bone's turns (radians about pitch, yaw, roll) and the gait (stride, "
                       "how fast planted feet sweep, slide against how fast it moves), and can "
                       "draw a filmstrip. motion tries a change without making it.",
        .capabilities = {Read, Filesystem},
        .effect = Effect::Read,
        .params = schema_of(
            who({{"motion", motion_property("A merge patch over its motion block to try.")},
                 {"set", library_set},
                 {"layer", library_layer},
                 {"samples", samples},
                 {"move_speed", move},
                 {"bones", bones},
                 {"filmstrip", std::move(strip)}}),
            {"animation"}),
        .handler = animation_preview,
    });
    json::Value slide = property("string", "Plant its feet: length changes its cadence "
                                           "(speed), swing how far its legs swing.");
    slide["enum"] = json::Array{"length", "swing"};
    json::Value ranges = property("object", "Bones' swings to reach: radians from least to "
                                            "most turn of their most-moving axis, by bone.");
    ranges["additionalProperties"]["type"] = "number";
    ranges["additionalProperties"]["minimum"] = 0;
    ranges["additionalProperties"]["maximum"] = 6.4;
    ranges["maxProperties"] = 16;
    json::Value into = property("string", "instance (the creature's own motion; default with "
                                          "a creature) or recipe (its .creature.json file).");
    into["enum"] = json::Array{"instance", "recipe"};
    json::Value set = motion_property("This animation's motion entry to set first (length, "
                                      "speed, loop, amplitude, bones, add, from), as a merge "
                                      "patch; a library's animation entry.");
    registry.add({
        .name = "animation.tune",
        .description = "Changes a creature's animation by goals and writes it to the "
                       "creature's motion or its recipe: set fields, slide to 0 (feet planted) "
                       "and bones' ranges. Returns the change and the animation measured "
                       "before and after.",
        .capabilities = {Read, Edit, Filesystem},
        .effect = Effect::Edit,
        .params = schema_of(who({{"set", std::move(set)},
                                 {"layer", std::move(library_layer)},
                                 {"slide", std::move(slide)},
                                 {"ranges", std::move(ranges)},
                                 {"move_speed", std::move(move)},
                                 {"samples", std::move(samples)},
                                 {"into", std::move(into)},
                                 {"bones", std::move(bones)}}),
                            {"animation"}),
        .handler = animation_tune,
    });
    detail::register_animation_edit(registry);
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
