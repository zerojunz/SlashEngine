#include "slash/command/npc.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/npc/npc.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
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

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

template <typename Range>
std::string did_you_mean(std::string_view name, const Range& names) {
    if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
        return "; did you mean " + quoted(hint) + "?";
    }
    return {};
}

// A cost with no ".0" on whole ones, so replies stay short.
json::Value short_number(float value) {
    constexpr double kScale = 10000.0;
    const double out = std::round(static_cast<double>(value) * kScale) / kScale;
    if (out == std::floor(out) && std::abs(out) < 1.0e15) {
        return static_cast<std::int64_t>(out);
    }
    return out;
}

std::vector<std::string> goal_names(const npc::Brain& brain) {
    std::vector<std::string> names;
    names.reserve(brain.goals.size());
    for (const npc::Goal& goal : brain.goals) {
        names.push_back(goal.name);
    }
    return names;
}

json::Value plan_value(const npc::Brain& brain, const npc::Plan& plan) {
    json::Array steps;
    for (const std::size_t action : plan.actions) {
        steps.emplace_back(brain.actions[action].name);
    }
    return steps;
}

Reply npc_plan(Session& session, const json::Value& params) {
    world::World& world = session.world();
    std::shared_ptr<const npc::Brain> brain;
    std::vector<int> facts;
    if (const json::Value* id = params.find("id"); id != nullptr) {
        const entt::entity entity = world.find(id->as_string());
        if (entity == entt::null) {
            return param_error(code::kNotFound, "/id",
                               "no entity has the id " + quoted(id->as_string()) +
                                   did_you_mean(id->as_string(), world.ids()));
        }
        if (!world.ecs().all_of<world::NpcAgent>(entity)) {
            return param_error(code::kInvalidParams, "/id",
                               quoted(id->as_string()) + " has no NpcAgent");
        }
        std::string error;
        brain = npc::brain_of(world, entity, &error);
        if (brain == nullptr) {
            return param_error(code::kInvalidParams, "/id", error);
        }
        facts = npc::facts_of(world, entity);
    } else if (const json::Value* path = params.find("brain"); path != nullptr) {
        auto loaded = session.assets().load<npc::Brain>(path->as_string());
        if (!loaded) {
            return param_error(code::kInvalidParams, "/brain", loaded.error().message);
        }
        brain = loaded.value().share();
        facts = brain->defaults;
    } else {
        return Reply::failure(code::kInvalidParams, "give id (an NpcAgent) or brain (a file)");
    }
    if (const json::Value* overrides = params.find("facts"); overrides != nullptr) {
        for (const auto& [name, value] : overrides->as_object()) {
            const std::size_t index = brain->fact(name);
            if (index >= brain->facts.size()) {
                return param_error(code::kInvalidParams, "/facts/" + json::pointer_escape(name),
                                   "the brain has no fact named " + quoted(name) +
                                       did_you_mean(name, brain->facts));
            }
            facts[index] = value.is_bool() ? static_cast<int>(value.as_bool())
                                           : static_cast<int>(value.as_int());
        }
    }
    json::Value result;
    if (const json::Value* name = params.find("goal"); name != nullptr) {
        const npc::Goal* goal = brain->goal(name->as_string());
        if (goal == nullptr) {
            return param_error(code::kInvalidParams, "/goal",
                               "the brain has no goal named " + quoted(name->as_string()) +
                                   did_you_mean(name->as_string(), goal_names(*brain)));
        }
        const npc::Plan plan = npc::plan(*brain, facts, *goal);
        result["goal"] = goal->name;
        result["found"] = plan.found;
        if (plan.found) {
            result["plan"] = plan_value(*brain, plan);
            result["cost"] = short_number(plan.cost);
        }
        result["searched"] = plan.searched;
    } else {
        const npc::Choice choice = npc::choose(*brain, facts);
        if (choice.goal != nullptr) {
            result["goal"] = choice.goal->name;
            result["plan"] = plan_value(*brain, choice.plan);
            result["cost"] = short_number(choice.plan.cost);
            result["searched"] = choice.plan.searched;
        } else {
            // Why no goal was chosen, each goal in the order they are tried.
            result["goal"] = nullptr;
            json::Object why;
            for (const std::size_t index : brain->priorities) {
                const npc::Goal& goal = brain->goals[index];
                if (!npc::holds(goal.when, facts)) {
                    why[goal.name] = "its when conditions don't hold";
                } else if (npc::holds(goal.want, facts)) {
                    why[goal.name] = "its want conditions already hold";
                } else {
                    why[goal.name] = "no plan within search_limit";
                }
            }
            result["goals"] = std::move(why);
        }
    }
    json::Object fact_values;
    for (std::size_t i = 0; i < brain->facts.size(); ++i) {
        fact_values[brain->facts[i]] = facts[i];
    }
    result["facts"] = std::move(fact_values);
    if (const bool check = params.find("check") != nullptr && params.find("check")->as_bool();
        check) {
        json::Array warnings;
        for (const json::ValidationError& warning : npc::check_brain(*brain)) {
            json::Value entry;
            entry["path"] = warning.path;
            entry["message"] = warning.message;
            warnings.push_back(std::move(entry));
        }
        result["warnings"] = std::move(warnings);
    }
    return Reply::success(std::move(result));
}

} // namespace

void register_npc_commands(Registry& registry) {
    using enum Capability;
    json::Value id = property("string", "An entity with an NpcAgent: plans from its facts.");
    id["minLength"] = 1;
    json::Value brain = property("string", "A .npc.json file: plans from its facts' defaults.");
    brain["minLength"] = 1;
    json::Value facts = property("object", "Facts to change first, by name.");
    facts["additionalProperties"]["anyOf"] =
        json::Array{json::Object{{"type", "integer"}}, json::Object{{"type", "boolean"}}};
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{
        {"id", std::move(id)},
        {"brain", std::move(brain)},
        {"facts", std::move(facts)},
        {"goal", property("string", "Plan for this goal, whatever its when and priority.")},
        {"check", property("boolean", "Also return the brain's warnings: goals no plan reaches "
                                      "and actions no plan uses. Default false.")},
    };
    registry.add({
        .name = "npc.plan",
        .description = "Returns the goal an NPC would choose and the cheapest plan to it, as its "
                       "npc system would, or why no goal is chosen. Takes id or brain.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = std::move(schema),
        .handler = npc_plan,
    });
}

} // namespace slash::command
