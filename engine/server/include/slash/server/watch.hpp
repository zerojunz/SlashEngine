#pragma once

#include "slash/command/work.hpp"
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
