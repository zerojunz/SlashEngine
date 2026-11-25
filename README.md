# SlashEngine

A video game engine completely built for AI Agents only. Welcome to the future baby!

SlashEngine is a lightweight, free and open-source C++20 game engine designed for agents rather
than manual human input: scenes are plain text, the engine describes itself, and every action is a
command an agent can call over MCP. It is model agnostic and works with Claude Code, Codex, Cursor,
opencode, Antigravity, Hermes Agent, OpenClaw or any other MCP client.

**Status:** milestones 1 to 8 are done, and so is every feature planned for the launch release,
including the [agent-built worlds](docs/roadmap.md#agent-built-worlds-and-smart-npcs) tool set;
the published release is what remains (see the [roadmap](docs/roadmap.md)). The engine covers
full 2D and 3D feature sets (clustered and compatibility renderers, ray tracing, physics on
Box2D and Jolt, navigation, animation, UI, audio, video, localization, accessibility, networking
and XR) and tools for agents to build whole worlds: procedural graphs, foliage, wind, characters
with faces and anatomy, animals, vehicles, NPCs with plans and stories, destruction, visual
effects, fluids and water, each in 2D and 3D. Scenes are JSON validated against a schema the
engine generates from its own components. Agents drive it through a command API with undo,
served over MCP (`slash-mcp`), JSON-RPC, a localhost REST API with an OpenAPI document, and the
command line, including while the game runs. Games import from Godot, Unity and Unreal Engine
5, and `slash-cli export` builds and signs a game for macOS, Windows, Linux, the web, Android or
iOS in one command, using presets in `export_presets.json`. One Mac builds every platform, with
toolchains `slash-cli` downloads, and the tools themselves run on macOS, Linux and Windows.

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
for Claude Code, Codex, Cursor, opencode and Antigravity (and Hermes Agent's and OpenClaw's entries), an `AGENTS.md`, an empty main scene, a `src/` folder for the game's C++, a first headless test and a storyboard. With `slash-mcp` on your
`PATH`, the agent connects on its next start. Claude Code can also add it by hand:

```bash
claude mcp add slashengine -- slash-mcp --project .
```

To change a game while it plays, start it with `slash-cli run --serve` and connect the agent with
`slash-mcp --live`:

```bash
claude mcp add slashengine-live -- slash-mcp --live --project .
```

To build a game for players, export it from its folder. `--test` runs the result headless:

```bash
slash-cli export macos --test        # or windows, linux, web, android, ios
```

## Layout

| Folder | What lives there |
| --- | --- |
| `engine/` | Engine libraries, one folder per module ([modules](docs/reference/engine-modules.md)) |
| `tools/` | `slash-cli`, `slash-mcp`, `slash-game`, exporter, importer, shader compiler, bench |
| `tests/`, `examples/` | Unit and smoke tests; one checked example per feature |
| `docs/` | Getting started, concepts, [design](docs/design.md), [roadmap](docs/roadmap.md), reference pages |

## Docs

[docs/README.md](docs/README.md) indexes every doc: a getting-started guide, the reference pages and the roadmap. Agents working on the engine itself start with [AGENTS.md](AGENTS.md).

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache 2.0. Every dependency is under a permissive license; see [THIRD_PARTY.md](THIRD_PARTY.md).
