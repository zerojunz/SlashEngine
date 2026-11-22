// Watch window: "slash-cli watch" shows an agent's world as it edits it, with what each command
// touched outlined, so a person can look over the agent's shoulder whichever vendor's agent it
// is.
//
// An agent's session journals its commands and serves itself read-only to watchers, as
// slash-mcp's do. A watcher follows it as the watch window does (server::Follower): the
// journal's new lines, and the world through the session's watch server. It draws that world on
// the software device with a debug::WatchView over it, and prints the bar and where each outline
// went: green round the crate the agent made, amber round the floor it moved, fading a few
// seconds after. Then it reads the same session's watch page, as a browser would.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/json.hpp"
#include "slash/debug/watch_view.hpp"
#include "slash/render/device.hpp"
#include "slash/server/client.hpp"
#include "slash/server/server.hpp"
#include "slash/server/watch.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace debug = slash::debug;
namespace json = slash::json;
namespace server = slash::server;
namespace work = slash::command::work;

constexpr slash::Vec2 kViewport{640.0F, 360.0F};

// A session as a host makes one: its components and commands.
struct Host {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Host(work::Journal* journal = nullptr) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        options.journal = journal;
        session = std::make_unique<command::Session>(components, commands, options);
    }
};

void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value parsed = json::parse(params).value();
    const command::Reply reply = session.call(name, parsed);
    std::printf("agent: %.*s %s%s\n", static_cast<int>(name.size()), name.data(),
                json::write(parsed, json::Style::Compact).c_str(), reply.ok ? "" : "  (failed)");
}

} // namespace

int main() {
    // The agent's session, journaled on a fixed clock and served to watchers.
    work::JournalOptions options;
    options.host = "slash-mcp";
    options.id = "20261122-140000-w1n2";
    auto time = std::make_shared<std::int64_t>(1795356000000); // 2026-11-22T14:00:00Z
    options.clock = [time] { return *time += 1000; };
    auto journal = work::Journal::open(std::move(options)).value();
    Host agent(journal.get());
    auto watched =
        server::Watched::start(*agent.session, journal->id(), "slash-mcp", journal->path()).value();

    // The watcher: a session of its own to show the world in, the view and a frame to draw on.
    Host watcher;
    slash::world::World& shown = watcher.session->world();
    slash::ui::set_viewport(shown, kViewport);
    const auto device = slash::render::create_software_device();
    canvas::CanvasRenderer renderer(*device, watcher.session->assets());
    const auto target =
        device->create_texture({.width = 640, .height = 360, .render_target = true}).value();
    debug::WatchView view(watcher.session->assets());
    server::Follower follower(".", journal->id());
    std::string last;

    // Follows until the world the agent left comes (or, with nothing new, polls once), answering
    // the watcher between the agent's commands as a host does, then draws and prints what the
    // window shows at `seconds`.
    const auto watch = [&](double seconds, bool edited) {
        bool changed = false;
        for (int i = 0; i < (edited ? 400 : 1) && !changed; ++i) {
            journal->flush();
            server::Follower::Update update = follower.poll(std::chrono::milliseconds(0));
            for (const json::Value& line : update.lines) {
                view.touch(line, seconds);
                last = work::describe(line);
            }
            if (update.scene) {
                shown.clear();
                (void)slash::world::load_scene(shown, *update.scene);
                changed = true;
            }
            watched->poll(std::chrono::milliseconds(5));
        }
        view.set_status("Watching " + follower.id() + "  slash-mcp  |  " + last.substr(10));
        canvas::FrameContents contents;
        canvas::CanvasOptions draw;
        draw.target = target;
        draw.width = kViewport.x;
        draw.height = kViewport.y;
        draw.contents = &contents;
        (void)renderer.draw(shown, draw);
        view.update(shown, watcher.session->assets(), contents, kViewport, seconds, changed);
        std::printf("window at %.1f s: %s\n", seconds, view.status().c_str());
        for (const debug::Outline& outline : view.outlines()) {
            std::printf("  %s outline round %s at [%.0f, %.0f] %.0f x %.0f, %.0f%% strong\n",
                        outline.added ? "green" : "amber", outline.id.c_str(), outline.x, outline.y,
                        outline.width, outline.height, std::round(outline.alpha * 100.0F));
        }
    };

    call(*agent.session, "entity.create", R"({"id": "floor", "components": {
        "Transform2D": {"position": [40, 300]},
        "Polygon2D": {"polygon": [[0, 0], [560, 0], [560, 20], [0, 20]]}}})");
    watch(0.0, true);
    call(*agent.session, "entity.create", R"({"id": "crate", "components": {
        "Transform2D": {"position": [280, 252]},
        "Polygon2D": {"polygon": [[0, 0], [48, 0], [48, 48], [0, 48]]}}})");
    call(*agent.session, "component.set",
         R"({"id": "floor", "components": {"Transform2D": {"position": [40, 320]}}})");
    watch(2.0, true);
    // Four seconds on, both outlines are half faded; a second and a half later they are gone.
    watch(6.0, false);
    watch(7.5, false);

    // The same session's watch page, for a browser: "slash-cli watch --browser" opens it. The page
    // needs no token, since it reads it from the link's fragment; its commands need it.
    const work::SessionEntry entry = work::live_sessions(".").front();
    server::Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = std::stoi(entry.url.substr(entry.url.rfind(':') + 1));
    server::HttpRequest request;
    request.method = "GET";
    request.path = "/watch";
    const server::HttpResponse page = server::send(endpoint, request).value();
    std::printf("watch page: %d %s, names %s: %s\n", page.status, page.content_type.c_str(),
                entry.id.c_str(), page.body.find(entry.id) != std::string::npos ? "yes" : "no");
    std::printf("its link: http://127.0.0.1:<port>/watch#token=<token>\n");
    return 0;
}
