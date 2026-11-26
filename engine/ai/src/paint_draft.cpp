// paint.draft: a model from providers.json writes or revises a .paint.json layer stack from a
// brief, the model's parts or the sprite it paints and the storyboard. Each draft is checked as
// paint.edit checks a stack (the schema, then parts, points and maps compiled against what it
// paints); a draft with problems goes back to the model with them, up to attempts times, and
// only a stack that checks is written, by paint.edit, which measures it.

#include "commands_internal.hpp"

#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/text.hpp"
#include "slash/procedural/paint.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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
constexpr std::size_t kMaxStory = 16;

constexpr std::string_view kInstructions =
    "You write layer stacks that texture a game's models and sprites, as Substance Painter's "
    "layer stacks do: layers bottom to top, each a material (flat values by channel, or a "
    "texture graph) laid over the ones below through a mask. Masks read the model's mesh maps "
    "(ao for crevices, curvature for edges, sky for open sky, facing for which way a face turns, "
    "streaks for runs down standing faces, noise for breakup), its parts and materials by name, "
    "points and strokes. A weather layer ages what is below by one age. Keep layers few, name "
    "them by what they are, and put numbers an artist would tune in parameters. Use only the "
    "part and material names given. Reply with the whole stack as one JSON object matching the "
    "schema, and nothing else.";

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

// What the stack paints, in words for the model: a model's parts, materials and bounds, as
// model.inspect gives them, or a sprite's size.
std::string surface_of(Session& session, const std::string& model, const std::string& sprite) {
    if (!model.empty()) {
        json::Value params;
        params["path"] = model;
        const Reply inspected = session.call("model.inspect", params);
        if (!inspected.ok) {
            return "It paints the model " + model + ".\n";
        }
        json::Array parts;
        if (const json::Value* list = inspected.value.find("parts"); list != nullptr) {
            for (const json::Value& part : list->as_array()) {
                if (const json::Value* node = part.find("node"); node != nullptr) {
                    parts.push_back(*node);
                }
            }
        }
        json::Array materials;
        if (const json::Value* list = inspected.value.find("materials"); list != nullptr) {
            for (const json::Value& material : list->as_array()) {
                if (const json::Value* name = material.find("name"); name != nullptr) {
                    materials.push_back(*name);
                }
            }
        }
        std::string out =
            "It paints the model " + model +
            ". Its parts (for part masks and named "
            "points): " +
            json::write(json::Value(std::move(parts)), json::Style::Compact) + ". Its materials: " +
            json::write(json::Value(std::move(materials)), json::Style::Compact) + ".";
        if (const json::Value* bounds = inspected.value.find("bounds"); bounds != nullptr) {
            out += " Its bounds, in its units (points, strokes and decals are placed in them): " +
                   json::write(*bounds, json::Style::Compact) + ".";
        }
        return out + "\n";
    }
    if (!sprite.empty()) {
        return "It paints the sprite " + sprite +
               ": points, strokes and decals are its pixels, y down; curvature is its outline's "
               "edge.\n";
    }
    return "";
}

Reply paint_draft(Session& session, const json::Value& params) {
    const std::string path = params.find("path")->as_string();
    if (!path.ends_with(procedural::kPaintSuffix)) {
        return failure_at(command::code::kInvalidParams, "/path",
                          "layer stacks are .paint.json files");
    }
    auto existing = session.read_project_file(path);
    if (!existing) {
        return failure_at(command::code::kInvalidParams, "/path", existing.error().message);
    }
    const std::optional<std::string> before = existing.value();
    json::Value old;
    if (before) {
        auto parsed = json::parse(*before);
        if (parsed && parsed.value().is_object()) {
            old = std::move(parsed).value();
        }
    }
    // What it paints: the params' choice, else the stack's own.
    const auto pick = [&](std::string_view key) {
        if (const json::Value* given = params.find(key); given != nullptr) {
            return given->as_string();
        }
        const json::Value* had = old.is_object() ? old.find(key) : nullptr;
        return had != nullptr && had->is_string() ? had->as_string() : std::string();
    };
    const std::string model = pick("model");
    const std::string sprite = pick("sprite");
    if (!model.empty() && !sprite.empty() && params.find("model") != nullptr &&
        params.find("sprite") != nullptr) {
        return failure_at(command::code::kInvalidParams, "/sprite",
                          "a stack paints a model or a sprite, not both");
    }

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

    std::string prompt = "The look: " + params.find("brief")->as_string() + "\n";
    prompt += surface_of(session, model, sprite);
    json::Array smart;
    for (const std::string_view name : procedural::builtin_paint_names()) {
        smart.emplace_back("builtin:paint/" + std::string(name));
    }
    prompt += "Smart materials a stack may extend: " +
              json::write(json::Value(std::move(smart)), json::Style::Compact) + "\n";
    if (!story.empty()) {
        prompt += "\nThe storyboard's entries about it:\n" +
                  json::write(json::Value(std::move(story)), json::Style::Compact) + "\n";
    }
    if (before) {
        prompt += "\nRevise this stack, keeping what the brief doesn't change:\n" + *before + "\n";
    }
    prompt += "\nThe stack's JSON Schema:\n" +
              json::write(procedural::paint_schema(), json::Style::Compact) + "\n";

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
    std::string model_name = opened.value().model;
    bool replayed = true;
    std::vector<json::ValidationError> problems;
    std::optional<Reply> written;
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
            model_name = response.value().model;
        }
        replayed = replayed && response.value().replayed;
        problems.clear();
        auto parsed = parse_output(response.value().message.text,
                                   json::Value(json::Object{{"type", "object"}}));
        if (!parsed) {
            problems = {{"", parsed.error().message}};
        } else {
            json::Value draft = std::move(parsed).value();
            draft["format"] = 1;
            draft["type"] = "paint";
            if (!model.empty()) {
                draft["model"] = model;
                draft.as_object().erase("sprite");
            } else if (!sprite.empty()) {
                draft["sprite"] = sprite;
                draft.as_object().erase("model");
            }
            problems = procedural::validate_paint(draft);
            if (problems.empty()) {
                // paint.edit checks it against what it paints, writes it and measures it; what
                // the old stack had that the draft leaves out goes.
                json::Value patch = draft;
                if (old.is_object()) {
                    for (const auto& [key, value] : old.as_object()) {
                        if (draft.find(key) == nullptr) {
                            patch[key] = json::Value();
                        }
                    }
                }
                json::Value edit;
                edit["path"] = path;
                edit["patch"] = std::move(patch);
                Reply reply = session.call("paint.edit", edit);
                if (reply.ok) {
                    written = std::move(reply);
                    break;
                }
                std::string at;
                if (const json::Value* pointer = reply.data.find("path");
                    pointer != nullptr && pointer->is_string() &&
                    pointer->as_string() != "/patch" && pointer->as_string() != "/path") {
                    at = pointer->as_string();
                }
                problems = {{at, reply.message}};
            }
        }
        // Back to the model with what is wrong.
        request.messages.push_back(std::move(response.value().message));
        request.messages.push_back(
            {.role = Role::User,
             .text = "That stack has these problems, each at its JSON Pointer:\n" +
                     json::write(errors_json(problems), json::Style::Compact) +
                     "\nReply with the whole stack, fixed."});
    }
    json::Value out;
    if (written) {
        out = std::move(written->value);
    }
    out["path"] = path;
    out["attempts"] = made;
    out["model"] = model_name;
    out["usage"] = std::move(usage);
    if (replayed) {
        out["replayed"] = true;
    }
    if (!written) {
        Reply reply =
            Reply::failure(command::code::kInvalidParams,
                           "the model's stack still has problems after " + std::to_string(made) +
                               " attempt" + (made == 1 ? "" : "s") + "; nothing was written");
        out["problems"] = errors_json(problems);
        reply.data = std::move(out);
        return reply;
    }
    return Reply::success(std::move(out));
}

} // namespace

void register_paint_draft(command::Registry& registry) {
    json::Value path = property("string", "The .paint.json file to write; one that exists is "
                                          "revised.");
    path["minLength"] = 1;
    json::Value brief = property("string", "The look: what it is, its materials, its age and "
                                           "wear.");
    brief["minLength"] = 1;
    json::Value story = property("array", "Storyboard entry IDs about it, such as "
                                          "palette/market, sent to the model whole.");
    story["items"]["type"] = "string";
    story["maxItems"] = static_cast<std::int64_t>(kMaxStory);
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
        {"model", property("string", "The model it paints. Default the stack's.")},
        {"sprite", property("string", "The sprite it paints instead. Default the stack's.")},
        {"story", std::move(story)},
        {"attempts", std::move(attempts)},
        {"role", property("string", "A role or provider from providers.json. Default: the "
                                    "\"default\" role, or the only provider.")},
        {"max_tokens", std::move(tokens)},
    };
    registry.add({
        .name = "paint.draft",
        .description = "Has a model from providers.json write or revise a .paint.json layer "
                       "stack from a brief, told the model's parts and materials (or the sprite) "
                       "and storyboard entries. Each draft is checked as paint.edit checks one and "
                       "sent back with its problems; only one that checks is written, and "
                       "measured.",
        .capabilities = {Capability::Read, Capability::Edit, Capability::Filesystem,
                         Capability::Network},
        .effect = command::Effect::Edit,
        .params = std::move(schema),
        .handler = paint_draft,
    });
}

} // namespace slash::ai::detail
