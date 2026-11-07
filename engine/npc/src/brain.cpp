// .npc.json files and the planner: goal-oriented action planning as Jeff Orkin describes F.E.A.R.'s
// ("Three States and a Plan", GDC 2006): facts as a fixed array of values, actions with
// preconditions, effects and a cost, and A* over the facts' values to the cheapest chain that
// meets a goal.

#include "slash/npc/npc.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::npc {

namespace {

json::Value facts_map_schema(std::string_view description) {
    json::Value out;
    out["type"] = "object";
    out["description"] = description;
    out["additionalProperties"]["type"] = json::Array{"boolean", "integer", "string"};
    return out;
}

json::Value number_property(std::string_view description, double fallback, double minimum,
                            bool exclusive) {
    json::Value out;
    out["type"] = "number";
    out[exclusive ? "exclusiveMinimum" : "minimum"] = minimum;
    out["default"] = fallback;
    out["description"] = description;
    return out;
}

json::Value string_property(std::string_view description) {
    json::Value out;
    out["type"] = "string";
    out["minLength"] = 1;
    out["description"] = description;
    return out;
}

json::Value do_schema() {
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] =
        "What the action does while it runs, at most one of wait, go_to and emit; with none it "
        "ends at once. Its effects apply when it ends.";
    out["maxProperties"] = 2;
    out["properties"]["wait"] = number_property("Seconds it takes.", 0.0, 0.0, false);
    out["properties"]["go_to"] =
        string_property("The ID of the entity to walk to, along the agent's NavigationAgent3D "
                        "or 2D path when it has one.");
    out["properties"]["emit"] = string_property(
        "A world event to emit, about the agent, with the action's name; gameplay code answers "
        "it by setting facts.");
    out["properties"]["speed"] =
        number_property("How fast a go_to walks; 0 is the agent's speed.", 0.0, 0.0, false);
    return out;
}

constexpr std::string_view kConditionHelp =
    "Facts by name, each a value it must equal (true, false or a whole number) or a string "
    "comparing it: \"!=0\", \"<5\", \"<=5\", \">0\" or \">=1\".";

// Reads a whole number, all of text.
bool read_int(std::string_view text, int& out) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc() && end == text.data() + text.size() && !text.empty();
}

// A condition's comparison and value from a file's value, or an error.
std::string read_condition(const json::Value& value, Compare& compare, int& out) {
    compare = Compare::Equal;
    if (value.is_bool()) {
        out = value.as_bool() ? 1 : 0;
        return {};
    }
    if (value.is_int()) {
        const std::int64_t number = value.as_int();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
            return "is out of range";
        }
        out = static_cast<int>(number);
        return {};
    }
    std::string_view text = value.as_string();
    constexpr std::array<std::pair<std::string_view, Compare>, 5> kPrefixes{
        {{"!=", Compare::NotEqual},
         {"<=", Compare::LessEqual},
         {">=", Compare::GreaterEqual},
         {"<", Compare::Less},
         {">", Compare::Greater}}};
    for (const auto& [prefix, kind] : kPrefixes) {
        if (text.starts_with(prefix)) {
            compare = kind;
            text.remove_prefix(prefix.size());
            if (!read_int(text, out)) {
                return "expected a whole number after " + std::string(prefix);
            }
            return {};
        }
    }
    return "expected true, false, a whole number, or a comparison such as \">=1\"";
}

// An effect from a file's value, or an error.
std::string read_effect(const json::Value& value, bool& add, int& out) {
    add = false;
    if (value.is_bool()) {
        out = value.as_bool() ? 1 : 0;
        return {};
    }
    if (value.is_int()) {
        const std::int64_t number = value.as_int();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
            return "is out of range";
        }
        out = static_cast<int>(number);
        return {};
    }
    const std::string_view text = value.as_string();
    if ((text.starts_with('+') || text.starts_with('-')) && text.size() > 1) {
        add = true;
        const bool negative = text.front() == '-';
        if (!read_int(text.substr(1), out)) {
            return "expected a whole number after " + std::string(1, text.front());
        }
        out = negative ? -out : out;
        return {};
    }
    return R"(expected true, false, a whole number, or a change such as "+1" or "-2")";
}

// The facts' names, by index.
std::map<std::string, std::size_t, std::less<>> fact_names(const json::Value& doc) {
    std::map<std::string, std::size_t, std::less<>> out;
    if (const json::Value* facts = doc.find("facts"); facts != nullptr && facts->is_object()) {
        for (const auto& [name, value] : facts->as_object()) {
            out.emplace(name, out.size());
        }
    }
    return out;
}

std::string closest_name(const std::map<std::string, std::size_t, std::less<>>& names,
                         std::string_view name) {
    // Fewest edits; a short list, so a plain table.
    std::string best;
    std::size_t best_cost = std::numeric_limits<std::size_t>::max();
    for (const auto& [candidate, index] : names) {
        std::vector<std::size_t> row(candidate.size() + 1);
        for (std::size_t j = 0; j < row.size(); ++j) {
            row[j] = j;
        }
        for (std::size_t i = 1; i <= name.size(); ++i) {
            std::size_t diagonal = row[0];
            row[0] = i;
            for (std::size_t j = 1; j <= candidate.size(); ++j) {
                const std::size_t above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1,
                                   diagonal + (name[i - 1] == candidate[j - 1] ? 0 : 1)});
                diagonal = above;
            }
        }
        if (row.back() < best_cost) {
            best_cost = row.back();
            best = candidate;
        }
    }
    return best_cost <= std::max<std::size_t>(2, name.size() / 3) ? best : std::string();
}

// The errors of a map of conditions or effects naming facts.
void check_facts_map(const json::Value* map, const std::string& at,
                     const std::map<std::string, std::size_t, std::less<>>& names, bool effects,
                     std::vector<json::ValidationError>& errors) {
    if (map == nullptr || !map->is_object()) {
        return;
    }
    for (const auto& [name, value] : map->as_object()) {
        const std::string where = at + "/" + json::pointer_escape(name);
        if (!names.contains(name)) {
            std::string message = "no fact named \"" + name + "\" in facts";
            if (const std::string close = closest_name(names, name); !close.empty()) {
                message += "; did you mean \"" + close + "\"?";
            }
            errors.push_back({where, std::move(message)});
            continue;
        }
        Compare compare = Compare::Equal;
        bool add = false;
        int number = 0;
        const std::string problem =
            effects ? read_effect(value, add, number) : read_condition(value, compare, number);
        if (!problem.empty()) {
            errors.push_back({where, problem});
        }
    }
}

std::vector<Condition> conditions_of(const json::Value* map, const Brain& brain) {
    std::vector<Condition> out;
    if (map == nullptr) {
        return out;
    }
    for (const auto& [name, value] : map->as_object()) {
        Condition condition{.fact = brain.fact(name), .compare = Compare::Equal, .value = 0};
        (void)read_condition(value, condition.compare, condition.value);
        out.push_back(condition);
    }
    return out;
}

std::vector<Effect> effects_of(const json::Value* map, const Brain& brain) {
    std::vector<Effect> out;
    if (map == nullptr) {
        return out;
    }
    for (const auto& [name, value] : map->as_object()) {
        Effect effect{.fact = brain.fact(name), .add = false, .value = 0};
        (void)read_effect(value, effect.add, effect.value);
        out.push_back(effect);
    }
    return out;
}

bool meets(const Condition& condition, int value) {
    switch (condition.compare) {
    case Compare::Equal:
        return value == condition.value;
    case Compare::NotEqual:
        return value != condition.value;
    case Compare::Less:
        return value < condition.value;
    case Compare::LessEqual:
        return value <= condition.value;
    case Compare::Greater:
        return value > condition.value;
    case Compare::GreaterEqual:
        return value >= condition.value;
    }
    return false;
}

int added(int value, int change) {
    const std::int64_t sum = std::int64_t{value} + change;
    return static_cast<int>(std::clamp<std::int64_t>(sum, std::numeric_limits<int>::min(),
                                                     std::numeric_limits<int>::max()));
}

std::size_t unmet(std::span<const Condition> conditions, std::span<const int> facts) {
    return static_cast<std::size_t>(std::ranges::count_if(
        conditions, [&](const Condition& c) { return !meets(c, facts[c.fact]); }));
}

} // namespace

std::size_t Brain::fact(std::string_view name) const {
    const auto found = std::ranges::find(facts, name);
    return static_cast<std::size_t>(found - facts.begin());
}

const Goal* Brain::goal(std::string_view name) const {
    const auto found = std::ranges::find(goals, name, &Goal::name);
    return found == goals.end() ? nullptr : &*found;
}

json::Value brain_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine NPC brain (.npc.json)";
    schema["description"] =
        "What an NPC knows, wants and can do, for goal-oriented action planning: facts with their "
        "starting values, goals by priority and actions with what they need, what they change "
        "and what they cost. The npc system plans the cheapest chain of actions to the first goal "
        "it can reach.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "facts", "goals", "actions"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "The file format's version.";
    properties["facts"]["type"] = "object";
    properties["facts"]["description"] =
        "Every fact by name, with its starting value: true, false (1 and 0) or a whole number.";
    properties["facts"]["additionalProperties"]["type"] = json::Array{"boolean", "integer"};
    properties["search_limit"]["type"] = "integer";
    properties["search_limit"]["minimum"] = 1;
    properties["search_limit"]["maximum"] = 1000000;
    properties["search_limit"]["default"] = 2048;
    properties["search_limit"]["description"] =
        "The most states a plan may look at before the goal counts as out of reach.";

    json::Value goal;
    goal["type"] = "object";
    goal["additionalProperties"] = false;
    goal["required"] = json::Array{"name", "want"};
    goal["properties"]["name"] = string_property("The goal's name.");
    goal["properties"]["priority"] = number_property(
        "Higher goals are tried first; equal ones in the file's order.", 0.0, -1e9, false);
    goal["properties"]["when"] =
        facts_map_schema(std::string("When the goal applies: ") + std::string(kConditionHelp));
    goal["properties"]["want"] =
        facts_map_schema(std::string("What the goal wants: ") + std::string(kConditionHelp));
    properties["goals"]["type"] = "array";
    properties["goals"]["items"] = std::move(goal);
    properties["goals"]["description"] = "What the NPC wants, tried highest priority first.";

    json::Value action;
    action["type"] = "object";
    action["additionalProperties"] = false;
    action["required"] = json::Array{"name"};
    action["properties"]["name"] = string_property("The action's name.");
    action["properties"]["cost"] =
        number_property("What it costs; plans take the cheapest chain.", 1.0, 0.0, true);
    action["properties"]["needs"] = facts_map_schema(
        std::string("What must hold before it runs: ") + std::string(kConditionHelp));
    action["properties"]["effects"] = facts_map_schema(
        "What it changes when it ends: facts by name, each a value to set (true, false or a "
        "whole number) or a change such as \"+1\" or \"-2\".");
    action["properties"]["do"] = do_schema();
    properties["actions"]["type"] = "array";
    properties["actions"]["items"] = std::move(action);
    properties["actions"]["description"] = "What the NPC can do, in the order ties are broken.";
    return schema;
}

std::vector<json::ValidationError> validate_brain(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, brain_schema());
    if (!errors.empty()) {
        return errors;
    }
    const auto names = fact_names(doc);
    std::set<std::string, std::less<>> seen;
    const json::Array& goals = doc.find("goals")->as_array();
    for (std::size_t i = 0; i < goals.size(); ++i) {
        const std::string at = "/goals/" + std::to_string(i);
        if (!seen.insert(goals[i].find("name")->as_string()).second) {
            errors.push_back({at + "/name", "another goal has this name"});
        }
        check_facts_map(goals[i].find("when"), at + "/when", names, false, errors);
        check_facts_map(goals[i].find("want"), at + "/want", names, false, errors);
    }
    seen.clear();
    const json::Array& actions = doc.find("actions")->as_array();
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const std::string at = "/actions/" + std::to_string(i);
        if (!seen.insert(actions[i].find("name")->as_string()).second) {
            errors.push_back({at + "/name", "another action has this name"});
        }
        check_facts_map(actions[i].find("needs"), at + "/needs", names, false, errors);
        check_facts_map(actions[i].find("effects"), at + "/effects", names, true, errors);
        if (const json::Value* behavior = actions[i].find("do"); behavior != nullptr) {
            const int kinds = (behavior->find("wait") != nullptr ? 1 : 0) +
                              (behavior->find("go_to") != nullptr ? 1 : 0) +
                              (behavior->find("emit") != nullptr ? 1 : 0);
            if (kinds > 1) {
                errors.push_back({at + "/do", "do takes one of wait, go_to and emit"});
            }
            if (behavior->find("speed") != nullptr && behavior->find("go_to") == nullptr) {
                errors.push_back({at + "/do/speed", "speed is a go_to's"});
            }
        }
    }
    return errors;
}

Result<Brain> parse_brain(const json::Value& doc) {
    if (const auto errors = validate_brain(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return Error{ErrorCode::InvalidArgument,
                     (first.path.empty() ? std::string("root") : first.path) + ": " +
                         first.message};
    }
    Brain brain;
    for (const auto& [name, value] : doc.find("facts")->as_object()) {
        brain.facts.push_back(name);
        brain.defaults.push_back(value.is_bool() ? static_cast<int>(value.as_bool())
                                                 : static_cast<int>(value.as_int()));
    }
    if (const json::Value* limit = doc.find("search_limit"); limit != nullptr) {
        brain.search_limit = static_cast<int>(limit->as_int());
    }
    for (const json::Value& item : doc.find("goals")->as_array()) {
        Goal goal;
        goal.name = item.find("name")->as_string();
        if (const json::Value* priority = item.find("priority"); priority != nullptr) {
            goal.priority = static_cast<float>(priority->as_double());
        }
        goal.when = conditions_of(item.find("when"), brain);
        goal.want = conditions_of(item.find("want"), brain);
        brain.goals.push_back(std::move(goal));
    }
    for (std::size_t i = 0; i < brain.goals.size(); ++i) {
        brain.priorities.push_back(i);
    }
    std::ranges::stable_sort(brain.priorities, std::greater<>(),
                             [&](std::size_t i) { return brain.goals[i].priority; });
    for (const json::Value& item : doc.find("actions")->as_array()) {
        Action action;
        action.name = item.find("name")->as_string();
        if (const json::Value* cost = item.find("cost"); cost != nullptr) {
            action.cost = static_cast<float>(cost->as_double());
        }
        action.needs = conditions_of(item.find("needs"), brain);
        action.effects = effects_of(item.find("effects"), brain);
        if (const json::Value* behavior = item.find("do"); behavior != nullptr) {
            if (const json::Value* wait = behavior->find("wait"); wait != nullptr) {
                action.behavior.kind = Behavior::Kind::Wait;
                action.behavior.seconds = wait->as_double();
            } else if (const json::Value* target = behavior->find("go_to"); target != nullptr) {
                action.behavior.kind = Behavior::Kind::GoTo;
                action.behavior.target = target->as_string();
                if (const json::Value* speed = behavior->find("speed"); speed != nullptr) {
                    action.behavior.speed = static_cast<float>(speed->as_double());
                }
            } else if (const json::Value* event = behavior->find("emit"); event != nullptr) {
                action.behavior.kind = Behavior::Kind::Emit;
                action.behavior.event = event->as_string();
            }
        }
        brain.actions.push_back(std::move(action));
    }
    return brain;
}

void apply_effects(std::span<const Effect> effects, std::vector<int>& facts) {
    for (const Effect& effect : effects) {
        int& value = facts.at(effect.fact);
        value = effect.add ? added(value, effect.value) : effect.value;
    }
}

bool holds(std::span<const Condition> conditions, std::span<const int> facts) {
    return std::ranges::all_of(conditions,
                               [&](const Condition& c) { return meets(c, facts[c.fact]); });
}

Plan plan(const Brain& brain, std::span<const int> facts, const Goal& goal) {
    Plan out;
    // Each state the search reached: its facts, the cheapest cost to it, and how.
    struct Node {
        std::vector<int> facts;
        float cost = 0.0F;
        std::size_t parent = 0;
        std::size_t action = 0;
    };
    std::vector<Node> nodes;
    nodes.push_back(
        {.facts = {facts.begin(), facts.end()}, .cost = 0.0F, .parent = 0, .action = 0});
    std::map<std::vector<int>, std::size_t> by_facts{{nodes.front().facts, 0}};
    // h: each unmet want takes at least one action, so at least the cheapest action's cost.
    float cheapest = std::numeric_limits<float>::max();
    for (const Action& action : brain.actions) {
        cheapest = std::min(cheapest, action.cost);
    }
    if (brain.actions.empty()) {
        cheapest = 0.0F;
    }
    const auto estimate = [&](const std::vector<int>& state) {
        return static_cast<float>(unmet(goal.want, state)) * cheapest;
    };
    // The open list by estimated total, then the deeper cost, then when it was reached, so the
    // search goes the same way everywhere.
    struct Open {
        float total = 0.0F;
        float cost = 0.0F;
        std::size_t node = 0;
    };
    const auto later = [](const Open& a, const Open& b) {
        if (a.total != b.total) {
            return a.total > b.total;
        }
        if (a.cost != b.cost) {
            return a.cost < b.cost;
        }
        return a.node > b.node;
    };
    std::priority_queue<Open, std::vector<Open>, decltype(later)> open(later);
    open.push({.total = estimate(nodes.front().facts), .cost = 0.0F, .node = 0});
    std::vector<bool> closed(1, false);
    while (!open.empty()) {
        const Open top = open.top();
        open.pop();
        if (closed[top.node] || top.cost > nodes[top.node].cost) {
            continue;
        }
        closed[top.node] = true;
        ++out.searched;
        if (holds(goal.want, nodes[top.node].facts)) {
            out.found = true;
            out.cost = nodes[top.node].cost;
            for (std::size_t at = top.node; at != 0; at = nodes[at].parent) {
                out.actions.push_back(nodes[at].action);
            }
            std::ranges::reverse(out.actions);
            return out;
        }
        if (out.searched >= static_cast<std::size_t>(brain.search_limit)) {
            break;
        }
        for (std::size_t a = 0; a < brain.actions.size(); ++a) {
            const Action& action = brain.actions[a];
            if (!holds(action.needs, nodes[top.node].facts)) {
                continue;
            }
            std::vector<int> next = nodes[top.node].facts;
            apply_effects(action.effects, next);
            const float cost = nodes[top.node].cost + action.cost;
            const auto found = by_facts.find(next);
            if (found != by_facts.end()) {
                Node& seen = nodes[found->second];
                if (closed[found->second] || seen.cost <= cost) {
                    continue;
                }
                seen.cost = cost;
                seen.parent = top.node;
                seen.action = a;
                open.push(
                    {.total = cost + estimate(seen.facts), .cost = cost, .node = found->second});
                continue;
            }
            const std::size_t index = nodes.size();
            const float total = cost + estimate(next);
            by_facts.emplace(next, index);
            nodes.push_back(
                {.facts = std::move(next), .cost = cost, .parent = top.node, .action = a});
            closed.push_back(false);
            open.push({.total = total, .cost = cost, .node = index});
        }
    }
    return out;
}

Choice choose(const Brain& brain, std::span<const int> facts) {
    for (const std::size_t index : brain.priorities) {
        const Goal& goal = brain.goals[index];
        if (!holds(goal.when, facts) || holds(goal.want, facts)) {
            continue;
        }
        Plan found = plan(brain, facts, goal);
        if (found.found) {
            return {.goal = &goal, .plan = std::move(found)};
        }
    }
    return {};
}

std::vector<json::ValidationError> check_brain(const Brain& brain) {
    std::vector<json::ValidationError> out;
    std::vector<bool> used(brain.actions.size(), false);
    for (std::size_t g = 0; g < brain.goals.size(); ++g) {
        const Goal& goal = brain.goals[g];
        std::vector<int> start = brain.defaults;
        // The goal's when conditions taken as met where they say a value.
        for (const Condition& condition : goal.when) {
            if (condition.compare == Compare::Equal) {
                start[condition.fact] = condition.value;
            }
        }
        if (holds(goal.want, start)) {
            continue;
        }
        const Plan found = plan(brain, start, goal);
        if (!found.found) {
            out.push_back({"/goals/" + std::to_string(g),
                           "goal \"" + goal.name + "\" is out of reach from the facts' defaults" +
                               (found.searched >= static_cast<std::size_t>(brain.search_limit)
                                    ? " within search_limit"
                                    : "")});
            continue;
        }
        for (const std::size_t a : found.actions) {
            used[a] = true;
        }
    }
    for (std::size_t a = 0; a < brain.actions.size(); ++a) {
        if (!used[a]) {
            out.push_back(
                {"/actions/" + std::to_string(a),
                 "no goal's plan from the facts' defaults uses \"" + brain.actions[a].name + "\""});
        }
    }
    return out;
}

} // namespace slash::npc
