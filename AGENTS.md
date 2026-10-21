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

An entity can also `"extends"` a `.scene.json` file that names its `"root"` (`scene.save` takes
`root`), as Godot instances a PackedScene: it becomes the root, and the scene's other entities
come with it as `<instance>/<their id>` (`World::create_part`), their references renamed through
`ComponentType::rename_entities`. Entries keyed with `/` hold overrides or `null`, saving writes
only what differs, and `entity.destroy` takes them along. `expand` in `engine/world/src/scene.cpp`
does the loading; `examples/instanced-scenes` has a player with a gun scene inside it.

The entities in a `Transform2D`'s `children` are placed in its space, as Godot's Node2D children:
`world::Tree2D` (`engine/world/src/tree2d.cpp`, built with `-ffp-contract=off`) gives each one's
global transform, its parent and its draw order, and everything that places things in the world
(drawing, physics, lights, particles, sound) asks it. Make one per use; a root's global transform
is its own to the bit, so flat scenes behave as before. A `top_level` entity keeps its parent
(`parent`) but is placed in the world (`transform_parent` is null) and draws after the rest of its
tree, as Godot's top-level items; `canvas::inherited` stops its parents' `z_index` and tint there,
and at a `z_as_relative` that is off. `check_tree` backs `scene validate`, and a session fails
any command that would give a child two parents or make it its own ancestor.
`entity.destroy` takes children with it. A body takes the shapes of its children that have no
body of their own (`shape_key_of` in `engine/physics2d/src/space.cpp`), as Godot's
`CollisionShape2D` nodes. `examples/parent-transforms` sails a boat.

```bash
$cli schema scene                                   # JSON Schema for scene files (also: prefab, frames, material, animation, process_material, tileset, font, theme, audio, bus_layout, replay, test, score, sdk)
$cli scene validate tests/data/basic.scene.json     # every error, with its JSON Pointer path
$cli scene format level.scene.json                  # rewrite in canonical form
$cli scene format --check level.scene.json          # exit 1 if not canonical
```

Validate after editing a scene or prefab by hand, and format before committing one. Both
commands take `.prefab.json`, `.frames.json`, `.material.json`, `.anim.json`,
`.process_material.json`, `.tileset.json`,
`.font.json`, `.theme.json`, `.audio.json`, `.bus_layout.json`, `.replay.json`, `.test.json` and
`.score.json` files too; validating a scene also checks each tile map layer against its tile
set, each `AnimationPlayer` against its library and that its controls lay out as written.

## Assets and hot reload

`engine/asset` loads files by project path into shared, reference-counted handles
(`assets.load<json::Value>("data/tuning.json")`); `.json`, `.txt` and `.csv` load out of the box,
`canvas::add_asset_types` adds textures (`.png`, `.bmp`), `.frames.json`, `.material.json`,
`.process_material.json` and `.tileset.json` files, `animation::add_asset_types` adds `.anim.json` files,
`text::add_asset_types` adds fonts (`.ttf`, `.otf`, `.font.json`), `audio::add_asset_types`
adds sounds (`.wav`, `.ogg`, `.mp3`), `.audio.json` and `.bus_layout.json` files, and games add types
with `add_type`. Sessions and games own an `asset::Assets`, and prefab files are read through it.
`Session::poll_assets()` reloads changed files, updates prefab instances (keeping overrides) and
applies a changed `project.json`'s settings again; every command polls first, and `slash-cli run`
and `serve` poll between ticks. `asset.list` shows what is loaded and the bytes each holds
(`add_type`'s optional `bytes` function, else the file's size), and subscribers get
`asset.reloaded` events.

## Storyboard

The storyboard is a game's facts that are not code (style, world, story, design rules, names,
decisions, open work) as typed entries in `storyboard/*.json`, under IDs such as
`character/mira`. Kinds and their fields are defined in `engine/storyboard`
(`$cli schema storyboard`); `tests/data/game/storyboard` has an example. `storyboard.query` finds
entries and returns their versions; `storyboard.edit` takes the version it read, refuses stale
ones with `-32005`, writes the files and appends to `storyboard/journal.jsonl`. Commands that
change project files stage them through `Session::stage_file` and record undo with
`Session::add_undo_call`, so a failed command writes nothing and undo replays the reverse edit.
Other files a command writes go through `Session::write_file`, which a session running a test
refuses.

`storyboard.brief` returns the entries a task touches: those its scenes' and entities' `StoryRef`
components name, those linked to its files, global rules and live work. `storyboard.check` finds
broken references, glossary words to avoid, unknown `StoryRef` IDs in every scene and prefab,
and textures larger than a `budget` or with colors a `palette` covering them lacks
(`storyboard::check_textures`); `$cli scene validate` checks a scene's `StoryRef` IDs and `$cli project validate` runs the whole
check. `storyboard.review` (`engine/ai/src/review.cpp`) asks a model from `providers.json`
whether new text, scene text, images or entries contradict the entries they touch; its findings
are advisory, and one that quotes nothing in the material or names no real entry is dropped.
Games made by `$cli init` start with a storyboard and are told to brief before a task and check
after it.

`project.map` (`engine/command/src/project_map.cpp`) says where things are, made from the files
on each call: scenes' and prefabs' components, prefabs, used files and storyboard entries, and the
game's C++ sources read as text by `outline_cpp` (`slash/command/cpp_outline.hpp`: types and
functions by line, macros, string literals with the call they are passed to, includes), so each
component, system, event and input action says where it is declared and used. `find` and
`paths` narrow it. `api.search` (`tools/exporter/src/api_search.cpp`) reads the engine's public
headers and the game's sources the same way and finds functions, types and macros by name or by
the words in their docs, with their signatures, fields, the header to include and the lines of
`examples/` (or of the game) that use them. It finds only what has a doc comment right above it
by what it does, so give each public declaration one.

`playtest.run` (`engine/command/src/playtest.cpp`) runs many headless copies of a game at once
and sums them up per combination of the values it varies (an entity's field, every entity's with
a component, or a prefab's): how runs ended by its named end conditions, ticks, event counts and
measured fields. Each run is a session of its own, made from the session's world (`Session::save`
and `restore`) with the game module attached, and played by a scripted bot through
`input::action_press`; sessions are made and dropped on the calling thread and their worlds step
on the job system's threads at once, each on a pool of no threads, so a run comes out the same as
it would alone. Box2D's world list is shared, so `physics2d` creates and destroys its worlds under
a lock.

## Asset library

`engine/library` keeps generated and added images and sounds once per machine
(`~/.slash/library`, or `SLASH_LIBRARY_DIR`; `SessionOptions::library_dir` in tests), stored by
the SHA-256 of their bytes (`slash/core/sha256.hpp`) with a record of how each was made: request,
generator, model, seed, license, tags, a short description and the parent it was refined from.
Files are written whole and moved into place (`platform::rename_file`), so agents can share a
library. Generators are `library::Generator` adapters named in `providers.json`'s `generators`,
as models are `ai::Provider`s: `openai-image`, `gemini-image`, `automatic1111`, `openai-speech`
and `elevenlabs-sound` (`src/adapters.cpp`, through `ai::post_with_retries`, with images fitted
to the size asked for by `fit_image`), and the built-in `test` generator, which draws
placeholders with integer math only, so they are the same bytes everywhere. `asset.search`, `asset.generate` (an exact repeat
calls no generator, close matches come back as candidates), `asset.refine`, `asset.info`,
`asset.use`, `asset.add`, `asset.feedback`, `asset.check` and `asset.sync` are in `slash_library_commands`, registered by
slash-cli and slash-mcp; using an asset copies it into the project, records its source and
license in the project's `library.json` and checks an image against the storyboard's palettes and
budgets. Feedback (accepted or rejected, and why) ranks search results, keeps rejected assets out
of candidates and, with the storyboard's tone and palette, goes into later prompts
(`prompt_context` in `src/commands.cpp`). `asset.check`, which `test.run` runs on a whole
project, records feedback by itself: it opens each scene that draws a library file and checks
what `canvas::summarize` sees with `SummaryOptions::contrast` (the edge contrast of each thing
in the frame `canvas::capture` draws, `Shown::contrast`) and how small each file is drawn, from
the texture paths on `FramePiece`s.

A team shares a library it hosts (`slash/library/team.hpp`): `providers.json`'s `library`, or
`SLASH_LIBRARY_TEAM`, names a folder every teammate reaches or a library server
(`slash-cli library serve`, `library::TeamServer`, answering `POST /v1/library/<op>` with a
bearer token through `answer_team_request`). `library::Team` is the one interface over both;
the commands open it through `Libraries` in `src/commands.cpp` when they first need it, search
and answer exact repeats from it, bring assets the machine lacks from it, put what they make
and the feedback they record there, and report a team that can't be used as `team_error`
while working on the machine's library alone. `asset.sync` shares what was made before.

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

A session checks each command's capabilities (`--allow read,edit,run,filesystem,build,network`),
validates its params against the command's JSON Schema, runs it, and records an undo step if it
changed the world. A failed command changes nothing. Paths are relative to `--project` and cannot
leave it. Commands that change the world take `"dry_run": true`, which returns what would change
and changes nothing; `world.snapshot`, `world.diff` and `world.restore` save, compare against and
go back to a copy of the entities. `events.subscribe` and `events.read` report entities created and
destroyed, each change to the world and events gameplay code emits with `World::emit`. Systems
react to them through `World::events()`, the events of the tick before, which each system sees
once whatever its priority; sessions put them back when a command fails or is a dry run.

Commands are built for agents, so keep them cheap to call:

- Return only what the caller needs: IDs before full components, changed fields after an edit.
- Report every error with the JSON Pointer of the bad value and, for a misspelled name, the
  closest valid one ("did you mean").
- Keep descriptions short; they are sent to the model as tool definitions.

The benchmark in `bench/` holds commands to that. `slash-cli bench` plays standard agent tasks
through MCP and counts each one's calls and estimated tokens, and `ctest` fails when a task
stops passing its checks or costs more than `bench/baseline.json` (setup included, so a new
command's definition counts). When a change costs more or less on purpose, run
`slash-cli bench --baseline bench/baseline.json --update` and commit the file. With `--live`, a
model does the tasks from their prompts instead; see
[docs/reference/benchmark.md](docs/reference/benchmark.md).

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

## Replays

`engine/command/src/replay.cpp` records runs and plays them back. `replay.record` (or
`slash-cli run --record <file>`) listens through `World::set_step_listener` (ticks the host
steps), `input::set_recorder` (events queued between ticks) and `Session::set_call_listener`
(commands that change the world; undo, redo, `world.restore`, scene opens and hot reloads are
kept as the entities they left), and `replay.stop` writes a `.replay.json` file
(`$cli schema replay`) with a hash of the world every 60 host ticks and after each command.
`replay.run` puts the start back, steps the same ticks with the same input and commands, and
reports `matched`, the first tick that `diverged` and the `changes` at the end. The session keeps
the playback, so `replay.seek` goes on from where it stopped (to a tick, by `ticks`, or to the
first tick a `world.query` selection matches) when `Session::revision()` says nothing else
changed the world, and plays from the start otherwise, since Box2D's state can't be copied.
Recording and
playback start with `World::reset_state`, so a module that keeps state outside components
(input, gui, physics, particles, audio) adds a reset with `World::add_reset`, and anything that
iterates entities in an order that matters uses registry storage order, never handle values
(`EntityMap` in `engine/physics2d/src/space.hpp`), so a replay steps the same in every session.
`examples/replays` records a run, plays it back and seeks the moment the coin is taken.

## Debug overlay

`engine/debug` is the panel a person watching `slash-cli run` sees: `debug::Overlay` keeps
its controls in a world of its own (an embedded scene in `src/overlay.cpp`), takes F3 and the
pointer on its panel through `SdlReader::set_filter` before the game sees them, and in
`update` runs its Pause, Step and Shapes buttons as the `pause`, `play` and `step` commands
through the game's session, then shows the frame stats, the tick, what `CanvasRenderer`
drew and the bodies under the mouse (`physics2d::overlap_point`). `CanvasOptions::overlay`
draws its controls last, laid out with the game's fonts so the glyph pages stay put. Collision
shapes are `canvas::collision_shapes` (`engine/canvas/src/debug_draw.cpp`), ports of Godot's
`CollisionShape2D` and `CollisionPolygon2D` debug drawing placed as `physics2d` places shapes;
`CanvasOptions::collisions` and `frame.capture`'s `collisions` draw them. `--debug-overlay`,
`--debug-collisions` and the `debug.overlay` command turn them on. `examples/debug-overlay`
captures the shapes and works the panel.

## Headless tests

`engine/command/src/test.cpp` is `test.run` (and `$cli test`): it runs `.test.json` files
(`$cli schema test`; tests by name, each a scene and steps of command calls with what each reply
must hold in `expect`) and returns only the checks that failed, with the JSON Pointer of each
into its test file. Each test opens its scene file in a new `Session` over the same registries,
with `SessionOptions::write_files` off (commands write files only through `Session::write_file`,
which refuses then) and no `build` capability. A test fails at a step whose command fails, a
reply that doesn't hold what it expects, or a warning or error logged while it runs, which it
sees by wrapping the log sink (`log::sink`). Run with no `paths` or `tests`, it also runs
`storyboard.check`. Games made by `$cli init` start with `tests/main.test.json`, and their
workflow runs `$cli test`. `examples/headless-tests` breaks a scene and reads the failures.

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

`slash-cli export <preset>` validates the project, its presets and every scene, prefab, texture,
font, sound, `.frames.json`, `.material.json`, `.process_material.json`, `.audio.json` and
`.bus_layout.json` file, stages them as the game's data, configures
`export/CMakeLists.txt` with the engine sources, builds, signs and installs into
`<project>/build/export/<preset>`. Its work folder
and full build log are in `<project>/.slash/export/<preset>/`; a failure reports the step and the
end of the log. A Mac builds every platform: macOS and iOS with Xcode's tools, Windows and Linux
with llvm-mingw and a glibc 2.31 Debian sysroot that the first such export downloads
(`$cli toolchain install` gets them ahead of time, into `SLASH_TOOLCHAINS_DIR` or
`~/.slash/toolchains`). Linux machines build Linux and Windows the same way, and Windows machines
build Windows with MSVC. Web exports need Emscripten (`EMSDK` set) and Android exports the Android
SDK, NDK and a JDK on any host. CI exports and runs an empty game for macOS, Windows, Linux, web,
Android (on an emulator) and iOS (in the Simulator) on every PR, cross-exports Windows and Linux
(x86_64 and arm64) from macOS and runs them on Windows and Ubuntu 20.04, and `slash-cli init` writes
the per-platform workflow into new games.

Closed vendor SDKs (ads, stores, Steamworks) come in as SDK packages, as Godot's Android and
iOS plugins: a folder with an `sdk.json` (`$cli schema sdk`; `tools/exporter/src/sdk.cpp`) naming
per platform the libraries, frameworks, defines, files, Info.plist, entitlement and Android
manifest entries and `.jar` files, with the vendor's binaries downloaded by URL and SHA-256 into
`~/.slash/sdks` or found at a local path, and API keys as environment variables.
`project.json`'s `sdks` lists them and a preset's `sdks` picks which it links. Exports build each
package's own CMake project with the game's compilers, write `slash_game_sdk()` calls into
`<build>/sdks.cmake` for `export/CMakeLists.txt`, ship shared libraries where each platform's
loader looks and define `SLASH_SDK_<NAME>` for the game's code. Android packages also list
`.aar` files and library folders, and Maven coordinates that `$cli sdk lock` resolves without
Gradle (`src/maven.cpp`, POMs read with `src/xml.cpp`) and pins by SHA-256 in `sdk.lock.json`;
Android exports merge the libraries' manifests, resources, code and native libraries into the
APK (`src/android_library.cpp`). `$cli sdk add`, `sdk check` and `sdk lock` (and the `sdk.add`,
`sdk.check` and `sdk.lock` commands) add, check and lock packages; `examples/sdk-package` has a
stand-in SDK built from source, which CI links into every platform's export. A package's glue
code (`sources`, compiled into the game; `java` on Android) implements `engine/services`'
backends, and its `register` function and `backends` (Java or Objective-C classes, made through
`slash/services/bridges.hpp`: `src/android.cpp` over JNI, `src/apple.mm` over the Objective-C
runtime) go into the `<build>/sdk_register.cpp` exports write, which the game calls at start.

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

## Gameplay code

A game's own components and systems are plain C++ in the folders and files `project.json`'s
`gameplay.sources` names (`src` in games `slash-cli init` makes), registered in one
`SLASH_GAME_MODULE(game) { ... }` function (`engine/world/include/slash/world/game_module.hpp`),
as Godot's scripts are. Exports compile the sources into the game (`SLASH_GAME_SOURCES` in
`export/CMakeLists.txt`). The tools load them as a library instead, on macOS and Linux:

```bash
$cli build --project path/to/game     # or the gameplay.build command; compiler errors with file and line
$cli build --check                    # or build.check: compile what changed, nothing linked; errors and fixes as data
```

`exporter::build_gameplay` configures `cmake/gameplay/CMakeLists.txt` with the compiler, flags
and include folders of the engine build the program came from (`build/<preset>/gameplay-sdk.cmake`)
and builds `.slash/gameplay/game.so`, its engine symbols left for the program to supply.
`exporter::check_gameplay` (`build.check`) builds only its `game_objects` target, so just the
files that changed compile and nothing links, and `read_compile_diagnostics` reads the errors,
warnings, notes and `-fdiagnostics-parseable-fixits` fixes from the log.
`gameplay::Host` (`engine/gameplay`) loads it from a copy with a name of its own, refuses one
built against other headers (`SLASH_API_HASH`, a hash of `engine/*/include` and the pinned
dependencies) or with another compiler, and when the file changes swaps the code in: it saves
each entity's components of the old types as JSON, removes the old systems and their pools,
moves the new types into the world's `ComponentRegistry` and puts the values back field by
field, reporting what it had to drop. Libraries are never unloaded. Hosts link the engine
libraries whole and export their symbols (`slash_gameplay_host` in CMake). Sessions poll it
before each command (`gameplay.reloaded` events), `slash-game` before each tick, and
`scene validate` and exports check the game's components by what its sources declare when the
library isn't there (`runtime::add_gameplay_types`, `ComponentRegistry::add_stand_in`). Sessions
in slash-cli and slash-mcp, and `project.import` after it writes skeletons, give the host
stand-ins for those components (`runtime::add_stand_ins`, `Host::add_stand_ins`), which keep
entities' values as JSON, so scenes using them open and save before the code is built; loading
the code swaps its types in for them and keeps the values, as a reload does. Because
the swap goes through JSON, gameplay state belongs in registered components. Restoring a world
saved under older code (`undo`, `world.restore`) leaves out what no longer fits.
`examples/gameplay-code` builds, changes and breaks a game's code while it runs, and
`examples/build-check` checks it as it is edited and applies the compiler's fix.

## Importing games from other engines

`tools/importer` converts a game made in another engine into a SlashEngine project: Godot 4 and
Unity (Unreal is on the roadmap). It registers `project.import`, which
`slash-cli import <folder>` and slash-mcp serve; `--dry-run` and `"dry_run": true` return the
report and the files it would write. `src/godot_text.cpp` parses Godot's text formats
(`.tscn`, `.tres`, `project.godot`) and `src/godot_import.cpp` converts them: a `.scene.json`
beside each `.tscn`, a `.frames.json` per `SpriteFrames`, a `.tileset.json` per `TileSet`, a
`.theme.json` per `Theme`, a `.bus_layout.json` per `AudioBusLayout`, a `.anim.json` per
`AnimationPlayer` (tracks keep their node paths as targets), `project.godot`'s settings
and input map merged into `project.json`, and a storyboard task per script (`task/port-<name>`)
to port it to C++. Each GDScript and C# file also becomes a C++ skeleton (`src/scripts.cpp`,
public header `slash/importer/scripts.hpp`; `read_gdscript` and `read_csharp`, which reads
Godot's and Unity's C#): `<gameplay folder>/imported/<name>.cpp` with its variables as a component
the scenes fill in, its signals as `emit_<signal>` helpers, and a stub per function, keeping the
script in comments, that a system calls when its engine would (`_ready` and `Start` on
`entity.created` events, `_process` and `Update` once a tick, `_input` per input event, Unity's
trigger and collision messages on physics events, connected methods on their signal's events).
`src/skeletons.cpp` names and writes them for both importers: skeletons that exist are kept,
`imported/scripts.cpp` registers them and a game module is written when the project's code has
none. Properties convert by the target field's reflected type, Godot's enum numbers
through its `choices`, and a value that fails its file's schema (scene, tile set, theme, bus
layout or animation) is left out and reported, so one bad value can't stop a file. Everything that didn't
convert goes in `import/godot.json`, once per kind. `docs/reference/godot-import.md` maps each
node and setting; `examples/import-godot` imports a small platformer and
`examples/import-godot-scripts` ports a script from its skeleton.

Games exported with Godot 4 import too: `src/godot_unpack.cpp` finds the pack in a `.pck` file or
an executable and turns it back into the project (`godot::unpack_game`, in
`slash/importer/godot_pack.hpp`): `src/godot_binary.cpp` reads binary resources, values and
`project.binary`, `src/gdscript_tokens.cpp` turns `.gdc` tokens back into source, and textures
and sounds become PNG, WAV, Ogg and MP3 files. `unpack_to` writes the recovered project into the
SlashEngine project. Encrypted packs are refused and encrypted files listed, never decrypted.
`src/godot_pack.cpp` writes all of it as Godot's export does (`godot::export_project`,
`godot::write_pack`), checked against Godot 4.5's own exports: tests and examples make their
exported games in code with it rather than committing binary files. `examples/unpack-godot`
imports a shipped game.

Games built with Unity import through AssetRipper, which is GPL and so only ever runs as a
program of its own: `src/unity_build.cpp` (`slash/importer/unity_build.hpp`) finds a build
(a player folder on any desktop, an `.apk` or `.ipa` file, a web build, compressed or not),
refuses encrypted bundles, starts AssetRipper's headless server (`SLASH_ASSETRIPPER`, else the
`assetripper` toolchain, installed on first use), has it export the Unity project into
`.slash/import/unity/`, then tidies it: scripts of Unity's packages get the GUIDs they have in
every project (`unity::package_script_guid`) and their decompiled stand-ins are dropped, so the
Unity importer converts the project as one made in Unity. Unpacking needs the `build`
capability, and a running game refuses it. Tests and `examples/unpack-unity` run
`tests/fake_assetripper.cpp`, a stand-in that serves the same calls and exports a folder.

`src/unity_yaml.cpp` reads Unity's YAML (scenes, prefabs, assets, settings and `.meta` files)
and `src/unity_import.cpp` converts a project with `Assets/` and `ProjectSettings/`: a
`.scene.json` beside each `.unity` and `.prefab` file, prefabs staying prefabs that scenes
instance (an instance's objects take Unity's IDs, `(instance ^ source) & 0x7FFFFFFFFFFFFFFF`,
and stripped objects resolve to what they stand for), units scaled to pixels by the pixels per
unit most sprites use, tile maps as `TileMapLayer`s over a `.tileset.json` cut from the sprites
their cells show (isometric and hexagonal Grids as Godot's shapes, the layer moved by
`GridPlan::offset` so cells stay where Unity draws them, anchors and pivots as `texture_origin`),
each Animator controller as a `.anim.json` library with an animation per state
(Unity's curves keyed every 60th of a second where they bend; tracks target objects by their
paths from the Animator), every `.mixer` file as one `.bus_layout.json`, 2D joints as Godot's,
URP's 2D lights as `CanvasModulate` and `PointLight2D`s through generated falloff textures and
shadow casters as `LightOccluder2D`s, objects under a `Canvas` as controls (rects as anchors and
offsets, uGUI and TextMesh Pro widgets as Godot's, their parts folded into the control's theme
and fields), settings and both input systems merged into `project.json`, and a task per C#
script and per state machine. Each script's skeleton takes its serialized fields, which scenes
fill in; its trigger and collision messages make the importer set `contact_monitor` on the
object's `RigidBody2D`, and UI events' persistent calls connect to its methods.
Objects a prefab brings are written whole, so `format_scene` keeps only what differs.
`src/common.cpp` holds what both importers share: paths, the report and the tasks.
`docs/reference/unity-import.md` maps each component and setting; `examples/import-unity`
imports a small platformer.

## Rendering

`engine/render` is the renderer's device layer: `render::RenderDevice`, a dozen calls over SDL3's
GPU API (Metal, Direct3D 12 or Vulkan), and a null device for headless runs that checks every
call the same way but draws nothing. `create_device_or_null` falls back to it, with a warning,
where no backend starts. `examples/draw-offscreen` draws into a texture and reads it back.

`src/gl_device.cpp` is the same device over OpenGL ES 3.0 (the `opengl` driver, Godot's
compatibility tier), which is WebGL 2 on the web, where `auto` picks it, since SDL3's GPU API has
no web backend. It loads its GL functions through SDL (links WebGL's on the web), needs a window
made with `platform::Config::opengl`, and draws every frame upside down into a texture so rows
and `gl_FragCoord` match the other backends, then copies window frames over turned back.
`uniforms.hpp` fills the material shader's uniforms for both hardware devices; a change to how
the GPU device draws goes in this one too.

`create_software_device` (`src/software_device.cpp`) draws on the CPU, into render targets
only: it batches as the GPU device does, rasterizes with the GPU's fill rules and shades with
the CPU twins (`run_program`, `render::shade`, `distance_field_coverage`, `sample_image`), so its
pixels are the same on every machine. The GPU tests draw on it too, and hold both devices to the
same pixels; a change to how the GPU device draws goes in both. `canvas::capture` draws a world's
frame on it at the viewport size, and backs `frame.capture` (pixel colors, a PNG or a file; MCP
sends the PNG as an image). `examples/frame-capture` checks a HUD by its pixels.

`frame.summary` reads the same frame as data. `CanvasOptions::contents` has `CanvasRenderer::draw`
fill a `canvas::FrameContents`: each sprite, layer, emitter and control drawn, as triangles in
window pixels over their textures' pixels (`ui::DrawList::spans` says which control drew which
vertices), and what couldn't draw. `canvas::summarize` (`src/summary.cpp`) draws on the null
device with it, then samples each thing's pixels against what was drawn after it, by texture
alpha times vertex alpha, to say how much shows, and fills the map. A change to what the
renderer draws shows up in summaries with no other change. `examples/frame-summary` walks a
player behind a bush and off the screen.

`engine/canvas` is 2D drawing, as Godot's canvas. `Sprite2D` draws a texture, or a frame of a
sprite sheet, at the entity's `Transform2D`, in `z_index` order, then the order entities were
made. `AnimatedSprite2D` plays a `.frames.json` file's animations on it through the
`animated_sprite_2d` system (`$cli schema frames`; `animation_finished` events). `canvas::install`
adds the asset types and the systems (that one, `camera_2d`, `parallax_2d`, `cpu_particles_2d` and
`gpu_particles_2d`) to a world;
sessions and games call it. `CanvasRenderer`
draws a world through a render device over the project's `rendering.clear_color`: games and
`slash-cli run` call `before_tick` before each tick and draw between ticks with
`Loop::interpolation()`, so sprites move smoothly at any frame rate. On the null device it checks
and counts sprites but draws nothing. `examples/sprites` plays and draws a scene's sprites. The
current `Camera2D` (`engine/world/src/camera2d.cpp`, Godot's `get_camera_transform` line for
line) picks what the window shows: the renderer draws the world through its canvas transform
(each draw's `render::Material::transform`) and controls over it in screen space, the
`camera_2d` system (priority 1050) moves its drag margins and smoothing on, and `frame.summary`
reports the world rect it shows. `examples/camera-2d` follows a player through a level.

Canvas layers follow Godot's `CanvasLayer` and `Parallax2D` (`engine/world/src/canvas_layer.cpp`).
`world::Canvases` lists a world's canvases in drawing order (by `layer`, the world first among
layer 0) and says which one each entity draws in: that of the `CanvasLayer` whose `children` hold
it or an entity above it, else the world's. `CanvasRenderer` draws each canvas's sprites, tiles
and particles, then its controls, through its transform (`canvas_layer_transform`, after the
camera's when `follow_viewport_enabled`), each lit by its own `CanvasModulate` and the lights
whose `range_layer_min`/`range_layer_max` hold its layer; `ui::layout` places each
control by its canvas's transform (`Placed::canvas`, `DrawCall::canvas`), so hit tests follow.
The `parallax_2d` system (priority 1060) sets each `Parallax2D`'s position from the camera as
Godot's `_update_scroll`, and the renderer draws a repeating one's children again at each
repeat. `canvas::to_world` and `to_screen` (and `canvas.convert`) turn points between the screen
and a canvas. `examples/canvas-layers` scrolls hills behind a level under a HUD.

Materials are `.material.json` node graphs (`$cli schema material`) that `Sprite2D`'s `material`
names and `material_params` sets. `engine/canvas/src/material.cpp` compiles a graph on load into
a `render::MaterialProgram` (`engine/render/include/slash/render/material.hpp`): at most 64
instructions over 32 registers, which `material.hlsl` interprets on the GPU and `run_program`
runs on the CPU for `material.eval`. A new op goes in all three, with a case in the test that
runs every op on both. `examples/materials` has three materials.

A material's `"shader"` names a game's own `.hlsl` file with Godot's `fragment()` and `vertex()`
(the built-ins are in `docs/reference/scene-format.md`). `tools/shader_compiler` wraps it in
`engine/render/shaders/canvas.hlsl`'s entry points and compiles it with glslang and SPIRV-Cross
for every backend and for `render::ShaderVm`, which the software device and `material.eval`
run. slash-cli, slash-mcp and exports link it and install it with `render::set_shader_compiler`;
games never do, and draw the `"compiled"` code their export wrote into each material file.
Renderers recompile a shader when its file changes. `examples/custom-shaders` has a dissolve and
a sway.

Tile maps follow Godot's TileSet and TileMapLayer (`tilemap.hpp` in `engine/canvas`). A
`.tileset.json` file (`$cli schema tileset`) cuts atlas textures into tiles and names terrain
sets; a terrain tile's `peering` names the terrain each side and corner must touch. A
`TileMapLayer` stores its cells as text: `origin`, a `legend` of short keys and `rows` of keys,
`.` for empty, which `decode_cells` and `encode_cells` read and write in canonical form.
`paint_terrain` picks connecting tiles for painted cells and their neighbors; the commands are
`tilemap.paint`, `tilemap.set` and `tilemap.get`. `CanvasRenderer` draws layers in runs of one
texture (`tiles_drawn()`), and `check_tile_layers` checks a scene's layers for `scene validate`
and exports. `examples/tilemaps` paints an island with a pond. An atlas source's
`normal_texture` and `specular_texture` are a Godot `CanvasTexture`'s, which lights shade its
tiles with (`material_of` in `renderer.cpp`). Tiles have Godot's physics layers,
collision polygons and alternative tiles, and a cell can turn its tile with `:h`, `:v` and `:t`
after the tile (`slope:h`, `rock:1:ht`), as Godot's cell transform flags; drawing and collision
turn together (`tile_look`, `tile_collision`). `examples/tile-collision` rolls balls on tiles.
A tile set's `tile_shape`, `tile_layout` and `tile_offset_axis` are Godot's: `map_to_local`,
`local_to_map` and `neighbor_cell` port Godot's `TileSet` functions line for line, peering
directions are 16 compass points (`Neighbor`) of which each shape has its own sides and corners
(`peering_kind`, `godot_neighbor` for Godot's bits), and cells draw by `draw_order`, down the
screen. A tile's `texture_origin` moves its texture as Godot's does (`TileLook::position`),
turning with the cell. `tilemap.convert` turns cells into points and back;
`examples/isometric-and-hex-tiles` paints diamonds and sets hexagons. Tiles span
`size_in_atlas` texture cells and play `animation_frames` laid out after them as Godot's
(`tile_region`, `tile_frame`, and `tile_animation_offset` for random start times), on
`CanvasOptions::time`; a tile set refuses tiles whose cells or frames overlap.
`examples/animated-tiles` has water, torches and a bridge two cells wide. Tile sets have
Godot's custom data layers (`TileSet::custom_data` gives a tile's value or the type's default;
`tilemap.get` with `cells` returns them) and occlusion layers: `collect_lighting` places each
cell's `tile_occluders` as `LightOccluder2D`s with the layer's `light_mask`, unless the layer's
`occlusion_enabled` is off. `examples/tile-data-and-occluders` has walls that cast shadows and
lava that hurts.

2D lights follow Godot's: `PointLight2D`, `DirectionalLight2D`, `LightOccluder2D` and
`CanvasModulate`, with `Sprite2D`'s `light_mask`, `normal_texture` and `specular_texture` and a
material's `light_mode`. `engine/render/include/slash/render/light.hpp` is the device side: a
frame's `render::Lighting` (up to 64 lights, a light atlas and a shadow map) and a draw's
`Material::shading` (its canvas's layer, ambient color and `to_lights`, which maps a canvas
layer's pixels into the world's, where lights are), shaded in `material.hlsl`; `render::shade` is its CPU twin, used by
`light.sample`, and a test compares the two pixel for pixel. A change to the light math goes in
both. `engine/canvas/src/light.cpp` turns a world's lights into that: `collect_lighting` casts
each shadowed light's occluder edges on the CPU into one row of `render::kShadowMapWidth` texels
(a point light's goes round it, a directional light's across it), places lights and occluders
in canvas layers into the world's canvas and gives each canvas its `CanvasLighting`, and
`pack_light_atlas` packs the light textures. `CanvasRenderer` uploads both only when they change (`lights_drawn()`,
`shadows_drawn()`). `examples/lights-2d` has a torch, the moon and a shadowed crate, and
`examples/lit-tiles-and-layers` a tile set's normal map and a HUD with its own light.

Shaders are HLSL in `engine/render/shaders/`. Their SPIR-V, MSL and GLSL ES are committed in
`engine/render/src/*_shaders.hpp`, so only someone changing a shader needs glslang and
SPIRV-Cross (`glslang-tools` and `spirv-cross` on Debian and Ubuntu, `glslang` and `spirv-cross`
in Homebrew; games' own shaders compile with the copies built as libraries from
`cmake/dependencies.json`). The FSR shaders are written as games' shaders are, and
`slash-shader-header` (`tools/shader_compiler`) compiles them into `fsr_shaders.hpp`. After
editing one, regenerate and commit the header; the `shaders_up_to_date_*` tests and the FSR
header's test in `tests/test_upscaler.cpp` fail if the two differ:

```bash
cmake --build --preset dev --target shaders
```

The GPU tests in `tests/test_render.cpp` skip when no backend starts; `tests/devices.hpp` gives
them every device that works (`drawing_devices()`, `hardware_devices()`). On Linux, Mesa's
software Vulkan (`mesa-vulkan-drivers`) and OpenGL ES (`libgles2`, `libegl-mesa0`,
`libgl1-mesa-dri`) run them with no GPU, and `SDL_VIDEO_DRIVER=offscreen` draws the windowed
path on them too. CI sets `SLASH_EXPECT_GPU=1` on Linux so they fail there instead of skipping.

V-Sync and the frame cap follow Godot: `window.vsync_mode` and `max_fps` in `project.json`,
`--vsync` and `--max-fps` on `slash-cli run`, and `display.set` while a served game runs
(`display.list` and `perf.stats` too; `engine/command/src/live.cpp`). `platform::Loop` paces
windowed frames to the cap, never headless ones, and `Loop::interpolation()` gives the fraction
of the next tick for drawing between ticks.

The render scale follows Godot's `scaling_3d` settings, applied to the 2D world:
`rendering.scaling` in `project.json` (`mode`, `scale`, `fsr_sharpness`, `upscaler`) and
`display.set`'s `scaling`. `CanvasRenderer` draws the world, with the canvas layers below it,
into an upscaler's input at the render size, scales it over the screen, then draws the world's
controls and the layers above at full size (`scaled()`; `frame.capture` returns `upscaler` and
`render_size`). `engine/render/include/slash/render/upscaler.hpp` is the interface:
`render::Upscaler`, built-in `bilinear` and `fsr` (AMD's FSR 1, ported in
`engine/render/shaders/fsr_easu.hlsl` and `fsr_rcas.hlsl` and checked against a C++ port of
`ffx_fsr1.h` on every device), and `add_upscaler` for a game's or an SDK package's own, which
refuses temporal ones until the 3D renderer exists. `examples/render-scale` changes it while a
game runs.

## Keyframe animation

`engine/animation` is keyframe animation, as Godot's AnimationPlayer. A `.anim.json` file
(`$cli schema animation`) holds animations by name: a length, a loop mode (`none`, `linear`,
`pingpong`) and tracks. A value track's `path` names a component field or part of one
(`Sprite2D/modulate/a`, `Transform2D/position/y`) and its keys are values with Godot's
interpolation and easing; an event track emits a world event at each key. The
`animation_player` system (priority 900, before `animated_sprite_2d`) plays each entity's
`AnimationPlayer` on its own components, through `ComponentType::assign`, so any reflected field
animates, a game's own included; a track with a `target` acts on the entity the player's
`targets` maps it to, as a Godot track's node path does, and `animation::check_players` (run by
`scene validate` and exports) catches targets missing from `targets`. Keys are passed as Godot passes them: after where playback was,
up to and including where it got to. `animation.eval` returns a track's values at given times;
`examples/keyframe-animation` plays a walk cycle and a hit.

## 2D physics

`engine/physics2d` is 2D physics on Box2D v3, as Godot's. Bodies and areas are components:
`StaticBody2D`, `AnimatableBody2D`, `RigidBody2D`, `CharacterBody2D` and `Area2D`, with
`CollisionShape2D` and `CollisionPolygon2D` shapes and `PinJoint2D`, `DampedSpringJoint2D` and
`GrooveJoint2D` joints. The ECS holds the truth: each tick the `physics2d` system (priority 950)
brings Box2D's world up to date with the components, steps it, writes rigid bodies back, moves
character bodies with `move_and_slide` (`src/character.cpp`) and emits `body_entered`,
`body_exited`, `area_entered` and `area_exited` events. Box2D's lengths are pixels
(`b2SetLengthUnitsPerMeter(64)`), and Godot's rules are kept where Box2D differs: layers and
masks collide when either mask matches, one-way shapes, and waking what rests on a moved body.
`project.json`'s `physics_2d` sets gravity and damping, and `Area2D`s with space overrides change
them for the bodies inside, by priority (`Space::forces` and `apply_areas` in `src/space.cpp`,
applied as forces with Box2D's gravity scale at 0). A `StaticBody2D` with constant velocities is
a kinematic Box2D body moved back after each step, so it carries what touches it without moving.
`CharacterBody2D` follows Godot 4's `move_and_slide` with its floor, wall, platform and
`safe_margin` options. `physics.raycast`, `physics.overlap`, `physics.contacts` and
`physics.gravity` (and `physics2d::raycast`, `gravity` and the rest in C++) ask what is where. Each
`TileMapLayer` with collision polygons is a static body: physics2d asks for its shapes through
the `TileShapeSource` that `canvas::add_systems` sets (`set_tile_shape_source`), so physics2d
doesn't depend on canvas, and rebuilds the body when they change. Results
must be the same on every platform: `physics2d` and the examples build with
`-ffp-contract=off`, and angles go through Box2D's own `b2ComputeCosSin`, not libm.
`examples/physics-2d` has every kind of body and `examples/gravity-and-conveyors` gravity zones and a
conveyor.

## Particles

`CPUParticles2D` is Godot's, with Godot's field names and angles in degrees. The
`cpu_particles_2d` system (priority 1100, after physics; `engine/canvas/src/particles.cpp`)
emits, moves and ages each emitter's particles on the CPU and emits `finished` when a one-shot's
last particle dies; `CanvasRenderer` draws them (`particles_drawn()`). Each `_min`/`_max` pair
is a random range per particle and each `_curve` is `[offset, value]` points over its life.
Random values hash the entity ID, `seed`, cycle and slot, so runs repeat everywhere: keep
particle math in `engine/canvas` (built with `-ffp-contract=off`) and use `slash::cos_sin`
(`core/math.hpp`), not libm. Particles live beside the components, kept by entity ID, so saving,
snapshots and undo leave them alone. `particles.eval` returns live particles, or runs the
settings for a time with `canvas::simulate`; `examples/particles` has a fountain, sparks, smoke
and a portal.

`GPUParticles2D` is Godot's too, its particles moved on the device as a `.process_material.json`
file (Godot's `ParticleProcessMaterial`; `$cli schema process_material`) says. The
`gpu_particles_2d` system (priority 1110; `engine/canvas/src/gpu_particles.cpp`) ports Godot's
node and particles storage: each tick it works out each emitter's `render::ParticleStep`s
(`fixed_fps`, preprocess, the emission cycle, one-shot timing) and keeps those that can still
have made a live particle (`canvas::particle_steps`). `CanvasRenderer` runs the steps its device
hasn't with `RenderDevice::process_particles` and draws with `draw_particles`, so a late
renderer catches up from the steps kept. `engine/render/shaders/particles_process.hlsl` is the
compute shader and `render::process_particles` (`engine/render/src/particles.cpp`) its CPU twin,
which the software and OpenGL ES devices run; a test holds them to the same particles, so a
change to the particle math goes in both. `particles.eval` runs the steps kept on the CPU
(`canvas::gpu_particles`); `examples/gpu-particles` has a fire, sparks and snow.

## Job system

`engine/core`'s `jobs::Pool` (`slash/core/jobs.hpp`) is Godot's `WorkerThreadPool`: tasks, group
tasks and `parallel_for`, on `threading.max_threads` worker threads (project.json; -1 is one per
core, and the web has none). A system added with `.threaded = true` declares the component names
(or other shared state) it `reads` and `writes`; neighbouring threaded systems that don't clash
form a stage and run at once on the world's pool (`World::pool`, `set_pool`). A threaded system
emits events as usual (delivered after its stage, in system order) and creates or destroys
entities, or adds and removes components, through `World::defer`; anything else it touches must
be its own, or declared under a name (physics queries bring Box2D up to date first, so a system
making them writes `"physics2d"`). Results must not depend on the thread count: tests run with
`jobs::Pool(0)` and a pool with threads and compare. `cpu_particles_2d` shares its emitters out
with `parallel_for`, and Box2D steps on the pool through its task callbacks (`enqueue_task` in
`engine/physics2d/src/space.cpp`). `examples/job-system` is a meadow of threaded systems.

## Profiling

Worlds time every tick and system over the last 120 ticks (`World::tick_timing`,
`SystemInfo::timing`; a threaded system's own time on its thread), and `perf.stats` returns
them in every session, the slowest systems first (`top` of them); a running game's version
(`register_live_commands` replaces the command) adds frame pacing. `perf.stats` is
`engine/command/src/perf.cpp`.

For a timeline of every thread, build with the `tracy` preset (`cmake --preset tracy`, which sets
`SLASH_PROFILER=tracy`) and connect the [Tracy](https://github.com/wolfpld/tracy) profiler, the
release that matches `cmake/dependencies.json`, to `127.0.0.1`. The macros in
`slash/core/profile.hpp` (`SLASH_PROFILE_ZONE`, `SLASH_PROFILE_ZONE_NAMED`, `SLASH_PROFILE_FRAME`,
`SLASH_PROFILE_THREAD`) send it zones, frames and thread names, and compile to nothing in any
other build: the world steps, each system and command, Box2D's step, drawing and asset polling
are zones, frames start in `platform::Loop`, and the job system's threads are named. Add zones to
code worth seeing; they change no layout, so gameplay modules built without Tracy still load.

`perf.stats` with `"memory": true` adds what the process uses (`platform::process_memory`) and
`World::memory`: the world's own part, one for components (each type's storage plus
`heap_bytes` of its fields, through `ComponentType::memory`), and a part from each memory source.
A module that keeps state outside components adds a source when it installs
(`World::add_memory_source`, as Godot's `Performance.add_custom_monitor`): physics2d, audio,
both particle systems and ui's glyph atlases do. Asset types say what a loaded asset holds with
`add_type`'s `bytes` function, else the file's size counts; a running game adds the render
device's texture bytes (`render::Stats::texture_bytes`). Count from capacities, and keep a
source cheap: agents call it between ticks. `examples/memory-report` finds a growing trail.

## UI and text

`engine/text` is fonts and text layout, as Godot's text server: `.ttf` and `.otf` files, or
`.font.json` files that name one with settings (`$cli schema font`), shaped with HarfBuzz and
broken into lines that wrap as Godot's autowrap modes. `text::Fonts` makes glyph images with
FreeType, or multichannel signed distance fields with msdfgen for fonts that ask, and packs them
into atlas pages. A font path of `""` is Roboto, built into the engine from the fetched
dependency (no font file is committed). Glyph advances are whole pixels except in distance
field fonts, as Godot's are without subpixel positioning.

Each paragraph is ordered by the Unicode Bidirectional Algorithm with SheenBidi and split into
runs by script, as Godot's `TextServerAdvanced`; glyphs are stored per line in drawing order,
and right-to-left lines (`Line::rtl`) hang their trailing spaces off the left and are trimmed
there. A run shapes with the font, then the clusters it lacks with its `.font.json` file's
`fallbacks` (ordered by `language_support` and `script_support` for the text's
`LayoutOptions::language` and script), then with fonts installed on the machine
(`platform::system_fonts_for`: CoreText, DirectWrite, fontconfig loaded with `dlopen`, Android's
font list; none on the web), counted in `TextLayout::system` and turned off by
`gui.allow_system_fallback`. Tests and examples that print text layout turn them off so their
output is the same on every machine, and make the fonts they need with `text::box_font` (a
TrueType font of box glyphs) rather than committing font files. `Control.layout_direction`
mirrors layout as Godot's `is_layout_rtl`; `examples/text-languages` is a menu in Arabic.

`engine/ui` is UI, as Godot's Control nodes. `Control` places a rect in its parent's (the
entity listed in a `Control`'s `children`, or the viewport) by anchors, offsets and grow
directions, turned and scaled around `pivot_offset`; `Label`, `ColorRect`, `Panel`,
`TextureRect`, `NinePatchRect` and `ProgressBar` draw in it. A container component (`HBoxContainer`,
`GridContainer`, `FlowContainer`, `MarginContainer`, `CenterContainer`, `PanelContainer` and the
rest) places the control's children instead of their anchors. `ui::layout` (`src/layout.cpp`)
follows Godot's `Control`, `Label` and container code, line for line where it can: minimum
sizes, size flags and stretch ratios, wrapping, `max_lines_visible`, trimming and alignment.
Theme items come from `theme_override_*` fields, then `.theme.json` themes (`src/theme.cpp`; a
control's own and its parents' `Control.theme`, then the project's `gui.theme`; `$cli schema
theme`), then `ui::default_theme`, Godot's defaults; `ui.theme` says which one each item came
from. Style boxes (`src/style.cpp`) are Godot's `StyleBoxFlat` and `StyleBoxTexture`.
`ui::draw_calls` (`src/draw.cpp`) turns a layout into triangles, drawing flat style boxes as
`StyleBoxFlat::draw` does, which `CanvasRenderer` draws above the world (`controls_drawn()`,
`boxes_drawn()`, `glyphs_drawn()`); distance field glyphs shade in `material.hlsl` through
`render::Material::field`, and `render::distance_field_coverage` is its CPU twin, compared with
it in a test. The viewport is the project's window size (`ui::settings`; windowed games and
`slash-cli run` set it to the window's each tick with `ui::set_viewport`); `ui.layout` returns
every control's rect, lines and problems, `text.measure` sizes text, and `ui::check_scene` backs
`scene validate` and exports. `examples/ui-and-text` is a pause screen and
`examples/ui-containers` an inventory laid out by containers and styled by a theme.

Buttons and focus are Godot's `BaseButton`, `Button`, `TextureButton` and the GUI half of its
`Viewport`. The `gui` system (`src/gui.cpp`, priority -900, right after `input`) sends each
tick's `input::events` to controls: hover by `mouse_filter` (`update_mouse_over`), mouse focus
and clicks, touches per finger, then keys, gamepads and actions to the control with focus and
the `ui_*` actions that move focus (`find_next_valid_focus`, `focus_neighbor` with Godot's legacy
score), then shortcuts. It keeps its state by entity ID in the world's context, so only controls
with IDs take part; `ui::save` and `ui::restore` let sessions put it back with the world. Each
port names the Godot function it follows. UI events (`ui::events`, `ui::was_pressed`) are world
events too. `ui::layout` lays buttons out by `ui::draw_mode` (`layout.buttons`), `ui.press` and
`ui.focus` drive them, and `ui::check_buttons` catches bad shortcuts, groups and focus fields.
`examples/ui-buttons` is a title menu worked by mouse, keys and touch.

`CheckBox` and `CheckButton` are structs derived from `Button` (reflection reads a base's
fields), so `button_of` serves every button type; layout adds their icon and Godot's internal
margin on its side (`CheckWork` in `src/layout.cpp`). `Slider` is Godot's `HSlider` and
`VSlider` over `Range`: `range_of`, `range_value` and `range_ratio` in `src/ui.cpp` follow
`Range::_calc_value`, `get_as_ratio` and `set_as_ratio`, `slider_input` in `src/gui.cpp` follows
`Slider::gui_input`, and `track_sliders` snaps values set from outside and emits their
`value_changed`. Theme `icons` name PNG files or Godot's default icons, which `src/icons.cpp`
draws in code from the same shapes (`ui::builtin_icon`, `builtin:` paths, which
`CanvasRenderer` uploads like files). `examples/ui-settings` is a settings screen.

`ScrollBar` and `ScrollContainer` are Godot's. Layout places a container's children and bars as
`ScrollContainer::_reposition_children` and `_update_scrollbars` do and records each one's
`ui::ScrollView` in `Layout::scrolls`; a control under a clip (`clip_contents`, every scroll
container) gets a `Placed::clip`, which drawing cuts its triangles to and hit tests honor. In
`src/gui.cpp`, `bar_input` follows `ScrollBar::gui_input` and `scroll_input`
`ScrollContainer::gui_input` (the wheel, and touch drags with a deadzone and inertia); a
container's own bars are pointer targets named `<id>/h_scroll` and `<id>/v_scroll` (`Target`),
which never take focus or emit events. The gui state keeps scrolls as doubles while the
component holds Godot's whole pixels: `absorb_scrolls` takes in changes made outside, and
`process_scrolls` glides touch drags, follows focus (`ensure_control_visible`) and holds each
scroll to the layout's range every tick. `examples/ui-scrolling` is a quest log.

`OptionButton`, `MenuButton` and `PopupMenu` are Godot's. A `PopupMenu` is an entity of its own,
not a control, since Godot's is a window; the gui state keeps each open popup's `PopupStatus`
(focused item, scroll, the held click that opened it) by owner ID, one open at a time as
embedded windows are. `ui::popup_items` gives an owner's items, an option button's as radio
items. Layout places each open popup as `PopupMenu::_get_contents_minimum_size`, the buttons'
`show_popup` and `Popup::_popup_adjust_rect` do and draws it as `_draw_items` does
(`Layout::popups`, `ui::PopupView`); `CanvasRenderer` draws popups over every control
(`canvas::FrameItem::Kind::Popup`). In `src/gui.cpp` the focused popup takes every event first
(`forward_to_popup`, as `Viewport::_sub_windows_forward_input`): `popup_menu_input` follows
`PopupMenu::_input_from_window_internal`, a left press outside marks it closing (hidden at the
end of the tick, as `Popup::_close_pressed` defers it) and goes on to the controls;
`show_popup`, `hide_popup` and `activate_item` follow the three classes' functions of those
names. `ui.popup`, `ui.select` (`ui::open_popup`, `ui::select_item`) drive them.
`examples/ui-menus` is a settings screen with a menu bar.

Tooltips and drag and drop are the rest of Godot's `Viewport` GUI: the gui state's
`TooltipStatus` is its `tooltip_*` (the rest timer runs in world time, so ticks repeat; an open
popup's items have tooltips) and `DragStatus` its `drag_*` and `global_dragging`, each function
named after the one it ports (`_gui_show_tooltip`, `_gui_drop`, `gui_perform_drop_at`,
`force_drag`). The `DragAndDrop` component stands for a script's `_get_drag_data`,
`_can_drop_data` and `_drop_data`; `ui::set_drag_forwarding` keeps a game's C++ calls by ID in a
holder snapshots leave alone. Text fields drag their selection as `LineEdit` and `TextEdit` do
(`drag_attempt`, the drop caret, `NOTIFICATION_DRAG_END` in `end_drag`). Layout makes
`Layout::tooltip` (`TooltipLabel` in a `TooltipPanel`, placed as `_gui_show_tooltip_at`) and
`Layout::drag` (the preview root placed at the pointer, or a text field's text): the tooltip
draws over the popups (`canvas::FrameItem::Kind::Tooltip`), the preview under them. `ui.drag`
and `ui.tooltip` drive them. `examples/ui-drag-and-drop` is an inventory.

Text fields are Godot's `LineEdit`. The gui system edits them (`Editor` in `src/gui.cpp`, which
follows `LineEdit::gui_input`, its undo stack and its caret and selection calls) from key events
with a `unicode` character, `ime` events and the `ui_text_*`, `ui_cut`/`copy`/`paste` and
`ui_undo`/`redo` actions, which `Map::with_defaults` binds to Ctrl on a PC and Command and
Option on a Mac (`input::Keyboard`; tests pin `Keyboard::Pc`). `src/line_edit.cpp` is the
geometry they share with the layout: shaping, scrolling, carets, hit tests and drawing, on
`text::ShapedLine`'s carets, selections and word breaks (`engine/text/src/editing.cpp`).
`LineEdit.text` and `caret_column` are the truth; the rest stays in the gui state, and a text
change from outside resets the undo history. `ui::text_input` tells the platform where the caret
is and which on-screen keyboard to show (`SdlReader::set_text_input`), and
`ui::set_clipboard_backend` connects SDL's clipboard in a window; `ui.clipboard` reads and sets
it. `examples/text-input` is a sign-up form typed into with `input.inject`'s `text` and `ime`
events.

`TextEdit` is Godot's text field of several lines. `src/text_edit.cpp` is its geometry, shared
with the layout: `TextArea` holds the lines shaped and wrapped (tabs every four spaces through
`text::LayoutOptions::tab_stops`) and ports Godot's viewport, scroll bar, hit test, minimum size
and drawing code. `src/text_editor.cpp` is the editing: `gui_input`, the caret and selection
calls and Godot's undo history (operations chained into complex steps, typing merged until a
three-second pause). `TextEdit.text`, `caret_line`, `caret_column` and the scroll are the truth;
`TextEditState` keeps the rest in the gui state, and text set from outside goes in as Godot's
`set_text`, as an undoable step. `text_changed` and `caret_changed` are deferred to the end of
the tick, as Godot defers them. Its scroll bars are the gui system's virtual bar targets, as a
`ScrollContainer`'s are. `src/syntax_highlighter.cpp` ports Godot's `CodeHighlighter` line by
line for the `CodeHighlighter` component, and `text_edit_search` (in `src/text_edit.cpp`) its
`TextEdit::search` for `search_text` and `ui.search`. Gutters are Godot's: `TextArea` sizes,
draws and hit tests them (a `CodeEdit`'s `main_gutter` and `line_numbers`, then the
`TextEdit`'s own), and tags each line with the line it came from (`mark_lines`, `moved_lines`)
so the editor's `move_line_marks` keeps breakpoints and gutter entries on their lines as
Godot's per-line data does. `examples/text-editor` writes a letter and `examples/code-editor`
colors, numbers, searches and scrolls code.

## Input

`engine/input` is input, as Godot's InputEvent, InputMap and Input. Events (`input::Event`: key,
mouse button and motion, screen touch and drag, gamepad button and motion, action) queue with
`input::push` and apply when the `input` system runs, first in each tick (priority -1000), so
runs repeat. Gameplay code asks about actions: `is_action_pressed`, `is_action_just_pressed`
(the tick it changed; a press and release in one tick count as both), `get_action_strength`,
`get_axis` and `get_vector`, as Godot's, and `input::map(world)` rebinds them. `project.json`'s
`input` adds actions over Godot's `ui_*` ones (`Map::with_defaults`) and `input_devices` sets
touch and mouse emulation. Matching follows Godot's `action_match` (`src/event.cpp`); the state
follows `Input::_parse_input_event_impl` (`src/state.cpp`). `input::SdlReader` (`src/sdl.cpp`)
turns SDL's events into input events: games call it from `Game::handle_event` and the platform's
event handler, and so does `slash-cli run`. Agents send events with `input.inject` and read
`input.state` and `input.actions`; sessions put input back with the world when a command fails or
is a dry run. `examples/input-actions` moves a player with keys, a stick and taps.

## Audio

`engine/audio` is sound, as Godot's AudioServer, on miniaudio's decoders. `.wav` files decode to
samples when they load (integer samples scaled as Godot's importer scales them), and `.ogg` and
`.mp3` files decode as they play. A `.audio.json` file (`$cli schema audio`) gives a sound
Godot's loop settings, or picks one of several at random each play, as Godot's
`AudioStreamRandomizer` (`src/stream.cpp`, with Godot 4.4's cubic resampler). It can also be
music, ported from Godot's `modules/interactive_music` (`src/music.cpp`): a playlist, layers
played together, or interactive clips that switch by its transitions when a player's
`switch_to_clip` parameter asks; players hand their `parameters` to their playbacks
(`Playback::set_parameter`), and the audio system writes `switch_to_clip` back as the music
moves on. Generators and polyphonic streams, as Godot's, are what gameplay code pushes frames to
or starts sounds on, through `audio::stream_playback` (`generator()`, `polyphonic()`). A
microphone stream plays the server's input, which records only with project.json's
`audio.enable_input`: each tick the audio system pushes the tick's frames from an
`audio::InputDevice` (`audio::SdlInput` in a window, silence headless) or the sound
`audio.input` plays in its place, before mixing. Exports of such games ask for the microphone
(`export/CMakeLists.txt`'s `SLASH_GAME_MICROPHONE`).
`audio::Server`
(`src/server.cpp`) is the mixer: playbacks mix into buses in steps of 512 frames, with a 64-frame
lookahead, volume ramps and stop fades; each bus runs its effects (`src/effects.cpp`: 24 of Godot's
`servers/audio/effects`, each ported from the one of the same name, the pitch shift and spectrum
analyzer with Bernsee's FFT code under its own notice) and sends to an earlier bus, down to
Master. `Server::effect_instance` and `spectrum_analyzer`, `capture_effect` and `record_effect`
reach a bus's analyzer, capture buffer and recorder from C++. A `.bus_layout.json` file
(`$cli schema bus_layout`; project.json's `audio.bus_layout`) is Godot's `default_bus_layout.tres`.
`AudioStreamPlayer`, `AudioStreamPlayer2D` and `AudioListener2D` are components, played by the
`audio` system (priority 1200, last), which mixes each tick's share of frames, so the same ticks
make the same sound, and hands them to a sink: `audio::SdlOutput` in a window, nowhere headless.
An `Area2D` with `audio_bus_override` sends the 2D players inside it whose `area_mask` it
matches to its `audio_bus_name`, as Godot's `_get_actual_bus`. Sessions keep ten seconds of each bus for `audio.capture`, which measures it (peak, RMS and
slices in dB) or writes a `.wav` file; `audio.spectrum` reads a bus's `spectrum_analyzer` in
frequency bands, `audio.record` starts and stops its `record` effect and writes the take, and
`audio.state`, `audio.play`, `audio.stop` and `audio.set_bus` do the rest. Sessions put the
buses back with the world when a command fails or is a dry run. `audio.analyze`
(`src/analysis.cpp`) judges music without listening: loudness and true peak as EBU R 128 measures
them (libebur128), clipping, silence, tempo and first beat, key, octave bands and whether a loop
clicks where it joins, for a sound, a `.audio.json` sound as it loops or what a bus played;
`examples/music-analysis` measures a groove. Example sounds are made in code
without libm, so outputs match everywhere. `examples/audio` plays music and footsteps panned
around a listener, `examples/audio-areas-and-analysis` a bat in a cave's reverb, measured
by its spectrum and recorded, and `examples/interactive-music` a score that follows a fight.

## Soundtracks

`engine/soundtrack` turns music into text agents read and write. A `.score.json` file
(`$cli schema score`; `src/score.cpp`) holds a tempo, meter, key, named sections and tracks of
General MIDI instruments or drums, each bar a line of notes such as `"D2:4 ; D3 F3 A3 D4@110"`;
`decode_bar` reads a line and `encode_bars` writes notes back as the shortest line that plays
them. `score.export` and `score.import` (`src/midi.cpp`) write and read Standard MIDI Files.
`soundtrack.render` (`src/render.cpp`) plays a score through TinySoundFont into an `.ogg` or
`.wav` stem per track, each with a `.audio.json` file giving its `bpm` and `beat_count`, and a
synchronized `.audio.json` file that plays them together; looping stems are the score's exact
length, with what rings past the end added onto the start. Its instruments are a SoundFont 2
file or the engine's own (`src/sound_bank.cpp`: a voice per General MIDI family and a drum kit,
synthesized without libm). TinySoundFont and libvorbis are fetched for the tools only. Scores
are a game's source, not its data, so exports leave them out. `examples/music-scores` sends a
piece through MIDI and back, and `examples/soundtrack-render` renders a loop and measures it.

## Ads, analytics, purchases and achievements

`engine/services` is ads, analytics, in-app purchases, achievements and leaderboards behind one
API, as Unity's services and Social API
([docs/reference/services.md](docs/reference/services.md)). `project.json`'s `services` names
ad placements (banner, interstitial or rewarded, with each platform's ad unit ID), products
(consumable, non-consumable or subscription), achievements (unlocked at once or in `steps`) and
leaderboards (`high_to_low` or `low_to_high`); games call `show_ad`, `log_event`, `buy`,
`finish`, `unlock_achievement`, `increment_achievement` and `submit_score` by those names and
read what happened from `services::events(world)` (also world events), which the `services`
system (priority -800) makes from what backends reported since the last tick. Backends
implement `AdsBackend`, `AnalyticsBackend`, `PurchasesBackend` and `GamesBackend` and report
through a thread-safe `Reports`; `set_backends` installs them. The player signs in at start,
as on Play Games and Game Center; progress and best scores made while signed out are kept and
sent once they sign in. Sessions run on the test backend (`use_test_backend`), whose ads,
purchases, sign-in and leaderboards behave as `services.test` says, and put its state back with
the world when a command fails or is a dry run; games with no backend fail ads, purchases and
sign-in and only keep events, progress and scores. Analytics events are checked against the
strictest common rules and sent only with the player's consent (`set_consent`).
`services.state` shows it all. `examples/services` is a shop selling coins for watched ads and
purchases, and `examples/achievements-and-leaderboards` a race with lap achievements and a
best-lap leaderboard.

## AI providers

`engine/ai` calls language models through `ai::Provider`, one interface over any vendor
([docs/reference/providers.md](docs/reference/providers.md)). Adapters translate a `Request`
(messages with text, images, tool calls and tool results; tools; an `output_schema`; params) and
the reply for OpenAI's chat completions (also Ollama, llama.cpp, vLLM, LM Studio and OpenRouter),
Anthropic's Messages API and Gemini (`src/openai.cpp`, `src/anthropic.cpp`, `src/gemini.cpp` on
`src/http_provider.cpp`, which retries and streams server-sent events). A reply keeps what its
provider needs back in later turns (thinking blocks, thought signatures) in `Message::native`.
`fit_request` puts an output schema in the prompt for models that can't take one, and
`parse_output` checks every reply against it. `providers.json` (`ai::load_config`,
`$cli schema providers`) names providers and roles; keys come from the environment variables it
names, never from the file. `ai::open` gives a role's provider, wrapped in a `ReplayProvider`
when the file names a replay file: answers keyed by the model and the whole request, recorded
once and replayed after, so tests run offline (`SLASH_AI_REPLAY=replay` in CI). `ai::Call` runs
a request on its own thread so games never wait on a model. Gameplay code calls `ai::ask`
(`src/world.cpp`), and the `ai` system (priority 1150) emits `ai_reply` or `ai_failed` world
events; sessions and games with a `providers.json` install it (`SLASH_GAME_AI` in exports, which
stage the file without its replay settings), headless runs wait for each reply so they repeat,
and a session without the `network` capability answers only from the replay file. `slash_ai_commands` adds
`command_tools` and `run_tool_calls` (engine commands as a model's tools) and the `ai.providers`
and `ai.complete` commands, which need the `network` capability. Tests talk to a fake
`ai::Transport` or a local server, never a real model. `src/transport.cpp` is the HTTP client:
httplib with Mbed TLS, which checks `https://` servers against the system's certificates
(SecTrust on Apple platforms, which `apple_trusts` asks), or `SSL_CERT_FILE`'s, and reaches them
through the proxies curl's variables name (`ai::proxy_for`). Its tests make their certificates
when they run, so no key is committed. The web has no threads, so there `Call` uses
`Provider::start`, which the adapters, roles and replay files run on `Transport::post_async` and
`Transport::later`: the web's transport calls the browser's `fetch` through `EM_JS` and reads
the body as it streams, and retries wait on browser timers (`DeferredTransport` in
`tests/test_ai.cpp` tests the same path natively). `examples/ai-provider` is a shopkeeper who talks through a
model, and `examples/ai-dialogue` a baker who talks from gameplay code.

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

A game's own components are registered the same way in its `SLASH_GAME_MODULE` function, with
`game.add_component<Health>()` (see [Gameplay code](#gameplay-code)).

Supported field types are listed in `engine/world/include/slash/world/serialize.hpp`. Use
`SLASH_REFLECT` for structs that appear inside components. Built-in components are in
`engine/world/include/slash/world/components.hpp`.

## Examples

`examples/` holds one small working example per feature, listed in
[examples/README.md](examples/README.md). An example is a folder with a `README.md` and a
`main.cpp` (built as `example-<folder>`, its output in `expected.txt`), a `calls.jsonl` of
JSON-RPC requests (the responses in `results.jsonl`), or both; a `project.json` makes it a game
project that must validate. `ctest` runs each in a copy under `build/dev/examples/` and compares
the output exactly. After changing an example or a command it calls, rewrite the expected output
and read the diff before committing:

```bash
SLASH_UPDATE_EXAMPLES=1 ctest --preset dev -R example
git diff examples/
```

## Lint

```bash
find engine tools tests examples -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
find engine tools examples -name '*.cpp' | xargs clang-tidy -p build/dev
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
  Modules: `core` (Result, logging, JSON, reflection, math, the job system), `platform` (SDL3 window, run loop,
  files and folder listings, child processes), `render` (the render device over SDL3's GPU API and OpenGL ES 3.0 / WebGL 2,
  the software and null devices, HLSL shaders, material programs, 2D light shading and particles), `asset` (asset handles, loaders, hot reload),
  `world` (EnTT world, components, scenes and prefabs), `input` (input events, action maps,
  the input state and reading SDL's events), `audio` (sound files and streams, the mixer, buses
  and effects, players and SDL's audio output), `services` (ads, analytics, in-app
  purchases, achievements and leaderboards, their backends and the test backend), `canvas` (sprites, `.frames.json`
  animations, `.material.json` materials, `.tileset.json` tile maps, 2D lights and shadows, CPU
  particles, GPU particles' steps and drawing them), `text` (fonts, shaping, line breaking, carets and selections, and glyph atlases), `ui`
  (controls, containers and themes: layout, draw calls, buttons, focus and text fields), `animation` (`.anim.json` keyframe
  animations and the system that plays them),
  `physics2d` (2D bodies, areas, joints and queries on Box2D), `library` (the asset library, its
  generators and commands), `storyboard` (storyboard kinds, files, versions,
  queries, edits, briefs and checks), `soundtrack` (`.score.json` scores, MIDI files and rendering stems),
  `command` (commands, sessions with undo, JSON-RPC and MCP),
  `debug` (the debug overlay over a running game), `server` (localhost HTTP over a session),
  `ai` (language model providers, their adapters, `providers.json`, replay and the `ai.*`
  commands),
  `gameplay` (loading and reloading a game's own code) and `runtime` (project files and the game
  executable).
  Tool-only libraries such as `tools/exporter`, `tools/importer` and `tools/shader_compiler` use
  the same layout and may use exceptions.
- New dependencies are pinned in `cmake/dependencies.json` (repository, tag, SPDX license and
  the license file's SHA-256), fetched with `slash_add_dependency` in `cmake/Dependencies.cmake`
  and listed in `THIRD_PARTY.md`. Only permissive licenses (MIT and its variants, BSD, zlib,
  Apache 2.0, Boost, MIT-0, the FreeType License, public domain; glslang's Bison-exception
  parser, in the tools only); the `licenses` test fails anything else. Never copy code from closed
  engines.
- Every behavior change comes with a test in `tests/`. Tests must pass headless.
- Every new feature comes with an example in `examples/` (see below).
- A change to a command, flag, component or file format updates its page in `docs/reference/`,
  and work that finishes a roadmap item ticks it in `docs/roadmap.md`.
