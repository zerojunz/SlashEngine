#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"

#include <array>
#include <map>
#include <string>
#include <string_view>

// A Model Context Protocol server (https://modelcontextprotocol.io) over a session, so Claude
// Code, Codex, Cursor and any other MCP client can drive the engine. Every command is a tool;
// the open scene, the scene schema and recent logs are resources. Like rpc::handle it is
// transport agnostic: slash-mcp feeds it stdio lines, and the running game will serve it over
// HTTP.
namespace slash::command {
class Session;
}

namespace slash::command::mcp {

// Protocol versions this server speaks, newest first.
inline constexpr std::array<std::string_view, 4> kProtocolVersions{"2025-11-25", "2025-06-18",
                                                                   "2025-03-26", "2024-11-05"};

// The tool name for a command: dots become underscores ("entity.create" is "entity_create"),
// because several MCP clients only accept letters, digits, '_' and '-' in tool names.
[[nodiscard]] std::string tool_name(std::string_view command);

// What the server tells the client on initialize. Clients pass it to the model, so it says how
// to use the tools well.
[[nodiscard]] std::string_view instructions();

class Server {
public:
    explicit Server(Session& session);

    // Handles one JSON-RPC message from the client. Returns the response as one line of compact
    // JSON, or "" for notifications.
    [[nodiscard]] std::string handle(std::string_view message);

    // Runs one MCP method, e.g. "tools/call".
    [[nodiscard]] Reply dispatch(std::string_view method, const json::Value& params);

private:
    [[nodiscard]] static Reply initialize(const json::Value& params);
    [[nodiscard]] Reply list_tools() const;
    [[nodiscard]] Reply call_tool(const json::Value& params);
    [[nodiscard]] static Reply list_resources();
    [[nodiscard]] Reply read_resource(const json::Value& params);

    Session* session_;
    std::map<std::string, const Command*, std::less<>> tools_; // keyed by tool name
};

} // namespace slash::command::mcp
