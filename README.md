# SlashEngine

A video game engine completely built for AI Agents only. Welcome to the future baby!

SlashEngine is a lightweight, free and open-source C++20 game engine designed for agents rather
than manual human input: scenes are plain text, the engine describes itself, and every action is a
command an agent can call over MCP. It is model agnostic and works with Claude Code, Codex, Cursor
or any other MCP client. See [docs/design.md](docs/design.md) for the full design.

**Status:** milestone 3 (commands and MCP). The engine runs an EnTT world on a fixed-timestep loop,
headless or in an SDL3 window, and loads and saves JSON scenes validated against a schema it
generates from its own components. Agents drive it through a command API with undo, served over
MCP (`slash-mcp`), JSON-RPC, a localhost REST API with an OpenAPI document, and the command line,
including while the game runs. The export pipeline comes next.

## Quick start

Requirements: CMake 3.25+, Ninja, and a C++20 compiler (Apple Clang, Clang, GCC or MSVC).

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/tools/slash-cli/slash-cli run
./build/dev/tools/slash-cli/slash-cli run --headless --ticks 60 --scene tests/data/basic.scene.json
```

To let an agent edit a game, run `slash-cli init` in the game's folder. It writes the MCP configs
for Claude Code, Codex and Cursor, an `AGENTS.md` and an empty main scene. With `slash-mcp` on your
`PATH`, the agent connects on its next start. Claude Code can also add it by hand:

```bash
claude mcp add slashengine -- slash-mcp --project .
```

To change a game while it plays, start it with `slash-cli run --serve` and connect the agent with
`slash-mcp --live`:

```bash
claude mcp add slashengine-live -- slash-mcp --live --project .
```

To contribute, read [CONTRIBUTING.md](CONTRIBUTING.md). Agents working on the engine itself should
start with [AGENTS.md](AGENTS.md).

## License

Apache 2.0. Every dependency is under a permissive license; see [THIRD_PARTY.md](THIRD_PARTY.md).
