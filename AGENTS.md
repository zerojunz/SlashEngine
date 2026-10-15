# AGENTS.md

Instructions for AI agents (Claude Code, Codex, Cursor and others) working on SlashEngine. Read
[docs/design.md](docs/design.md) for the full design; this file covers the day-to-day loop.
[CONTRIBUTING.md](CONTRIBUTING.md) covers what a pull request needs. [docs/](docs/README.md) has the
reference pages and the [roadmap](docs/roadmap.md).

## Goals

SlashEngine is a lightweight, open-source C++20 game engine whose primary user is an AI agent.
Keep every change true to these:

- An agent can create, inspect, edit, run and test a game with no GUI editor. Anything that can
  only be done by hand is a bug.
- Projects are text (canonical JSON), the engine describes itself (`engine.describe`), and every
  change to a world is a command, served the same way over the CLI, JSON-RPC, MCP and REST.
- Commands cost agents as few calls and tokens as possible.
- Model agnostic: nothing depends on one AI vendor.
- Lightweight and modular; macOS first, games export to every platform.
- Permissive open-source code only. When a design choice is open, follow Godot's approach.
- Gameplay code is plain C++; no embedded scripting language.

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

An entity can `"extends"` a `.prefab.json` file (one entity's components, path relative to the
project) and override its fields; a `null` component removes one the prefab has. Saving writes
only what differs from the prefab. `entity.create` takes `"extends"`, and `prefab.save` writes an
entity to a prefab file. `tests/data/game` has an example.

```bash
$cli schema scene                                   # JSON Schema for scene files (also: prefab)
$cli scene validate tests/data/basic.scene.json     # every error, with its JSON Pointer path
$cli scene format level.scene.json                  # rewrite in canonical form
$cli scene format --check level.scene.json          # exit 1 if not canonical
```

Validate after editing a scene or prefab by hand, and format before committing one. Both
commands take `.prefab.json` files too.

## Assets and hot reload

`engine/asset` loads files by project path into shared, reference-counted handles
(`assets.load<json::Value>("data/tuning.json")`); `.json`, `.txt` and `.csv` load out of the box
and games add types with `add_type`. Sessions and games own an `asset::Assets`, and prefab files
are read through it. `Session::poll_assets()` reloads changed files and updates prefab instances
(keeping overrides); every command polls first, and `slash-cli run` and `serve` poll between
ticks. `asset.list` shows what is loaded, and subscribers get `asset.reloaded` events.

## Storyboard

The storyboard is a game's facts that are not code (style, world, story, design rules, names,
decisions, open work) as typed entries in `storyboard/*.json`, under IDs such as
`character/mira`. Kinds and their fields are defined in `engine/storyboard`
(`$cli schema storyboard`); `tests/data/game/storyboard` has an example. `storyboard.query` finds
entries and returns their versions; `storyboard.edit` takes the version it read, refuses stale
ones with `-32005`, writes the files and appends to `storyboard/journal.jsonl`. Commands that
change project files stage them through `Session::stage_file` and record undo with
`Session::add_undo_call`, so a failed command writes nothing and undo replays the reverse edit.

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

A session checks each command's capabilities (`--allow read,edit,run,filesystem,build`),
validates its params against the command's JSON Schema, runs it, and records an undo step if it
changed the world. A failed command changes nothing. Paths are relative to `--project` and cannot
leave it. Commands that change the world take `"dry_run": true`, which returns what would change
and changes nothing; `world.snapshot`, `world.diff` and `world.restore` save, compare against and
go back to a copy of the entities. `events.subscribe` and `events.read` report entities created and
destroyed, each change to the world and events gameplay code emits with `World::emit`.

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
never race the game loop. A running game adds `play` and `pause`; while paused it keeps serving
but stops stepping the world. Tests drive the server from a helper thread while the test thread polls;
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

`slash-cli export <preset>` validates the project, its presets and every scene and prefab, stages
them as the game's data, configures `export/CMakeLists.txt` with the engine sources, builds, signs
and installs into `<project>/build/export/<preset>`. Its work folder and full build log are in
`<project>/.slash/export/<preset>/`; a failure reports the step and the end of the log. A Mac
builds every platform: macOS and iOS with Xcode's tools, Windows and Linux with llvm-mingw and a
glibc 2.31 Debian sysroot that the first such export downloads (`$cli toolchain install` gets
them ahead of time, into `SLASH_TOOLCHAINS_DIR` or `~/.slash/toolchains`). Linux machines build
Linux and Windows the same way, and Windows machines build Windows with MSVC. Web exports need
Emscripten (`EMSDK` set) and Android exports the Android SDK, NDK and a JDK on any host. CI
exports and runs an empty game for macOS, Windows, Linux, web, Android (on an emulator) and iOS
(in the Simulator) on every PR, cross-exports Windows and Linux (x86_64 and arm64) from macOS and
runs them on Windows and Ubuntu 20.04, and `slash-cli init` writes the per-platform workflow into
new games.

The export code is the `tools/exporter` library, shared by slash-cli and slash-mcp. It also
registers `export.run`, the one command that needs the `build` capability; a running game leaves
it out because an export would stall its loop.

Presets live in `export_presets.json` (`$cli schema presets`), like Godot's
`export_presets.cfg`: a platform, an output folder, a debug flag, signing settings and, for Linux,
the CPU architecture. A preset
named after a platform replaces that platform's defaults. Secrets never go in the file:

| Variable | Used for |
| --- | --- |
| `SLASH_APPLE_API_KEY_PATH`, `SLASH_APPLE_API_KEY_ID`, `SLASH_APPLE_API_ISSUER` | Notarizing macOS apps (App Store Connect API key) |
| `SLASH_WINDOWS_CERTIFICATE_PATH`, `SLASH_WINDOWS_CERTIFICATE_PASSWORD` | Signing Windows executables with signtool |
| `SLASH_ANDROID_KEYSTORE_PATH`, `SLASH_ANDROID_KEYSTORE_PASSWORD`, `SLASH_ANDROID_KEY_ALIAS`, `SLASH_ANDROID_KEY_PASSWORD` | Signing release APKs (the key password defaults to the keystore's) |
| `SLASH_IOS_PROVISIONING_PROFILE` | The provisioning profile for iOS device builds (default: the newest installed one for the team and bundle ID) |

macOS exports are always signed, ad hoc unless a preset names a `codesign_identity`. Android
exports are APKs for arm64-v8a and x86_64, built without Gradle: the NDK builds `libmain.so` per
ABI, then javac, d8, aapt2, zipalign and apksigner package SDL's Java activity
(`export/android/`) with the game. Debug builds, and release builds with no keystore in the
environment, are signed with the debug keystore in `~/.android`, as Android Studio does.

iOS exports build against Xcode's SDK with CMake, with no Xcode project, then codesign signs
them (`export/ios/Info.plist.in` is the app's Info.plist). By default they are ad hoc signed
`.app` bundles for the Simulator, where `--test` runs them. A preset with a `team_id` builds an
`.ipa` for iPhones and iPads instead, signed with the team's keychain identity (Apple Development
for debug builds, Apple Distribution for release) and a provisioning profile Xcode downloaded.

Games support OS versions from 2020 on: macOS 11, iOS 14, Windows 10 (an MSVC build's subsystem
version is 10.0; llvm-mingw builds keep 6.0, since Windows 11 won't start a MinGW executable
marked 10.0), Android 11 (API level 30, targeting 36), and Chrome 85, Firefox 79 and Safari 15
(pinned in `export/CMakeLists.txt`).

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
  files, child processes), `asset` (asset handles, loaders, hot reload), `world` (EnTT world,
  components, scenes and prefabs), `storyboard` (storyboard kinds, files, versions, queries and
  edits), `command` (commands, sessions with undo, JSON-RPC and MCP), `server` (localhost HTTP
  over a session) and `runtime` (project files and the game executable). Tool-only libraries such as `tools/exporter` use the same layout and may use
  exceptions.
- New dependencies go in `cmake/Dependencies.cmake`, pinned to a tag, and in `THIRD_PARTY.md`
  with their license. Only permissive licenses (MIT, BSD, zlib, Apache 2.0, Boost, MIT-0,
  public domain). Never copy code from closed engines.
- Every behavior change comes with a test in `tests/`. Tests must pass headless.
- A change to a command, flag, component or file format updates its page in `docs/reference/`,
  and work that finishes a roadmap item ticks it in `docs/roadmap.md`.
