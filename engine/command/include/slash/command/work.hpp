#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The work journal: what every session's commands did, written as they run, so a person can see
// what an agent is doing and has done, whichever vendor's agent it is.
//
// Each session that keeps one (slash-mcp, slash-cli rpc, serve and run) appends a line of JSON
// per command to .slash/work/<session>.jsonl in the project: the time, the command, a short form
// of its params, the entities and files it changed, its error, and the client that sent it. Runs
// of reads are folded into one line, and the results of tests and checks are kept, since they say
// whether the work is good. Sessions also list themselves in .slash/work/sessions.json with the
// read-only server watchers attach to (slash/server/watch.hpp). project.json's "work" block turns
// either off.
namespace slash::world {
class ComponentRegistry;
}

namespace slash::command::work {

inline constexpr std::string_view kFolder = ".slash/work";
inline constexpr std::string_view kSessionsFile = ".slash/work/sessions.json";
// Each session's checkpoints, in a folder named after it.
inline constexpr std::string_view kCheckpointsFolder = ".slash/work/checkpoints";

// project.json's "work" block.
struct Settings {
    // Whether sessions keep a journal.
    bool journal = true;
    // Whether sessions serve themselves read-only on the loopback interface for watchers.
    bool serve = true;
    // Whether sessions take checkpoints (scene.save, a passing test.run, work.checkpoint).
    bool checkpoints = true;
    // The most the project's journals and checkpoints hold together; the oldest sessions' go first.
    std::uint64_t max_bytes = std::uint64_t{64} << 20U;
};

// Reads project.json's "work" block in a project folder: the defaults where there is no
// project.json or no block, or where a value is wrong (project validate reports those).
[[nodiscard]] Settings read_settings(std::string_view project_root);

// Unix time in milliseconds.
using Clock = std::function<std::int64_t()>;

struct JournalOptions {
    std::string project_root = ".";
    // The program keeping the session, such as "slash-mcp".
    std::string host;
    std::uint64_t max_bytes = Settings{}.max_bytes;
    bool checkpoints = Settings{}.checkpoints;
    // The session's ID; "" makes one from the time and a random number, which sorts by age.
    std::string id;
    // The system clock when empty. Tests and examples pass their own, so lines repeat.
    Clock clock;
};

// What one outermost command did, as a session hands it to its journal.
struct Call {
    std::string_view command;
    const json::Value& params;
    const Reply& reply;
    // A command that changes nothing: a Read command or a dry run.
    bool read = false;
    // world::diff_scenes of the world before and after it, or null.
    json::Value changes;
    // Project paths it wrote or removed.
    std::vector<std::string> files;
    double milliseconds = 0.0;
    // Who sent it: the MCP client's name, or "".
    std::string_view client;
};

// A moment of a session's work to look back at and go back to: the files it had changed since
// the last one as they were, its world and a frame of it. Taken at each scene.save, each test.run
// that passes and each work.checkpoint, in .slash/work/checkpoints/<session>/<n>/.
struct Checkpoint {
    std::int64_t n = 0;
    std::string time;
    // "scene.save", "test.run", "work.checkpoint" or "restore".
    std::string reason;
    std::string label;
    // The journal line it follows.
    std::int64_t line = 0;
    // Project paths it holds, as they were then; removed ones too.
    std::vector<std::string> files;
    std::vector<std::string> removed;
    // Whether it holds a frame.
    bool frame = false;

    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static std::optional<Checkpoint> from_json(const json::Value& value);
};

class Journal {
public:
    // Starts a session's journal, written from its first line on, removing the oldest sessions'
    // journals, past any live session's, until the project's take less than half of max_bytes.
    [[nodiscard]] static Result<std::unique_ptr<Journal>> open(JournalOptions options);
    // Writes what is pending and an "end" line.
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&&) = delete;
    Journal& operator=(Journal&&) = delete;

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    // The journal's project path, .slash/work/<id>.jsonl.
    [[nodiscard]] std::string path() const;

    void record(const Call& call);
    // Something that changed the world outside a command, such as a hot reload.
    void record_event(std::string_view what, const json::Value& changes);
    // Writes the run of reads since the last line if the last read was over a second ago; hosts
    // call it while they wait, so a watcher sees reads soon.
    void idle();
    // Writes the run of reads since the last line.
    void flush();
    // A line of steering: {"paused": {"why"}}, {"resumed": true}, {"note": "<text>"} from the
    // people watching, or {"plan": {...}} from the agent.
    void steer(json::Value line);

    // The session calls this before it writes or removes a project file: the first time for each
    // path, it keeps what the file held, so a restore can put it back.
    void before_write(std::string_view path);
    // Takes a checkpoint: the files written since the last one, the world as scene-file JSON and
    // a frame as PNG bytes ("" for none), with a journal line saying so.
    [[nodiscard]] Result<Checkpoint> checkpoint(std::string_view reason, std::string_view label,
                                                const json::Value& world, std::string_view png);
    [[nodiscard]] bool keeps_checkpoints() const noexcept { return options_.checkpoints; }

private:
    explicit Journal(JournalOptions options);
    void write(json::Value line);
    [[nodiscard]] std::int64_t now() const;

    JournalOptions options_;
    std::string id_;
    std::string full_path_;
    std::uint64_t bytes_ = 0;
    std::uint64_t sequence_ = 0;
    // Whether the first line, which says who started the session, is written.
    bool started_ = false;
    // The run of reads not yet written: each command's count, the failures among them.
    json::Object reads_;
    std::size_t read_count_ = 0;
    std::int64_t reads_from_ = 0;
    std::int64_t reads_until_ = 0;
    std::string reads_client_;
    // Each path whose first contents are kept: the file holding them, or null where there was
    // none. And the paths written since the last checkpoint.
    json::Value kept_ = json::Object{}; // NOLINT(readability-redundant-member-init)
    std::vector<std::string> written_;
    std::int64_t checkpoints_ = 0;
};

// A session's journal as it is in the project.
struct JournalFile {
    std::string id;
    // Project paths: the rotated part first, if there is one, then the live file.
    std::vector<std::string> paths;
    std::uint64_t bytes = 0;
};

// Every journal in the project, oldest first.
[[nodiscard]] std::vector<JournalFile> list_journals(std::string_view project_root);

// The lines of a session's journal, oldest first; lines that aren't JSON are skipped.
[[nodiscard]] Result<std::vector<json::Value>> read_journal(std::string_view project_root,
                                                            std::string_view id);

// A session listed in sessions.json.
struct SessionEntry {
    std::string id;
    std::string host;
    std::int64_t pid = 0;
    // The read-only server's URL and token, or "" when it doesn't serve.
    std::string url;
    std::string token;
    // The journal's project path, or "" when it keeps none.
    std::string journal;
    std::string started;

    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static std::optional<SessionEntry> from_json(const json::Value& value);
};

// The sessions in sessions.json whose process is still running, oldest first.
[[nodiscard]] std::vector<SessionEntry> live_sessions(std::string_view project_root);
// Adds a session to sessions.json, dropping those whose process ended. Only the owner may read
// the file, since it holds tokens.
[[nodiscard]] Result<void> add_session(std::string_view project_root, const SessionEntry& entry);
[[nodiscard]] Result<void> remove_session(std::string_view project_root, std::string_view id);

// The session a watcher means: the one named, else the newest live one, else the newest
// journal. "" when there is none.
[[nodiscard]] std::string pick_session(std::string_view project_root, std::string_view id = {});

// A journal line in plain words, one line, for slash-cli watch --text and work.feed.
[[nodiscard]] std::string describe(const json::Value& line);
// A plan (work.plan) in a few words: its task, how many steps are done, its status and the next
// step.
[[nodiscard]] std::string plan_gist(const json::Value& plan);

// Reads "45s", "90m", "3h" or "2d" as milliseconds.
[[nodiscard]] Result<std::int64_t> parse_duration(std::string_view text);
// A digest's start: now less a duration, or a UTC time as a journal writes one (seconds and
// milliseconds may be left out).
[[nodiscard]] Result<std::int64_t> parse_since(std::string_view text);
// Unix milliseconds of a journal line's "time", or nullopt.
[[nodiscard]] std::optional<std::int64_t> line_time(const json::Value& line);
// A time as a journal writes it: "2026-11-22T13:05:03.120Z".
[[nodiscard]] std::string format_time(std::int64_t unix_milliseconds);

// What the project's sessions did since a time (or one session's, when named): sessions and
// clients, commands, reads and failures, scenes and files changed, entities added, removed and
// changed, the last result of each test command, and each check's state, with "lines", the same
// in a dozen plain lines.
[[nodiscard]] json::Value digest(std::string_view project_root, std::int64_t since,
                                 std::string_view session = {});

// A session's checkpoints, oldest first.
[[nodiscard]] std::vector<Checkpoint> list_checkpoints(std::string_view project_root,
                                                       std::string_view session);
// A checkpoint's frame as PNG bytes.
[[nodiscard]] Result<std::string> checkpoint_frame(std::string_view project_root,
                                                   std::string_view session, std::int64_t n);
// What changed from one checkpoint to another: "files" added, removed and changed (with lines
// added and removed), and "entities" as world::diff_scenes says.
[[nodiscard]] Result<json::Value> compare_checkpoints(std::string_view project_root,
                                                      std::string_view session, std::int64_t from,
                                                      std::int64_t to,
                                                      const world::ComponentRegistry& components);
// Puts the files the session changed back as they were at a checkpoint, first keeping them as
// they are now as a checkpoint of their own. Refuses while any session in the project runs.
// Returns the files it changed, the new checkpoint and the checkpoint's world file.
[[nodiscard]] Result<json::Value> restore_checkpoint(std::string_view project_root,
                                                     std::string_view session, std::int64_t n);
// Takes a checkpoint of a session's world and the files its journal saw written, with a frame
// of the world drawn on the CPU. Fails when the session keeps no journal or is serving a watcher.
[[nodiscard]] Result<Checkpoint> take_checkpoint(Session& session, std::string_view reason,
                                                 std::string_view label);

// work.feed, work.digest, work.checkpoint, work.checkpoints and work.compare.
void register_work_commands(Registry& registry);

} // namespace slash::command::work
