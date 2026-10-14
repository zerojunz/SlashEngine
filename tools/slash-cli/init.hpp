#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <string>

namespace slash::cli {

struct InitOptions {
    std::string directory = ".";
    std::string mcp_command = "slash-mcp"; // how agents launch the MCP server
    bool force = false;                    // replace files that already exist
};

// Sets up a game project for agents: a project.json named after the folder, MCP configs for
// Claude Code (.mcp.json), Codex (.codex/config.toml) and Cursor (.cursor/mcp.json), an AGENTS.md
// with a CLAUDE.md pointing to it, a Claude Code skill, a GitHub Actions workflow that exports
// every platform, an empty main scene and .gitignore entries for .slash/ and build/. Returns
// {"created": [...], "skipped": [...]} plus "updated" when an existing .gitignore gained entries,
// with paths relative to the directory.
[[nodiscard]] Result<json::Value> init_project(const InitOptions& options);

} // namespace slash::cli
