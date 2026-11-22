// Remote viewing: a person away from the machine asks their own agent how another agent's work is
// going, and that agent looks in and steers through the engine, whichever vendor each one is.
//
// The working agent's session journals its commands and serves itself to watchers, as slash-mcp's
// do. A second agent's session (its own journal, as any slash-mcp or slash-cli rpc has) calls
// work.view with "session" naming the first: the call goes through the first session's watch
// server and comes back with its world drawn as it is now, its plan and its last lines. It pauses
// the first agent, leaves it a note and lets it go on, then gets the way there in one picture
// (work.timeline) and a page to hand on (work.report). Once the first session ends, work.view
// shows it from its files. Both journals run on fixed clocks so the output repeats.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/server/watch.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace server = slash::server;
namespace work = slash::command::work;

constexpr std::string_view kWorker = "20261122-140000-y4rd";
constexpr std::int64_t kStart = 1795356000000; // 2026-11-22T14:00:00Z

// A session as a host makes one, journaled on a clock that starts at `start` and moves a second
// each time it is read.
struct Host {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<work::Journal> journal;
    std::unique_ptr<command::Session> session;

    Host(std::string id, std::int64_t start) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        work::JournalOptions options;
        options.host = "slash-mcp";
        options.id = std::move(id);
        auto time = std::make_shared<std::int64_t>(start);
        options.clock = [time] { return *time += 1000; };
        journal = work::Journal::open(std::move(options)).value();
        command::SessionOptions session_options;
        session_options.capture_logs = false;
        session_options.journal = journal.get();
        session = std::make_unique<command::Session>(components, commands, session_options);
    }
};

command::Reply work_call(command::Session& session, std::string_view name,
                         std::string_view params) {
    const command::Reply reply = session.call(name, json::parse(params).value());
    std::printf("worker: %.*s\n", static_cast<int>(name.size()), name.data());
    if (!reply.ok) {
        std::printf("  failed: %s\n", reply.message.c_str());
    }
    const json::Value& carried = reply.ok ? reply.value : reply.data;
    if (const json::Value* notes = carried.find("notes"); notes != nullptr) {
        for (const json::Value& note : notes->as_array()) {
            std::printf("  a note came with it: %s\n", note.find("text")->as_string().c_str());
        }
    }
    return reply;
}

// The size of the PNG a reply carries, or "none".
std::string picture(const json::Value& reply) {
    const json::Value* png = reply.find("png");
    if (png == nullptr) {
        return "none";
    }
    const std::optional<std::string> bytes = slash::base64_decode(png->as_string());
    if (!bytes.has_value()) {
        return "not base64";
    }
    auto image = slash::render::decode_image(bytes.value());
    return std::to_string(image.value().width) + " x " + std::to_string(image.value().height);
}

std::size_t count(std::string_view text, std::string_view what) {
    std::size_t found = 0;
    for (std::size_t at = text.find(what); at != std::string_view::npos;
         at = text.find(what, at + what.size())) {
        ++found;
    }
    return found;
}

} // namespace

int main() {
    // A host installs this at start, so work commands naming another live session reach it.
    server::install_work_forwarder();

    Host helper("20261122-141500-h3lp", kStart + 900000);
    const std::string named = R"("session": ")" + std::string(kWorker) + "\"";
    {
        Host worker(std::string(kWorker), kStart);
        auto watched = server::Watched::start(*worker.session, std::string(kWorker), "slash-mcp",
                                              worker.journal->path())
                           .value();
        work_call(*worker.session, "work.plan",
                  R"({"task": "Build the yard", "steps": ["Lay the floor", "Stack the crates"]})");
        work_call(*worker.session, "entity.create", R"({"id": "floor", "components": {
            "Transform2D": {"position": [40, 300]},
            "Polygon2D": {"polygon": [[0, 0], [560, 0], [560, 20], [0, 20]]}}})");
        work_call(*worker.session, "scene.save", R"({"path": "yard.scene.json"})");
        work_call(*worker.session, "work.plan", R"({"done": [1]})");
        work_call(*worker.session, "entity.create", R"({"id": "crate", "components": {
            "Transform2D": {"position": [280, 252]},
            "Polygon2D": {"polygon": [[0, 0], [48, 0], [48, 48], [0, 48]]}}})");
        work_call(*worker.session, "work.checkpoint", R"({"label": "first crate"})");

        // The helper's calls go through the worker's watch server, which answers between the
        // worker's commands, as slash-mcp does.
        const auto ask = [&](std::string_view name, const std::string& params) {
            std::printf("helper: %.*s\n", static_cast<int>(name.size()), name.data());
            auto future = std::async(std::launch::async, [&] {
                return helper.session->call(name, json::parse(params).value());
            });
            while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                watched->poll(std::chrono::milliseconds(5));
            }
            const command::Reply reply = future.get();
            if (!reply.ok) {
                std::printf("  failed: %s\n", reply.message.c_str());
            }
            return reply;
        };

        const command::Reply view =
            ask("work.view", "{" + named + R"(, "width": 320, "lines": 3})");
        std::printf("  live: %s, frame: %s\n", view.value.find("live")->as_bool() ? "yes" : "no",
                    picture(view.value).c_str());
        std::printf("  plan: %s\n",
                    json::write(*view.value.find("plan"), json::Style::Compact).c_str());
        for (const json::Value& line : view.value.find("recent")->as_array()) {
            std::printf("  %s\n", line.as_string().c_str());
        }

        ask("work.pause", "{" + named + R"(, "why": "Crates go by the gate"})");
        work_call(*worker.session, "entity.create", R"({"id": "crate_2"})");
        ask("work.note", "{" + named + R"(, "text": "The gate is at x 520"})");
        ask("work.resume", "{" + named + "}");
        work_call(*worker.session, "component.set",
                  R"({"id": "crate", "components": {"Transform2D": {"position": [500, 252]}}})");
        work_call(*worker.session, "scene.save", R"({"path": "yard.scene.json"})");

        // Its own session is not another: steering it is a person's job, through a watcher.
        ask("work.pause", R"({"session": "20261122-141500-h3lp", "why": "no"})");
    } // the worker's session ends

    const command::Reply timeline =
        helper.session->call("work.timeline", json::parse("{" + named + R"(, "width": 160,
            "columns": 2})")
                                                  .value());
    std::printf("helper: work.timeline\n  sheet: %s\n", picture(timeline.value).c_str());
    for (const json::Value& checkpoint : timeline.value.find("checkpoints")->as_array()) {
        std::printf("  %s\n", json::write(checkpoint, json::Style::Compact).c_str());
    }

    const command::Reply report =
        helper.session->call("work.report", json::parse("{" + named + "}").value());
    const std::string path = report.value.find("path")->as_string();
    const std::string page = slash::platform::read_file(path).value();
    std::printf("helper: work.report\n  %s: %zu frames, %s\n", path.c_str(), count(page, "<img "),
                page.find("Crates go by the gate") != std::string::npos ? "with the pause"
                                                                        : "no pause");

    const command::Reply after =
        helper.session->call("work.view", json::parse("{" + named + R"(, "lines": 1})").value());
    std::printf("helper: work.view once the worker's session ended\n  live: %s, frame: %s of "
                "checkpoint %lld\n  %s\n",
                after.value.find("live")->as_bool() ? "yes" : "no", picture(after.value).c_str(),
                static_cast<long long>(after.value.find("frame")->find("checkpoint")->as_int()),
                after.value.find("recent")->as_array()[0].as_string().c_str());
    return 0;
}
