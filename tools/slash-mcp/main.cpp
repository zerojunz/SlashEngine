// slash-mcp: serves SlashEngine to MCP clients such as Claude Code, Codex and Cursor over stdio.
// Each line on stdin is one JSON-RPC message and each response is one line on stdout. Logs go to
// stderr as JSON lines.

#include "slash/ai/commands.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/mcp.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/version.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/gameplay.hpp"
#include "slash/runtime/plugins.hpp"
#include "slash/server/client.hpp"
#include "slash/server/server.hpp"
#include "slash/server/watch.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"
#include "slash/soundtrack/commands.hpp"
#include "slash/video/commands.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace command = slash::command;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;

constexpr std::string_view kUsage = R"(slash-mcp: serves SlashEngine to MCP clients over stdio

Usage:
  slash-mcp [options]
      --project <dir>        Project folder; scene paths are relative to it (default ".")
      --scene <file>         Open this scene at startup, or start a new one saved there
      --allow <list>         Capabilities to grant: read, edit, run, filesystem, build,
                             network or all (default all), e.g. --allow read,run
      --tools <set>          Tools to offer: core (default), the commands most tasks need and
                             tools_load, which adds others by group, or all
      --live                 Drive the game already running in the project ("slash-cli run
                             --serve" or "slash-cli serve") instead of an engine of its own
      --watch                Look in on another agent's session in the project instead: only
                             the work tools (view, digest, timeline, report, pause, note...),
                             none of which changes its game
      --session <id>         With --watch, the session to watch (default the newest running)
      --version              Print the engine version
      --help                 Show this message

Claude Code:  claude mcp add slashengine -- slash-mcp --project .
Watching:     claude mcp add slashengine-watch -- slash-mcp --project . --watch
Or run "slash-cli init" in a project to write the configs for Claude Code, Codex and Cursor.
)";

int usage_error(std::string_view message) {
    slash::log::error("usage error", {{"detail", message}});
    std::fwrite(kUsage.data(), 1, kUsage.size(), stderr);
    return kExitUsage;
}

void send(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Whether a forwarded message was a tools_load call that loaded tools.
bool loaded_tools(std::string_view message, std::string_view response) {
    auto request = slash::json::parse(message);
    auto reply = slash::json::parse(response);
    if (!request || !reply) {
        return false;
    }
    const slash::json::Value* method = request.value().find("method");
    const slash::json::Value* params = request.value().find("params");
    if (method == nullptr || !method->is_string() || method->as_string() != "tools/call" ||
        params == nullptr || !params->is_object()) {
        return false;
    }
    const slash::json::Value* name = params->find("name");
    const slash::json::Value* arguments = params->find("arguments");
    const slash::json::Value* result = reply.value().find("result");
    const slash::json::Value* error = result != nullptr ? result->find("isError") : nullptr;
    return name != nullptr && name->is_string() && name->as_string() == command::mcp::kLoadTool &&
           arguments != nullptr && arguments->is_object() && !arguments->as_object().empty() &&
           error != nullptr && error->is_bool() && !error->as_bool();
}

// A tools/call message with "session" set to the watched session where the call names none;
// work.digest is left to cover every session. Other messages as they are.
std::string with_session(const std::string& message, const std::string& session) {
    if (session.empty()) {
        return message;
    }
    auto parsed = slash::json::parse(message);
    if (!parsed || !parsed.value().is_object()) {
        return message;
    }
    slash::json::Value request = std::move(parsed).value();
    const slash::json::Value* method = request.find("method");
    slash::json::Value* params = request.find("params");
    if (method == nullptr || !method->is_string() || method->as_string() != "tools/call" ||
        params == nullptr || !params->is_object()) {
        return message;
    }
    const slash::json::Value* name = params->find("name");
    if (name == nullptr || !name->is_string() || name->as_string() == "work_digest") {
        return message;
    }
    slash::json::Value& arguments = (*params)["arguments"];
    if (arguments.is_null()) {
        arguments = slash::json::Object{};
    }
    if (!arguments.is_object() || arguments.find("session") != nullptr) {
        return message;
    }
    arguments["session"] = session;
    return slash::json::write(request, slash::json::Style::Compact);
}

// slash-mcp --watch: the work tools over a session of its own that changes nothing, each call
// looking at the watched session, which it reaches through its watch server.
int serve_watcher(const std::string& project, const std::string& pinned) {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project;
    options.capabilities = {command::Capability::Read};
    command::Session session(components, commands, options);
    command::mcp::Server server(session, command::mcp::Tools::Watch);
    slash::log::info("slash-mcp watching", {{"project", project}, {"session", pinned}});
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        // The newest running session for each call, unless one was named, so a new agent's
        // session is picked up.
        const std::string watched = pinned.empty() ? command::work::pick_session(project) : pinned;
        if (const std::string response = server.handle(with_session(line, watched));
            !response.empty()) {
            send(response);
        }
    }
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    // export.run's Linux exports run this program as their wayland-scanner (tools/exporter).
    if (argc > 1 && std::string_view(argv[1]) == slash::exporter::kWaylandScanner) {
        return slash::exporter::run_wayland_scanner(argc - 1, argv + 1);
    }
    slash::shader_compiler::install();       // for materials with a game's own shader
    slash::server::install_work_forwarder(); // work tools reach other sessions
    const std::vector<std::string> args(argv + 1, argv + argc);
    command::SessionOptions options;
    std::string scene;
    bool live = false;
    bool watch = false;
    std::string watched;
    bool restricted = false;
    command::mcp::Tools tools = command::mcp::Tools::Core;
    bool tools_set = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--help" || arg == "-h") {
            std::fwrite(kUsage.data(), 1, kUsage.size(), stdout);
            return kExitOk;
        }
        if (arg == "--version") {
            send(R"({"name":"SlashEngine","version":")" + std::string(slash::version()) + "\"}");
            return kExitOk;
        }
        if (arg == "--live") {
            live = true;
        } else if (arg == "--watch") {
            watch = true;
        } else if (arg == "--session" && has_value) {
            watched = args[++i];
        } else if (arg == "--project" && has_value) {
            options.project_root = args[++i];
        } else if (arg == "--scene" && has_value) {
            scene = args[++i];
        } else if (arg == "--tools" && has_value) {
            auto parsed = command::mcp::parse_tools(args[++i]);
            if (!parsed) {
                return usage_error(parsed.error().message);
            }
            tools = parsed.value();
            tools_set = true;
        } else if (arg == "--allow" && has_value) {
            auto capabilities = command::parse_capabilities(args[++i]);
            if (!capabilities) {
                return usage_error(capabilities.error().message);
            }
            options.capabilities = std::move(capabilities).value();
            restricted = true;
        } else {
            return usage_error("unknown option or missing value: " + arg);
        }
    }
    if (live && !scene.empty()) {
        return usage_error("--scene has no effect with --live; the running game has its scene");
    }
    if (live && restricted) {
        return usage_error("--allow has no effect with --live; pass it to \"slash-cli serve\"");
    }
    if (live && tools_set) {
        return usage_error("--tools has no effect with --live; the running game offers core "
                           "tools and tools_load");
    }
    if (watch && (live || !scene.empty() || restricted || tools_set)) {
        return usage_error("--watch takes only --project and --session: it looks in on another "
                           "session and changes nothing");
    }
    if (!watched.empty() && !watch) {
        return usage_error("--session needs --watch");
    }
    if (!slash::platform::exists(options.project_root)) {
        return usage_error("the project folder does not exist: " + options.project_root);
    }
    if (watch) {
        return serve_watcher(options.project_root, watched);
    }

    std::string line;
    if (live) {
        // The endpoint is read for every message, so a restarted game is picked up.
        slash::log::info("slash-mcp forwarding to the running game",
                         {{"project", options.project_root}});
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.find_first_not_of(" \t") == std::string::npos) {
                continue;
            }
            const std::string response = slash::server::forward_mcp(
                slash::server::read_endpoint(options.project_root), line);
            if (!response.empty()) {
                send(response);
            }
            // HTTP gives the game no way to tell the client its tools changed, so say it here.
            if (loaded_tools(line, response)) {
                send(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
            }
        }
        return kExitOk;
    }

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::exporter::register_export_commands(commands);
    slash::exporter::register_api_commands(commands);
    slash::importer::register_import_commands(commands);
    slash::ai::register_commands(commands);
    slash::library::register_commands(commands);
    slash::soundtrack::register_commands(commands);
    slash::video::register_commands(commands);
    // The project's own C++ code, loaded again whenever gameplay.build rebuilds it.
    slash::gameplay::Host gameplay(components, slash::gameplay::library_path(options.project_root));
    options.gameplay = &gameplay;
    // The project's plugins, loaded before its scenes open.
    slash::plugin::Plugins plugins(components);
    (void)slash::runtime::load_plugins(plugins, options.project_root);
    options.plugins = &plugins;
    // The work journal, and the read-only server watchers attach to (slash-cli watch).
    slash::server::Work work(options.project_root, "slash-mcp");
    options.journal = work.journal();
    command::Session session(components, commands, options);
    work.watch(session);
    // Until the code is built, its components are stand-ins, so its scenes still open.
    slash::runtime::add_stand_ins(gameplay, options.project_root);

    if (!scene.empty()) {
        // Keep serving even if the scene is broken, so the agent can see the error in the log
        // and fix the file.
        if (const command::Reply opened = command::open_or_new_scene(session, scene); !opened.ok) {
            slash::log::error("could not open the scene",
                              {{"path", scene}, {"detail", opened.message}});
        }
    }

    command::mcp::Server server(session, tools);
    slash::log::info("slash-mcp ready",
                     {{"project", options.project_root}, {"tools", commands.commands().size()}});

    // Watchers are served while the agent thinks, between its messages.
    slash::server::StdinLines input;
    constexpr std::chrono::milliseconds kWatchPoll{50};
    while (!input.ended()) {
        work.poll();
        const std::optional<std::string> next = input.next(kWatchPoll);
        if (!next || next->find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        if (const std::string response = server.handle(*next); !response.empty()) {
            send(response);
        }
        for (const std::string& notification : server.take_notifications()) {
            send(notification);
        }
    }
    return kExitOk;
}
