// slash-mcp: serves SlashEngine to MCP clients such as Claude Code, Codex and Cursor over stdio.
// Each line on stdin is one JSON-RPC message and each response is one line on stdout. Logs go to
// stderr as JSON lines.

#include "slash/ai/commands.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/mcp.hpp"
#include "slash/command/session.hpp"
#include "slash/core/log.hpp"
#include "slash/core/version.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/gameplay.hpp"
#include "slash/server/client.hpp"
#include "slash/server/server.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"
#include "slash/soundtrack/commands.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <iostream>
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
      --live                 Drive the game already running in the project ("slash-cli run
                             --serve" or "slash-cli serve") instead of an engine of its own
      --version              Print the engine version
      --help                 Show this message

Claude Code:  claude mcp add slashengine -- slash-mcp --project .
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

} // namespace

int main(int argc, char** argv) {
    // export.run's Linux exports run this program as their wayland-scanner (tools/exporter).
    if (argc > 1 && std::string_view(argv[1]) == slash::exporter::kWaylandScanner) {
        return slash::exporter::run_wayland_scanner(argc - 1, argv + 1);
    }
    slash::shader_compiler::install(); // for materials with a game's own shader
    const std::vector<std::string> args(argv + 1, argv + argc);
    command::SessionOptions options;
    std::string scene;
    bool live = false;
    bool restricted = false;
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
        } else if (arg == "--project" && has_value) {
            options.project_root = args[++i];
        } else if (arg == "--scene" && has_value) {
            scene = args[++i];
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
    if (!slash::platform::exists(options.project_root)) {
        return usage_error("the project folder does not exist: " + options.project_root);
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
    // The project's own C++ code, loaded again whenever gameplay.build rebuilds it.
    slash::gameplay::Host gameplay(components, slash::gameplay::library_path(options.project_root));
    options.gameplay = &gameplay;
    command::Session session(components, commands, options);
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

    command::mcp::Server server(session);
    slash::log::info("slash-mcp ready",
                     {{"project", options.project_root}, {"tools", commands.commands().size()}});

    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        if (const std::string response = server.handle(line); !response.empty()) {
            send(response);
        }
    }
    return kExitOk;
}
