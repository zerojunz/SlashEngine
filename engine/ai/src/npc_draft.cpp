// npc.draft: a model from providers.json writes or revises an NPC's .npc.json brain from a brief
// and the storyboard, and each draft is checked as scene validate checks one (the schema, fact
// names, go_to targets the scene has, emit events the game answers and goals no plan reaches).
// A draft with problems goes back to the model with them, up to attempts times; only a brain
// that checks is written.

#include "commands_internal.hpp"

#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/text.hpp"
#include "slash/npc/npc.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ai::detail {
namespace {

using command::Capability;
using command::Reply;
using command::Session;

constexpr std::int64_t kDefaultAttempts = 2;
constexpr std::int64_t kMaxAttempts = 4;
constexpr std::size_t kMaxTargets = 64;
constexpr std::size_t kMaxStory = 16;

constexpr std::string_view kInstructions =
    "You write NPC brains for goal-oriented action planning, as in F.E.A.R.: facts the NPC "
    "knows (true, false or whole numbers), goals it wants (tried highest priority first; each "
    "applies while its when conditions hold and is done when its want conditions hold) and "
    "actions it can take (each needs some facts, changes some when it ends and costs "
    "something). A planner chains the cheapest actions from the facts to the first goal it can "
    "reach. Keep facts few and concrete. Give every goal a chain of actions that reaches it from "
    "the facts' starting values. An action's do is wait (seconds), go_to (one of the entity IDs "
    "given) or emit (an event the game's code answers by setting facts); give each emit action "
    "the effects the game's answer stands for only when it ends at once. Reply with the whole "
    "brain as one JSON object matching the schema, and nothing else.";

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

Reply failure_at(int code, const std::string& path, std::string message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(code, path + ": " + std::move(message), std::move(data));
}

std::vector<std::string> strings_of(const json::Value* list) {
    std::vector<std::string> out;
    if (list != nullptr) {
        for (const json::Value& item : list->as_array()) {
            out.push_back(item.as_string());
        }
    }
    return out;
}

// The entities a go_to may walk to by default: the open scene's with a place, in ID order.
std::vector<std::string> scene_targets(world::World& world) {
    std::vector<std::string> out;
    for (const std::string_view id : world.ids()) {
        const entt::entity entity = world.find(id);
        if (world.ecs().any_of<world::Transform2D, world::Transform3D>(entity) &&
            !world.ecs().all_of<world::NpcAgent>(entity)) {
            out.emplace_back(id);
        }
    }
    std::ranges::sort(out);
    if (out.size() > kMaxTargets) {
        out.resize(kMaxTargets);
    }
    return out;
}

// The draft's problems: what validate_brain finds, go_to targets and emit events it may not
// use, and goals no chain of actions reaches. Unused actions are only warnings.
std::vector<json::ValidationError> problems_of(const json::Value& draft,
                                               const std::vector<std::string>& targets,
                                               const std::vector<std::string>& events,
                                               std::vector<json::ValidationError>& warnings) {
    std::vector<json::ValidationError> out = npc::validate_brain(draft);
    if (!out.empty()) {
        return out;
    }
    const json::Array& actions = draft.find("actions")->as_array();
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const json::Value* behavior = actions[i].find("do");
        if (behavior == nullptr) {
            continue;
        }
        const std::string at = "/actions/" + std::to_string(i) + "/do/";
        if (const json::Value* target = behavior->find("go_to");
            target != nullptr && std::ranges::find(targets, target->as_string()) == targets.end()) {
            out.push_back({at + "go_to", "no entity \"" + target->as_string() +
                                             "\" to walk to; use one of the entity IDs given"});
        }
        if (const json::Value* event = behavior->find("emit");
            event != nullptr && !events.empty() &&
            std::ranges::find(events, event->as_string()) == events.end()) {
            out.push_back({at + "emit", "the game answers no event \"" + event->as_string() +
                                            "\"; use one of the events given"});
        }
    }
    auto brain = npc::parse_brain(draft);
    for (json::ValidationError& warning : npc::check_brain(brain.value())) {
        if (warning.path.starts_with("/goals/")) {
            out.push_back(std::move(warning));
        } else {
            warnings.push_back(std::move(warning));
        }
    }
    return out;
}

json::Value errors_json(const std::vector<json::ValidationError>& errors) {
    json::Array out;
    for (const json::ValidationError& error : errors) {
        json::Value entry;
        entry["path"] = error.path;
        entry["message"] = error.message;
        out.push_back(std::move(entry));
    }
    return out;
}

json::Value names_of(const json::Value& draft, std::string_view list) {
    json::Array out;
    for (const json::Value& item : draft.find(list)->as_array()) {
        out.push_back(*item.find("name"));
    }
    return out;
}

// The go_to targets or emit events the draft uses, sorted.
json::Value uses_of(const json::Value& draft, std::string_view kind) {
    std::set<std::string, std::less<>> used;
    for (const json::Value& action : draft.find("actions")->as_array()) {
        if (const json::Value* behavior = action.find("do"); behavior != nullptr) {
            if (const json::Value* value = behavior->find(kind); value != nullptr) {
                used.insert(value->as_string());
            }
        }
    }
    json::Array out;
    for (const std::string& name : used) {
        out.emplace_back(name);
    }
    return out;
}

Reply npc_draft(Session& session, const json::Value& params) {
    const std::string path = params.find("path")->as_string();
    if (!path.ends_with(npc::kBrainSuffix)) {
        return failure_at(command::code::kInvalidParams, "/path", "NPC brains are .npc.json files");
    }
    auto existing = session.read_project_file(path);
    if (!existing) {
        return failure_at(command::code::kInvalidParams, "/path", existing.error().message);
    }
    const std::optional<std::string> before = existing.value();
    const std::vector<std::string> targets = params.find("targets") != nullptr
                                                 ? strings_of(params.find("targets"))
                                                 : scene_targets(session.world());
    const std::vector<std::string> events = strings_of(params.find("events"));

    // The storyboard entries named, whole.
    json::Object story;
    if (const json::Value* ids = params.find("story"); ids != nullptr) {
        auto loaded = storyboard::load(
            [&session](std::string_view each) { return session.read_project_file(each); });
        if (!loaded) {
            return Reply::failure(loaded.error());
        }
        const json::Array& list = ids->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string& id = list[i].as_string();
            const auto found = loaded.value().entries.find(id);
            if (found == loaded.value().entries.end()) {
                std::vector<std::string> known;
                known.reserve(loaded.value().entries.size());
                for (const auto& entry : loaded.value().entries) {
                    known.push_back(entry.first);
                }
                std::string message = "the storyboard has no entry \"" + id + "\"";
                if (const std::string_view hint = closest_match(id, known); !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                return failure_at(command::code::kNotFound, "/story/" + std::to_string(i),
                                  std::move(message));
            }
            story[id] = found->second;
        }
    }

    std::string prompt = "The NPC: " + params.find("brief")->as_string() + "\n";
    if (!story.empty()) {
        prompt += "\nThe storyboard's entries about it:\n" +
                  json::write(json::Value(std::move(story)), json::Style::Compact) + "\n";
    }
    prompt +=
        "\nEntity IDs a go_to may walk to: " +
        (targets.empty() ? std::string("none, so use no go_to")
                         : json::write(json::Value(json::Array(targets.begin(), targets.end())),
                                       json::Style::Compact)) +
        "\n";
    if (!events.empty()) {
        prompt += "Events the game's code answers, which an emit may use: " +
                  json::write(json::Value(json::Array(events.begin(), events.end())),
                              json::Style::Compact) +
                  "\n";
    }
    if (before) {
        prompt += "\nRevise this brain, keeping what the brief doesn't change:\n" + *before + "\n";
    }
    prompt += "\nThe brain's JSON Schema:\n" +
              json::write(npc::brain_schema(), json::Style::Compact) + "\n";

    const std::string& root = session.options().project_root;
    auto config = load_config(root);
    if (!config) {
        return Reply::failure(config.error());
    }
    const json::Value* role = params.find("role");
    auto opened = open(config.value(), role != nullptr ? role->as_string() : std::string(),
                       {.project_root = root, .write_replays = session.options().write_files});
    if (!opened) {
        Reply reply = Reply::failure(opened.error());
        if (role != nullptr) {
            reply.data["path"] = "/role";
        }
        return reply;
    }
    const std::int64_t attempts =
        params.find("attempts") != nullptr ? params.find("attempts")->as_int() : kDefaultAttempts;
    Request request;
    request.messages.push_back({.role = Role::System, .text = std::string(kInstructions)});
    request.messages.push_back({.role = Role::User, .text = std::move(prompt)});
    if (const json::Value* tokens = params.find("max_tokens"); tokens != nullptr) {
        request.params.max_tokens = tokens->as_int();
    }
    json::Value usage;
    usage["input_tokens"] = 0;
    usage["output_tokens"] = 0;
    std::string model = opened.value().model;
    bool replayed = true;
    json::Value draft;
    std::vector<json::ValidationError> problems;
    std::vector<json::ValidationError> warnings;
    std::int64_t made = 0;
    while (made < attempts) {
        ++made;
        auto response = opened.value().provider->complete(request);
        if (!response) {
            return Reply::failure(response.error());
        }
        usage["input_tokens"] =
            usage.find("input_tokens")->as_int() + response.value().usage.input_tokens;
        usage["output_tokens"] =
            usage.find("output_tokens")->as_int() + response.value().usage.output_tokens;
        if (!response.value().model.empty()) {
            model = response.value().model;
        }
        replayed = replayed && response.value().replayed;
        const std::string text = response.value().message.text;
        warnings.clear();
        auto parsed = parse_output(text, json::Value(json::Object{{"type", "object"}}));
        if (parsed) {
            draft = std::move(parsed).value();
            problems = problems_of(draft, targets, events, warnings);
        } else {
            problems = {{"", parsed.error().message}};
        }
        if (problems.empty()) {
            break;
        }
        // Back to the model with what is wrong.
        request.messages.push_back(std::move(response.value().message));
        request.messages.push_back(
            {.role = Role::User,
             .text = "That brain has these problems, each at its JSON Pointer:\n" +
                     json::write(errors_json(problems), json::Style::Compact) +
                     "\nReply with the whole brain, fixed."});
    }
    json::Value out;
    out["path"] = path;
    out["attempts"] = made;
    out["model"] = model;
    out["usage"] = std::move(usage);
    if (replayed) {
        out["replayed"] = true;
    }
    if (!problems.empty()) {
        Reply reply =
            Reply::failure(command::code::kInvalidParams,
                           "the model's brain still has problems after " + std::to_string(made) +
                               " attempt" + (made == 1 ? "" : "s") + "; nothing was written");
        out["problems"] = errors_json(problems);
        reply.data = std::move(out);
        return reply;
    }
    std::string after = json::write(draft, json::Style::Pretty) + "\n";
    if (before && after == *before) {
        session.mark_unchanged();
        out["unchanged"] = true;
    } else {
        if (auto staged = session.stage_file(path, after); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(path, before, std::move(after));
    }
    out["goals"] = names_of(draft, "goals");
    out["actions"] = names_of(draft, "actions");
    out["go_to"] = uses_of(draft, "go_to");
    out["emits"] = uses_of(draft, "emit");
    if (!warnings.empty()) {
        out["warnings"] = errors_json(warnings);
    }
    return Reply::success(std::move(out));
}

} // namespace

void register_npc_draft(command::Registry& registry) {
    json::Value path = property("string", "The .npc.json file to write; one that exists is "
                                          "revised.");
    path["minLength"] = 1;
    json::Value brief = property("string", "Who the NPC is and what it should do.");
    brief["minLength"] = 1;
    json::Value story = property("array", "Storyboard entry IDs about it, such as "
                                          "character/mira, sent to the model whole.");
    story["items"]["type"] = "string";
    story["maxItems"] = static_cast<std::int64_t>(kMaxStory);
    json::Value targets = property("array", "Entity IDs its go_to actions may walk to. Default: "
                                            "the open scene's entities with a transform and no "
                                            "NpcAgent.");
    targets["items"]["type"] = "string";
    targets["maxItems"] = static_cast<std::int64_t>(kMaxTargets);
    json::Value events = property("array", "Events the game's code answers, which its emit "
                                           "actions may use. Default: any.");
    events["items"]["type"] = "string";
    json::Value attempts = property("integer", "Drafts the model may make, each after hearing "
                                               "the last one's problems. Default 2.");
    attempts["minimum"] = 1;
    attempts["maximum"] = kMaxAttempts;
    json::Value tokens = property("integer", "The most tokens the model may reply with.");
    tokens["minimum"] = 1;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"path", "brief"};
    schema["properties"] = json::Object{
        {"path", std::move(path)},
        {"brief", std::move(brief)},
        {"story", std::move(story)},
        {"targets", std::move(targets)},
        {"events", std::move(events)},
        {"attempts", std::move(attempts)},
        {"role", property("string", "A role or provider from providers.json. Default: the "
                                    "\"default\" role, or the only provider.")},
        {"max_tokens", std::move(tokens)},
    };
    registry.add({
        .name = "npc.draft",
        .description = "Has a model from providers.json write or revise an NPC's .npc.json "
                       "brain from a brief and storyboard entries. Each draft is checked (schema, "
                       "facts, go_to targets, emit events, goals no plan reaches) and sent back "
                       "with its problems; only one that checks is written.",
        .capabilities = {Capability::Read, Capability::Edit, Capability::Filesystem,
                         Capability::Network},
        .effect = command::Effect::Edit,
        .params = std::move(schema),
        .handler = npc_draft,
    });
}

} // namespace slash::ai::detail
