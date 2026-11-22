// Work checkpoints: a journaled session keeps checkpoints of its work (the files it wrote, the
// world and a frame) after each scene.save, each passing test.run and each work.checkpoint, so a
// person can see how the agent got where it is, compare two points and go back to one.
//
// An agent's session builds a scene and saves it, moves the crate and marks a checkpoint, then
// adds a lamp in a scene of its own and saves both. A reviewer's session lists the checkpoints and
// compares the first with the last, and once no session runs the files go back to the first
// checkpoint, as `slash-cli work restore 1` does. The journal runs on a fixed clock so the output
// repeats.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
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

constexpr std::string_view kSession = "20261122-140000-e5f6";

// 2026-11-22T14:00:00Z, then a second on each time it is read.
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

    Agent(std::string id, std::int64_t start) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        work::JournalOptions options;
        options.host = "slash-mcp";
        options.id = std::move(id);
        options.clock = clock_from(start);
        journal = work::Journal::open(std::move(options)).value();
        command::SessionOptions session_options;
        session_options.capture_logs = false;
        session_options.journal = journal.get();
        session = std::make_unique<command::Session>(components, commands, session_options);
    }
};

// Calls a command in a session, prints it and, when it failed, why.
command::Reply call(command::Session& session, std::string_view name,
                    std::string_view params = "{}") {
    const command::Reply reply = session.call(name, json::parse(params).value());
    std::printf("%.*s %.*s\n", static_cast<int>(name.size()), name.data(),
                static_cast<int>(params.size()), params.data());
    if (!reply.ok) {
        std::printf("  failed: %s\n", reply.message.c_str());
    }
    return reply;
}

void print(const json::Value& value) {
    std::printf("  %s\n", json::write(value, json::Style::Compact).c_str());
}

// Where the crate stands across in a scene file.
std::string crate_x(const std::string& path) {
    auto text = slash::platform::read_file(path);
    if (!text) {
        return "no file";
    }
    const json::Value scene = json::parse(text.value()).value();
    const json::Value* position = scene.find("entities")
                                      ->find("crate")
                                      ->find("components")
                                      ->find("Transform2D")
                                      ->find("position");
    return json::write(position->as_array()[0], json::Style::Compact);
}

} // namespace

int main() {
    constexpr std::int64_t kStart = 1795356000000; // 2026-11-22T14:00:00Z
    {
        std::printf("-- the agent's session\n");
        Agent agent(std::string(kSession), kStart);
        agent.session->set_client("claude-code");
        call(*agent.session, "entity.create",
             R"({"id": "crate", "components": {"Transform2D": {"position": [40, 0]}}})");
        call(*agent.session, "scene.save", R"({"path": "main.scene.json"})");
        call(*agent.session, "component.set",
             R"({"id": "crate", "components": {"Transform2D": {"position": [400, 180]}}})");
        print(call(*agent.session, "work.checkpoint", R"({"label": "crate on the shelf"})").value);
        call(*agent.session, "scene.save", R"({"path": "main.scene.json"})");
        call(*agent.session, "entity.create",
             R"({"id": "lamp", "components": {"Transform2D": {"position": [200, 60]}}})");
        call(*agent.session, "scene.save", R"({"path": "lamp.scene.json"})");
    } // the session ends

    {
        std::printf("-- a reviewer's session\n");
        Agent reviewer("20261122-141000-a7b8", kStart + 600000);
        reviewer.session->set_client("codex");
        const command::Reply list =
            call(*reviewer.session, "work.checkpoints", R"({"session": "20261122-140000-e5f6"})");
        for (const json::Value& checkpoint : list.value.find("checkpoints")->as_array()) {
            print(checkpoint);
        }
        print(call(*reviewer.session, "work.compare",
                   R"({"session": "20261122-140000-e5f6", "from": 1, "to": 4})")
                  .value);
        // A host (slash-mcp, slash-cli rpc) lists its session in sessions.json while it runs.
        work::SessionEntry entry;
        entry.id = "20261122-141000-a7b8";
        entry.host = "slash-cli rpc";
        entry.pid = slash::platform::process_id();
        (void)work::add_session(".", entry);
        std::printf("-- restore while the reviewer's session runs\n");
        auto refused = work::restore_checkpoint(".", kSession, 1);
        std::printf("  %s\n", refused ? "restored" : refused.error().message.c_str());
        (void)work::remove_session(".", entry.id);
    }

    std::printf("-- restore once no session runs, as slash-cli work restore 1\n");
    std::printf("  before: crate at x %s, lamp.scene.json %s\n", crate_x("main.scene.json").c_str(),
                slash::platform::exists("lamp.scene.json") ? "there" : "gone");
    print(work::restore_checkpoint(".", kSession, 1).value());
    std::printf("  after: crate at x %s, lamp.scene.json %s\n", crate_x("main.scene.json").c_str(),
                slash::platform::exists("lamp.scene.json") ? "there" : "gone");
    // The files as they were before the restore are a checkpoint of their own, so it can be undone.
    for (const work::Checkpoint& checkpoint : work::list_checkpoints(".", kSession)) {
        if (checkpoint.reason == "restore") {
            std::printf("  checkpoint %lld: %s\n", static_cast<long long>(checkpoint.n),
                        checkpoint.label.c_str());
        }
    }
    print(work::restore_checkpoint(".", kSession, 5).value());
    std::printf("  after: crate at x %s, lamp.scene.json %s\n", crate_x("main.scene.json").c_str(),
                slash::platform::exists("lamp.scene.json") ? "there" : "gone");
    return 0;
}
