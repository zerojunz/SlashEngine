#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"

#include <functional>
#include <string>
#include <string_view>

// JSON-RPC 2.0 framing (https://www.jsonrpc.org/specification). The adapter is transport
// agnostic: stdio, MCP and (later) HTTP feed it one message at a time.
namespace slash::command {
class Session;
}

namespace slash::command::rpc {

// Runs one method call. A failed Reply becomes an error response.
using Method = std::function<Reply(std::string_view method, const json::Value& params)>;

// Handles one JSON-RPC message: a request, a notification or a batch of them. Returns the
// response as one line of compact JSON, or "" when nothing should be sent back (the message held
// only notifications). Params must be an object or left out; positional params are rejected.
[[nodiscard]] std::string handle(std::string_view message, const Method& method);

// Serves a session's commands: the method name is the command name, e.g. "entity.create".
[[nodiscard]] std::string handle(Session& session, std::string_view message);

} // namespace slash::command::rpc
