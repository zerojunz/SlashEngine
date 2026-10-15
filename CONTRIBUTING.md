# Contributing to SlashEngine

Thanks for helping build SlashEngine. Contributions from people and from AI agents are both
welcome, and they go through the same review. This guide covers what the engine is for, how to
build and test it, and what a change needs before it can merge.

[AGENTS.md](AGENTS.md) is the detailed day-to-day reference (commands, scenes, the server, how to
add a component or command) and [docs/design.md](docs/design.md) is the full design. Read the
section of the design that covers the area you are changing before you start.

## What SlashEngine is for

SlashEngine is a lightweight, free and open-source C++20 game engine whose primary user is an AI
agent. Every change is judged against these goals:

- **An agent can do everything.** An agent can create, inspect, edit, run and test a game without
  a GUI editor. If something can only be done by hand, that is a bug.
- **The whole project is text.** Scenes and settings are canonical JSON, validated by a schema the
  engine generates from its own components. Nothing binary lives in the source tree.
- **The engine describes itself.** Components and commands carry their own docs, ranges and
  schemas, and `engine.describe` returns them, so an agent learns the engine from the engine.
- **Every action is a command.** Changes to a world go through one command registry, which backs
  the CLI, JSON-RPC, MCP and the REST mirror alike. A failed command changes nothing.
- **Cheap for agents.** Commands return as little as the caller needs and errors point at the bad
  value, so agents spend fewer calls and tokens than they would editing files by hand.
- **Model agnostic.** Nothing depends on a specific AI vendor; local models work as well as
  hosted ones. Claude Code, Codex, Cursor and any other MCP client connect the same way.
- **Lightweight and portable.** Every feature is a module. The engine runs on macOS first, with
  Windows and Linux hosts after launch, and games export to macOS, Windows, Linux, iOS, Android
  and web.
- **Open source only.** The engine is Apache 2.0 and every dependency is under a permissive
  license.

Where a design choice is open, the default is to follow Godot's approach, adapted for agents.
Gameplay code is plain C++; there is no embedded scripting language.

## Set up and build

Requirements: CMake 3.25+, Ninja, and a C++20 compiler (Apple Clang, Clang 16+, GCC 12+ or MSVC
19.3x). Dependencies are fetched by CMake at pinned versions, so nothing else needs installing,
except on Linux, where SDL3 needs the X11 and Wayland development packages listed under
`LINUX_SDL_DEPS` in [.github/workflows/ci.yml](.github/workflows/ci.yml).

```bash
cmake --preset dev               # configure; the first run fetches the dependencies
cmake --build --preset dev       # build the engine, slash-cli, slash-mcp, slash-game and tests
ctest --preset dev               # run unit and smoke tests
```

Then try the engine:

```bash
./build/dev/tools/slash-cli/slash-cli run --headless --ticks 60 --scene tests/data/basic.scene.json
./build/dev/tools/slash-cli/slash-cli run       # opens a window until closed
```

On a machine with no display, set `SDL_VIDEO_DRIVER=dummy` to exercise the windowed path.

## Before you push

CI builds and tests on macOS, Linux and Windows with warnings as errors, then checks formatting and
runs clang-tidy. Run the same checks locally first:

```bash
cmake --preset ci && cmake --build --preset ci && ctest --preset ci
find engine tools tests examples -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
find engine tools examples -name '*.cpp' | xargs clang-tidy -p build/dev
```

If you edited a scene file by hand, validate it and rewrite it in canonical form:

```bash
./build/dev/tools/slash-cli/slash-cli scene validate path/to/level.scene.json
./build/dev/tools/slash-cli/slash-cli scene format path/to/level.scene.json
```

## Coding conventions

- **Language.** C++20. Engine libraries build with no exceptions and no RTTI; fallible calls
  return `slash::Result<T>`. Tools and tests may use exceptions.
- **Invariants.** Use `SLASH_ASSERT`, which logs and aborts in every build type.
- **Logging.** Use `slash::log::info("message", {{"key", value}})`. Engine code never prints
  free-form text to stdout, because tools print JSON there.
- **Names.** `PascalCase` types, `snake_case` functions and variables, `trailing_` private members,
  `kPascalCase` constants.
- **Layout.** Headers in `engine/<module>/include/slash/<module>/`, sources in
  `engine/<module>/src/`. The modules are `core`, `platform`, `asset`, `world`, `storyboard`,
  `command`, `server` and `runtime`.
- **Formatting.** clang-format with the repo's `.clang-format` (4-space indent, 100 columns).
- **Tests.** Every behavior change comes with a test in `tests/`, and tests must pass headless.
- **Examples.** Every new feature comes with a small working example in
  [`examples/`](examples/README.md): C++ code, command calls, or both, with their output checked
  in. CI runs each one and compares the output exactly, so agents can copy them.

## Making changes agent-friendly

Most changes add something an agent will use, so check these when you design it:

- **New behavior is reachable through a command.** Register it in the command registry with a
  short description, its capabilities and a params schema with `"additionalProperties": false`.
  Don't add a feature that only C++ code or a window can reach.
- **New data is reflected.** Components use `SLASH_COMPONENT` and register docs and ranges, so
  scenes can save them, the schema covers them and `engine.describe` lists them.
- **Responses stay small.** Return IDs before full objects and changed fields after an edit.
  Descriptions are sent to models as tool definitions, so keep them short.
- **Errors are actionable.** Report every error with the JSON Pointer of the bad value and, for a
  misspelled name, the closest valid one.
- **Output is deterministic.** Same input, same output: sorted keys, canonical JSON, seeded
  randomness. Agents and tests compare results as text.

[AGENTS.md](AGENTS.md) shows how to add a component and a command step by step.

## Dependencies and licensing

- Add a dependency only when it clearly earns its size. Put it in `cmake/Dependencies.cmake`
  pinned to a tag, and add it with its license to [THIRD_PARTY.md](THIRD_PARTY.md).
- Allowed licenses: MIT, BSD, zlib, Apache 2.0, Boost, MIT-0 and public domain. No GPL or LGPL,
  so games built with SlashEngine can ship under any license.
- Never copy code from closed engines (including Unreal Engine) or their docs. Godot is MIT, so its
  ideas and code may be borrowed with attribution in `THIRD_PARTY.md`.
- Console SDKs are closed and under NDA, so no console code goes in this repo. As with Godot,
  console ports come from licensed third parties.

## Pull requests

- Keep each pull request to one topic, and describe what changes for someone using the engine.
- Include the tests that cover the change. CI must be green on every platform before merge.
- Update [AGENTS.md](AGENTS.md), this guide or the design doc when you change how something is
  built, run or called.
- For a large change, such as a new module, a new dependency or a change to the scene format or
  the command API, open an issue first so the design can be agreed before the code.

## Contributing with an AI agent

Agents are first-class contributors here. Claude Code reads [CLAUDE.md](CLAUDE.md), which loads
[AGENTS.md](AGENTS.md); Codex, Cursor and most other agents read AGENTS.md directly. Point your
agent at those files and at the part of [docs/design.md](docs/design.md) it is changing.

You are responsible for what you submit, whoever wrote it: read the diff, run the checks above,
and make sure nothing in it is copied from code under an incompatible license.

## License

SlashEngine is licensed under Apache 2.0. Unless you say otherwise, any contribution you submit is
licensed under the same terms, as described in section 5 of the [LICENSE](LICENSE).
