#include "init.hpp"

#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace slash::cli {
namespace {

constexpr std::string_view kMainScene = "scenes/main.scene.json";

constexpr std::string_view kAgents = R"(# AGENTS.md

This is a SlashEngine game project. SlashEngine is built for agents: you inspect and change the
game through engine commands instead of editing files by hand.

## Connect to the engine

The `slashengine` MCP server is configured for Claude Code (`.mcp.json`), Codex
(`.codex/config.toml`) and Cursor (`.cursor/mcp.json`), and opens `scenes/main.scene.json` at
startup. Its tools check every change against the engine's schemas, support undo and return
compact results. Use them rather than editing `.scene.json` files directly: the server holds the
open scene, so a hand edit is overwritten the next time the scene is saved.

Without MCP, the same commands run from the shell:

```bash
slash-cli call engine.describe
slash-cli call world.query '{"with": ["Transform2D"]}' --scene scenes/main.scene.json
slash-cli call batch '{"commands": [...]}' --scene scenes/main.scene.json --save
```

## Drive a running game

`slash-cli run --serve` plays the game and serves the same commands over HTTP on this machine,
so you can inspect and change the live world. `slash-mcp --live --project .` connects an MCP
client to it; for Claude Code: `claude mcp add slashengine-live -- slash-mcp --live --project .`.
The address and token are in `.slash/server.json`, which stays out of git.

## Work efficiently

- Call `engine_describe` once to learn the components (with default field values) and commands.
- Group related edits in one `batch` call. It is all or nothing, and one `undo` reverts it.
- `world_query` returns only entity IDs unless you pass `include`. Filter with `where`.
- `component_set` returns only the fields that changed.
- Errors give the JSON Pointer of the bad value and suggest the right name; fix it and retry.
- Call `scene_save` when you are done, then `slash-cli scene validate <file>` to double-check.

## Files

- `scenes/`: scenes as canonical JSON, keyed by stable entity ID. Run
  `slash-cli scene format <file>` before committing a scene edited outside the engine.
)";

constexpr std::string_view kSkill = R"(---
name: slashengine
description: Build or change scenes in this SlashEngine game through the slashengine MCP tools. Use when creating scenes, or adding, finding or editing entities and their components.
---

# Editing SlashEngine scenes

1. Call `engine_describe` to see the components, their default field values and the open scene.
2. Open a scene with `scene_open`, or start one with `scene_new` and a path.
3. Make related changes in one `batch` call, for example:

   ```json
   {"commands": [
     {"command": "entity.create",
      "params": {"id": "player", "components": {"Transform2D": {"position": [64, 32]}}}},
     {"command": "component.set",
      "params": {"id": "enemy_01", "components": {"Transform2D": {"rotation": 1.57}}}}
   ]}
   ```

4. Check the result with `world_query` (IDs only unless you pass `include`) or `entity_get`.
5. If something is wrong, `undo` reverts the whole batch.
6. Call `scene_save`.

Do not edit `.scene.json` files by hand while the server runs: it holds the open scene and would
overwrite the edit on the next save.
)";

// Engine state that stays on this machine: caches and the running server's token.
constexpr std::string_view kIgnoredFolder = ".slash/";
constexpr std::string_view kGitignore =
    "# SlashEngine caches and the running server's token\n.slash/\n";

bool has_line(std::string_view text, std::string_view line) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view current = text.substr(start, end - start);
        if (!current.empty() && current.back() == '\r') {
            current.remove_suffix(1);
        }
        if (current == line) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

json::Value server_entry(const InitOptions& options, std::string_view project) {
    json::Value entry;
    entry["command"] = options.mcp_command;
    entry["args"] = json::Array{"--project", project, "--scene", kMainScene};
    return entry;
}

std::string json_config(const InitOptions& options, std::string_view project) {
    json::Value config;
    config["mcpServers"]["slashengine"] = server_entry(options, project);
    return json::write(config) + "\n";
}

std::string toml_string(std::string_view text) {
    return "\"" + log::escape_json(text) + "\"";
}

std::string codex_config(const InitOptions& options) {
    return "[mcp_servers.slashengine]\ncommand = " + toml_string(options.mcp_command) +
           "\nargs = [\"--project\", \".\", \"--scene\", " + toml_string(kMainScene) + "]\n";
}

std::string empty_scene() {
    const world::ComponentRegistry components;
    const world::World scene(components);
    return json::write(world::save_scene(scene)) + "\n";
}

} // namespace

Result<json::Value> init_project(const InitOptions& options) {
    // Cursor expands ${workspaceFolder}; Claude Code and Codex start servers in the project.
    const std::vector<std::pair<std::string_view, std::string>> files{
        {"AGENTS.md", std::string(kAgents)},
        {"CLAUDE.md", "@AGENTS.md\n"},
        {".mcp.json", json_config(options, ".")},
        {".cursor/mcp.json", json_config(options, "${workspaceFolder}")},
        {".codex/config.toml", codex_config(options)},
        {".claude/skills/slashengine/SKILL.md", std::string(kSkill)},
        {kMainScene, empty_scene()},
    };

    std::string root = options.directory;
    if (!root.empty() && root.back() != '/' && root.back() != '\\') {
        root += '/';
    }
    json::Array created;
    json::Array skipped;
    for (const auto& [relative, contents] : files) {
        const std::string path = root + std::string(relative);
        if (!options.force && platform::exists(path)) {
            skipped.emplace_back(relative);
            continue;
        }
        if (const std::size_t slash = path.find_last_of('/'); slash != std::string::npos) {
            if (auto made = platform::create_directories(path.substr(0, slash)); !made) {
                return made.error();
            }
        }
        if (auto written = platform::write_file(path, contents); !written) {
            return written.error();
        }
        created.emplace_back(relative);
    }
    // .gitignore is never replaced, even with --force: the line is added if it is missing.
    const std::string gitignore = root + ".gitignore";
    json::Array updated;
    if (!platform::exists(gitignore)) {
        if (auto written = platform::write_file(gitignore, kGitignore); !written) {
            return written.error();
        }
        created.emplace_back(".gitignore");
    } else if (auto text = platform::read_file(gitignore); !text) {
        return text.error();
    } else if (!has_line(text.value(), kIgnoredFolder)) {
        std::string contents = text.value();
        if (!contents.empty() && contents.back() != '\n') {
            contents += '\n';
        }
        contents += std::string(kIgnoredFolder) + "\n";
        if (auto written = platform::write_file(gitignore, contents); !written) {
            return written.error();
        }
        updated.emplace_back(".gitignore");
    } else {
        skipped.emplace_back(".gitignore");
    }

    json::Object result;
    result.emplace("created", std::move(created));
    result.emplace("skipped", std::move(skipped));
    if (!updated.empty()) {
        result.emplace("updated", std::move(updated));
    }
    return json::Value(std::move(result));
}

} // namespace slash::cli
