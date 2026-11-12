#include "slash/command/npc.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/npc/npc.hpp"
#include "slash/npc/story.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/components.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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
    if (brain->hour != npc::kNoFact || brain->day != npc::kNoFact) {
        const npc::Clock now = npc::clock(world);
        result["clock"] =
            json::Object{{"day", now.day}, {"hour", now.hour}, {"minute", now.minute}};
    }
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

// The entity params name, or the reply saying why there is none.
std::variant<entt::entity, Reply> entity_at(Session& session, const json::Value& params,
                                            std::string_view key) {
    const world::World& world = session.world();
    const json::Value* id = params.find(key);
    if (id == nullptr) {
        return entt::entity{entt::null};
    }
    const entt::entity entity = world.find(id->as_string());
    if (entity == entt::null) {
        return param_error(code::kNotFound, "/" + std::string(key),
                           "no entity has the id " + quoted(id->as_string()) +
                               did_you_mean(id->as_string(), world.ids()));
    }
    return entity;
}

json::Value quest_json(const npc::QuestView& view) {
    json::Value out;
    out["quest"] = view.quest;
    if (!view.title.empty()) {
        out["title"] = view.title;
    }
    out["state"] = view.state;
    if (!view.stage.empty()) {
        out["stage"] = view.stage;
    }
    if (!view.journal.empty()) {
        json::Array journal;
        for (const std::string& entry : view.journal) {
            journal.emplace_back(entry);
        }
        out["journal"] = std::move(journal);
    }
    if (!view.objectives.empty()) {
        json::Array objectives;
        for (const npc::QuestView::ObjectiveView& objective : view.objectives) {
            json::Value item;
            item["id"] = objective.id;
            item["text"] = objective.text;
            item["done"] = objective.done;
            if (objective.optional) {
                item["optional"] = true;
            }
            objectives.push_back(std::move(item));
        }
        out["objectives"] = std::move(objectives);
    }
    return out;
}

json::Value facts_json(const world::World& world, entt::entity log) {
    json::Object facts;
    if (const auto* quests = world.ecs().try_get<world::QuestLog>(log); quests != nullptr) {
        for (const auto& [name, value] : quests->facts) {
            facts[name] = value;
        }
    }
    return facts;
}

// Each quest's state and stage, to tell what a command moved on.
std::map<std::string, std::pair<std::string, std::string>> quest_places(const world::World& world,
                                                                        entt::entity log) {
    std::map<std::string, std::pair<std::string, std::string>> out;
    if (const auto* quests = world.ecs().try_get<world::QuestLog>(log); quests != nullptr) {
        for (const auto& [path, progress] : quests->quests) {
            out[path] = {progress.state, progress.stage};
        }
    }
    return out;
}

// The quests a command moved on, as the journal shows them.
Reply moved_quests(Session& session, entt::entity log,
                   const std::map<std::string, std::pair<std::string, std::string>>& before) {
    world::World& world = session.world();
    auto views = npc::quests_of(world, log);
    if (!views) {
        return Reply::failure(code::kInvalidParams, views.error().message);
    }
    json::Value result;
    result["facts"] = facts_json(world, log);
    json::Array moved;
    for (const npc::QuestView& view : views.value()) {
        const auto was = before.find(view.quest);
        if (was == before.end() || was->second != std::pair{view.state, view.stage}) {
            moved.push_back(quest_json(view));
        }
    }
    result["quests"] = std::move(moved);
    return Reply::success(std::move(result));
}

Reply quest_start(Session& session, const json::Value& params) {
    auto found = entity_at(session, params, "id");
    if (auto* reply = std::get_if<Reply>(&found)) {
        return std::move(*reply);
    }
    const entt::entity log = std::get<entt::entity>(found);
    const auto before = quest_places(session.world(), log);
    const std::string& path = params.find("quest")->as_string();
    if (!path.ends_with(npc::kQuestSuffix)) {
        return param_error(code::kInvalidParams, "/quest", "quests are .quest.json files");
    }
    if (auto started = npc::start_quest(session.world(), log, path); !started) {
        return param_error(code::kInvalidParams, "/quest", started.error().message);
    }
    return moved_quests(session, log, before);
}

Reply quest_set(Session& session, const json::Value& params) {
    auto found = entity_at(session, params, "id");
    if (auto* reply = std::get_if<Reply>(&found)) {
        return std::move(*reply);
    }
    const entt::entity log = std::get<entt::entity>(found);
    const auto before = quest_places(session.world(), log);
    for (const auto& [name, value] : params.find("facts")->as_object()) {
        npc::set_story_fact(session.world(), log, name,
                            value.is_bool() ? static_cast<int>(value.as_bool())
                                            : static_cast<int>(value.as_int()));
    }
    return moved_quests(session, log, before);
}

Reply quest_state(Session& session, const json::Value& params) {
    auto found = entity_at(session, params, "id");
    if (auto* reply = std::get_if<Reply>(&found)) {
        return std::move(*reply);
    }
    const entt::entity log = std::get<entt::entity>(found);
    auto views = npc::quests_of(session.world(), log);
    if (!views) {
        return Reply::failure(code::kInvalidParams, views.error().message);
    }
    const json::Value* only = params.find("quest");
    json::Value result;
    result["facts"] = facts_json(session.world(), log);
    json::Array quests;
    for (const npc::QuestView& view : views.value()) {
        if (only == nullptr || only->as_string() == view.quest) {
            quests.push_back(quest_json(view));
        }
    }
    if (only != nullptr && quests.empty()) {
        return param_error(code::kNotFound, "/quest",
                           quoted(only->as_string()) + " isn't in the entity's QuestLog");
    }
    result["quests"] = std::move(quests);
    return Reply::success(std::move(result));
}

Reply quest_check(Session& session, const json::Value& params) {
    npc::StoryFiles files;
    std::vector<std::string> listed;
    if (const json::Value* paths = params.find("paths"); paths != nullptr) {
        for (const json::Value& path : paths->as_array()) {
            listed.push_back(path.as_string());
        }
    } else {
        auto all = platform::list_files(session.options().project_root,
                                        [](std::string_view folder) { return folder != "build/"; });
        if (!all) {
            return Reply::failure(code::kInternalError, all.error().message);
        }
        listed = std::move(all).value();
    }
    json::Array errors;
    json::Array warnings;
    const auto problem = [](std::string file, std::string path, std::string message) {
        json::Value item;
        item["file"] = std::move(file);
        item["path"] = std::move(path);
        item["message"] = std::move(message);
        return item;
    };
    for (std::size_t i = 0; i < listed.size(); ++i) {
        const std::string& path = listed[i];
        const bool quest = path.ends_with(npc::kQuestSuffix);
        const bool dialogue = path.ends_with(npc::kDialogueSuffix);
        const bool brain = path.ends_with(npc::kBrainSuffix);
        if (!quest && !dialogue && !brain) {
            if (params.find("paths") != nullptr) {
                return param_error(code::kInvalidParams, "/paths/" + std::to_string(i),
                                   "check .quest.json, .dialogue.json and .npc.json files");
            }
            continue;
        }
        auto text = session.read_project_file(path);
        if (!text) {
            errors.push_back(problem(path, "", text.error().message));
            continue;
        }
        const std::optional<std::string>& contents = text.value();
        if (!contents.has_value()) {
            errors.push_back(problem(path, "", "no such file"));
            continue;
        }
        auto parsed = json::parse(contents.value());
        if (!parsed) {
            errors.push_back(problem(path, "", parsed.error().message));
            continue;
        }
        auto* into = &files.brains;
        if (quest) {
            into = &files.quests;
        } else if (dialogue) {
            into = &files.dialogues;
        }
        into->emplace(path, std::move(parsed).value());
    }
    const world::FileReader reader = [&session](std::string_view path) -> Result<std::string> {
        auto text = session.read_project_file(path);
        if (!text) {
            return text.error();
        }
        if (!text.value()) {
            return Error{ErrorCode::NotFound, "no such file"};
        }
        return std::move(*text.value());
    };
    for (npc::StoryProblem& found : npc::check_story(files, reader)) {
        (found.error ? errors : warnings)
            .push_back(
                problem(std::move(found.file), std::move(found.path), std::move(found.message)));
    }
    json::Value result;
    result["quests"] = files.quests.size();
    result["dialogues"] = files.dialogues.size();
    result["errors"] = std::move(errors);
    if (!warnings.empty()) {
        result["warnings"] = std::move(warnings);
    }
    return Reply::success(std::move(result));
}

json::Value line_json(const npc::LineView& view) {
    json::Value out;
    out["talking"] = view.talking;
    if (!view.talking) {
        return out;
    }
    out["dialogue"] = view.dialogue;
    out["line"] = view.line;
    out["speaker"] = view.speaker;
    out["text"] = view.text;
    if (!view.choices.empty()) {
        json::Array choices;
        for (const std::string& choice : view.choices) {
            choices.emplace_back(choice);
        }
        out["choices"] = std::move(choices);
    }
    return out;
}

Reply line_reply(Session& session, entt::entity listener) {
    auto view = npc::current_line(session.world(), listener);
    if (!view) {
        return Reply::failure(code::kInvalidParams, view.error().message);
    }
    return Reply::success(line_json(view.value()));
}

Reply dialogue_start(Session& session, const json::Value& params) {
    auto listener = entity_at(session, params, "id");
    if (auto* reply = std::get_if<Reply>(&listener)) {
        return std::move(*reply);
    }
    auto speaker = entity_at(session, params, "speaker");
    if (auto* reply = std::get_if<Reply>(&speaker)) {
        return std::move(*reply);
    }
    const std::string& path = params.find("dialogue")->as_string();
    if (!path.ends_with(npc::kDialogueSuffix)) {
        return param_error(code::kInvalidParams, "/dialogue", "dialogues are .dialogue.json files");
    }
    const json::Value* line = params.find("line");
    const entt::entity entity = std::get<entt::entity>(listener);
    if (auto started =
            npc::start_dialogue(session.world(), entity, std::get<entt::entity>(speaker), path,
                                line != nullptr ? line->as_string() : std::string());
        !started) {
        return param_error(code::kInvalidParams, line != nullptr ? "/line" : "/dialogue",
                           started.error().message);
    }
    return line_reply(session, entity);
}

Reply dialogue_choose(Session& session, const json::Value& params) {
    auto listener = entity_at(session, params, "id");
    if (auto* reply = std::get_if<Reply>(&listener)) {
        return std::move(*reply);
    }
    const entt::entity entity = std::get<entt::entity>(listener);
    std::optional<std::size_t> choice;
    if (const json::Value* index = params.find("choice"); index != nullptr) {
        choice = static_cast<std::size_t>(index->as_int());
    }
    if (auto moved = npc::advance_dialogue(session.world(), entity, choice); !moved) {
        return param_error(code::kInvalidParams, choice ? "/choice" : "/id", moved.error().message);
    }
    return line_reply(session, entity);
}

Reply dialogue_state(Session& session, const json::Value& params) {
    auto listener = entity_at(session, params, "id");
    if (auto* reply = std::get_if<Reply>(&listener)) {
        return std::move(*reply);
    }
    return line_reply(session, std::get<entt::entity>(listener));
}

json::Value object_schema(json::Object properties, json::Array required) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

json::Value id_property(std::string_view description) {
    json::Value id = property("string", description);
    id["minLength"] = 1;
    return id;
}

void register_story_commands(Registry& registry) {
    using enum Capability;
    json::Value facts = property("object", "Story facts to set, by name.");
    facts["additionalProperties"]["anyOf"] =
        json::Array{json::Object{{"type", "integer"}}, json::Object{{"type", "boolean"}}};
    const json::Value log =
        id_property("The entity whose QuestLog it is, usually the player; one is added if it "
                    "has none.");
    registry.add({
        .name = "quest.start",
        .description = "Starts a .quest.json quest in an entity's QuestLog and moves it on as "
                       "far as the story's facts take it. Returns the facts and the quests that "
                       "moved.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = object_schema({{"id", log}, {"quest", id_property("The .quest.json file.")}},
                                json::Array{"id", "quest"}),
        .handler = quest_start,
    });
    registry.add({
        .name = "quest.set",
        .description = "Sets story facts in an entity's QuestLog, moving its quests on. Returns "
                       "the facts and the quests that moved.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params =
            object_schema({{"id", log}, {"facts", std::move(facts)}}, json::Array{"id", "facts"}),
        .handler = quest_set,
    });
    registry.add({
        .name = "quest.state",
        .description = "An entity's story facts and quests as its journal shows them: state, "
                       "stage, journal text and the current objectives, translated.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = object_schema({{"id", id_property("The entity with the QuestLog.")},
                                 {"quest", property("string", "Only this .quest.json quest.")}},
                                json::Array{"id"}),
        .handler = quest_state,
    });
    json::Value paths = property("array", "Only these .quest.json, .dialogue.json and .npc.json "
                                          "files; default every one in the project.");
    paths["items"]["type"] = "string";
    registry.add({
        .name = "quest.check",
        .description = "Checks the project's quests and dialogue: that every quest can start "
                       "and be finished and every stage and line be reached, from every value "
                       "any file sets each story fact to. Returns errors and warnings.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = object_schema({{"paths", std::move(paths)}}, {}),
        .handler = quest_check,
    });
    registry.add({
        .name = "dialogue.start",
        .description = "Starts a .dialogue.json conversation for a listener and returns its first "
                       "line, translated, with the choices shown.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params = object_schema(
            {{"id", id_property("The listener, whose QuestLog the facts are.")},
             {"speaker", id_property("The NPC talking, whose facts are self.<name>.")},
             {"dialogue", id_property("The .dialogue.json file.")},
             {"line", id_property("Start at this line rather than the file's start.")}},
            json::Array{"id", "dialogue"}),
        .handler = dialogue_start,
    });
    json::Value choice = property("integer", "The choice's number among those shown, from 0; "
                                             "leave out for a line without choices.");
    choice["minimum"] = 0;
    registry.add({
        .name = "dialogue.choose",
        .description = "Moves a listener's conversation on, by a choice or to the next line, and "
                       "returns the line it reaches, or talking false at the end.",
        .capabilities = {Edit},
        .effect = Effect::Edit,
        .params =
            object_schema({{"id", id_property("The listener.")}, {"choice", std::move(choice)}},
                          json::Array{"id"}),
        .handler = dialogue_choose,
    });
    registry.add({
        .name = "dialogue.state",
        .description = "The line a listener is hearing, translated, with the choices shown.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = object_schema({{"id", id_property("The listener.")}}, json::Array{"id"}),
        .handler = dialogue_state,
    });
}

} // namespace

void register_npc_commands(Registry& registry) {
    register_story_commands(registry);
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
