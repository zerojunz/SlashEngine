#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A Model Context Protocol server (https://modelcontextprotocol.io) over a session, so Claude
// Code, Codex, Cursor and any other MCP client can drive the engine. Every command is a tool;
// the open scene, the scene schema and recent logs are resources. Like rpc::handle it is
// transport agnostic: slash-mcp feeds it stdio lines, and the running game will serve it over
// HTTP.
//
// tools/list offers only the core commands by default, with tools_load, a directory of every
// command by group that adds the groups or commands an agent asks for to the tools (and tells
// the client with a tools/list_changed notification): OpenAI's API takes at most 128 tools and
// some clients fewer, and every definition is read again on each turn. Every command can still
// be called by its tool name, and batch runs any of them.
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

// Which commands tools/list offers as tools.
enum class Tools : std::uint8_t {
    Core, // the commands most tasks need (core_commands), tools_load and what it loaded
    All,  // every command
};

// The commands offered as tools by default. engine.describe lists the rest, schema.get with kind
// "command" gives one's params, and batch runs it.
[[nodiscard]] std::span<const std::string_view> core_commands();

// The directory tool's name. No command maps to it.
inline constexpr std::string_view kLoadTool = "tools_load";

// Reads "core" or "all", as slash-mcp's --tools takes it.
[[nodiscard]] Result<Tools> parse_tools(std::string_view text);

class Server {
public:
    explicit Server(Session& session, Tools tools = Tools::Core);

    // Handles one JSON-RPC message from the client. Returns the response as one line of compact
    // JSON, or "" for notifications.
    [[nodiscard]] std::string handle(std::string_view message);

    // Runs one MCP method, e.g. "tools/call".
    [[nodiscard]] Reply dispatch(std::string_view method, const json::Value& params);

    // Notifications for the client since the last call, as JSON-RPC lines: tools/list_changed
    // after tools_load added tools. Transports that can send them do so after the response.
    [[nodiscard]] std::vector<std::string> take_notifications();

private:
    [[nodiscard]] Reply initialize(const json::Value& params) const;
    [[nodiscard]] Reply list_tools() const;
    [[nodiscard]] Reply call_tool(const json::Value& params);
    [[nodiscard]] Reply load_tools(const json::Value& arguments);
    [[nodiscard]] json::Value definition(const std::string& name, const Command& command) const;
    [[nodiscard]] bool offered(const Command& command) const;
    [[nodiscard]] static Reply list_resources();
    [[nodiscard]] Reply read_resource(const json::Value& params);

    Session* session_;
    Tools offered_;
    std::set<std::string, std::less<>> loaded_; // commands tools_load added
    std::vector<std::string> notifications_;
    std::map<std::string, const Command*, std::less<>> tools_; // keyed by tool name
};

} // namespace slash::command::mcp
