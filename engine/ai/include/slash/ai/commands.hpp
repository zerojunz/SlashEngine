#pragma once

#include "slash/ai/provider.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/result.hpp"

#include <span>
#include <string>
#include <vector>

// The engine's commands as tools for a model, and the ai.* commands. These live in
// slash_ai_commands, apart from slash_ai, so a game that calls a model needs no command registry.
namespace slash::ai {

// Tools for the named commands, with the names MCP gives them ('_' for '.') and their params
// schemas, so a model in the game has the same powers as an agent over MCP. An unknown name fails
// with the closest one as a hint.
[[nodiscard]] Result<std::vector<ToolSpec>> command_tools(const command::Registry& registry,
                                                          std::span<const std::string> names);

// Runs each tool call in the message as the command it names, in the session, and returns a Tool
// message per call with the command's result or error as JSON text.
[[nodiscard]] std::vector<Message> run_tool_calls(command::Session& session,
                                                  const Message& message);

// ai.providers, ai.complete, storyboard.review and l10n.translate.
void register_commands(command::Registry& registry);

} // namespace slash::ai
