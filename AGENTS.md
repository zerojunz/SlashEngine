# AGENTS.md

SlashEngine is a lightweight, open-source C++20 game engine whose primary user is an AI agent.
This file is the day-to-day loop; detail per area is in `docs/reference/` (index below), the design in [docs/design.md](docs/design.md), PR rules in [CONTRIBUTING.md](CONTRIBUTING.md).

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
- Permissive open-source code only. When a design choice is open, follow the approach of the MIT-licensed engine much of the code is ported from (see `THIRD_PARTY.md`).
- Gameplay code is plain C++; no embedded scripting language.

## Build and test

```bash
cmake --preset dev               # configure (first run fetches the dependencies)
cmake --build --preset dev       # build engine, slash-cli and tests
ctest --preset dev               # run unit and smoke tests
```

```bash
cmake --preset dev && cmake --build --preset dev   # build
ctest --preset dev                                 # every test
ctest --preset dev -R 'unit_|example_physics-2d'   # some, by ctest name
./build/dev/tests/slash_tests -tc='*tile*'         # unit test cases by name (doctest flags)
./build/dev/tests/slash_tests -sf='*test_mesh*'    # a test file's cases
cmake --build --preset dev --target slash-cli      # one target
SLASH_UPDATE_EXAMPLES=1 ctest --preset dev -R example_  # rewrite examples' expected output
```

Before pushing (`ci` preset: warnings as errors, no debug info):

```bash
cmake --preset ci && cmake --build --preset ci && ctest --preset ci
git diff --name-only --diff-filter=d origin/main -- '*.cpp' '*.hpp' | xargs clang-format-18 -i
.github/scripts/check-libcxx.py                    # changed files against libc++; needs `slash-cli toolchain install web`
git diff --name-only --diff-filter=d origin/main -- '*.cpp' | grep -E '^(engine|tools|examples)/' \
    | xargs clang-tidy-18 -p build/tidy --quiet    # build/tidy setup: build-and-test.md
```

Per-platform setup, ccache flags, `-DSLASH_DEBUG_INFO=OFF`, the Ninja link pool, test shards, the stuck-test
watchdog and the `[task-nudge]` hook: [build-and-test](docs/reference/build-and-test.md).

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

## Layout

| Folder | What lives there |
| --- | --- |
| `engine/<module>/` | Engine libraries (`include/slash/<module>/`, `src/`); modules listed in [engine-modules](docs/reference/engine-modules.md) |
| `tools/` | `slash-cli`, `slash-mcp`, `slash-game`, exporter, importer, shader compiler, bench |
| `tests/` | Unit and smoke tests, `tests/data/` fixtures; must pass headless |
| `examples/` | One working example per feature ([examples/README.md](examples/README.md)) |
| `docs/` | [Index](docs/README.md), concepts, design, roadmap, `reference/` pages |
| `cmake/`, `.github/` | Presets, pinned dependencies (`cmake/dependencies.json`), CI and scripts |

## Rules and conventions

- C++20. Engine libraries build with no exceptions and no RTTI; fallible calls return
  `slash::Result<T>` (`engine/core/include/slash/core/result.hpp`). Tools and tests may use
  exceptions.
- Use `slash::bit_cast` (`slash/core/bits.hpp`), not `std::bit_cast`: Linux exports build against
  libstdc++ 10, which lacks it.
- Invariants use `SLASH_ASSERT`, which logs and aborts in every build type.
- Log with `slash::log::info("message", {{"key", value}})`; never print free-form text to stdout
  from engine code.
- Names: `PascalCase` types, `snake_case` functions and variables, `trailing_` private members,
  `kPascalCase` constants.
- Headers live in `engine/<module>/include/slash/<module>/`, sources in `engine/<module>/src/`. What each module holds: [engine-modules](docs/reference/engine-modules.md). Tool-only libraries such as `tools/exporter`, `tools/importer` and `tools/shader_compiler` use the same layout and may use exceptions.
- New dependencies are pinned in `cmake/dependencies.json` (repository, tag, SPDX license and
  the license file's SHA-256), fetched with `slash_add_dependency` in `cmake/Dependencies.cmake`
  and listed in `THIRD_PARTY.md`. Only permissive licenses (MIT and its variants, BSD, zlib,
  Apache 2.0, Boost, MIT-0, the FreeType License, public domain, Unicode-DFS-2016;
  glslang's Bison-exception parser, in the tools only); the `licenses` test fails anything else. Never copy code from closed
  engines.
- Every behavior change comes with a test in `tests/`. Tests must pass headless.
- Every new feature comes with an example in `examples/` (see [Examples](docs/reference/build-and-test.md#examples)).
- A change to a command, flag, component or file format updates its page in `docs/reference/`,
  and work that finishes a roadmap item ticks it in `docs/roadmap.md`.
- **No owner name:** never write the owner's name (first name, full name, username or a possessive) in code, comments, tests, docs, commit messages or PRs; write "the owner". Test data uses a neutral name such as "Player". `scripts/check-owner-name.sh` enforces it in CI (owner, 20 Nov 2026).

Format and lint: `clang-format` and `clang-tidy` as in the commands above; CI rejects unformatted code and any clang-tidy finding ([build-and-test](docs/reference/build-and-test.md#lint)).

## PR flow

1. One roadmap item per PR, on a branch from the latest `main`.
2. A change to a command, CLI flag, component or file format updates its page in `docs/reference/` in the same PR; finishing a roadmap item ticks it in `docs/roadmap.md`.
3. Add or extend tests, and an example in `examples/` where it fits.
4. Run the "Before pushing" commands above; CI must be green on macOS, Linux and Windows.
5. Moving or adding a doc: update [docs/README.md](docs/README.md) and follow [docs/DOCS-STANDARD.md](docs/DOCS-STANDARD.md).
Full PR rules: [CONTRIBUTING.md](CONTRIBUTING.md).

## Reference index

Subsystem detail moved here from this file (20 Nov 2026, see [docs/DOCS-STANDARD.md](docs/DOCS-STANDARD.md)).

| Area | Page |
| --- | --- |
| Build, test, lint, examples, headless tests (full) | [build-and-test](docs/reference/build-and-test.md) |
| Engine modules | [engine-modules](docs/reference/engine-modules.md) |
| Add a component | [components](docs/reference/components.md) |
| Scenes | [scenes](docs/reference/scenes.md) |
| Assets, hot reload | [assets](docs/reference/assets.md) |
| Asset library | [asset-library](docs/reference/asset-library.md) |
| Procedural generation | [procedural](docs/reference/procedural.md) |
| Storyboard | [storyboard-format](docs/reference/storyboard-format.md) |
| Commands, MCP, JSON-RPC; HTTP server | [protocols](docs/reference/protocols.md) |
| Replays | [replays](docs/reference/replays.md) |
| Watching agents (work journal) | [watching-agents](docs/reference/watching-agents.md) |
| Debug overlay | [debug-overlay](docs/reference/debug-overlay.md) |
| Autopilot | [autopilot](docs/reference/autopilot.md) |
| Game projects and exports | [game-projects](docs/reference/game-projects.md) |
| Gameplay code | [gameplay-code](docs/reference/gameplay-code.md) |
| Importing from Godot, Unity, Unreal | [importing](docs/reference/importing.md) |
| Rendering | [rendering](docs/reference/rendering.md) |
| Keyframe animation | [animation](docs/reference/animation.md) |
| 2D physics | [physics-2d](docs/reference/physics-2d.md) |
| 3D physics | [physics-3d](docs/reference/physics-3d.md) |
| Navigation | [navigation](docs/reference/navigation.md) |
| NPCs | [npcs](docs/reference/npcs.md) |
| Particles | [particles](docs/reference/particles.md) |
| Job system | [jobs](docs/reference/jobs.md) |
| Profiling | [profiling](docs/reference/profiling.md) |
| UI and text | [ui-text](docs/reference/ui-text.md) |
| Localization | [localization](docs/reference/localization.md) |
| Input | [input](docs/reference/input.md) |
| Audio | [audio](docs/reference/audio.md) |
| Video | [video](docs/reference/video.md) |
| Soundtracks | [soundtracks](docs/reference/soundtracks.md) |
| Ads, analytics, purchases, achievements | [services](docs/reference/services.md) |
| Networking | [networking](docs/reference/networking.md) |
| XR | [xr](docs/reference/xr.md) |
| AI providers | [providers](docs/reference/providers.md) |
| Command, CLI, scene, project and other formats | [docs/reference/](docs/reference/) (`commands.md`, `cli.md`, `scene-format.md`, `project-format.md`, ...) |
