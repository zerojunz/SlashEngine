// Work journal: every session that serves an agent writes what its commands did to
// .slash/work/<session>.jsonl, so a person, or another agent, can see what any agent is doing and
// has done, whichever vendor's agent it is.
//
// An agent's session (Claude Code, say) builds a small scene: it creates a floor and a crate,
// looks around, moves the crate, misspells an entity, runs the storyboard check and saves. Then a
// reviewer's session reads that journal back with work.feed, in plain lines and as JSON, and sums
// it up with work.digest. Both journals run on a fixed clock so the output repeats.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace work = slash::command::work;

// 2026-11-22T13:05:00Z, then a second on each time it is read.
work::Clock clock_from(std::int64_t start) {
    auto time = std::make_shared<std::int64_t>(start);
    return [time] {
        const std::int64_t now = *time;
        *time += 1000;
        return now;
    };
}

struct Agent {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<work::Journal> journal;
    std::unique_ptr<command::Session> session;

    Agent(std::string id, std::string host, std::int64_t start) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        work::JournalOptions options;
        options.host = std::move(host);
        options.id = std::move(id);
        options.clock = clock_from(start);
        journal = work::Journal::open(std::move(options)).value();
        command::SessionOptions session_options;
        session_options.capture_logs = false;
        session_options.journal = journal.get();
        session = std::make_unique<command::Session>(components, commands, session_options);
    }
};

// Calls a command in a session and prints it.
command::Reply call(command::Session& session, std::string_view name,
                    std::string_view params = "{}") {
    const command::Reply reply = session.call(name, json::parse(params).value());
    std::printf("%.*s %.*s\n", static_cast<int>(name.size()), name.data(),
                static_cast<int>(params.size()), params.data());
    return reply;
}

void print(const json::Value& value) {
    std::printf("%s\n", json::write(value, json::Style::Compact).c_str());
}

} // namespace

int main() {
    constexpr std::int64_t kStart = 1795352700000; // 2026-11-22T13:05:00Z
    {
        std::printf("-- the agent's session\n");
        Agent agent("20261122-130500-a1b2", "slash-mcp", kStart);
        agent.session->set_client("claude-code");
        call(*agent.session, "entity.create",
             R"({"id": "floor", "components": {"Transform2D": {"position": [0, 200]}}})");
        call(*agent.session, "entity.create",
             R"({"id": "crate", "components": {"Transform2D": {"position": [40, 0]}}})");
        call(*agent.session, "world.query");
        call(*agent.session, "entity.get", R"({"id": "crate"})");
        call(*agent.session, "component.set",
             R"({"id": "crate", "components": {"Transform2D": {"position": [64, 180]}}})");
        call(*agent.session, "entity.get", R"({"id": "crat"})");
        call(*agent.session, "storyboard.check");
        call(*agent.session, "scene.save", R"({"path": "main.scene.json"})");
    } // the session ends; its journal's last line says so

    std::printf("-- a reviewer's session\n");
    Agent reviewer("20261122-131000-c3d4", "slash-cli rpc", kStart + 600000);
    reviewer.session->set_client("codex");
    const command::Reply feed =
        call(*reviewer.session, "work.feed",
             R"({"session": "20261122-130500-a1b2", "text": true, "limit": 50})");
    for (const json::Value& line : feed.value.find("lines")->as_array()) {
        std::printf("  %s\n", line.as_string().c_str());
    }
    command::Reply first = call(*reviewer.session, "work.feed",
                                R"({"session": "20261122-130500-a1b2", "after": 1, "limit": 2})");
    // Each line's "ms" is how long the command took on this machine, so it is left out here.
    for (json::Value& line : first.value["lines"].as_array()) {
        line.as_object().erase("ms");
    }
    print(first.value);
    const command::Reply digest =
        call(*reviewer.session, "work.digest", R"({"since": "2026-11-22T13:00:00Z"})");
    for (const json::Value& line : digest.value.find("lines")->as_array()) {
        std::printf("  %s\n", line.as_string().c_str());
    }
    json::Value summary = digest.value;
    summary.as_object().erase("lines");
    print(summary);
    return 0;
}
