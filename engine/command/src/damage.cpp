// damage.apply, damage.state and damage.check: hurting and breaking Destructible3Ds and
// Destructible2Ds by their .damage.json profiles (slash/world/damage.hpp), what they have left,
// and what a profile takes to break.
#include "slash/world/damage.hpp"

#include "procedural.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace slash::command {

namespace {

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

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

double rounded(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

// The kind a call names, or the reply that refuses it.
std::optional<Reply> check_kind(std::string_view kind) {
    const auto& kinds = world::damage_kinds();
    if (std::ranges::find(kinds, kind) != kinds.end()) {
        return std::nullopt;
    }
    std::string list;
    for (const std::string& known : kinds) {
        list += (list.empty() ? "" : ", ");
        list += known;
    }
    std::string message = quoted(kind) + " is no kind of damage (" + list + ")";
    if (const std::string_view hint = closest_match(kind, kinds); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kInvalidParams, "/kind", message);
}

// The reply for an ID no entity has, with the closest one.
Reply unknown_id(const world::World& world, std::string_view id, const std::string& path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kInvalidParams, path, message);
}

template <typename Report>
json::Value report_value(const Report& report) {
    json::Value out;
    out["id"] = report.id;
    out["dealt"] = rounded(static_cast<double>(report.dealt));
    out["health"] = rounded(static_cast<double>(report.health));
    if (report.state > 0) {
        out["state"] = report.state;
    }
    if (report.broke) {
        out["broke"] = true;
        json::Array pieces;
        for (const std::string& piece : report.pieces) {
            pieces.emplace_back(piece);
        }
        out["pieces"] = std::move(pieces);
    }
    if (report.fell) {
        out["fell"] = true;
    }
    if (!report.deformed.empty()) {
        out["deformed"] = report.deformed;
    }
    return out;
}

// Whether damage of a kind tears cloth: cuts, shots and blasts do.
bool tears(std::string_view kind) {
    return kind == "cut" || kind == "pierce" || kind == "explosive";
}

json::Value torn_value(std::string_view id, std::size_t count) {
    json::Value out;
    out["id"] = id;
    out["torn"] = static_cast<std::int64_t>(count);
    return out;
}

double number_or(const json::Value& params, std::string_view key, double fallback) {
    const json::Value* found = params.find(key);
    return found != nullptr ? found->as_double() : fallback;
}

Reply damage_apply(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const auto amount = static_cast<float>(params.find("amount")->as_double());
    const std::string kind =
        params.find("kind") != nullptr ? params.find("kind")->as_string() : "blunt";
    if (auto refused = check_kind(kind)) {
        return std::move(*refused);
    }
    const auto push = static_cast<float>(number_or(params, "push", 0.0));
    const auto point_of = [](const json::Value* point, bool& flat) -> std::optional<Vec3> {
        if (point == nullptr) {
            return std::nullopt;
        }
        const json::Array& numbers = point->as_array();
        flat = numbers.size() == 2;
        return Vec3{static_cast<real_t>(numbers[0].as_double()),
                    static_cast<real_t>(numbers[1].as_double()),
                    flat ? real_t{0} : static_cast<real_t>(numbers[2].as_double())};
    };
    bool flat_at = false;
    bool flat_direction = false;
    const std::optional<Vec3> at = point_of(params.find("at"), flat_at);
    const std::optional<Vec3> direction = point_of(params.find("direction"), flat_direction);
    if (at && direction && flat_at != flat_direction) {
        return param_error(code::kInvalidParams, "/direction",
                           "a direction has as many numbers as at");
    }
    const auto flat = [](const std::optional<Vec3>& v) {
        return v ? std::optional<Vec2>(Vec2{v->x, v->y}) : std::nullopt;
    };
    const json::Value* radius_value = params.find("radius");
    json::Array hurt;
    json::Array torn;
    if (const json::Value* id_value = params.find("id"); id_value != nullptr) {
        const std::string& id = id_value->as_string();
        const entt::entity entity = world.find(id);
        if (entity == entt::null) {
            return unknown_id(world, id, "/id");
        }
        const auto& ecs = world.ecs();
        const bool cloth_2d = ecs.all_of<world::Cloth2D>(entity);
        if ((cloth_2d || ecs.all_of<world::SoftBody3D>(entity)) &&
            !ecs.any_of<world::Destructible2D, world::Destructible3D>(entity)) {
            // Cloth tears where a cut, a shot or a blast reaches it.
            if (!at || radius_value == nullptr) {
                return param_error(code::kInvalidParams, at ? "/radius" : "/at",
                                   "tearing cloth takes a point at and a radius");
            }
            if (cloth_2d != flat_at) {
                return param_error(code::kInvalidParams, "/at",
                                   cloth_2d ? "a Cloth2D is torn at [x, y] in pixels"
                                            : "a SoftBody3D is torn at [x, y, z] in meters");
            }
            const auto radius = static_cast<float>(radius_value->as_double());
            std::size_t count = 0;
            if (tears(kind) && cloth_2d) {
                count = physics2d::tear_cloth_2d(world, entity, *flat(at), radius).size();
            } else if (tears(kind)) {
                count = physics3d::tear_soft_body(world, entity, *at, radius).size();
            }
            torn.emplace_back(torn_value(id, count));
        } else if (ecs.all_of<world::Destructible2D>(entity)) {
            if (at && !flat_at) {
                return param_error(code::kInvalidParams, "/at",
                                   "a Destructible2D is hit at [x, y] in pixels");
            }
            auto report = physics2d::damage_body_2d(world, entity, amount, kind, flat(at), push,
                                                    flat(direction));
            if (!report) {
                return param_error(code::kInvalidParams, "/id",
                                   quoted(id) + " " + report.error().message);
            }
            hurt.emplace_back(report_value(report.value()));
        } else {
            if (at && flat_at) {
                return param_error(code::kInvalidParams, "/at",
                                   "a Destructible3D is hit at [x, y, z] in meters");
            }
            auto report = physics3d::damage_body(world, entity, amount, kind, at, push, direction);
            if (!report) {
                return param_error(code::kInvalidParams, "/id",
                                   quoted(id) + " " + report.error().message);
            }
            hurt.emplace_back(report_value(report.value()));
        }
    } else {
        if (!at) {
            return param_error(code::kInvalidParams, "", "name an id, or a point at and a radius");
        }
        if (radius_value == nullptr) {
            return param_error(code::kInvalidParams, "/radius",
                               "damage at a point reaches what is within a radius");
        }
        const auto radius = static_cast<float>(radius_value->as_double());
        if (flat_at) {
            for (const auto& report :
                 physics2d::damage_area_2d(world, Vec2{at->x, at->y}, radius, amount, kind, push)) {
                hurt.emplace_back(report_value(report));
            }
        } else {
            for (const auto& report :
                 physics3d::damage_area(world, *at, radius, amount, kind, push)) {
                hurt.emplace_back(report_value(report));
            }
        }
        if (tears(kind)) {
            std::vector<entt::entity> cloths;
            if (flat_at) {
                for (const entt::entity entity : world.ecs().view<const world::Cloth2D>()) {
                    cloths.push_back(entity);
                }
            } else {
                for (const entt::entity entity : world.ecs().view<const world::SoftBody3D>()) {
                    cloths.push_back(entity);
                }
            }
            std::ranges::sort(cloths, {}, [](entt::entity e) { return entt::to_entity(e); });
            for (const entt::entity entity : cloths) {
                const std::size_t count =
                    flat_at ? physics2d::tear_cloth_2d(world, entity, *flat(at), radius).size()
                            : physics3d::tear_soft_body(world, entity, *at, radius).size();
                if (count > 0) {
                    torn.emplace_back(torn_value(world.id_of(entity), count));
                }
            }
        }
    }
    json::Value result;
    result["hurt"] = std::move(hurt);
    if (!torn.empty()) {
        result["torn"] = std::move(torn);
    }
    return Reply::success(std::move(result));
}

// What damage.state says of one Destructible: its profile's numbers and what it has left.
template <typename Destructible>
json::Value destructible_state(const world::World& world, std::string_view id,
                               const Destructible& destructible, bool flat) {
    json::Value out;
    out["id"] = id;
    out["dimension"] = flat ? "2d" : "3d";
    if (destructible.level > 0) {
        out["level"] = destructible.level;
    }
    if (destructible.profile.empty()) {
        out["threshold"] = rounded(static_cast<double>(destructible.damage_threshold));
        out["pieces"] = destructible.pieces;
        return out;
    }
    out["profile"] = destructible.profile;
    auto profile = world::load_damage(world, destructible.profile, destructible.overrides);
    if (!profile) {
        out["error"] = profile.error().message;
        return out;
    }
    const world::DamageProfile& p = *profile.value();
    const float full = p.health * std::pow(0.5F, static_cast<float>(destructible.level));
    const float health = destructible.health < 0.0F ? full : destructible.health;
    out["material"] = p.material;
    out["health"] = rounded(static_cast<double>(health));
    out["full"] = rounded(static_cast<double>(full));
    const int state = world::damage_state(p, health);
    if (state > 0) {
        out["state"] = state;
    }
    if (p.fracture.pattern == "none") {
        out["unbreakable"] = true;
    }
    return out;
}

// What damage.state says of a group of linked pieces.
struct GroupState {
    int standing = 0;
    int loose = 0;
    int anchored = 0;
    int links = 0;
    bool flat = false;
};

Reply damage_state(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const auto& ecs = world.ecs();
    std::vector<std::string> wanted;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        const json::Array& list = ids->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string& id = list[i].as_string();
            const auto is_group = [&](auto view) {
                auto pieces = view.each();
                return std::any_of(pieces.begin(), pieces.end(),
                                   [&](const auto& each) { return std::get<1>(each).group == id; });
            };
            if (world.find(id) == entt::null &&
                !is_group(ecs.view<const world::FracturePiece3D>()) &&
                !is_group(ecs.view<const world::FracturePiece2D>())) {
                return unknown_id(world, id, "/ids/" + std::to_string(i));
            }
            wanted.push_back(id);
        }
    }
    const auto included = [&](std::string_view id) {
        return wanted.empty() || std::ranges::find(wanted, id) != wanted.end();
    };
    json::Array bodies;
    for (const auto [entity, destructible] : ecs.view<const world::Destructible3D>().each()) {
        const std::string_view id = world.id_of(entity);
        if (!id.empty() && included(id)) {
            json::Value body = destructible_state(world, id, destructible, false);
            if (!destructible.dents.empty()) {
                body["dents"] = static_cast<std::int64_t>(destructible.dents.size());
            }
            if (!destructible.carves.empty()) {
                body["carves"] = static_cast<std::int64_t>(destructible.carves.size());
            }
            if (const auto* terrain = ecs.try_get<const world::HTerrain>(entity);
                terrain != nullptr && !terrain->craters.empty()) {
                body["craters"] = static_cast<std::int64_t>(terrain->craters.size());
            }
            bodies.emplace_back(std::move(body));
        }
    }
    for (const auto [entity, destructible] : ecs.view<const world::Destructible2D>().each()) {
        const std::string_view id = world.id_of(entity);
        if (!id.empty() && included(id)) {
            bodies.emplace_back(destructible_state(world, id, destructible, true));
        }
    }
    // Cloth torn, by how many edges (2D) or triangles (3D).
    json::Array torn;
    for (const auto [entity, cloth] : ecs.view<const world::Cloth2D>().each()) {
        const std::string_view id = world.id_of(entity);
        if (!cloth.torn.empty() && !id.empty() && included(id)) {
            torn.emplace_back(torn_value(id, cloth.torn.size()));
        }
    }
    for (const auto [entity, soft] : ecs.view<const world::SoftBody3D>().each()) {
        const std::string_view id = world.id_of(entity);
        if (!soft.torn.empty() && !id.empty() && included(id)) {
            torn.emplace_back(torn_value(id, soft.torn.size()));
        }
    }
    std::map<std::string, GroupState, std::less<>> groups;
    const auto count = [&](const auto& piece, bool flat) {
        if (piece.group.empty() || !included(piece.group)) {
            return;
        }
        GroupState& group = groups[piece.group];
        group.flat = flat;
        if (piece.anchored || !piece.links.empty()) {
            ++group.standing;
        } else {
            ++group.loose;
        }
        group.anchored += piece.anchored ? 1 : 0;
        group.links += static_cast<int>(piece.links.size());
    };
    for (const auto [entity, piece] : ecs.view<const world::FracturePiece3D>().each()) {
        count(piece, false);
    }
    for (const auto [entity, piece] : ecs.view<const world::FracturePiece2D>().each()) {
        count(piece, true);
    }
    json::Value result;
    result["bodies"] = std::move(bodies);
    if (!groups.empty()) {
        json::Array list;
        for (const auto& [name, group] : groups) {
            json::Value out;
            out["group"] = name;
            out["dimension"] = group.flat ? "2d" : "3d";
            out["standing"] = group.standing;
            out["loose"] = group.loose;
            out["anchored"] = group.anchored;
            out["links"] = group.links / 2; // each link is kept by both its pieces
            list.emplace_back(std::move(out));
        }
        result["groups"] = std::move(list);
    }
    if (!torn.empty()) {
        result["torn"] = std::move(torn);
    }
    return Reply::success(std::move(result));
}

// What damage.check says of one profile: what it is and, for an amount, the hits of each kind it
// takes to break.
json::Value profile_summary(const world::DamageProfile& p, std::optional<float> amount) {
    json::Value out;
    if (!p.name.empty()) {
        out["name"] = p.name;
    }
    out["material"] = p.material;
    out["health"] = rounded(static_cast<double>(p.health));
    json::Value resist;
    for (const std::string& kind : world::damage_kinds()) {
        resist[kind] = rounded(static_cast<double>(world::damage_taken(p, 1.0F, kind)));
    }
    out["resist"] = std::move(resist);
    out["states"] = static_cast<int>(p.states.size());
    json::Value fracture;
    fracture["pattern"] = p.fracture.pattern;
    fracture["pieces"] = p.fracture.pieces;
    fracture["levels"] = p.fracture.levels;
    out["fracture"] = std::move(fracture);
    if (p.connected) {
        out["connected"] = true;
        out["anchors"] = p.anchors;
    }
    if (amount) {
        json::Value hits;
        for (const std::string& kind : world::damage_kinds()) {
            const float each = world::damage_taken(p, *amount, kind);
            if (each <= 0.0F || p.fracture.pattern == "none") {
                hits[kind] = json::Value();
            } else {
                hits[kind] = static_cast<int>(std::ceil(p.health / each));
            }
        }
        out["hits"] = std::move(hits);
    }
    return out;
}

Reply damage_check(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    std::optional<float> amount;
    if (const json::Value* value = params.find("amount"); value != nullptr) {
        amount = static_cast<float>(value->as_double());
    }
    json::Value result;
    if (const json::Value* path = params.find("profile"); path != nullptr) {
        auto profile = world::load_damage(world, path->as_string(), json::Value());
        if (!profile) {
            return param_error(code::kInvalidParams, "/profile",
                               quoted(path->as_string()) + ": " + profile.error().message);
        }
        result["profile"] = profile_summary(*profile.value(), amount);
        return Reply::success(std::move(result));
    }
    // Every Destructible's profile, each once, and the problems of those that can't be used.
    json::Array problems;
    json::Object profiles;
    const auto check = [&](entt::entity entity, const auto& destructible, bool flat) {
        const std::string id(world.id_of(entity));
        if (destructible.profile.empty()) {
            return;
        }
        auto profile = world::load_damage(world, destructible.profile, destructible.overrides);
        if (!profile) {
            json::Value problem;
            problem["id"] = id;
            problem["message"] = profile.error().message;
            problems.emplace_back(std::move(problem));
            return;
        }
        if (profiles.find(destructible.profile) == profiles.end() &&
            !(destructible.overrides.is_object() && !destructible.overrides.as_object().empty())) {
            profiles[destructible.profile] = profile_summary(*profile.value(), amount);
        }
        if (profile.value()->connected && profile.value()->anchors == "none") {
            json::Value problem;
            problem["id"] = id;
            problem["message"] = "its pieces stand linked with no anchors, so all of them fall";
            problems.emplace_back(std::move(problem));
        }
        if (!flat && !world.ecs().all_of<world::MeshInstance3D>(entity)) {
            json::Value problem;
            problem["id"] = id;
            problem["message"] = "a Destructible3D needs a MeshInstance3D to cut";
            problems.emplace_back(std::move(problem));
        }
    };
    for (const auto [entity, destructible] :
         world.ecs().view<const world::Destructible3D>().each()) {
        check(entity, destructible, false);
    }
    for (const auto [entity, destructible] :
         world.ecs().view<const world::Destructible2D>().each()) {
        check(entity, destructible, true);
    }
    result["profiles"] = std::move(profiles);
    result["problems"] = std::move(problems);
    return Reply::success(std::move(result));
}

} // namespace

void register_damage_commands(Registry& registry) {
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"amount"};
        json::Value& p = schema["properties"];
        p["id"] = property("string", "A Destructible3D or Destructible2D to damage, or a "
                                     "Cloth2D or SoftBody3D to tear within radius of at.");
        p["at"] = point_property("Where it is hit: [x, y, z] in meters, or [x, y] in pixels for "
                                 "2D. Without an id, the center of an area.");
        p["radius"] =
            property("number", "Without an id: how far from at the damage reaches, less with "
                               "distance, as an explosion.");
        p["radius"]["exclusiveMinimum"] = 0;
        p["amount"] = property("number", "Damage before the profile's resistance.");
        p["amount"]["minimum"] = 0;
        p["kind"] = property("string", "blunt (default), pierce, cut, explosive, fire or impact.");
        p["direction"] = point_property("Which way the hit goes, as at: dents and holes go in "
                                        "along it. Default: toward the body's middle.");
        p["push"] = property("number", "Speed loose pieces fly from the hit at (m/s, 2D px/s); "
                                       "0 takes the profile's.");
        p["push"]["minimum"] = 0;
        registry.add({
            .name = "damage.apply",
            .description = "Damages a Destructible3D or Destructible2D by its .damage.json "
                           "profile, or all within a radius: damage states, dents, holes and "
                           "craters, breaking where hit, linked pieces collapsing; cuts, shots "
                           "and blasts tear cloth. Returns each one hurt and torn.",
            .capabilities = {Capability::Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = damage_apply,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        json::Value& p = schema["properties"];
        p["ids"] = property("array", "Destructibles and groups (a broken body's ID) to report; "
                                     "default all.");
        p["ids"]["items"]["type"] = "string";
        registry.add({
            .name = "damage.state",
            .description = "Each Destructible3D and Destructible2D's health, full health, "
                           "damage state, dents, carves and craters, each broken body's linked "
                           "pieces standing and loose, and cloth torn.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = damage_state,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        json::Value& p = schema["properties"];
        p["profile"] = property("string", "A .damage.json file; default every profile the "
                                          "world's Destructibles name.");
        p["amount"] = property("number", "Also how many hits of this much of each kind break it.");
        p["amount"]["exclusiveMinimum"] = 0;
        registry.add({
            .name = "damage.check",
            .description = "Reads damage profiles as their material's defaults fill them: health, "
                           "resistances, fracture and hits to break; and the world's "
                           "Destructibles' problems. $ slash-cli schema damage.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = damage_check,
        });
    }
}

} // namespace slash::command
