#include "slash/library/commands.hpp"

#include "slash/ai/config.hpp"
#include "slash/command/session.hpp"
#include "slash/library/generator.hpp"
#include "slash/library/library.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::library {
namespace {

using command::Capability;
using command::Reply;
using command::Session;

// Where a project records the files it took from the library: who made each and under what
// license.
constexpr std::string_view kProjectFile = "library.json";
// How much of a generate request's words an asset must match to come back as a candidate.
constexpr double kSimilar = 0.75;
constexpr std::size_t kMaxDescription = 300;
constexpr std::size_t kMaxCandidates = 5;
// How much of a request's words an asset must match for its feedback to go into the prompt, and
// how many reasons of each verdict go in.
constexpr double kRelated = 0.5;
constexpr std::size_t kMaxNotes = 3;
constexpr std::string_view kStyleCheck = "storyboard.check";

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value integer(std::string_view description, std::int64_t minimum, std::int64_t maximum) {
    json::Value schema = property("integer", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    return schema;
}

json::Value string_array(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "string";
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

Reply failure_at(const Error& error, std::string_view path) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(Reply::failure(error).code, std::string(path) + ": " + error.message,
                          std::move(data));
}

std::string text(const json::Value& params, std::string_view key) {
    const json::Value* member = params.find(key);
    return member != nullptr ? member->as_string() : std::string();
}

std::vector<std::string> strings(const json::Value& params, std::string_view key) {
    std::vector<std::string> out;
    if (const json::Value* member = params.find(key); member != nullptr) {
        for (const json::Value& item : member->as_array()) {
            out.push_back(item.as_string());
        }
    }
    return out;
}

std::string short_id(std::string_view id) {
    return std::string(id.substr(0, kShortId));
}

std::string trimmed(std::string description) {
    if (description.size() > kMaxDescription) {
        std::size_t cut = kMaxDescription;
        // Never in the middle of a UTF-8 character.
        while (cut > 0 && (static_cast<unsigned char>(description[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        description.resize(cut);
        description += "...";
    }
    return description;
}

Result<Library> open_library(const Session& session) {
    std::string dir = session.options().library_dir;
    if (dir.empty()) {
        dir = default_dir();
    }
    if (dir.empty()) {
        return make_error(ErrorCode::NotFound,
                          "no asset library: set SLASH_LIBRARY_DIR, or HOME for ~/.slash/library");
    }
    return Library(std::move(dir));
}

// The library writes files outside the project, which sessions that run tests never do.
Result<void> check_writes(const Session& session) {
    if (!session.options().write_files) {
        return make_error(ErrorCode::PermissionDenied,
                          "tests don't write files, so the library stays as it was");
    }
    return {};
}

// An asset as search results show it: enough to pick one without opening the file.
json::Value summary(const Record& record) {
    json::Value out;
    out["id"] = short_id(record.id);
    out["type"] = record.type;
    if (!record.description.empty()) {
        out["description"] = record.description;
    }
    if (!record.tags.empty()) {
        json::Array tags;
        for (const std::string& tag : record.tags) {
            tags.emplace_back(tag);
        }
        out["tags"] = std::move(tags);
    }
    if (record.width > 0) {
        out["width"] = record.width;
        out["height"] = record.height;
    }
    if (record.sample_rate > 0) {
        out["seconds"] = record.seconds;
    }
    int accepted = 0;
    int rejected = 0;
    for (const Feedback& item : record.feedback) {
        accepted += item.verdict == "accepted" ? 1 : 0;
        rejected += item.verdict == "rejected" ? 1 : 0;
    }
    if (accepted > 0) {
        out["accepted"] = accepted;
    }
    if (rejected > 0) {
        out["rejected"] = rejected;
    }
    return out;
}

// The project's storyboard entries, or none if it has no storyboard or it doesn't load; storyboard
// .check reports those problems.
json::Object storyboard_entries(Session& session) {
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded || !loaded.value().errors.empty()) {
        return {};
    }
    return std::move(loaded.value().entries);
}

// The file's extension must be the asset's format: a .png stays a .png.
Result<void> check_path(std::string_view path, std::string_view format) {
    const std::size_t dot = path.rfind('.');
    std::string extension(dot == std::string_view::npos ? std::string_view()
                                                        : path.substr(dot + 1));
    std::ranges::transform(extension, extension.begin(), [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    if (extension != format) {
        return make_error(ErrorCode::InvalidArgument,
                          "the asset is a ." + std::string(format) + " file; name the path so");
    }
    return {};
}

// Writes the asset into the project and records where it came from in library.json. Returns the
// bytes written.
Result<std::string> copy_into_project(Session& session, const Library& library,
                                      const Record& record, const std::string& path) {
    auto full = session.resolve_path(path);
    if (!full) {
        return full.error();
    }
    if (auto checked = check_path(path, record.format); !checked) {
        return checked.error();
    }
    auto data = library.read(record);
    if (!data) {
        return data.error();
    }
    auto list_path = session.resolve_path(kProjectFile);
    if (!list_path) {
        return list_path.error();
    }
    json::Value list;
    if (platform::exists(list_path.value())) {
        auto existing = platform::read_file(list_path.value());
        if (!existing) {
            return existing.error();
        }
        auto parsed = json::parse(existing.value());
        if (!parsed || !parsed.value().is_object()) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(kProjectFile) + " is not a JSON object");
        }
        list = std::move(parsed).value();
    }
    json::Value entry;
    entry["id"] = record.id;
    if (!record.generator.empty()) {
        entry["generator"] = record.generator;
    }
    if (!record.model.empty()) {
        entry["model"] = record.model;
    }
    if (!record.license.empty()) {
        entry["license"] = record.license;
    }
    if (!record.author.empty()) {
        entry["author"] = record.author;
    }
    list["assets"][path] = std::move(entry);
    if (auto written = session.write_file(full.value(), data.value()); !written) {
        return written.error();
    }
    if (auto written = session.write_file(list_path.value(), json::write(list)); !written) {
        return written.error();
    }
    return data;
}

// Checks an image put in the project against the storyboard's palettes and budgets that cover
// its path, and records the result as the asset's feedback: what an agent would otherwise find
// out later from storyboard.check. Returns the problems found.
json::Array check_style(Session& session, const Library& library, const Record& record,
                        const std::string& path, std::string_view data) {
    json::Array problems;
    if (record.type != "image") {
        return problems;
    }
    const json::Object entries = storyboard_entries(session);
    if (entries.empty()) {
        return problems;
    }
    bool covered = false;
    std::vector<storyboard::FileError> errors;
    storyboard::check_textures(
        entries, {path},
        [&](std::string_view /*path*/) -> std::optional<storyboard::Texture> {
            covered = true;
            auto image = render::decode_image(data);
            if (!image) {
                return std::nullopt;
            }
            return storyboard::Texture{.width = image.value().width,
                                       .height = image.value().height,
                                       .pixels = std::move(image.value().pixels)};
        },
        errors);
    if (!covered) {
        return problems;
    }
    Feedback feedback{.verdict = errors.empty() ? "accepted" : "rejected",
                      .reason = errors.empty() ? "fits the storyboard's palette and budget"
                                               : errors.front().message,
                      .source = std::string(kStyleCheck)};
    for (const storyboard::FileError& error : errors) {
        problems.emplace_back(error.message);
    }
    // The same result as last time adds nothing; sessions that run tests write no feedback.
    const bool repeat = !record.feedback.empty() &&
                        record.feedback.back().verdict == feedback.verdict &&
                        record.feedback.back().reason == feedback.reason &&
                        record.feedback.back().source == feedback.source;
    if (!repeat && session.options().write_files) {
        Record updated = record;
        add_feedback(updated, std::move(feedback));
        (void)library.update(updated);
    }
    return problems;
}

Reply asset_search(Session& session, const json::Value& params) {
    auto library = open_library(session);
    if (!library) {
        return Reply::failure(library.error());
    }
    auto records = library.value().all();
    if (!records) {
        return Reply::failure(records.error());
    }
    Query query;
    query.text = text(params, "text");
    query.tags = strings(params, "tags");
    query.type = text(params, "type");
    if (const json::Value* limit = params.find("limit"); limit != nullptr) {
        query.limit = static_cast<std::size_t>(limit->as_int());
    }
    json::Value out;
    out["assets"] = json::Array{};
    for (const Match& match : search(records.value(), query)) {
        json::Value item = summary(*match.record);
        if (!query.text.empty()) {
            item["score"] = match.score;
        }
        out["assets"].push_back(std::move(item));
    }
    return Reply::success(std::move(out));
}

struct Asked {
    GenerateRequest request;
    json::Value keyed; // the request as its key hashes it
    std::string description;
    std::vector<std::string> tags;
    std::string path;
};

Reply finish(Session& session, const Library& library, const Record& record, const Asked& asked,
             bool reused) {
    json::Value out = summary(record);
    out.as_object().erase("description");
    out.as_object().erase("tags");
    out.as_object().erase("type");
    if (!record.parent.empty()) {
        out["parent"] = short_id(record.parent);
    }
    if (reused) {
        out["reused"] = true;
    }
    if (!asked.path.empty()) {
        auto copied = copy_into_project(session, library, record, asked.path);
        if (!copied) {
            Reply reply = failure_at(copied.error(), "/path");
            // The asset is in the library all the same; asset.use puts it somewhere else.
            reply.data["id"] = short_id(record.id);
            return reply;
        }
        out["path"] = asked.path;
        if (json::Array problems =
                check_style(session, library, record, asked.path, copied.value());
            !problems.empty()) {
            out["problems"] = std::move(problems);
        }
    }
    return Reply::success(std::move(out));
}

std::string field_text(const json::Value& value, std::string_view key) {
    const json::Value* member = value.find(key);
    return member != nullptr && member->is_string() ? member->as_string() : std::string();
}

std::string joined(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (const std::string& part : parts) {
        out += (out.empty() ? "" : std::string(separator)) + part;
    }
    return out;
}

// What goes along with a prompt so results fit the game: the storyboard's tone, palette and
// references that cover the asset's path or every asset, and the reasons agents and checks gave
// for accepting or rejecting the asset being refined and assets like the one asked for.
std::string prompt_context(Session& session, std::span<const Record> records, const Asked& asked,
                           const Record* parent) {
    std::vector<std::string> lines;
    if (const json::Object entries = storyboard_entries(session); !entries.empty()) {
        std::vector<std::string> paths;
        if (!asked.path.empty()) {
            paths.push_back(asked.path);
        }
        std::vector<std::string> tones;
        std::vector<std::string> colors;
        std::vector<std::string> references;
        for (const json::Value& item : storyboard::brief(entries, {}, paths).entries) {
            const storyboard::Kind* kind = storyboard::kind_of(field_text(item, "id"));
            const json::Value* fields = item.find("fields");
            if (kind == nullptr || fields == nullptr) {
                continue;
            }
            std::string summary = field_text(*fields, "summary");
            if (summary.empty()) {
                summary = field_text(*fields, "name");
            }
            if (kind->name == "tone" && !summary.empty()) {
                tones.push_back(std::move(summary));
            } else if (kind->name == "reference" && !summary.empty()) {
                references.push_back(std::move(summary));
            } else if (const json::Value* named = fields->find("colors");
                       kind->name == "palette" && named != nullptr && named->is_object()) {
                for (const auto& [name, color] : named->as_object()) {
                    colors.push_back(name + " " + (color.is_string() ? color.as_string() : ""));
                }
            }
        }
        if (!tones.empty()) {
            lines.push_back("Style: " + joined(tones, "; ") + ".");
        }
        if (asked.request.type == "image" && !colors.empty()) {
            lines.push_back("Use only these colors: " + joined(colors, ", ") + ".");
        }
        if (!references.empty()) {
            lines.push_back("Follow: " + joined(references, "; ") + ".");
        }
    }

    std::vector<std::string> worked;
    std::vector<std::string> wrong;
    const auto take = [&worked, &wrong](const Record& record) {
        for (const Feedback& item : std::ranges::reverse_view(record.feedback)) {
            std::string reason = item.reason;
            while (!reason.empty() && (reason.back() == '.' || reason.back() == ' ')) {
                reason.pop_back();
            }
            // A check passing says nothing a prompt can use.
            const bool accepted = item.verdict == "accepted";
            if (reason.empty() || (accepted && item.source == kStyleCheck)) {
                continue;
            }
            std::vector<std::string>& list = accepted ? worked : wrong;
            if (list.size() < kMaxNotes && std::ranges::find(list, reason) == list.end()) {
                list.push_back(std::move(reason));
            }
        }
    };
    if (parent != nullptr) {
        take(*parent);
    }
    for (const Match& match : search(records, {.text = asked.request.prompt,
                                               .tags = {},
                                               .type = asked.request.type,
                                               .limit = kMaxCandidates})) {
        if (match.score >= kRelated && (parent == nullptr || match.record->id != parent->id)) {
            take(*match.record);
        }
    }
    if (!worked.empty()) {
        lines.push_back("What worked before: " + joined(worked, "; ") + ".");
    }
    if (!wrong.empty()) {
        lines.push_back("Avoid what was wrong before: " + joined(wrong, "; ") + ".");
    }
    return joined(lines, "\n");
}

Reply run_generator(Session& session, const json::Value& params, Asked asked,
                    const Record* parent) {
    auto library = open_library(session);
    if (!library) {
        return Reply::failure(library.error());
    }
    if (auto writes = check_writes(session); !writes) {
        return Reply::failure(writes.error());
    }
    auto config = ai::load_config(session.options().project_root);
    if (!config) {
        return Reply::failure(config.error());
    }
    std::string name = text(params, "generator");
    if (name.empty() && parent != nullptr && parent->generator == "test") {
        name = "test";
    }
    auto opened = open_generator(config.value(), name, asked.request.type);
    if (!opened) {
        return params.find("generator") != nullptr ? failure_at(opened.error(), "/generator")
                                                   : Reply::failure(opened.error());
    }
    const OpenedGenerator& generator = opened.value();
    const GeneratorCapabilities& can = generator.generator->capabilities();
    if (parent != nullptr &&
        !(asked.request.type == "image" ? can.refines_images : can.refines_sounds)) {
        return Reply::failure(command::code::kInvalidArgument, "the generator \"" + generator.name +
                                                                   "\" can't refine " +
                                                                   asked.request.type + "s");
    }

    const std::string key = request_key(generator.type, generator.model, asked.keyed);
    if (const std::optional<std::string> made = library.value().find_request(key); made) {
        auto record = library.value().get(*made);
        if (record) {
            return finish(session, library.value(), record.value(), asked, true);
        }
    }
    auto records = library.value().all();
    if (!records) {
        return Reply::failure(records.error());
    }
    const json::Value* similar = params.find("similar");
    if (parent == nullptr && (similar == nullptr || similar->as_bool())) {
        json::Array candidates;
        for (const Match& match : search(records.value(), {.text = asked.request.prompt,
                                                           .tags = {},
                                                           .type = asked.request.type,
                                                           .limit = kMaxCandidates})) {
            // One agents rejected more than accepted is what they didn't want.
            if (match.score >= kSimilar && net_feedback(*match.record) >= 0) {
                json::Value item = summary(*match.record);
                item["score"] = match.score;
                candidates.push_back(std::move(item));
            }
        }
        if (!candidates.empty()) {
            json::Value out;
            out["candidates"] = std::move(candidates);
            return Reply::success(std::move(out));
        }
    }

    std::string context;
    if (const json::Value* with = params.find("context"); with == nullptr || with->as_bool()) {
        context = prompt_context(session, records.value(), asked, parent);
    }
    if (!context.empty()) {
        asked.request.prompt += "\n\n" + context;
    }
    auto generated = generator.generator->generate(asked.request);
    if (!generated) {
        return Reply::failure(generated.error());
    }
    Record record;
    record.description = trimmed(std::move(asked.description));
    record.tags = asked.tags;
    record.request = asked.keyed;
    if (!context.empty()) {
        record.request["context"] = context;
    }
    record.generator = generator.type;
    record.model = generated.value().model.empty() ? generator.model : generated.value().model;
    record.seed = generated.value().seed;
    record.license = generator.license;
    if (parent != nullptr) {
        record.parent = parent->id;
    }
    const std::string& data = generated.value().data;
    if (auto described = describe_file(data, "generated." + generated.value().format, record);
        !described) {
        return Reply::failure(
            command::code::kPlatformFailure,
            "the generator \"" + generator.name +
                "\" returned a file the library can't read: " + described.error().message);
    }
    auto stored = library.value().put(data, std::move(record));
    if (!stored) {
        return Reply::failure(stored.error());
    }
    if (auto remembered = library.value().remember_request(key, stored.value().id); !remembered) {
        return Reply::failure(remembered.error());
    }
    return finish(session, library.value(), stored.value(), asked, false);
}

// The params generate and refine share: size or length, seed, description, tags and path.
Reply read_shared(const json::Value& params, Asked& asked) {
    GenerateRequest& request = asked.request;
    if (const json::Value* width = params.find("width"); width != nullptr) {
        request.width = static_cast<int>(width->as_int());
        asked.keyed["width"] = width->as_int();
    }
    if (const json::Value* height = params.find("height"); height != nullptr) {
        request.height = static_cast<int>(height->as_int());
        asked.keyed["height"] = height->as_int();
    }
    if (const json::Value* seconds = params.find("seconds"); seconds != nullptr) {
        request.seconds = seconds->as_double();
        asked.keyed["seconds"] = seconds->as_double();
    }
    const bool sized = request.width > 0 || request.height > 0;
    if (request.type == "image" && request.seconds > 0.0) {
        return Reply::failure(command::code::kInvalidParams,
                              "/seconds: images have a width and height, not seconds",
                              json::Value(json::Object{{"path", "/seconds"}}));
    }
    if (request.type == "sound" && sized) {
        return Reply::failure(command::code::kInvalidParams,
                              "/width: sounds have seconds, not a width and height",
                              json::Value(json::Object{{"path", "/width"}}));
    }
    if (const json::Value* seed = params.find("seed"); seed != nullptr) {
        request.seed = seed->as_int();
        asked.keyed["seed"] = seed->as_int();
    }
    asked.description = text(params, "description");
    asked.tags = strings(params, "tags");
    asked.path = text(params, "path");
    if (!asked.path.empty()) {
        const std::size_t dot = asked.path.rfind('.');
        const std::string_view type =
            type_of(dot == std::string::npos ? std::string_view() : asked.path.substr(dot));
        if (type != request.type) {
            return Reply::failure(
                command::code::kInvalidParams,
                "/path: \"" + asked.path + "\" is not " +
                    (request.type == "image" ? "an image (.png)" : "a sound (.wav, .ogg or .mp3)"),
                json::Value(json::Object{{"path", "/path"}}));
        }
    }
    return Reply::success({});
}

Reply asset_generate(Session& session, const json::Value& params) {
    Asked asked;
    asked.request.prompt = text(params, "prompt");
    asked.request.type = params.find("type") != nullptr ? text(params, "type") : "image";
    asked.keyed["type"] = asked.request.type;
    asked.keyed["prompt"] = asked.request.prompt;
    if (Reply shared = read_shared(params, asked); !shared.ok) {
        return shared;
    }
    if (asked.description.empty()) {
        asked.description = asked.request.prompt;
    }
    return run_generator(session, params, std::move(asked), nullptr);
}

Reply asset_refine(Session& session, const json::Value& params) {
    auto library = open_library(session);
    if (!library) {
        return Reply::failure(library.error());
    }
    auto parent = library.value().get(text(params, "id"));
    if (!parent) {
        return failure_at(parent.error(), "/id");
    }
    auto data = library.value().read(parent.value());
    if (!data) {
        return Reply::failure(data.error());
    }
    Asked asked;
    asked.request.type = parent.value().type;
    asked.request.prompt = text(params, "prompt");
    asked.request.parent = &parent.value();
    asked.request.parent_data = std::move(data).value();
    asked.keyed["type"] = asked.request.type;
    asked.keyed["prompt"] = asked.request.prompt;
    asked.keyed["parent"] = parent.value().id;
    if (Reply shared = read_shared(params, asked); !shared.ok) {
        return shared;
    }
    if (asked.description.empty()) {
        asked.description = parent.value().description.empty()
                                ? asked.request.prompt
                                : parent.value().description + "; " + asked.request.prompt;
    }
    if (params.find("tags") == nullptr) {
        asked.tags = parent.value().tags;
    }
    return run_generator(session, params, std::move(asked), &parent.value());
}

Reply asset_info(Session& session, const json::Value& params) {
    auto library = open_library(session);
    if (!library) {
        return Reply::failure(library.error());
    }
    auto record = library.value().get(text(params, "id"));
    if (!record) {
        return failure_at(record.error(), "/id");
    }
    json::Value out = to_json(record.value());
    out["id"] = short_id(record.value().id);
    if (json::Value* request = out.find("request"); request != nullptr) {
        if (const json::Value* parent = request->find("parent"); parent != nullptr) {
            (*request)["parent"] = short_id(parent->as_string());
        }
    }
    if (!record.value().parent.empty()) {
        out["parent"] = short_id(record.value().parent);
        // Every version before this one, oldest first.
        json::Array versions;
        std::string at = record.value().parent;
        while (!at.empty() && versions.size() < 100) {
            versions.insert(versions.begin(), json::Value(short_id(at)));
            auto earlier = library.value().get(at);
            at = earlier ? earlier.value().parent : std::string();
        }
        out["versions"] = std::move(versions);
    }
    auto records = library.value().all();
    if (!records) {
        return Reply::failure(records.error());
    }
    json::Array children;
    for (const Record& other : records.value()) {
        if (other.parent == record.value().id) {
            children.emplace_back(short_id(other.id));
        }
    }
    if (!children.empty()) {
        out["children"] = std::move(children);
    }
    return Reply::success(std::move(out));
}

Reply asset_use(Session& session, const json::Value& params) {
    auto library = open_library(session);
    if (!library) {
        return Reply::failure(library.error());
    }
    auto record = library.value().get(text(params, "id"));
    if (!record) {
        return failure_at(record.error(), "/id");
    }
    const std::string path = text(params, "path");
    auto copied = copy_into_project(session, library.value(), record.value(), path);
    if (!copied) {
        return failure_at(copied.error(), "/path");
    }
    json::Value out;
    out["path"] = path;
    if (json::Array problems =
            check_style(session, library.value(), record.value(), path, copied.value());
        !problems.empty()) {
        out["problems"] = std::move(problems);
    }
    return Reply::success(std::move(out));
}

Reply asset_add(Session& session, const json::Value& params) {
    auto library = open_library(session);
    if (!library) {
        return Reply::failure(library.error());
    }
    if (auto writes = check_writes(session); !writes) {
        return Reply::failure(writes.error());
    }
    const std::string path = text(params, "path");
    auto data = platform::read_file_in(session.options().project_root, path);
    if (!data) {
        return failure_at(data.error(), "/path");
    }
    Record record;
    if (auto described = describe_file(data.value(), path, record); !described) {
        return failure_at(described.error(), "/path");
    }
    record.description = trimmed(text(params, "description"));
    record.tags = strings(params, "tags");
    record.license = text(params, "license");
    record.author = text(params, "author");
    auto stored = library.value().put(data.value(), std::move(record));
    if (!stored) {
        return Reply::failure(stored.error());
    }
    if (auto copied = copy_into_project(session, library.value(), stored.value(), path); !copied) {
        return failure_at(copied.error(), "/path");
    }
    json::Value out = summary(stored.value());
    out.as_object().erase("description");
    out.as_object().erase("tags");
    return Reply::success(std::move(out));
}

Reply asset_feedback(Session& session, const json::Value& params) {
    auto library = open_library(session);
    if (!library) {
        return Reply::failure(library.error());
    }
    if (auto writes = check_writes(session); !writes) {
        return Reply::failure(writes.error());
    }
    auto record = library.value().get(text(params, "id"));
    if (!record) {
        return failure_at(record.error(), "/id");
    }
    std::string source = text(params, "source");
    add_feedback(record.value(), {.verdict = text(params, "verdict"),
                                  .reason = trimmed(text(params, "reason")),
                                  .source = source.empty() ? "agent" : std::move(source)});
    if (auto updated = library.value().update(record.value()); !updated) {
        return Reply::failure(updated.error());
    }
    json::Value out = summary(record.value());
    for (const std::string_view key :
         {"description", "tags", "type", "width", "height", "seconds"}) {
        out.as_object().erase(std::string(key));
    }
    return Reply::success(std::move(out));
}

json::Value type_property(std::string_view description) {
    json::Value type = property("string", description);
    type["enum"] = json::Array{"image", "sound"};
    return type;
}

// The params generate and refine share.
json::Object shared_properties() {
    json::Value seconds = property("number", "A sound's length.");
    seconds["exclusiveMinimum"] = 0;
    seconds["maximum"] = 600;
    return {
        {"generator", property("string", "A generator in providers.json, or \"test\" for "
                                         "placeholders.")},
        {"width", integer("Image width in pixels.", 1, 4096)},
        {"height", integer("Image height in pixels.", 1, 4096)},
        {"seconds", std::move(seconds)},
        {"seed", property("integer", "For a repeatable result, where the generator takes one.")},
        {"description", property("string", "A sentence for agents to read instead of the "
                                           "file.")},
        {"tags", string_array("Words to find it by.")},
        {"path", property("string", "Also copy it into the project here; images are checked "
                                    "against the storyboard's palette and budget.")},
        {"context", property("boolean", "false: send the prompt alone, without the "
                                        "storyboard's style and notes from earlier feedback.")},
    };
}

} // namespace

void register_commands(command::Registry& registry) {
    json::Value limit = integer("Most results; default 10.", 1, 50);
    registry.add({
        .name = "asset.search",
        .description = "Searches the machine's asset library (generated and added images and "
                       "sounds, shared by every project) by words, tags and type. Returns IDs "
                       "with descriptions, best first. Search before generating.",
        .capabilities = {Capability::Read},
        .effect = command::Effect::Read,
        .params = params_schema({
            {"text", property("string", "Words to find in descriptions, prompts and tags.")},
            {"tags", string_array("Tags every result must have.")},
            {"type", type_property("Only images or only sounds.")},
            {"limit", std::move(limit)},
        }),
        .handler = asset_search,
    });

    json::Object generate = shared_properties();
    generate.emplace("prompt", property("string", "What to make."));
    generate.emplace("type", type_property("Default image."));
    generate.emplace("similar", property("boolean", "false: generate even when the library has "
                                                    "close matches."));
    registry.add({
        .name = "asset.generate",
        .description = "Makes an image or sound with a generator and keeps it in the asset "
                       "library. An exact repeat returns the stored asset with no model call; "
                       "close matches come back as candidates instead of generating.",
        .capabilities = {Capability::Read, Capability::Filesystem, Capability::Network},
        .effect = command::Effect::Read,
        .params = params_schema(std::move(generate), {"prompt"}),
        .handler = asset_generate,
    });

    json::Object refine = shared_properties();
    refine.emplace("id", property("string", "The asset to change."));
    refine.emplace("prompt", property("string", "What to change."));
    registry.add({
        .name = "asset.refine",
        .description = "Makes a new version of a library asset, changed as the prompt says and "
                       "linked to it, so earlier versions stay to compare and go back to.",
        .capabilities = {Capability::Read, Capability::Filesystem, Capability::Network},
        .effect = command::Effect::Read,
        .params = params_schema(std::move(refine), {"id", "prompt"}),
        .handler = asset_refine,
    });

    registry.add({
        .name = "asset.info",
        .description = "Returns a library asset's record: how it was made (request, generator, "
                       "model, seed), license, size or length, and its versions.",
        .capabilities = {Capability::Read},
        .effect = command::Effect::Read,
        .params =
            params_schema({{"id", property("string", "Its ID, or the start of it.")}}, {"id"}),
        .handler = asset_info,
    });

    registry.add({
        .name = "asset.use",
        .description = "Copies a library asset into the project and records its source and "
                       "license in library.json.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema({{"id", property("string", "Its ID, or the start of it.")},
                                 {"path", property("string", "Where in the project.")}},
                                {"id", "path"}),
        .handler = asset_use,
    });

    json::Value verdict = property("string", "Whether the asset did the job.");
    verdict["enum"] = json::Array{"accepted", "rejected"};
    registry.add({
        .name = "asset.feedback",
        .description = "Records whether a library asset worked and why. Feedback ranks search "
                       "results, keeps rejected assets out of candidates and goes into later "
                       "prompts.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema(
            {
                {"id", property("string", "Its ID, or the start of it.")},
                {"verdict", std::move(verdict)},
                {"reason", property("string", "What worked or what was wrong, in a few words.")},
                {"source", property("string", "Who judged it; default \"agent\".")},
            },
            {"id", "verdict"}),
        .handler = asset_feedback,
    });

    registry.add({
        .name = "asset.add",
        .description = "Adds a project's image or sound file to the asset library, with its "
                       "author and license, so other projects find it.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema(
            {
                {"path", property("string", "A .png, .bmp, .wav, .ogg or .mp3 file.")},
                {"description", property("string", "A sentence for agents to read instead of "
                                                   "the file.")},
                {"tags", string_array("Words to find it by.")},
                {"license", property("string", "Its license, such as CC0-1.0.")},
                {"author", property("string", "Who made it.")},
            },
            {"path", "description"}),
        .handler = asset_add,
    });
}

} // namespace slash::library
