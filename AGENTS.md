# AGENTS.md

Instructions for AI agents (Claude Code, Codex, Cursor and others) working on SlashEngine. Read
[docs/design.md](docs/design.md) for the full design; this file covers the day-to-day loop.

## Build and test

```bash
cmake --preset dev               # configure (first run fetches SDL3 and doctest)
cmake --build --preset dev       # build engine, slash-cli and tests
ctest --preset dev               # run unit and smoke tests
```

The `ci` preset is what CI runs: same build with warnings as errors. Use it before you push.

On Linux, SDL3 needs the X11/Wayland development packages listed in
`.github/workflows/ci.yml`.

## Run the engine

```bash
./build/dev/tools/slash-cli/slash-cli version                     # {"name":"SlashEngine","version":"..."}
./build/dev/tools/slash-cli/slash-cli run --headless --ticks 120  # no window or GPU
./build/dev/tools/slash-cli/slash-cli run                         # opens a window until closed
```

`slash-cli` prints results to stdout as JSON and logs to stderr as JSON lines. Exit codes: 0
success, 1 runtime failure, 2 usage error. On a machine with no display, set
`SDL_VIDEO_DRIVER=offscreen` to exercise the windowed path.

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
- New dependencies go in `cmake/Dependencies.cmake`, pinned to a tag, and in `THIRD_PARTY.md`
  with their license. Only permissive licenses (MIT, BSD, zlib, Apache 2.0, Boost, MIT-0,
  public domain). Never copy code from closed engines.
- Every behavior change comes with a test in `tests/`. Tests must pass headless.
