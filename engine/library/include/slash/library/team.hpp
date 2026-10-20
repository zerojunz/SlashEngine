#pragma once

#include "slash/ai/config.hpp"
#include "slash/ai/transport.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/library/library.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Team sharing: a library the team hosts, which each teammate's library searches and adds to, so
// a teammate's request finds assets someone else already made. Nothing goes to a service the
// engine runs. It is either a folder every teammate reaches (a network share, or a folder a sync
// service keeps the same), holding a library laid out as the machine's, or a library server that
// `slash-cli library serve` runs over a folder:
//
//   POST <url>/v1/library/<op>   with "Authorization: Bearer <token>" and a JSON body
//
//   search   {"text", "tags", "type", "limit"}       -> {"records": [...]}, best first
//   get      {"id"}  (an ID or a prefix of one)      -> {"record": {...} or null}
//   read     {"id"}                                  -> {"data": "<base64>"}
//   request  {"key"} (a request_key)                 -> {"record": {...} or null}
//   put      {"record", "data", "requests": [keys]}  -> {"record": {...}}
//   feedback {"id", "verdict", "reason", "source", "once"} -> {"record": {...} or null}
//   missing  {"ids": [...]}                          -> {"missing": [...]}
//
// Errors are {"error": {"message": "..."}} with a 4xx or 5xx status. providers.json's "library"
// names the team ({"team": "<folder or URL>", "token_env": "SLASH_LIBRARY_TOKEN"}), and the
// SLASH_LIBRARY_TEAM variable overrides it on one machine.
namespace slash::library {

inline constexpr std::string_view kTeamVariable = "SLASH_LIBRARY_TEAM";

class Team {
public:
    virtual ~Team() = default;

    // The folder or URL, for messages.
    [[nodiscard]] virtual const std::string& where() const = 0;
    // The team's records that match the query, best first.
    [[nodiscard]] virtual Result<std::vector<Record>> search(const Query& query) = 0;
    // The asset with this ID or ID prefix; nullopt if the team has none.
    [[nodiscard]] virtual Result<std::optional<Record>> get(std::string_view id) = 0;
    [[nodiscard]] virtual Result<std::string> read(const Record& record) = 0;
    // The asset an exact request made (request_key); nullopt if the team has none.
    [[nodiscard]] virtual Result<std::optional<Record>> find_request(std::string_view key) = 0;
    // Stores the asset and the keys of the requests that made it. An asset the team has keeps
    // its record, with the tags added. Returns the team's record.
    [[nodiscard]] virtual Result<Record> put(std::string_view data, const Record& record,
                                             std::span<const std::string> requests) = 0;
    // Adds feedback to the team's record, unless once is set and its last feedback from the same
    // source says the same. Returns the record; nullopt if the team doesn't have the asset.
    [[nodiscard]] virtual Result<std::optional<Record>>
    add_feedback(std::string_view id, const Feedback& feedback, bool once) = 0;
    // Of these IDs (all 64 digits), those the team doesn't have.
    [[nodiscard]] virtual Result<std::vector<std::string>>
    missing(std::span<const std::string> ids) = 0;
};

// The team library in a folder, which must exist: one missing is a share not mounted.
[[nodiscard]] Result<std::unique_ptr<Team>> folder_team(std::string dir);

struct ServerTeamOptions {
    std::string url; // http:// or https://, with no /v1/library
    std::string token;
    int timeout_seconds = 60;
    // null for ai::http_transport().
    std::shared_ptr<ai::Transport> transport{}; // NOLINT(readability-redundant-member-init)
};

// The team library a library server serves.
[[nodiscard]] std::unique_ptr<Team> server_team(ServerTeamOptions options);

struct TeamOptions {
    std::string project_root = ".";
    // false refuses a library server, for sessions without the network capability.
    bool network = true;
    // null for ai::http_transport().
    std::shared_ptr<ai::Transport> transport{}; // NOLINT(readability-redundant-member-init)
    // Reads environment variables; null for the process's own.
    std::function<std::string(const std::string& name)>
        environment{}; // NOLINT(readability-redundant-member-init)
};

// The team library SLASH_LIBRARY_TEAM or providers.json's "library" names: a library server for
// an http:// or https:// URL, a folder otherwise (~/ in the home folder, relative to the project).
// null if neither names one.
[[nodiscard]] Result<std::unique_ptr<Team>> open_team(const ai::Config& config,
                                                      const TeamOptions& options = {});

// A library server's answer to one request: an HTTP status and its JSON body.
struct TeamAnswer {
    int status = 200;
    json::Value body;
};

// Answers a request to a library server over the library in a folder, as the comment at the top
// describes. Callers keep requests that write (put, feedback) from running at the same time.
[[nodiscard]] TeamAnswer answer_team_request(const Library& library, std::string_view op,
                                             const json::Value& body);

struct TeamServerOptions {
    // Where to listen: "127.0.0.1" for this machine only, "0.0.0.0" or "::" for every interface,
    // so teammates reach it.
    std::string host = "127.0.0.1";
    int port = 0; // 0 picks a free port
    // The bearer token clients must send; generated at random if empty.
    std::string token;
};

// A library server: serves a folder's library to the team over HTTP from background threads.
class TeamServer {
public:
    [[nodiscard]] static Result<std::unique_ptr<TeamServer>>
    start(std::string dir, const TeamServerOptions& options = {});
    ~TeamServer();

    TeamServer(const TeamServer&) = delete;
    TeamServer& operator=(const TeamServer&) = delete;
    TeamServer(TeamServer&&) = delete;
    TeamServer& operator=(TeamServer&&) = delete;

    // The URL teammates put in providers.json: http://<host>:<port>.
    [[nodiscard]] const std::string& url() const noexcept;
    [[nodiscard]] const std::string& token() const noexcept;

    // Stops listening; the destructor calls it.
    void stop();

private:
    struct Impl;
    explicit TeamServer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace slash::library
