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
  files) and `world` (EnTT world, components, scenes).
- New dependencies go in `cmake/Dependencies.cmake`, pinned to a tag, and in `THIRD_PARTY.md`
  with their license. Only permissive licenses (MIT, BSD, zlib, Apache 2.0, Boost, MIT-0,
  public domain). Never copy code from closed engines.
- Every behavior change comes with a test in `tests/`. Tests must pass headless.
