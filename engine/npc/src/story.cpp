// .quest.json and .dialogue.json files: their schemas, the errors JSON Schema can't find and
// reading them.

#include "slash/npc/story.hpp"

#include "facts.hpp"

#include <algorithm>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace slash::npc {

namespace {

json::Value text_property(std::string_view description, bool non_empty = true) {
    json::Value out;
    out["type"] = "string";
    if (non_empty) {
        out["minLength"] = 1;
    }
    out["description"] = description;
    return out;
}

json::Value facts_schema(std::string_view description) {
    json::Value out;
    out["type"] = "object";
    out["description"] = description;
    out["additionalProperties"]["type"] = json::Array{"boolean", "integer", "string"};
    return out;
}

// Conditions by fact: one value or comparison, or a list that must all hold.
json::Value conditions_schema(std::string_view description) {
    json::Value one;
    one["type"] = json::Array{"boolean", "integer", "string"};
    json::Value list;
    list["type"] = "array";
    list["minItems"] = 1;
    list["items"] = one;
    json::Value out;
    out["type"] = "object";
    out["description"] = description;
    out["additionalProperties"]["anyOf"] = json::Array{std::move(one), std::move(list)};
    return out;
}

constexpr std::string_view kConditions =
    "Facts by name, each a value it must equal (true, false or a whole number), a string "
    "comparing it (\"!=0\", \"<5\", \"<=5\", \">0\" or \">=1\") or a list of them that must all "
    "hold. A missing fact is 0.";
constexpr std::string_view kEffects =
    "Facts by name, each a value to set (true, false or a whole number) or a change such as "
    "\"+1\" or \"-2\".";

// Where to go next: one id, or branches tried in order, the first whose when holds.
json::Value goto_schema(std::string_view key, std::string_view description) {
    json::Value branch;
    branch["type"] = "object";
    branch["additionalProperties"] = false;
    branch["required"] = json::Array{std::string(key)};
    branch["properties"]["when"] = conditions_schema("When this branch is taken.");
    branch["properties"][key] = text_property("Where it goes.");
    json::Value list;
    list["type"] = "array";
    list["minItems"] = 1;
    list["items"] = std::move(branch);
    json::Value out;
    out["description"] = description;
    out["anyOf"] = json::Array{text_property("Where it goes."), std::move(list)};
    return out;
}

void check_conditions(const json::Value* map, const std::string& at, bool self_allowed,
                      std::vector<json::ValidationError>& errors) {
    if (map == nullptr || !map->is_object()) {
        return;
    }
    for (const auto& [name, value] : map->as_object()) {
        const std::string where = at + "/" + json::pointer_escape(name);
        if (!self_allowed && name.starts_with(kSelfPrefix)) {
            errors.push_back({where, "self.<name> facts are a speaking NPC's, in dialogue only"});
        }
        Compare compare = Compare::Equal;
        int number = 0;
        if (value.is_array()) {
            const json::Array& list = value.as_array();
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (std::string problem = detail::read_condition(list[i], compare, number);
                    !problem.empty()) {
                    errors.push_back({where + "/" + std::to_string(i), std::move(problem)});
                }
            }
            continue;
        }
        if (std::string problem = detail::read_condition(value, compare, number);
            !problem.empty()) {
            errors.push_back({where, std::move(problem)});
        }
    }
}

void check_effects(const json::Value* map, const std::string& at, bool self_allowed,
                   std::vector<json::ValidationError>& errors) {
    if (map == nullptr || !map->is_object()) {
        return;
    }
    for (const auto& [name, value] : map->as_object()) {
        const std::string where = at + "/" + json::pointer_escape(name);
        if (!self_allowed && name.starts_with(kSelfPrefix)) {
            errors.push_back({where, "self.<name> facts are a speaking NPC's, in dialogue only"});
        }
        bool add = false;
        int number = 0;
        if (std::string problem = detail::read_effect(value, add, number); !problem.empty()) {
            errors.push_back({where, std::move(problem)});
        }
    }
}

// The errors of where a goto goes: ids that aren't in names.
void check_goto(const json::Value* value, std::string_view key, const std::string& at,
                const std::set<std::string, std::less<>>& names, std::string_view kind,
                bool self_allowed, std::vector<json::ValidationError>& errors) {
    if (value == nullptr) {
        return;
    }
    const auto check_name = [&](const std::string& name, const std::string& where) {
        if (!names.contains(name)) {
            errors.push_back({where, "no " + std::string(kind) + " has the id \"" + name + "\""});
        }
    };
    if (value->is_string()) {
        check_name(value->as_string(), at);
        return;
    }
    const json::Array& branches = value->as_array();
    for (std::size_t i = 0; i < branches.size(); ++i) {
        const std::string where = at + "/" + std::to_string(i);
        check_name(branches[i].find(key)->as_string(), where + "/" + std::string(key));
        check_conditions(branches[i].find("when"), where + "/when", self_allowed, errors);
    }
}

std::vector<NamedCondition> conditions_of(const json::Value* map) {
    std::vector<NamedCondition> out;
    if (map == nullptr) {
        return out;
    }
    for (const auto& [name, value] : map->as_object()) {
        // A list of conditions on one fact, all of which must hold.
        const std::span<const json::Value> items =
            value.is_array() ? std::span<const json::Value>(value.as_array())
                             : std::span<const json::Value>(&value, 1);
        for (const json::Value& item : items) {
            NamedCondition condition{.fact = name, .compare = Compare::Equal, .value = 0};
            (void)detail::read_condition(item, condition.compare, condition.value);
            out.push_back(std::move(condition));
        }
    }
    return out;
}

std::vector<NamedEffect> effects_of(const json::Value* map) {
    std::vector<NamedEffect> out;
    if (map == nullptr) {
        return out;
    }
    for (const auto& [name, value] : map->as_object()) {
        NamedEffect effect{.fact = name, .add = false, .value = 0};
        (void)detail::read_effect(value, effect.add, effect.value);
        out.push_back(std::move(effect));
    }
    return out;
}

std::vector<Branch> branches_of(const json::Value* value, std::string_view key) {
    std::vector<Branch> out;
    if (value == nullptr) {
        return out;
    }
    if (value->is_string()) {
        out.push_back({.when = {}, .to = value->as_string()});
        return out;
    }
    for (const json::Value& branch : value->as_array()) {
        out.push_back(
            {.when = conditions_of(branch.find("when")), .to = branch.find(key)->as_string()});
    }
    return out;
}

std::string string_of(const json::Value& doc, std::string_view key) {
    const json::Value* value = doc.find(key);
    return value != nullptr ? value->as_string() : std::string();
}

Error first_error(const std::vector<json::ValidationError>& errors) {
    const json::ValidationError& first = errors.front();
    return Error{ErrorCode::InvalidArgument,
                 (first.path.empty() ? std::string("root") : first.path) + ": " + first.message};
}

} // namespace

const Stage* Quest::stage(std::string_view id) const {
    const auto found = std::ranges::find(stages, id, &Stage::id);
    return found == stages.end() ? nullptr : &*found;
}

const Line* Dialogue::line(std::string_view id) const {
    const auto found = std::ranges::find(lines, id, &Line::id);
    return found == lines.end() ? nullptr : &*found;
}

json::Value quest_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine quest (.quest.json)";
    schema["description"] =
        "A questline: stages, each with journal text and objectives over the story's facts (a "
        "QuestLog's), its rewards and where it goes next. A quest starts with its first stage; a "
        "stage whose required objectives are met applies its rewards and goes on, and an end "
        "stage finishes the quest.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "title", "stages"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "The file format's version.";
    properties["title"] = text_property("The quest's name, as the journal shows it (translated).");
    properties["summary"] =
        text_property("What the quest is about, as the journal shows it (translated).", false);
    properties["start_when"] = conditions_schema(
        std::string("When a quest waiting in a QuestLog starts: ") + std::string(kConditions));
    properties["external"]["type"] = "array";
    properties["external"]["items"] = text_property("A fact's name.");
    properties["external"]["description"] =
        "Story facts that game code sets, which quest.check counts as settable to any value.";

    json::Value objective;
    objective["type"] = "object";
    objective["additionalProperties"] = false;
    objective["required"] = json::Array{"id", "text", "when"};
    objective["properties"]["id"] = text_property("The objective's id, unique in its stage.");
    objective["properties"]["text"] = text_property("What the journal says to do (translated).");
    objective["properties"]["when"] = conditions_schema(
        std::string("When it is met; once met it stays met: ") + std::string(kConditions));
    objective["properties"]["optional"]["type"] = "boolean";
    objective["properties"]["optional"]["default"] = false;
    objective["properties"]["optional"]["description"] = "Whether the stage can go on without it.";

    json::Value stage;
    stage["type"] = "object";
    stage["additionalProperties"] = false;
    stage["required"] = json::Array{"id"};
    stage["properties"]["id"] = text_property("The stage's id, unique in the quest.");
    stage["properties"]["journal"] =
        text_property("What the journal says once the stage is reached (translated).", false);
    stage["properties"]["objectives"]["type"] = "array";
    stage["properties"]["objectives"]["items"] = std::move(objective);
    stage["properties"]["objectives"]["description"] =
        "What the player has to do; with none, the stage goes on at once.";
    stage["properties"]["fail_when"] = conditions_schema(
        std::string("When the quest fails at this stage: ") + std::string(kConditions));
    stage["properties"]["rewards"] = facts_schema(
        std::string("What changes when its objectives are met: ") + std::string(kEffects));
    stage["properties"]["next"] = goto_schema(
        "stage", "The stage after it: an id, or branches tried in order (a branch without when "
                 "always holds; with none holding it waits). Without next, the next stage in the "
                 "file, or success after the last.");
    stage["properties"]["end"]["enum"] = json::Array{"success", "failure"};
    stage["properties"]["end"]["description"] =
        "Finishes the quest when reached, with this result; an end stage has no objectives, "
        "rewards or next.";
    properties["stages"]["type"] = "array";
    properties["stages"]["minItems"] = 1;
    properties["stages"]["items"] = std::move(stage);
    properties["stages"]["description"] = "The quest's stages; it starts at the first.";
    return schema;
}

std::vector<json::ValidationError> validate_quest(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, quest_schema());
    if (!errors.empty()) {
        return errors;
    }
    check_conditions(doc.find("start_when"), "/start_when", false, errors);
    const json::Array& stages = doc.find("stages")->as_array();
    std::set<std::string, std::less<>> ids;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (!ids.insert(stages[i].find("id")->as_string()).second) {
            errors.push_back({"/stages/" + std::to_string(i) + "/id", "another stage has this id"});
        }
    }
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const json::Value& stage = stages[i];
        const std::string at = "/stages/" + std::to_string(i);
        if (stage.find("end") != nullptr) {
            for (const std::string_view key : {"objectives", "rewards", "next", "fail_when"}) {
                if (stage.find(key) != nullptr) {
                    errors.push_back(
                        {at + "/" + std::string(key),
                         "an end stage finishes the quest: it has no " + std::string(key)});
                }
            }
        }
        if (const json::Value* objectives = stage.find("objectives"); objectives != nullptr) {
            std::set<std::string, std::less<>> seen;
            const json::Array& list = objectives->as_array();
            for (std::size_t o = 0; o < list.size(); ++o) {
                const std::string where = at + "/objectives/" + std::to_string(o);
                if (!seen.insert(list[o].find("id")->as_string()).second) {
                    errors.push_back({where + "/id", "another objective of the stage has this id"});
                }
                check_conditions(list[o].find("when"), where + "/when", false, errors);
            }
        }
        check_conditions(stage.find("fail_when"), at + "/fail_when", false, errors);
        check_effects(stage.find("rewards"), at + "/rewards", false, errors);
        check_goto(stage.find("next"), "stage", at + "/next", ids, "stage", false, errors);
    }
    return errors;
}

Result<Quest> parse_quest(const json::Value& doc) {
    if (const auto errors = validate_quest(doc); !errors.empty()) {
        return first_error(errors);
    }
    Quest quest;
    quest.title = string_of(doc, "title");
    quest.summary = string_of(doc, "summary");
    quest.start_when = conditions_of(doc.find("start_when"));
    if (const json::Value* external = doc.find("external"); external != nullptr) {
        for (const json::Value& fact : external->as_array()) {
            quest.external.push_back(fact.as_string());
        }
    }
    for (const json::Value& item : doc.find("stages")->as_array()) {
        Stage stage;
        stage.id = string_of(item, "id");
        stage.journal = string_of(item, "journal");
        if (const json::Value* objectives = item.find("objectives"); objectives != nullptr) {
            for (const json::Value& objective : objectives->as_array()) {
                const json::Value* optional = objective.find("optional");
                stage.objectives.push_back(
                    {.id = string_of(objective, "id"),
                     .text = string_of(objective, "text"),
                     .when = conditions_of(objective.find("when")),
                     .optional = optional != nullptr && optional->as_bool()});
            }
        }
        stage.fail_when = conditions_of(item.find("fail_when"));
        stage.rewards = effects_of(item.find("rewards"));
        stage.next = branches_of(item.find("next"), "stage");
        if (const json::Value* end = item.find("end"); end != nullptr) {
            stage.end = end->as_string() == "success" ? Stage::End::Success : Stage::End::Failure;
        }
        quest.stages.push_back(std::move(stage));
    }
    return quest;
}

json::Value dialogue_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine dialogue (.dialogue.json)";
    schema["description"] =
        "A conversation: lines by id, each said by someone, going on to the next line or by the "
        "listener's choices. Conditions and effects name the story's facts (the listener's "
        "QuestLog's) or, as self.<name>, the speaking NPC's own. Text is translated.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "start", "lines"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "The file format's version.";
    properties["start"] =
        goto_schema("line", "The first line: an id, or branches tried in order (a branch without "
                            "when always holds).");

    json::Value choice;
    choice["type"] = "object";
    choice["additionalProperties"] = false;
    choice["required"] = json::Array{"text"};
    choice["properties"]["text"] = text_property("What the listener says (translated).");
    choice["properties"]["when"] =
        conditions_schema(std::string("When it is shown: ") + std::string(kConditions));
    choice["properties"]["effects"] =
        facts_schema(std::string("What choosing it changes: ") + std::string(kEffects));
    choice["properties"]["start_quest"] = text_property("A .quest.json file choosing it starts.");
    choice["properties"]["next"] =
        goto_schema("line", "The line it leads to; without next the conversation ends.");

    json::Value line;
    line["type"] = "object";
    line["additionalProperties"] = false;
    line["required"] = json::Array{"text"};
    line["properties"]["speaker"] =
        text_property("Who says it, as shown (translated); without it, the speaking entity.");
    line["properties"]["text"] = text_property("What is said (translated).");
    line["properties"]["effects"] =
        facts_schema(std::string("What saying it changes: ") + std::string(kEffects));
    line["properties"]["start_quest"] = text_property("A .quest.json file saying it starts.");
    line["properties"]["choices"]["type"] = "array";
    line["properties"]["choices"]["minItems"] = 1;
    line["properties"]["choices"]["items"] = std::move(choice);
    line["properties"]["choices"]["description"] =
        "What the listener can answer, those whose when holds shown in order.";
    line["properties"]["next"] = goto_schema(
        "line", "The line after it, for a line without choices; without next the conversation "
                "ends.");
    properties["lines"]["type"] = "object";
    properties["lines"]["minProperties"] = 1;
    properties["lines"]["additionalProperties"] = std::move(line);
    properties["lines"]["description"] = "Every line by id.";
    return schema;
}

std::vector<json::ValidationError> validate_dialogue(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, dialogue_schema());
    if (!errors.empty()) {
        return errors;
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& [id, line] : doc.find("lines")->as_object()) {
        ids.insert(id);
    }
    check_goto(doc.find("start"), "line", "/start", ids, "line", true, errors);
    const auto check_quest = [&](const json::Value* path, const std::string& at) {
        if (path != nullptr && !path->as_string().ends_with(kQuestSuffix)) {
            errors.push_back({at, "start_quest names a .quest.json file"});
        }
    };
    for (const auto& [id, line] : doc.find("lines")->as_object()) {
        const std::string at = "/lines/" + json::pointer_escape(id);
        check_effects(line.find("effects"), at + "/effects", true, errors);
        check_quest(line.find("start_quest"), at + "/start_quest");
        check_goto(line.find("next"), "line", at + "/next", ids, "line", true, errors);
        const json::Value* choices = line.find("choices");
        if (choices == nullptr) {
            continue;
        }
        if (line.find("next") != nullptr) {
            errors.push_back({at + "/next", "a line with choices goes on by its choices"});
        }
        const json::Array& list = choices->as_array();
        for (std::size_t c = 0; c < list.size(); ++c) {
            const std::string where = at + "/choices/" + std::to_string(c);
            check_conditions(list[c].find("when"), where + "/when", true, errors);
            check_effects(list[c].find("effects"), where + "/effects", true, errors);
            check_quest(list[c].find("start_quest"), where + "/start_quest");
            check_goto(list[c].find("next"), "line", where + "/next", ids, "line", true, errors);
        }
    }
    return errors;
}

Result<Dialogue> parse_dialogue(const json::Value& doc) {
    if (const auto errors = validate_dialogue(doc); !errors.empty()) {
        return first_error(errors);
    }
    Dialogue dialogue;
    dialogue.start = branches_of(doc.find("start"), "line");
    for (const auto& [id, item] : doc.find("lines")->as_object()) {
        Line line;
        line.id = id;
        line.speaker = string_of(item, "speaker");
        line.text = string_of(item, "text");
        line.effects = effects_of(item.find("effects"));
        line.start_quest = string_of(item, "start_quest");
        line.next = branches_of(item.find("next"), "line");
        if (const json::Value* choices = item.find("choices"); choices != nullptr) {
            for (const json::Value& choice : choices->as_array()) {
                line.choices.push_back({.text = string_of(choice, "text"),
                                        .when = conditions_of(choice.find("when")),
                                        .effects = effects_of(choice.find("effects")),
                                        .start_quest = string_of(choice, "start_quest"),
                                        .next = branches_of(choice.find("next"), "line")});
            }
        }
        dialogue.lines.push_back(std::move(line));
    }
    return dialogue;
}

} // namespace slash::npc
