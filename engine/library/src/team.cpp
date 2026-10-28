#include "slash/library/team.hpp"

#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::library {
namespace {

constexpr std::size_t kMaxResults = 50;
constexpr std::size_t kKeyLength = 64;

bool is_hex(std::string_view text) {
    return std::ranges::all_of(
        text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Bytes and their record as a library server's put request carries them.
Result<Record> describe_upload(std::string_view data, const Record& sent, const Library& library) {
    Record record = sent;
    record.id.clear();
    if (type_of("." + record.format).empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the library keeps .png, .bmp, .wav, .ogg and .mp3 files, not \"" +
                              record.format + "\"");
    }
    // A recipe's model parts are put in first, so they are here to build it with.
    if (auto described =
            describe_file(data, "upload." + record.format, record, part_reader(library));
        !described) {
        return described.error();
    }
    return record;
}

// The team library in a folder: a library like the machine's, which every teammate writes to.
class FolderTeam final : public Team {
public:
    explicit FolderTeam(std::string dir) : library_(std::move(dir)) {}

    [[nodiscard]] const std::string& where() const override { return library_.dir(); }

    Result<std::vector<Record>> search(const Query& query) override {
        auto records = library_.all();
        if (!records) {
            return records.error();
        }
        std::vector<Record> found;
        for (const Match& match : library::search(records.value(), query)) {
            found.push_back(*match.record);
        }
        return found;
    }

    Result<std::optional<Record>> get(std::string_view id) override {
        auto record = library_.get(id);
        if (!record) {
            if (record.error().code == ErrorCode::NotFound) {
                return std::optional<Record>();
            }
            return record.error();
        }
        return std::optional<Record>(std::move(record).value());
    }

    Result<std::string> read(const Record& record) override { return library_.read(record); }

    Result<std::optional<Record>> find_request(std::string_view key) override {
        const std::optional<std::string> id = library_.find_request(key);
        if (!id) {
            return std::optional<Record>();
        }
        return get(*id);
    }

    Result<Record> put(std::string_view data, const Record& record,
                       std::span<const std::string> requests) override {
        auto checked = describe_upload(data, record, library_);
        if (!checked) {
            return checked.error();
        }
        auto stored = library_.put(data, std::move(checked).value());
        if (!stored) {
            return stored.error();
        }
        for (const std::string& key : requests) {
            if (key.size() != kKeyLength || !is_hex(key)) {
                return make_error(ErrorCode::InvalidArgument,
                                  "\"" + key + "\" is not a request key: 64 hex digits");
            }
            if (library_.find_request(key) != stored.value().id) {
                if (auto remembered = library_.remember_request(key, stored.value().id);
                    !remembered) {
                    return remembered.error();
                }
            }
        }
        return stored;
    }

    Result<std::optional<Record>> add_feedback(std::string_view id, const Feedback& feedback,
                                               bool once) override {
        auto found = get(id);
        if (!found) {
            return found;
        }
        std::optional<Record> record = std::move(found).value();
        if (record && (!once || !repeats_last(*record, feedback))) {
            library::add_feedback(*record, feedback);
            if (auto updated = library_.update(*record); !updated) {
                return updated.error();
            }
        }
        return record;
    }

    Result<std::vector<std::string>> missing(std::span<const std::string> ids) override {
        std::vector<std::string> out;
        for (const std::string& id : ids) {
            auto found = get(id);
            if (!found) {
                return found.error();
            }
            if (!found.value()) {
                out.push_back(id);
            }
        }
        return out;
    }

private:
    Library library_;
};

// The team library a library server serves, over HTTP.
class ServerTeam final : public Team {
public:
    explicit ServerTeam(ServerTeamOptions options) : options_(std::move(options)) {
        while (options_.url.ends_with('/')) {
            options_.url.pop_back();
        }
        if (!options_.transport) {
            options_.transport = ai::http_transport();
        }
    }

    [[nodiscard]] const std::string& where() const override { return options_.url; }

    Result<std::vector<Record>> search(const Query& query) override {
        json::Value body;
        body["text"] = query.text;
        json::Array tags;
        for (const std::string& tag : query.tags) {
            tags.emplace_back(tag);
        }
        body["tags"] = std::move(tags);
        body["type"] = query.type;
        body["limit"] = static_cast<std::int64_t>(std::min(query.limit, kMaxResults));
        auto reply = call("search", body);
        if (!reply) {
            return reply.error();
        }
        std::vector<Record> records;
        if (const json::Value* list = reply.value().find("records");
            list != nullptr && list->is_array()) {
            for (const json::Value& item : list->as_array()) {
                auto record = record_from_json(item);
                if (!record) {
                    return bad_reply(record.error());
                }
                records.push_back(std::move(record).value());
            }
        }
        return records;
    }

    Result<std::optional<Record>> get(std::string_view id) override {
        json::Value body;
        body["id"] = id;
        return record_reply(call("get", body));
    }

    Result<std::string> read(const Record& record) override {
        json::Value body;
        body["id"] = record.id;
        auto reply = call("read", body);
        if (!reply) {
            return reply.error();
        }
        const json::Value* data = reply.value().find("data");
        std::optional<std::string> bytes;
        if (data != nullptr && data->is_string()) {
            bytes = base64_decode(data->as_string());
        }
        if (!bytes) {
            return bad_reply(make_error(ErrorCode::InvalidArgument, "no base64 \"data\""));
        }
        return std::move(*bytes);
    }

    Result<std::optional<Record>> find_request(std::string_view key) override {
        json::Value body;
        body["key"] = key;
        return record_reply(call("request", body));
    }

    Result<Record> put(std::string_view data, const Record& record,
                       std::span<const std::string> requests) override {
        json::Value body;
        body["record"] = to_json(record);
        body["data"] = base64_encode(data);
        json::Array keys;
        for (const std::string& key : requests) {
            keys.emplace_back(key);
        }
        body["requests"] = std::move(keys);
        auto stored = record_reply(call("put", body));
        if (!stored) {
            return stored.error();
        }
        std::optional<Record> kept = std::move(stored).value();
        if (!kept) {
            return bad_reply(make_error(ErrorCode::InvalidArgument, R"(no "record")"));
        }
        return std::move(*kept);
    }

    Result<std::optional<Record>> add_feedback(std::string_view id, const Feedback& feedback,
                                               bool once) override {
        json::Value body;
        body["id"] = id;
        body["verdict"] = feedback.verdict;
        body["reason"] = feedback.reason;
        body["source"] = feedback.source;
        body["once"] = once;
        return record_reply(call("feedback", body));
    }

    Result<std::vector<std::string>> missing(std::span<const std::string> ids) override {
        json::Value body;
        json::Array list;
        for (const std::string& id : ids) {
            list.emplace_back(id);
        }
        body["ids"] = std::move(list);
        auto reply = call("missing", body);
        if (!reply) {
            return reply.error();
        }
        std::vector<std::string> out;
        if (const json::Value* found = reply.value().find("missing");
            found != nullptr && found->is_array()) {
            for (const json::Value& item : found->as_array()) {
                out.push_back(item.as_string());
            }
        }
        return out;
    }

private:
    Result<json::Value> call(std::string_view op, const json::Value& body) const {
        ai::HttpRequest request;
        request.url = options_.url + "/v1/library/" + std::string(op);
        request.headers.emplace_back("Authorization", "Bearer " + options_.token);
        request.body = json::write(body, json::Style::Compact);
        request.timeout_seconds = options_.timeout_seconds;
        auto reply = ai::post_with_retries(*options_.transport, request,
                                           "the team's library server at " + options_.url, 2);
        if (!reply) {
            return reply.error();
        }
        auto parsed = json::parse(reply.value().body);
        if (!parsed || !parsed.value().is_object()) {
            return bad_reply(make_error(ErrorCode::InvalidArgument, "not a JSON object"));
        }
        return std::move(parsed).value();
    }

    Result<std::optional<Record>> record_reply(Result<json::Value> reply) const {
        if (!reply) {
            return reply.error();
        }
        const json::Value* record = reply.value().find("record");
        if (record == nullptr || record->is_null()) {
            return std::optional<Record>();
        }
        auto parsed = record_from_json(*record);
        if (!parsed) {
            return bad_reply(parsed.error());
        }
        return std::optional<Record>(std::move(parsed).value());
    }

    [[nodiscard]] Error bad_reply(const Error& error) const {
        return make_error(ErrorCode::PlatformFailure,
                          "the team's library server at " + options_.url +
                              " sent a reply the engine can't read: " + error.message);
    }

    ServerTeamOptions options_;
};

// The path with its "." and "folder/.." parts taken out, as replies show it.
std::string normalized(std::string path) {
    std::ranges::replace(path, '\\', '/');
    const bool absolute = path.starts_with('/');
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = std::min(path.find('/', start), path.size());
        std::string part = path.substr(start, slash - start);
        if (part == ".." && !parts.empty() && parts.back() != "..") {
            parts.pop_back();
        } else if (part == ".." && absolute) {
            // Nothing is above the root.
        } else if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        start = slash + 1;
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += (i > 0 ? "/" : "") + parts[i];
    }
    return out.empty() ? "." : out;
}

bool is_absolute(std::string_view path) {
    return path.starts_with('/') || path.starts_with('\\') ||
           (path.size() > 1 && path[1] == ':'); // C:/ on Windows
}

TeamAnswer answer(int status, json::Object body) {
    return {status, json::Value(std::move(body))};
}

TeamAnswer refuse(int status, const std::string& message) {
    json::Object error;
    error["message"] = message;
    json::Object body;
    body["error"] = std::move(error);
    return answer(status, std::move(body));
}

TeamAnswer refuse(const Error& error) {
    return refuse(
        error.code == ErrorCode::InvalidArgument || error.code == ErrorCode::NotFound ? 400 : 500,
        error.message);
}

std::string member_text(const json::Value& body, std::string_view key) {
    const json::Value* member = body.find(key);
    return member != nullptr && member->is_string() ? member->as_string() : std::string();
}

TeamAnswer record_answer(Result<std::optional<Record>> found) {
    if (!found) {
        return refuse(found.error());
    }
    const std::optional<Record> record = std::move(found).value();
    json::Object body;
    body["record"] = record ? to_json(*record) : json::Value();
    return answer(200, std::move(body));
}

} // namespace

Result<std::unique_ptr<Team>> folder_team(std::string dir) {
    while (dir.size() > 1 && (dir.ends_with('/') || dir.ends_with('\\'))) {
        dir.pop_back();
    }
    if (!platform::exists(dir)) {
        return make_error(ErrorCode::NotFound, "the team's library folder \"" + dir +
                                                   "\" doesn't exist; is the share mounted?");
    }
    return std::unique_ptr<Team>(new FolderTeam(std::move(dir)));
}

std::unique_ptr<Team> server_team(ServerTeamOptions options) {
    return std::make_unique<ServerTeam>(std::move(options));
}

Result<std::unique_ptr<Team>> open_team(const ai::Config& config, const TeamOptions& options) {
    const auto environment = [&options](const std::string& name) {
        return options.environment ? options.environment(name)
                                   : platform::environment_variable(name);
    };
    std::string team = environment(std::string(kTeamVariable));
    if (team.empty() && config.library) {
        team = config.library->team;
    }
    if (team.empty()) {
        return std::unique_ptr<Team>();
    }
    const ai::LibraryConfig settings = config.library.value_or(ai::LibraryConfig{});
    if (team.starts_with("http://") || team.starts_with("https://")) {
        if (!options.network) {
            return make_error(ErrorCode::PermissionDenied, "the team's library server " + team +
                                                               " needs the network capability");
        }
        ServerTeamOptions server;
        server.url = team;
        server.token = environment(settings.token_env);
        if (server.token.empty()) {
            return make_error(ErrorCode::PermissionDenied, "the team's library server " + team +
                                                               " needs its token in " +
                                                               settings.token_env);
        }
        server.timeout_seconds = settings.timeout_seconds;
        server.transport = options.transport;
        return server_team(std::move(server));
    }
    if (team.starts_with("~/")) {
#if defined(_WIN32)
        const std::string home = environment("USERPROFILE");
#else
        const std::string home = environment("HOME");
#endif
        if (home.empty()) {
            return make_error(ErrorCode::NotFound, "the team's library folder \"" + team +
                                                       "\" is in the home folder, but HOME is not "
                                                       "set");
        }
        team = home + team.substr(1);
    } else if (!is_absolute(team)) {
        std::string root = options.project_root;
        if (!root.empty() && root.back() != '/') {
            root += '/';
        }
        team = root + team;
    }
    return folder_team(normalized(std::move(team)));
}

TeamAnswer answer_team_request(const Library& library, std::string_view op,
                               const json::Value& body) {
    if (!body.is_object()) {
        return refuse(400, "the body must be a JSON object");
    }
    FolderTeam team(library.dir());
    if (op == "search") {
        Query query;
        query.text = member_text(body, "text");
        query.type = member_text(body, "type");
        if (const json::Value* tags = body.find("tags"); tags != nullptr && tags->is_array()) {
            for (const json::Value& tag : tags->as_array()) {
                if (tag.is_string()) {
                    query.tags.push_back(tag.as_string());
                }
            }
        }
        if (const json::Value* limit = body.find("limit"); limit != nullptr && limit->is_number()) {
            query.limit = static_cast<std::size_t>(std::clamp<std::int64_t>(
                limit->as_int(), 1, static_cast<std::int64_t>(kMaxResults)));
        }
        auto found = team.search(query);
        if (!found) {
            return refuse(found.error());
        }
        json::Array records;
        for (const Record& record : found.value()) {
            records.push_back(to_json(record));
        }
        json::Object out;
        out["records"] = std::move(records);
        return answer(200, std::move(out));
    }
    if (op == "get") {
        return record_answer(team.get(member_text(body, "id")));
    }
    if (op == "read") {
        auto found = team.get(member_text(body, "id"));
        if (!found) {
            return refuse(found.error());
        }
        const std::optional<Record> record = std::move(found).value();
        if (!record) {
            return refuse(404, "no asset with ID \"" + member_text(body, "id") +
                                   "\" in the team's library");
        }
        auto data = team.read(*record);
        if (!data) {
            return refuse(data.error());
        }
        json::Object out;
        out["data"] = base64_encode(data.value());
        return answer(200, std::move(out));
    }
    if (op == "request") {
        const std::string key = member_text(body, "key");
        if (key.size() != kKeyLength || !is_hex(key)) {
            return refuse(400, "\"key\" must be a request key: 64 hex digits");
        }
        return record_answer(team.find_request(key));
    }
    if (op == "put") {
        const json::Value* sent = body.find("record");
        if (sent == nullptr) {
            return refuse(400, "a put needs the asset's \"record\"");
        }
        auto record = record_from_json(*sent);
        if (!record) {
            return refuse(400, "\"record\": " + record.error().message);
        }
        const std::optional<std::string> data = base64_decode(member_text(body, "data"));
        if (!data) {
            return refuse(400, "\"data\" must be the file's bytes in base64");
        }
        std::vector<std::string> keys;
        if (const json::Value* requests = body.find("requests");
            requests != nullptr && requests->is_array()) {
            for (const json::Value& key : requests->as_array()) {
                keys.push_back(key.is_string() ? key.as_string() : std::string());
            }
        }
        auto stored = team.put(*data, record.value(), keys);
        if (!stored) {
            return refuse(stored.error());
        }
        json::Object out;
        out["record"] = to_json(stored.value());
        return answer(200, std::move(out));
    }
    if (op == "feedback") {
        Feedback feedback{.verdict = member_text(body, "verdict"),
                          .reason = member_text(body, "reason"),
                          .source = member_text(body, "source")};
        if (feedback.verdict != "accepted" && feedback.verdict != "rejected") {
            return refuse(400, R"("verdict" must be "accepted" or "rejected")");
        }
        if (feedback.source.empty()) {
            feedback.source = "agent";
        }
        const json::Value* once = body.find("once");
        return record_answer(
            team.add_feedback(member_text(body, "id"), feedback,
                              once != nullptr && once->is_bool() && once->as_bool()));
    }
    if (op == "missing") {
        std::vector<std::string> ids;
        if (const json::Value* list = body.find("ids"); list != nullptr && list->is_array()) {
            for (const json::Value& id : list->as_array()) {
                const std::string text = id.is_string() ? id.as_string() : std::string();
                if (text.size() != kKeyLength || !is_hex(text)) {
                    return refuse(400, "\"ids\" must be asset IDs of 64 hex digits");
                }
                ids.push_back(text);
            }
        }
        auto missing = team.missing(ids);
        if (!missing) {
            return refuse(missing.error());
        }
        json::Array out_ids;
        for (const std::string& id : missing.value()) {
            out_ids.emplace_back(id);
        }
        json::Object out;
        out["missing"] = std::move(out_ids);
        return answer(200, std::move(out));
    }
    return refuse(404, "no library operation \"" + std::string(op) +
                           "\"; there are search, get, read, request, put, feedback and missing");
}

} // namespace slash::library
