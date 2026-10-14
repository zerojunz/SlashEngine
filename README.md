# SlashEngine

A video game engine completely built for AI Agents only. Welcome to the future baby!

SlashEngine is a lightweight, free and open-source C++20 game engine designed for agents rather
than manual human input: scenes are plain text, the engine describes itself, and every action is a
command an agent can call over MCP. It is model agnostic and works with Claude Code, Codex, Cursor
or any other MCP client. See [docs/design.md](docs/design.md) for the full design.

**Status:** milestone 2 (core and world). The engine runs an EnTT world on a fixed-timestep loop,
headless or in an SDL3 window, and loads and saves JSON scenes validated against a schema it
generates from its own components. Rendering and the command API come next.

## Quick start

Requirements: CMake 3.25+, Ninja, and a C++20 compiler (Apple Clang, Clang, GCC or MSVC).

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/tools/slash-cli/slash-cli run
./build/dev/tools/slash-cli/slash-cli run --headless --ticks 60 --scene tests/data/basic.scene.json
```

Agents should start with [AGENTS.md](AGENTS.md).

## License

Apache 2.0. Every dependency is under a permissive license; see [THIRD_PARTY.md](THIRD_PARTY.md).
