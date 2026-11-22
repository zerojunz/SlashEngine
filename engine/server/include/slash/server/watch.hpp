#pragma once

#include "slash/command/command.hpp"
#include "slash/command/work.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/server/server.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace slash::command {
class Session;
} // namespace slash::command

// Sessions that serve themselves to watchers: a read-only server over the session on the loopback
// interface, listed in .slash/work/sessions.json with its URL and token until it ends, so
// "slash-cli watch" and other tools on the machine can see the agent's world and journal as it
// works. Watchers' requests run between the agent's commands, as every server's do, so watching
// never races it.
namespace slash::server {

class Watched {
public:
    // Starts the session's watch server and lists it. id names it in sessions.json: the journal's
    // ID, so watchers find the journal by it.
    [[nodiscard]] static Result<std::unique_ptr<Watched>>
    start(command::Session& session, std::string id, std::string host, std::string journal = {});
    // Stops serving and takes the session out of sessions.json.
    ~Watched();
    Watched(const Watched&) = delete;
    Watched& operator=(const Watched&) = delete;
    Watched(Watched&&) = delete;
    Watched& operator=(Watched&&) = delete;

    [[nodiscard]] const command::work::SessionEntry& entry() const noexcept { return entry_; }
    // Runs the watchers' requests waiting; hosts call it between commands.
    void poll(std::chrono::milliseconds wait = std::chrono::milliseconds(0));

private:
    Watched(std::string project_root, std::unique_ptr<Server> server,
            command::work::SessionEntry entry);

    std::string project_root_;
    std::unique_ptr<Server> server_;
    command::work::SessionEntry entry_;
};

// Runs a command on a live session through its watch server, as a watcher: what slash-cli work
// pause, resume and note send. Fails when the session doesn't serve or doesn't answer; a reply
// with an error is a reply, not a failure.
[[nodiscard]] Result<command::Reply> call_watched(const command::work::SessionEntry& entry,
                                                  std::string_view command,
                                                  const json::Value& params);

// Lets this program's sessions reach other sessions through call_watched: work.pause, work.note,
// work.resume and work.view with "session", and slash-mcp --watch (command::work::set_forwarder).
void install_work_forwarder();

// What a host keeps for its session's watchers, by project.json's "work" block: the journal the
// session writes (made before the session, which takes it in SessionOptions::journal) and,
// once the session is made, its watch server. Either can be off, and one that can't start is
// logged and left out, as the work goes on without it.
class Work {
public:
    Work(std::string project_root, std::string host);
    ~Work();
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;
    Work(Work&&) = delete;
    Work& operator=(Work&&) = delete;

    // For SessionOptions::journal; null when there is none.
    [[nodiscard]] command::work::Journal* journal() const noexcept { return journal_.get(); }
    // Serves the session to watchers, if the settings say so.
    void watch(command::Session& session);
    // Runs watchers' requests and writes the journal's waiting reads; hosts call it while they
    // wait for the agent.
    void poll();

private:
    std::string project_root_;
    std::string host_;
    command::work::Settings settings_;
    std::unique_ptr<command::work::Journal> journal_;
    std::unique_ptr<Watched> watched_;
};

// Follows a session for "slash-cli watch": the lines its journal gains and, through its watch
// server, its world as scene.get gives it. The journal is read again only when its files grow.
class Follower {
public:
    // With world off, only the journal is read.
    Follower(std::string project_root, std::string id, bool world = true);
    // Leaves a fetch still waiting on the session to finish on its own.
    ~Follower();
    Follower(const Follower&) = delete;
    Follower& operator=(const Follower&) = delete;
    Follower(Follower&&) = delete;
    Follower& operator=(Follower&&) = delete;

    struct Update {
        std::vector<json::Value> lines;   // journal lines written since the last poll, in order
        std::optional<json::Value> scene; // the session's scene file, when it changed
        // The session ended: its last line came, or its process ended with nothing more written.
        bool ended = false;
    };
    // Reads what is new. The world is asked for when the journal grew, and otherwise at most
    // every `refresh`, as a running game moves things without commands; never with refresh 0
    // and nothing written. The session answers between the agent's commands, so the request
    // runs on a thread of its own and its answer comes with a later poll.
    [[nodiscard]] Update poll(std::chrono::milliseconds refresh = std::chrono::seconds(1));

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    // The session's entry in sessions.json, once a poll has found it live.
    [[nodiscard]] const std::optional<command::work::SessionEntry>& entry() const noexcept {
        return entry_;
    }

private:
    std::string project_root_;
    std::string id_;
    bool world_ = true;
    std::optional<command::work::SessionEntry> entry_;
    std::int64_t seen_ = 0;
    std::uint64_t bytes_ = 0;
    bool read_once_ = false;
    bool ended_ = false;
    std::string scene_text_;
    std::chrono::steady_clock::time_point fetched_;
    bool wanted_ = false; // the journal grew since the last fetch began
    struct Fetch {
        std::mutex mutex;
        bool done = false;
        std::optional<std::string> body; // scene.get's answer, when it came
    };
    std::shared_ptr<Fetch> fetch_;
    std::thread fetcher_;
};

// Lines from stdin, read on a thread of their own, so a host speaking JSON-RPC or MCP over
// stdio can serve watchers while it waits for the agent's next message.
class StdinLines {
public:
    StdinLines();
    // Waits for the reader if stdin has ended, else leaves it blocked in its read, which ends
    // with the process.
    ~StdinLines();
    StdinLines(const StdinLines&) = delete;
    StdinLines& operator=(const StdinLines&) = delete;
    StdinLines(StdinLines&&) = delete;
    StdinLines& operator=(StdinLines&&) = delete;

    // The next line, waiting up to `wait` for one; nullopt if none came, or stdin ended.
    [[nodiscard]] std::optional<std::string> next(std::chrono::milliseconds wait);
    // Whether stdin ended and every line was taken.
    [[nodiscard]] bool ended() const;

private:
    struct Shared {
        mutable std::mutex mutex;
        std::condition_variable arrived;
        std::deque<std::string> lines;
        bool ended = false;
    };
    std::shared_ptr<Shared> shared_;
    std::thread reader_;
};

} // namespace slash::server
