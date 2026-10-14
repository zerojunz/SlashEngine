# AGENTS.md

Instructions for AI agents (Claude Code, Codex, Cursor and others) working on SlashEngine. Read
[docs/design.md](docs/design.md) for the full design; this file covers the day-to-day loop.

## Build and test

```bash
cmake --preset dev               # configure (first run fetches the dependencies)
cmake --build --preset dev       # build engine, slash-cli and tests
ctest --preset dev               # run unit and smoke tests
```

The `ci` preset is what CI runs: same build with warnings as errors. Use it before you push.

On Linux, SDL3 needs the X11/Wayland development packages listed in
`.github/workflows/ci.yml`.

## Run the engine

```bash
cli=./build/dev/tools/slash-cli/slash-cli
$cli version                                   # {"name":"SlashEngine","version":"..."}
$cli run --headless --ticks 120                # no window or GPU
$cli run --headless --ticks 60 --scene tests/data/basic.scene.json --save build/out.scene.json
$cli run                                       # opens a window until closed
$cli call engine.describe                      # components, commands and the open scene
```

`slash-cli` prints results to stdout as JSON and logs to stderr as JSON lines. Exit codes: 0
success, 1 runtime failure, 2 usage error. On a machine with no display, set
`SDL_VIDEO_DRIVER=dummy` to exercise the windowed path.

## Scenes

Scenes are `.scene.json` files: entities keyed by stable ID, each with its components. Fields
equal to their defaults are left out. `tests/data/basic.scene.json` is a small example.

```bash
$cli schema scene                                   # JSON Schema for scene files
$cli scene validate tests/data/basic.scene.json     # every error, with its JSON Pointer path
$cli scene format level.scene.json                  # rewrite in canonical form
$cli scene format --check level.scene.json          # exit 1 if not canonical
```

Validate after editing a scene by hand, and format before committing one.

## Commands, MCP and JSON-RPC

Every change to a world goes through a command registered in `engine/command`. The same registry
backs three front ends, so they never drift apart:

```bash
mcp=./build/dev/tools/slash-mcp/slash-mcp
$mcp --project . --scene tests/data/basic.scene.json     # MCP over stdio; tools use '_' for '.'
$cli rpc --project .                                     # JSON-RPC 2.0, one message per line
$cli call world.query '{"with": ["Transform2D"]}' --scene tests/data/basic.scene.json
$cli call batch '{"commands": [{"command": "entity.create", "params": {"id": "a"}}]}' \
    --scene build/out.scene.json --save
$cli init path/to/game                                   # MCP configs for Claude Code, Codex, Cursor
```

A session checks each command's capabilities (`--allow read,edit,run,filesystem`), validates its
params against the command's JSON Schema, runs it, and records an undo step if it changed the
world. A failed command changes nothing. Paths are relative to `--project` and cannot leave it.

Commands are built for agents, so keep them cheap to call:

- Return only what the caller needs: IDs before full components, changed fields after an edit.
- Report every error with the JSON Pointer of the bad value and, for a misspelled name, the
  closest valid one ("did you mean").
- Keep descriptions short; they are sent to the model as tool definitions.

To add a command, register it in `engine/command/src/builtin.cpp` (or your own registry) with a
name, description, capabilities, an `Effect` and a params schema with
`"additionalProperties": false`, then add a test to `tests/test_command.cpp`.

## HTTP server and live games

`engine/server` serves a session over HTTP on the loopback interface, so agents can drive a game
while it runs:

```bash
$cli serve --scene tests/data/basic.scene.json     # until Ctrl+C; prints the URL and token
$cli run --serve                                   # a running game, served between ticks
$mcp --live --project .                            # MCP over stdio, forwarded to that game
```

Endpoints: `POST /v1/commands/<name>` (REST, params as the body), `GET /v1/commands`,
`GET /v1/openapi.json`, `POST /rpc` (JSON-RPC) and `POST /mcp` (MCP over Streamable HTTP). Every
request needs `Authorization: Bearer <token>`; a `Host` or `Origin` naming another site gets 403.
The URL and token are written to `.slash/server.json` (owner-only on macOS and Linux) and removed
on exit.

Requests are queued and run only when the session's owner calls `Server::poll()`, so commands
never race the game loop. Tests drive the server from a helper thread while the test thread polls;
see `tests/test_server.cpp`.

## Game projects and exports

A game is a folder with a `project.json` (name, version, main scene; `$cli schema project`) and
its scenes. `slash-cli init` creates one. `engine/runtime` runs it: `Game` loads the main scene
and ticks it, and `src/main.cpp` is every game's entry point, built on SDL's main callbacks so
the same loop works on the web and on phones.

```bash
game=./build/dev/tools/slash-game/slash-game
$game --data tests/data/game --headless --ticks 60     # run a project as its export would
$cli project validate tests/data/game
$cli export linux --project path/to/game --test        # build, lay out and run headless
```

`slash-cli export <platform>` validates the project and every scene, stages them as the game's
data, configures `export/CMakeLists.txt` with the engine sources, builds and installs into
`<project>/build/export/<platform>`. Its work folder and full build log are in
`<project>/.slash/export/<platform>/`; a failure reports the step and the end of the log. Each
host builds its own platform; web exports need Emscripten (`EMSDK` set) on any host. CI exports
and runs an empty game for macOS, Windows, Linux and web on every PR.

## Add a component

Components are plain structs reflected with one macro, at namespace scope in the struct's own
namespace. Register them so scenes can save, load and describe them:

```cpp
struct Health {
    float current = 100.0F;
    float max = 100.0F;
};
SLASH_COMPONENT(Health, current, max)

registry.add<Health>()
    .set_doc("Hit points for anything that can take damage.")
    .describe("current", {.doc = "Hit points left.", .min = 0.0});
```

Supported field types are listed in `engine/world/include/slash/world/serialize.hpp`. Use
`SLASH_REFLECT` for structs that appear inside components. Built-in components are in
`engine/world/include/slash/world/components.hpp`.

## Lint

```bash
find engine tools tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
find engine tools -name '*.cpp' | xargs clang-tidy -p build/dev
```

CI rejects unformatted code and any clang-tidy finding.

## Conventions

- C++20. Engine libraries build with no exceptions and no RTTI; fallible calls return
  `slash::Result<T>` (`engine/core/include/slash/core/result.hpp`). Tools and tests may use
  exceptions.
- Invariants use `SLASH_ASSERT`, which logs and aborts in every build type.
- Log with `slash::log::info("message", {{"key", value}})`; never print free-form text to stdout
  from engine code.
- Names: `PascalCase` types, `snake_case` functions and variables, `trailing_` private members,
  `kPascalCase` constants.
- Headers live in `engine/<module>/include/slash/<module>/`, sources in `engine/<module>/src/`.
  Modules: `core` (Result, logging, JSON, reflection, math), `platform` (SDL3 window, run loop,
  files, child processes), `world` (EnTT world, components, scenes), `command` (commands, sessions with undo,
  JSON-RPC and MCP), `server` (localhost HTTP over a session) and `runtime` (project files and
  the game executable).
- New dependencies go in `cmake/Dependencies.cmake`, pinned to a tag, and in `THIRD_PARTY.md`
  with their license. Only permissive licenses (MIT, BSD, zlib, Apache 2.0, Boost, MIT-0,
  public domain). Never copy code from closed engines.
- Every behavior change comes with a test in `tests/`. Tests must pass headless.
