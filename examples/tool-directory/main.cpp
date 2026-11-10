// The tools an MCP client starts with, and the directory that adds the rest as a task needs them.
//
// An agent starts with the commands most tasks need and tools_load. Called with no arguments,
// tools_load lists every command by group; with groups or commands, it adds them to the tools,
// returns their params and tells the client (notifications/tools/list_changed) to list the tools
// again. Commands that aren't listed still run by their tool name or through batch.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/mcp.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace mcp = command::mcp;

json::Value call(mcp::Server& server, std::string_view method, std::string_view params) {
    const std::string message = R"({"jsonrpc":"2.0","id":1,"method":")" + std::string(method) +
                                R"(","params":)" + std::string(params) + "}";
    return *json::parse(server.handle(message)).value().find("result");
}

std::string tool_names(mcp::Server& server) {
    std::string names;
    const json::Value listed = call(server, "tools/list", "{}");
    for (const json::Value& tool : listed.find("tools")->as_array()) {
        names += (names.empty() ? "" : " ") + tool.find("name")->as_string();
    }
    return names;
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    mcp::Server server(session);

    std::printf("commands: %zu\n", commands.commands().size());
    std::printf("tools: %s\n", tool_names(server).c_str());

    // The directory: each group's commands by name.
    const json::Value directory = call(server, "tools/call", R"({"name":"tools_load"})");
    const json::Value& groups = *directory.find("structuredContent")->find("groups");
    std::printf("groups: %zu\n", groups.as_object().size());
    std::printf("audio: %s\n", json::write(*groups.find("audio"), json::Style::Compact).c_str());

    // Loading the audio group and one command makes them tools, with their params.
    const json::Value loaded = call(server, "tools/call",
                                    R"({"name":"tools_load","arguments":
                                        {"groups":["audio"],"commands":["light.sample"]}})");
    for (const json::Value& tool : loaded.find("structuredContent")->find("tools")->as_array()) {
        std::printf("loaded %s: %zu params\n", tool.find("name")->as_string().c_str(),
                    tool.find("inputSchema")->find("properties")->as_object().size());
    }
    for (const std::string& notification : server.take_notifications()) {
        std::printf("notification: %s\n", notification.c_str());
    }
    std::printf("tools now: %s\n", tool_names(server).c_str());

    // A misspelled group is named back.
    const json::Value wrong =
        call(server, "tools/call", R"({"name":"tools_load","arguments":{"groups":["phyiscs"]}})");
    std::printf("error: %s\n",
                wrong.find("content")->as_array().front().find("text")->as_string().c_str());
    return 0;
}
