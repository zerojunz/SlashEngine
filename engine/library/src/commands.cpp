#include "slash/library/commands.hpp"

#include "slash/ai/config.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/sha256.hpp"
#include "slash/library/generator.hpp"
#include "slash/library/library.hpp"
#include "slash/library/team.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
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
// asset.check: how the game shows the project's library assets.
constexpr std::string_view kFrameCheck = "asset.check";
// An image drawn at this share of its size or less would look the same as a smaller file.
constexpr float kOversized = 0.25F;
constexpr std::int64_t kMaxCheckTicks = 600;
// How many files deep asset.check follows what a scene names to find the images it draws.
constexpr int kNamedDepth = 4;

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

// The machine's library and the team's, which a command opens when it first needs it.
class Libraries {
public:
    Libraries(Session& session, Library local) : session_(&session), local_(std::move(local)) {}

    [[nodiscard]] const Library& local() const noexcept { return local_; }

    // The team's library SLASH_LIBRARY_TEAM or providers.json names; null if neither names one,
    // or if it can't be opened or failed already, which the reply then says.
    Team* team() {
        if (!error_.empty()) {
            return nullptr;
        }
        if (!opened_) {
            opened_ = true;
            auto config = ai::load_config(session_->options().project_root);
            if (!config) {
                fail(config.error());
                return nullptr;
            }
            TeamOptions options;
            options.project_root = session_->options().project_root;
            options.network = session_->allowed(Capability::Network);
            auto team = open_team(config.value(), options);
            if (!team) {
                fail(team.error());
            } else {
                team_ = std::move(team).value();
            }
        }
        return team_.get();
    }

    // Keeps the first reason the team's library failed, for the reply, and leaves the team out of
    // the rest of the command, so a server that is down costs one wait.
    void fail(const Error& error) {
        if (error_.empty()) {
            error_ = error.message;
        }
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // The reply, with why the team's library failed if it did: the command did what it could
    // with the machine's library alone.
    [[nodiscard]] Reply noted(Reply reply) const {
        if (reply.ok && !error_.empty()) {
            reply.value["team_error"] = error_;
        }
        return reply;
    }

private:
    Session* session_;
    Library local_;
    std::unique_ptr<Team> team_;
    bool opened_ = false;
    std::string error_;
};

Result<Libraries> open_libraries(Session& session) {
    auto library = open_library(session);
    if (!library) {
        return library.error();
    }
    return Libraries(session, std::move(library).value());
}

// Copies an asset from the team's library into the machine's, with the request that made it.
Result<Record> pull(const Libraries& libs, Team& team, const Record& shared,
                    std::string_view request) {
    auto data = team.read(shared);
    if (!data) {
        return data.error();
    }
    if (sha256_hex(data.value()) != shared.id) {
        return make_error(ErrorCode::PlatformFailure, "the team's library sent other bytes for "
                                                      "asset \"" +
                                                          short_id(shared.id) + "\"");
    }
    auto stored = libs.local().put(data.value(), shared);
    if (!stored) {
        return stored.error();
    }
    if (!request.empty()) {
        if (auto remembered = libs.local().remember_request(request, stored.value().id);
            !remembered) {
            return remembered.error();
        }
    }
    return stored;
}

// The asset with this ID or ID prefix from the machine's library, or else from the team's,
// copied into the machine's unless the session writes no files.
Result<Record> find_asset(const Session& session, Libraries& libs, std::string_view id) {
    auto record = libs.local().get(id);
    if (record || record.error().code != ErrorCode::NotFound) {
        return record;
    }
    Team* team = libs.team();
    if (team == nullptr) {
        if (!libs.error().empty()) {
            return make_error(ErrorCode::NotFound,
                              record.error().message +
                                  ", and the team's library failed: " + libs.error());
        }
        return record;
    }
    auto shared = team->get(id);
    if (!shared) {
        libs.fail(shared.error());
        return make_error(ErrorCode::NotFound,
                          record.error().message +
                              ", and the team's library failed: " + shared.error().message);
    }
    std::optional<Record> found = std::move(shared).value();
    if (!found) {
        return make_error(ErrorCode::NotFound, "no asset with ID \"" + std::string(id) +
                                                   "\" in the library or the team's");
    }
    if (!session.options().write_files) {
        return std::move(*found);
    }
    return pull(libs, *team, *found, {});
}

// The machine's records, with the team's that match the query beside them, or in place of the
// same asset's: the team's copy carries every teammate's feedback.
std::vector<Record> with_team(Libraries& libs, std::vector<Record> records, const Query& query) {
    Team* team = libs.team();
    if (team == nullptr) {
        return records;
    }
    auto found = team->search(query);
    if (!found) {
        libs.fail(found.error());
        return records;
    }
    for (Record& record : found.value()) {
        if (auto same = std::ranges::find(records, record.id, &Record::id); same != records.end()) {
            *same = std::move(record);
        } else {
            records.push_back(std::move(record));
        }
    }
    return records;
}

// Puts the machine's assets with these IDs, and the versions they were refined from, in the
// team's library where it lacks them, each with the requests that made it. Returns the IDs put.
Result<std::vector<std::string>> share_missing(const Libraries& libs, Team& team,
                                               const std::vector<std::string>& ids) {
    std::vector<std::string> wanted;
    for (const std::string& id : ids) {
        std::string at = id;
        for (int depth = 0; !at.empty() && depth < 100; ++depth) {
            if (std::ranges::find(wanted, at) != wanted.end()) {
                break;
            }
            wanted.push_back(at);
            auto record = libs.local().get(at);
            at = record ? record.value().parent : std::string();
        }
    }
    std::vector<std::string> put;
    if (wanted.empty()) {
        return put;
    }
    auto missing = team.missing(wanted);
    if (!missing) {
        return missing.error();
    }
    for (const std::string& id : missing.value()) {
        auto record = libs.local().get(id);
        if (!record) {
            continue; // named by library.json, but not in this machine's library
        }
        auto data = libs.local().read(record.value());
        if (!data) {
            return data.error();
        }
        auto stored = team.put(data.value(), record.value(), libs.local().requests_for(id));
        if (!stored) {
            return stored.error();
        }
        put.push_back(id);
    }
    return put;
}

// Puts a new asset in the team's library with the request that made it, and the versions it was
// refined from where the team lacks them, so teammates' searches and exact repeats find it.
void share(Libraries& libs, const Record& record, std::string_view data, std::string_view request) {
    Team* team = libs.team();
    if (team == nullptr) {
        return;
    }
    std::vector<std::string> requests;
    if (!request.empty()) {
        requests.emplace_back(request);
    }
    if (auto stored = team->put(data, record, requests); !stored) {
        libs.fail(stored.error());
        return;
    }
    if (!record.parent.empty()) {
        if (auto shared = share_missing(libs, *team, {record.parent}); !shared) {
            libs.fail(shared.error());
        }
    }
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

// The problems the storyboard's palettes and budgets that cover a project path find in an image
// there, or nullopt when none covers it: what an agent would otherwise find out later from
// storyboard.check.
std::optional<std::vector<std::string>> style_problems(Session& session, const std::string& path,
                                                       std::string_view data) {
    const json::Object entries = storyboard_entries(session);
    if (entries.empty()) {
        return std::nullopt;
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
        return std::nullopt;
    }
    std::vector<std::string> problems;
    problems.reserve(errors.size());
    for (storyboard::FileError& error : errors) {
        problems.push_back(std::move(error.message));
    }
    return problems;
}

// Records what a check found as an asset's feedback, here and in the team's library, unless its
// last feedback from the same check said the same; sessions that run tests write none.
void record_check(const Session& session, Libraries& libs, Record record,
                  const Feedback& feedback) {
    if (!session.options().write_files || repeats_last(record, feedback)) {
        return;
    }
    add_feedback(record, feedback);
    (void)libs.local().update(record);
    if (Team* team = libs.team(); team != nullptr) {
        if (auto added = team->add_feedback(record.id, feedback, true); !added) {
            libs.fail(added.error());
        }
    }
}

// Checks an image put in the project against the storyboard's style, as style_problems, and
// records the result as the asset's feedback. Returns the problems found.
json::Array check_style(Session& session, Libraries& libs, const Record& record,
                        const std::string& path, std::string_view data) {
    json::Array problems;
    if (record.type != "image") {
        return problems;
    }
    const std::optional<std::vector<std::string>> found = style_problems(session, path, data);
    if (!found) {
        return problems;
    }
    record_check(
        session, libs, record,
        {.verdict = found->empty() ? "accepted" : "rejected",
         .reason = found->empty() ? "fits the storyboard's palette and budget" : found->front(),
         .source = std::string(kStyleCheck)});
    for (const std::string& problem : *found) {
        problems.emplace_back(problem);
    }
    return problems;
}

Reply asset_search(Session& session, const json::Value& params) {
    auto libs = open_libraries(session);
    if (!libs) {
        return Reply::failure(libs.error());
    }
    auto records = libs.value().local().all();
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
    const std::vector<Record> known = with_team(libs.value(), std::move(records).value(), query);
    json::Value out;
    out["assets"] = json::Array{};
    for (const Match& match : search(known, query)) {
        json::Value item = summary(*match.record);
        if (!query.text.empty()) {
            item["score"] = match.score;
        }
        out["assets"].push_back(std::move(item));
    }
    return libs.value().noted(Reply::success(std::move(out)));
}

struct Asked {
    GenerateRequest request;
    json::Value keyed; // the request as its key hashes it
    std::string description;
    std::vector<std::string> tags;
    std::string path;
};

Reply finish(Session& session, Libraries& libs, const Record& record, const Asked& asked,
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
        auto copied = copy_into_project(session, libs.local(), record, asked.path);
        if (!copied) {
            Reply reply = failure_at(copied.error(), "/path");
            // The asset is in the library all the same; asset.use puts it somewhere else.
            reply.data["id"] = short_id(record.id);
            return reply;
        }
        out["path"] = asked.path;
        if (json::Array problems = check_style(session, libs, record, asked.path, copied.value());
            !problems.empty()) {
            out["problems"] = std::move(problems);
        }
    }
    return libs.noted(Reply::success(std::move(out)));
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
            if (reason.empty() ||
                (accepted && (item.source == kStyleCheck || item.source == kFrameCheck))) {
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
    auto opened_libs = open_libraries(session);
    if (!opened_libs) {
        return Reply::failure(opened_libs.error());
    }
    Libraries& libs = opened_libs.value();
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
    if (const std::optional<std::string> made = libs.local().find_request(key); made) {
        auto record = libs.local().get(*made);
        if (record) {
            return finish(session, libs, record.value(), asked, true);
        }
    }
    // A teammate may have made it already.
    if (Team* team = libs.team(); team != nullptr) {
        auto shared = team->find_request(key);
        std::optional<Record> made;
        if (!shared) {
            libs.fail(shared.error());
        } else {
            made = std::move(shared).value();
        }
        if (made) {
            auto pulled = pull(libs, *team, *made, key);
            if (pulled) {
                Reply reply = finish(session, libs, pulled.value(), asked, true);
                if (reply.ok) {
                    reply.value["team"] = true;
                }
                return reply;
            }
            libs.fail(pulled.error());
        }
    }
    const Query similar_query{.text = asked.request.prompt,
                              .tags = {},
                              .type = asked.request.type,
                              .limit = kMaxCandidates};
    auto local_records = libs.local().all();
    if (!local_records) {
        return Reply::failure(local_records.error());
    }
    const std::vector<Record> records =
        with_team(libs, std::move(local_records).value(), similar_query);
    const json::Value* similar = params.find("similar");
    if (parent == nullptr && (similar == nullptr || similar->as_bool())) {
        json::Array candidates;
        for (const Match& match : search(records, similar_query)) {
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
            return libs.noted(Reply::success(std::move(out)));
        }
    }

    std::string context;
    if (const json::Value* with = params.find("context"); with == nullptr || with->as_bool()) {
        context = prompt_context(session, records, asked, parent);
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
    auto stored = libs.local().put(data, std::move(record));
    if (!stored) {
        return Reply::failure(stored.error());
    }
    if (auto remembered = libs.local().remember_request(key, stored.value().id); !remembered) {
        return Reply::failure(remembered.error());
    }
    share(libs, stored.value(), data, key);
    return finish(session, libs, stored.value(), asked, false);
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
    auto libs = open_libraries(session);
    if (!libs) {
        return Reply::failure(libs.error());
    }
    auto parent = find_asset(session, libs.value(), text(params, "id"));
    if (!parent) {
        return failure_at(parent.error(), "/id");
    }
    if (auto writes = check_writes(session); !writes) {
        return Reply::failure(writes.error());
    }
    auto data = libs.value().local().read(parent.value());
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
    auto libs = open_libraries(session);
    if (!libs) {
        return Reply::failure(libs.error());
    }
    const Library& library = libs.value().local();
    auto record = find_asset(session, libs.value(), text(params, "id"));
    if (!record) {
        return failure_at(record.error(), "/id");
    }
    // The team's copy has every teammate's feedback.
    std::optional<bool> shared;
    if (Team* team = libs.value().team(); team != nullptr) {
        if (auto copy = team->get(record.value().id); !copy) {
            libs.value().fail(copy.error());
        } else {
            const std::optional<Record>& team_copy = copy.value();
            shared = team_copy.has_value();
            if (team_copy) {
                record.value().feedback = team_copy->feedback;
            }
        }
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
            auto earlier = library.get(at);
            at = earlier ? earlier.value().parent : std::string();
        }
        out["versions"] = std::move(versions);
    }
    auto records = library.all();
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
    if (shared) {
        out["shared"] = *shared;
    }
    return libs.value().noted(Reply::success(std::move(out)));
}

Reply asset_use(Session& session, const json::Value& params) {
    auto libs = open_libraries(session);
    if (!libs) {
        return Reply::failure(libs.error());
    }
    auto record = find_asset(session, libs.value(), text(params, "id"));
    if (!record) {
        return failure_at(record.error(), "/id");
    }
    const std::string path = text(params, "path");
    auto copied = copy_into_project(session, libs.value().local(), record.value(), path);
    if (!copied) {
        return failure_at(copied.error(), "/path");
    }
    json::Value out;
    out["path"] = path;
    if (json::Array problems =
            check_style(session, libs.value(), record.value(), path, copied.value());
        !problems.empty()) {
        out["problems"] = std::move(problems);
    }
    return libs.value().noted(Reply::success(std::move(out)));
}

Reply asset_add(Session& session, const json::Value& params) {
    auto libs = open_libraries(session);
    if (!libs) {
        return Reply::failure(libs.error());
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
    auto stored = libs.value().local().put(data.value(), std::move(record));
    if (!stored) {
        return Reply::failure(stored.error());
    }
    if (auto copied = copy_into_project(session, libs.value().local(), stored.value(), path);
        !copied) {
        return failure_at(copied.error(), "/path");
    }
    share(libs.value(), stored.value(), data.value(), {});
    json::Value out = summary(stored.value());
    out.as_object().erase("description");
    out.as_object().erase("tags");
    return libs.value().noted(Reply::success(std::move(out)));
}

Reply asset_feedback(Session& session, const json::Value& params) {
    auto libs = open_libraries(session);
    if (!libs) {
        return Reply::failure(libs.error());
    }
    if (auto writes = check_writes(session); !writes) {
        return Reply::failure(writes.error());
    }
    auto record = find_asset(session, libs.value(), text(params, "id"));
    if (!record) {
        return failure_at(record.error(), "/id");
    }
    std::string source = text(params, "source");
    const Feedback feedback{.verdict = text(params, "verdict"),
                            .reason = trimmed(text(params, "reason")),
                            .source = source.empty() ? "agent" : std::move(source)};
    add_feedback(record.value(), feedback);
    if (auto updated = libs.value().local().update(record.value()); !updated) {
        return Reply::failure(updated.error());
    }
    // The counts the reply gives are the team's, where it has the asset.
    if (Team* team = libs.value().team(); team != nullptr) {
        if (auto shared = team->add_feedback(record.value().id, feedback, false); !shared) {
            libs.value().fail(shared.error());
        } else if (std::optional<Record> team_copy = std::move(shared).value(); team_copy) {
            record.value() = std::move(*team_copy);
        }
    }
    json::Value out = summary(record.value());
    for (const std::string_view key :
         {"description", "tags", "type", "width", "height", "seconds"}) {
        out.as_object().erase(std::string(key));
    }
    return libs.value().noted(Reply::success(std::move(out)));
}

// The files the project took from the library, by project path: library.json's assets and their
// IDs.
Result<std::map<std::string, std::string>> project_assets(Session& session) {
    std::map<std::string, std::string> assets;
    auto read = session.read_project_file(kProjectFile);
    if (!read) {
        return read.error();
    }
    const std::optional<std::string>& text = read.value();
    if (!text) {
        return assets;
    }
    auto list = json::parse(*text);
    if (!list || !list.value().is_object()) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kProjectFile) + " is not a JSON object");
    }
    if (const json::Value* entries = list.value().find("assets");
        entries != nullptr && entries->is_object()) {
        for (const auto& [path, entry] : entries->as_object()) {
            if (const json::Value* id = entry.find("id"); id != nullptr && id->is_string()) {
                assets.emplace(path, id->as_string());
            }
        }
    }
    return assets;
}

// Each string in a JSON value that is one of the files.
void named_files(const json::Value& value, const std::set<std::string>& files,
                 std::set<std::string>& out) {
    if (value.is_string()) {
        if (files.contains(value.as_string())) {
            out.insert(value.as_string());
        }
    } else if (value.is_array()) {
        for (const json::Value& item : value.as_array()) {
            named_files(item, files, out);
        }
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.as_object()) {
            named_files(item, files, out);
        }
    }
}

// The project's scenes that name one of the paths, themselves or through the files they name
// (prefabs, sprite frames, tile sets, themes), sorted.
std::vector<std::string> scenes_naming(Session& session,
                                       const std::map<std::string, std::string>& paths) {
    const auto outside_build = [](std::string_view folder) { return folder != "build/"; };
    auto listed = platform::list_files(session.options().project_root, outside_build);
    if (!listed) {
        return {};
    }
    const std::set<std::string> files(listed.value().begin(), listed.value().end());
    std::map<std::string, std::set<std::string>> named; // by file, the files it names
    const auto names = [&](const std::string& file) -> const std::set<std::string>& {
        const auto [found, made] = named.try_emplace(file);
        if (made && file.ends_with(".json")) {
            if (auto text = session.read_project_file(file); text && text.value()) {
                if (auto doc = json::parse(*text.value()); doc) {
                    named_files(doc.value(), files, found->second);
                }
            }
        }
        return found->second;
    };
    std::vector<std::string> scenes;
    for (const std::string& file : files) {
        if (!file.ends_with(world::kSceneSuffix)) {
            continue;
        }
        std::set<std::string> reached{file};
        std::vector<std::string> next{file};
        bool draws = false;
        for (int depth = 0; depth < kNamedDepth && !next.empty() && !draws; ++depth) {
            std::vector<std::string> found;
            for (const std::string& from : next) {
                for (const std::string& to : names(from)) {
                    draws = draws || paths.contains(to);
                    if (reached.insert(to).second) {
                        found.push_back(to);
                    }
                }
            }
            next = std::move(found);
        }
        if (draws) {
            scenes.push_back(file);
        }
    }
    return scenes;
}

// Window pixels per texel where an item draws a texture: the most over its triangles that sample
// it, or 0 if none does.
float drawn_scale(const canvas::FrameItem& item, std::string_view texture) {
    float most = 0.0F;
    for (const canvas::FramePiece& piece : item.pieces) {
        if (piece.texture != texture || piece.width == 0 || piece.height == 0) {
            continue;
        }
        for (std::size_t first = 0; first + 3 <= piece.vertices.size(); first += 3) {
            const render::Vertex2D& a = piece.vertices[first];
            const render::Vertex2D& b = piece.vertices[first + 1];
            const render::Vertex2D& c = piece.vertices[first + 2];
            const float pixels = ((b.x - a.x) * (c.y - a.y)) - ((c.x - a.x) * (b.y - a.y));
            const float texels = (((b.u - a.u) * (c.v - a.v)) - ((c.u - a.u) * (b.v - a.v))) *
                                 static_cast<float>(piece.width) * static_cast<float>(piece.height);
            if (texels != 0.0F) {
                most = std::max(most, std::sqrt(std::abs(pixels / texels)));
            }
        }
    }
    return most;
}

// A problem asset.check found: where, if in a scene, and what.
struct Found {
    std::string scene;
    std::string entity;
    std::string message;
};

// What asset.check found for one asset.
struct Checked {
    std::vector<Found> problems;
    std::vector<std::string> clear; // scenes where it shows clearly
};

json::Value problem_value(std::string_view id, std::string_view path, const Found& found) {
    json::Value item;
    if (!id.empty()) {
        item["id"] = short_id(id);
        item["path"] = path;
    }
    if (!found.scene.empty()) {
        item["scene"] = found.scene;
    }
    if (!found.entity.empty()) {
        item["entity"] = found.entity;
    }
    item["message"] = found.message;
    return item;
}

std::string percent(float share) {
    return std::to_string(std::max(1, static_cast<int>(std::lround(share * 100.0F)))) + "%";
}

Reply asset_check(Session& session, const json::Value& params) {
    auto assets = project_assets(session);
    if (!assets) {
        return Reply::failure(assets.error());
    }
    std::vector<std::string> scenes = strings(params, "scenes");
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        if (auto full = session.resolve_path(scenes[i]); !full) {
            return failure_at(full.error(), "/scenes/" + std::to_string(i));
        }
    }
    if (params.find("scenes") == nullptr && !assets.value().empty()) {
        scenes = scenes_naming(session, assets.value());
    }
    const std::int64_t ticks = params.find("ticks") != nullptr ? params.find("ticks")->as_int() : 0;

    std::map<std::string, Checked> found; // by project path
    json::Array failures;
    if (!assets.value().empty()) {
        // Each scene in a session of its own that writes nothing, as tests run.
        command::SessionOptions options = session.options();
        options.capture_logs = false;
        options.write_files = false;
        std::erase(options.capabilities, Capability::Build);
        canvas::SummaryOptions look;
        look.contrast = true;
        for (const std::string& scene : scenes) {
            Session shown(session.components(), session.commands(), options);
            json::Value open;
            open["path"] = scene;
            Reply opened = shown.call("scene.open", open);
            if (opened.ok && ticks > 0) {
                json::Value step;
                step["ticks"] = ticks;
                opened = shown.call("step", step);
            }
            if (!opened.ok) {
                failures.push_back(problem_value("", "", {scene, "", opened.message}));
                continue;
            }
            auto summary = canvas::summarize(shown.world(), shown.assets(), look);
            if (!summary) {
                failures.push_back(problem_value("", "", {scene, "", summary.error().message}));
                continue;
            }
            const std::vector<canvas::FrameItem>& items = summary.value().contents.items;
            for (const canvas::Shown& each : summary.value().shown) {
                const canvas::FrameItem& item = items[each.item];
                std::set<std::string_view> textures;
                for (const canvas::FramePiece& piece : item.pieces) {
                    if (!piece.texture.empty() &&
                        assets.value().contains(std::string(piece.texture))) {
                        textures.insert(piece.texture);
                    }
                }
                const std::string entity(shown.world().id_of(item.entity));
                for (const std::string_view texture : textures) {
                    Checked& checked = found[std::string(texture)];
                    const auto add = [&](std::string message) {
                        checked.problems.push_back({scene, entity, std::move(message)});
                    };
                    const std::size_t before = checked.problems.size();
                    if (each.contrast > 0.0F && each.contrast < canvas::kHardToSee) {
                        const long tenths = std::lround(each.contrast * 10.0F);
                        const std::string ratio =
                            std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
                        add("hard to see: its edge has " + ratio + ":1 contrast with the " +
                            (each.darker ? "lighter" : "darker") + " colors around it");
                    }
                    if (const float scale = drawn_scale(item, texture);
                        scale > 0.0F && scale <= kOversized) {
                        add("drawn at " + percent(scale) + " of its size, so an image 1/" +
                            std::to_string(static_cast<int>(std::floor((1.0F / scale) + 0.01F))) +
                            " as wide and high would look the same");
                    }
                    if (checked.problems.size() == before &&
                        std::ranges::find(checked.clear, scene) == checked.clear.end()) {
                        checked.clear.push_back(scene);
                    }
                }
            }
        }
    }

    auto libs = open_libraries(session);
    json::Array problems;
    for (const auto& [path, id] : assets.value()) {
        std::optional<Record> record;
        if (libs) {
            if (auto got = find_asset(session, libs.value(), id); got) {
                record = std::move(got).value();
            }
        }
        if (path.ends_with(".png") || path.ends_with(".bmp")) {
            if (auto data = session.read_project_file(path); data && data.value()) {
                if (const auto style = style_problems(session, path, *data.value()); style) {
                    for (const std::string& message : *style) {
                        problems.push_back(problem_value(id, path, {"", "", message}));
                    }
                    if (record) {
                        record_check(session, libs.value(), *record,
                                     {.verdict = style->empty() ? "accepted" : "rejected",
                                      .reason = style->empty()
                                                    ? "fits the storyboard's palette and budget"
                                                    : style->front(),
                                      .source = std::string(kStyleCheck)});
                        if (auto again = libs.value().local().get(id); again) {
                            record = std::move(again).value();
                        }
                    }
                }
            }
        }
        const auto checked = found.find(path);
        if (checked == found.end()) {
            continue;
        }
        for (const Found& problem : checked->second.problems) {
            problems.push_back(problem_value(id, path, problem));
        }
        if (!record) {
            continue;
        }
        if (!checked->second.problems.empty()) {
            const Found& first = checked->second.problems.front();
            record_check(session, libs.value(), *record,
                         {.verdict = "rejected",
                          .reason = first.message + " (" + first.scene + ")",
                          .source = std::string(kFrameCheck)});
        } else if (!checked->second.clear.empty()) {
            const std::vector<std::string>& clear = checked->second.clear;
            record_check(
                session, libs.value(), *record,
                {.verdict = "accepted",
                 .reason = clear.size() == 1
                               ? "shows clearly in " + clear.front()
                               : "shows clearly in " + std::to_string(clear.size()) + " scenes",
                 .source = std::string(kFrameCheck)});
        }
    }
    for (json::Value& failure : failures) {
        problems.push_back(std::move(failure));
    }
    json::Value out;
    out["checked"] = static_cast<std::int64_t>(assets.value().size());
    out["scenes"] = static_cast<std::int64_t>(assets.value().empty() ? 0 : scenes.size());
    out["problems"] = std::move(problems);
    return libs ? libs.value().noted(Reply::success(std::move(out)))
                : Reply::success(std::move(out));
}

Reply asset_sync(Session& session, const json::Value& params) {
    auto libs = open_libraries(session);
    if (!libs) {
        return Reply::failure(libs.error());
    }
    if (auto writes = check_writes(session); !writes) {
        return Reply::failure(writes.error());
    }
    Team* team = libs.value().team();
    if (team == nullptr) {
        return Reply::failure(command::code::kNotFound,
                              libs.value().error().empty()
                                  ? "no team library: name a shared folder or a library server's "
                                    "URL in providers.json's \"library\": {\"team\": ...}, or in "
                                    "SLASH_LIBRARY_TEAM"
                                  : libs.value().error());
    }
    std::vector<std::string> ids;
    if (const json::Value* all = params.find("all"); all != nullptr && all->as_bool()) {
        auto records = libs.value().local().all();
        if (!records) {
            return Reply::failure(records.error());
        }
        for (const Record& record : records.value()) {
            ids.push_back(record.id);
        }
    } else {
        auto assets = project_assets(session);
        if (!assets) {
            return Reply::failure(assets.error());
        }
        for (const auto& [path, id] : assets.value()) {
            if (std::ranges::find(ids, id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }
    auto shared = share_missing(libs.value(), *team, ids);
    if (!shared) {
        return Reply::failure(shared.error());
    }
    json::Value out;
    out["team"] = team->where();
    json::Array put;
    for (const std::string& id : shared.value()) {
        put.emplace_back(short_id(id));
    }
    out["shared"] = std::move(put);
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

    json::Value scenes = string_array("Scenes to look at. Default: every scene that draws one.");
    scenes["minItems"] = 1;
    json::Value ticks =
        integer("Ticks each scene runs before it is looked at; default 0.", 0, kMaxCheckTicks);
    registry.add({
        .name = "asset.check",
        .description = "Checks the files the project took from the library as its scenes show "
                       "them (too dark or light to see, drawn far smaller than their size) and "
                       "against the storyboard's style, and records the results as their "
                       "feedback. Returns the problems.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema({{"scenes", std::move(scenes)}, {"ticks", std::move(ticks)}}),
        .handler = asset_check,
    });

    registry.add({
        .name = "asset.sync",
        .description = "Puts the assets this project uses (library.json), and the versions they "
                       "came from, in the team's library where it lacks them, so teammates find "
                       "them. Generated, refined and added assets go there by themselves.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = params_schema({{"all", property("boolean", "Every asset in this machine's "
                                                             "library instead.")}}),
        .handler = asset_sync,
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
