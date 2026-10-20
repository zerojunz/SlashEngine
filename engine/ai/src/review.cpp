// storyboard.review: a model reads new text and images against the storyboard and reports what
// likely contradicts it. Its findings are advisory, so each must quote the material and name a
// real entry; findings that don't are dropped and counted, never passed on as facts.
#include "commands_internal.hpp"

#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/text.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

// A file's text is reviewed in one request, so a longer one is split by the caller.
constexpr std::size_t kMaxTextBytes = std::size_t{64} * 1024;

constexpr std::string_view kInstructions =
    "You check new material for a game (dialogue, interface text, descriptions, images) against "
    "its storyboard: the facts its makers set down about its world, story, style and rules. "
    "Report only where the material contradicts a fact: a wrong name, relationship, event, "
    "number or rule, or a tone or style the storyboard rules out. Quote the material's exact "
    "words (for an image, say what in it is wrong) and give the ID of the entry it contradicts. "
    "Do not report what the storyboard does not mention, matters of taste, or spelling. If "
    "nothing contradicts the storyboard, return no findings.";

// What is reviewed: a piece of text, or an image, with where it came from.
struct Item {
    std::string source;  // "text", a project path, or the storyboard file of an entry
    std::string pointer; // JSON Pointer inside the source, or ""
    std::string text;
    std::string media_type; // an image's, or "" for text
};

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value string_array(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "string";
    return schema;
}

Reply failure_at(const Error& error, std::string path) {
    Reply reply = Reply::failure(error);
    reply.data["path"] = std::move(path);
    return reply;
}

bool ends_with(std::string_view text, std::string_view end) {
    return text.size() >= end.size() && text.substr(text.size() - end.size()) == end;
}

// Lowercase, with each run of spaces and line breaks one space, for finding a quote.
std::string normalized(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out += ' ';
            space = false;
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Whether a quote appears in the text, ignoring case, spacing and quotation marks around it.
bool quotes(std::string_view text, std::string_view quote) {
    std::string wanted = normalized(quote);
    const auto is_mark = [](char c) { return c == '"' || c == '\'' || c == '`'; };
    while (!wanted.empty() && is_mark(wanted.front())) {
        wanted.erase(wanted.begin());
    }
    while (!wanted.empty() && is_mark(wanted.back())) {
        wanted.pop_back();
    }
    return !wanted.empty() && normalized(text).find(wanted) != std::string::npos;
}

// Whether a name appears in the text as a whole word, ignoring case.
bool mentions(std::string_view lowered_text, std::string_view name) {
    const std::string wanted = normalized(name);
    if (wanted.size() < 3) {
        return false;
    }
    const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (std::size_t at = lowered_text.find(wanted); at != std::string_view::npos;
         at = lowered_text.find(wanted, at + 1)) {
        const std::size_t end = at + wanted.size();
        if ((at == 0 || !word(lowered_text[at - 1])) &&
            (end == lowered_text.size() || !word(lowered_text[end]))) {
            return true;
        }
    }
    return false;
}

// Player-facing text in a scene or prefab: each control's text, placeholder and tooltip,
// including those in lists such as a menu's items.
void scene_text(const json::Value& value, const std::string& pointer, std::string_view source,
                std::vector<Item>& items) {
    constexpr std::array<std::string_view, 3> kTextFields = {"text", "placeholder_text",
                                                             "tooltip_text"};
    if (value.is_object()) {
        for (const auto& [key, member] : value.as_object()) {
            const std::string at = pointer + "/" + json::pointer_escape(key);
            if (member.is_string() && !member.as_string().empty() &&
                std::ranges::find(kTextFields, key) != kTextFields.end()) {
                items.push_back({std::string(source), at, member.as_string(), ""});
            } else {
                scene_text(member, at, source, items);
            }
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            scene_text(value.as_array()[i], pointer + "/" + std::to_string(i), source, items);
        }
    }
}

// Sentences in any other JSON file: strings with a space in them.
void json_text(const json::Value& value, const std::string& pointer, std::string_view source,
               std::vector<Item>& items) {
    if (value.is_string()) {
        if (value.as_string().find(' ') != std::string::npos) {
            items.push_back({std::string(source), pointer, value.as_string(), ""});
        }
    } else if (value.is_object()) {
        for (const auto& [key, member] : value.as_object()) {
            json_text(member, pointer + "/" + json::pointer_escape(key), source, items);
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            json_text(value.as_array()[i], pointer + "/" + std::to_string(i), source, items);
        }
    }
}

// Reads a path into items, or fails. Scenes also give the entries their entities stand for.
std::optional<Reply> read_path(Session& session, const std::string& path, const std::string& at,
                               std::vector<Item>& items, std::vector<std::string>& ids) {
    if (!is_project_path(path)) {
        return failure_at(make_error(ErrorCode::InvalidArgument,
                                     "paths are relative to the project folder, separated by "
                                     "'/', with no \".\" or \"..\" parts; got \"" +
                                         path + "\""),
                          at);
    }
    const bool scene = ends_with(path, ".scene.json") || ends_with(path, ".prefab.json");
    json::Value document;
    std::string text;
    if (scene && path == session.scene_path()) {
        document = session.save().scene; // with the edits not saved yet
    } else {
        auto read = session.read_project_file(path);
        if (!read) {
            return failure_at(read.error(), at);
        }
        std::optional<std::string>& contents = read.value();
        if (!contents) {
            return failure_at(make_error(ErrorCode::NotFound, "no file \"" + path + "\""), at);
        }
        text = std::move(*contents);
    }
    if (const std::string_view type = media_type(path); !type.empty()) {
        items.push_back({path, "", std::move(text), std::string(type)});
        return std::nullopt;
    }
    if (text.find('\0') != std::string::npos) {
        return failure_at(
            make_error(ErrorCode::InvalidArgument, "\"" + path + "\" is not text or an image"), at);
    }
    if (scene || ends_with(path, ".json")) {
        if (document.is_null()) {
            auto parsed = json::parse(text);
            if (!parsed) {
                return failure_at(
                    make_error(ErrorCode::InvalidArgument,
                               "\"" + path + "\" is not valid JSON: " + parsed.error().message),
                    at);
            }
            document = std::move(parsed).value();
        }
        if (scene) {
            for (const storyboard::StoryRefId& ref : storyboard::story_refs(document)) {
                ids.push_back(ref.id);
            }
            scene_text(document, "", path, items);
        } else {
            json_text(document, "", path, items);
        }
        return std::nullopt;
    }
    if (text.size() > kMaxTextBytes) {
        return failure_at(make_error(ErrorCode::InvalidArgument,
                                     "\"" + path +
                                         "\" is longer than 64 KiB; review it in parts "
                                         "with text"),
                          at);
    }
    items.push_back({path, "", std::move(text), ""});
    return std::nullopt;
}

json::Value output_schema() {
    json::Value finding;
    finding["type"] = "object";
    finding["additionalProperties"] = false;
    finding["required"] = json::Array{"item", "quote", "entry", "problem", "confidence"};
    json::Value& properties = finding["properties"];
    properties["item"] = property("integer", "The number of the material it is about.");
    properties["quote"] =
        property("string", "The material's exact words; for an image, what in it is wrong.");
    properties["entry"] = property("string", "The ID of the storyboard entry it contradicts.");
    properties["problem"] = property("string", "What contradicts what, in one sentence.");
    properties["confidence"]["enum"] = json::Array{"high", "medium", "low"};
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"findings"};
    schema["properties"]["findings"]["type"] = "array";
    schema["properties"]["findings"]["items"] = std::move(finding);
    return schema;
}

// An entry as the model reads it: its ID and fields, without the version.
json::Array facts_of(const json::Array& brief, const std::set<std::string, std::less<>>& skip) {
    json::Array out;
    for (const json::Value& entry : brief) {
        const std::string& id = entry.find("id")->as_string();
        if (skip.contains(id)) {
            continue;
        }
        json::Value fact;
        fact["id"] = id;
        fact["fields"] = *entry.find("fields");
        out.push_back(std::move(fact));
    }
    return out;
}

Reply storyboard_review(Session& session, const json::Value& params) {
    const json::Value* text = params.find("text");
    const json::Value* paths = params.find("paths");
    const json::Value* reviewed_entries = params.find("entries");
    if (text == nullptr && paths == nullptr && reviewed_entries == nullptr) {
        return Reply::failure(command::code::kInvalidParams,
                              "storyboard.review needs text, paths or entries to review");
    }
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded) {
        return Reply::failure(loaded.error());
    }
    if (!loaded.value().errors.empty()) {
        const storyboard::FileError& first = loaded.value().errors.front();
        return Reply::failure(command::code::kInvalidParams, first.file + " at " + first.path +
                                                                 ": " + first.message +
                                                                 "; run storyboard.check");
    }
    const json::Object& entries = loaded.value().entries;

    std::vector<Item> items;
    std::vector<std::string> ids;       // entries the material touches
    std::vector<std::string> file_list; // paths it is in
    std::set<std::string, std::less<>> reviewed;
    if (text != nullptr) {
        items.push_back({"text", "", text->as_string(), ""});
    }
    if (paths != nullptr) {
        for (std::size_t i = 0; i < paths->size(); ++i) {
            const std::string& path = paths->as_array()[i].as_string();
            if (auto failed = read_path(session, path, "/paths/" + std::to_string(i), items, ids)) {
                return std::move(*failed);
            }
            file_list.push_back(path);
        }
    }
    if (reviewed_entries != nullptr) {
        for (std::size_t i = 0; i < reviewed_entries->size(); ++i) {
            const std::string& id = reviewed_entries->as_array()[i].as_string();
            const auto found = entries.find(id);
            if (found == entries.end()) {
                const std::string message = storyboard::id_error(id);
                return failure_at(
                    make_error(ErrorCode::NotFound,
                               message.empty() ? storyboard::unknown_entry(entries, id) : message),
                    "/entries/" + std::to_string(i));
            }
            reviewed.insert(id);
            const storyboard::Kind& kind = *storyboard::kind_of(id);
            const std::string source = storyboard::file_path(kind.file);
            for (const auto& [field, value] : found->second.as_object()) {
                if (value.is_string()) {
                    items.push_back(
                        {source,
                         "/entries/" + json::pointer_escape(id) + "/" + json::pointer_escape(field),
                         value.as_string(), ""});
                }
            }
            for (const storyboard::Reference& reference :
                 storyboard::references(kind, found->second)) {
                ids.push_back(reference.target);
            }
        }
    }
    if (items.empty()) {
        return Reply::failure(command::code::kInvalidParams,
                              "there is no text or image to review in what was given");
    }

    // Entries the material names, by name or alias.
    std::string lowered;
    for (const Item& item : items) {
        if (item.media_type.empty()) {
            lowered += normalized(item.text);
            lowered += '\n';
        }
    }
    for (const auto& [id, entry] : entries) {
        std::vector<std::string_view> names;
        if (const json::Value* name = entry.find("name"); name != nullptr && name->is_string()) {
            names.push_back(name->as_string());
        }
        if (const json::Value* aliases = entry.find("aliases");
            aliases != nullptr && aliases->is_array()) {
            for (const json::Value& alias : aliases->as_array()) {
                if (alias.is_string()) {
                    names.push_back(alias.as_string());
                }
            }
        }
        if (std::ranges::any_of(names,
                                [&](std::string_view name) { return mentions(lowered, name); })) {
            ids.push_back(id);
        }
    }
    const storyboard::Brief brief = storyboard::brief(entries, ids, file_list);
    const json::Array facts = facts_of(brief.entries, reviewed);
    const json::Array related = facts_of(brief.related, reviewed);
    if (facts.empty() && related.empty()) {
        json::Value out;
        out["advisory"] = true;
        out["findings"] = json::Array{};
        out["facts"] = 0;
        out["items"] = static_cast<std::int64_t>(items.size());
        out["note"] = "no storyboard entries apply to this material, so no model was asked";
        return Reply::success(std::move(out));
    }

    std::string prompt = "Storyboard facts:\n" + json::write(facts, json::Style::Compact) + "\n";
    if (!related.empty()) {
        prompt += "\nEntries those name:\n" + json::write(related, json::Style::Compact) + "\n";
    }
    prompt += "\nNew material to check:\n";
    Message user{.role = Role::User};
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        prompt += "\n[" + std::to_string(i + 1) + "] " + item.source;
        if (!item.pointer.empty()) {
            prompt += " " + item.pointer;
        }
        if (item.media_type.empty()) {
            prompt += "\n" + item.text + "\n";
        } else {
            prompt += " (image " + std::to_string(user.images.size() + 1) + ")\n";
            user.images.push_back({item.media_type, item.text});
        }
    }
    user.text = std::move(prompt);
    Request request;
    request.messages.push_back({.role = Role::System, .text = std::string(kInstructions)});
    request.messages.push_back(std::move(user));
    request.output_schema = output_schema();
    if (const json::Value* tokens = params.find("max_tokens"); tokens != nullptr) {
        request.params.max_tokens = tokens->as_int();
    }

    const std::string& root = session.options().project_root;
    auto config = load_config(root);
    if (!config) {
        return Reply::failure(config.error());
    }
    const json::Value* role = params.find("role");
    auto opened = open(config.value(), role != nullptr ? role->as_string() : std::string(),
                       {.project_root = root, .write_replays = session.options().write_files});
    if (!opened) {
        return role != nullptr ? failure_at(opened.error(), "/role")
                               : Reply::failure(opened.error());
    }
    auto response = opened.value().provider->complete(request);
    if (!response) {
        return Reply::failure(response.error());
    }

    // HTTP providers check the reply against the schema, but a game's own provider may not.
    const json::Value& output = response.value().output;
    if (const std::vector<json::ValidationError> errors =
            json::validate(output, request.output_schema);
        !errors.empty()) {
        return Reply::failure(make_error(ErrorCode::InvalidArgument,
                                         "the model's reply does not list findings as asked: " +
                                             errors.front().message));
    }
    json::Array findings;
    std::int64_t unverified = 0;
    for (const json::Value& finding : output.find("findings")->as_array()) {
        const std::int64_t number = finding.find("item")->as_int();
        const std::string& entry = finding.find("entry")->as_string();
        const std::string& quote = finding.find("quote")->as_string();
        if (number < 1 || std::cmp_greater(number, items.size()) || !entries.contains(entry)) {
            ++unverified;
            continue;
        }
        const Item& item = items[static_cast<std::size_t>(number) - 1];
        if (item.media_type.empty() && !quotes(item.text, quote)) {
            ++unverified;
            continue;
        }
        json::Value out;
        out["source"] = item.source;
        if (!item.pointer.empty()) {
            out["path"] = item.pointer;
        }
        out["quote"] = quote;
        out["entry"] = entry;
        out["problem"] = *finding.find("problem");
        out["confidence"] = *finding.find("confidence");
        findings.push_back(std::move(out));
    }
    json::Value out;
    out["advisory"] = true;
    out["findings"] = std::move(findings);
    if (unverified > 0) {
        out["unverified"] = unverified;
    }
    out["facts"] = static_cast<std::int64_t>(facts.size() + related.size());
    out["items"] = static_cast<std::int64_t>(items.size());
    out["model"] = response.value().model.empty() ? opened.value().model : response.value().model;
    out["usage"]["input_tokens"] = response.value().usage.input_tokens;
    out["usage"]["output_tokens"] = response.value().usage.output_tokens;
    if (response.value().replayed) {
        out["replayed"] = true;
    }
    return Reply::success(std::move(out));
}

} // namespace

void register_review(command::Registry& registry) {
    json::Value tokens = property("integer", "The most tokens the model may reply with.");
    tokens["minimum"] = 1;
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{
        {"text", property("string", "New text to review, such as a line of dialogue.")},
        {"paths", string_array("Project files to review: scenes and prefabs (their controls' "
                               "text), text files, other JSON files (their sentences) and "
                               "images (.png, .jpg, .gif, .webp) for a model that takes them.")},
        {"entries", string_array("Storyboard entries to review against the rest, such as ones "
                                 "just written.")},
        {"role", property("string", "A role or provider from providers.json. Default: the "
                                    "\"default\" role, or the only provider.")},
        {"max_tokens", std::move(tokens)},
    };
    registry.add({
        .name = "storyboard.review",
        .description =
            "Asks a model from providers.json whether new text or images contradict the "
            "storyboard. Returns advisory findings, each quoting the material and naming the "
            "entry it contradicts; findings that quote nothing real are dropped and counted.",
        .capabilities = {Capability::Read, Capability::Network},
        .effect = command::Effect::Read,
        .params = std::move(schema),
        .handler = storyboard_review,
    });
}

} // namespace slash::ai::detail
