# Build and test

Full build, test, lint and CI-parity commands for engine work, per platform.

Status: current · Updated: 2026-11-22

```bash
cmake --preset dev               # configure (first run fetches the dependencies)
cmake --build --preset dev       # build engine, slash-cli and tests
ctest --preset dev               # run unit and smoke tests
```

The `ci` preset is what CI runs: warnings as errors and no debug info. Use it before you push.
`-DSLASH_DEBUG_INFO=OFF` leaves debug info out of any build: compiles take about half as long,
objects a twentieth of the space (so ccache keeps a whole build) and links far less memory; CI
and cloud sessions build that way, and a debugger still has function names.
Ninja links one Debug executable per 6 GB of memory (the `slash_link` pool in `CMakeLists.txt`,
since each link with debug info takes nearly 4 GB with GNU ld); set `CMAKE_JOB_POOL_LINK` to use
your own.

On Linux, SDL3 needs the X11/Wayland development packages listed in
`.github/workflows/ci.yml`.

## Cheat sheet

Set up once per machine:

| Platform | Setup |
| --- | --- |
| Linux | `sudo apt-get install` the `LINUX_SDL_DEPS` in `.github/workflows/ci.yml`, plus `mesa-vulkan-drivers libgl1 libgles2 libegl-mesa0 libgl1-mesa-dri` for the GPU tests, `at-spi2-core` for the screen reader test and `ccache clang-format-18 clang-tidy-18` |
| macOS | Xcode's command line tools, then `brew install ninja ccache` |
| Windows | Visual Studio 2022's C++ tools, `choco install ninja` and Git for Windows (libaom's build runs its Perl); run everything from a Developer PowerShell (x64) |
| Claude Code cloud session | Nothing: `.claude/hooks/session-start.sh` installs the Linux packages, then configures `dev` with ccache, builds the tools and `slash_tests`, installs the web toolchain and, where dl.google.com is reachable, the Android SDK in the background (Wayland comes from Ubuntu's archive where gitlab.freedesktop.org is blocked). Wait for it with `flock /tmp/slash-session-setup.lock true`; it logs to `.slash/session-setup.log`. Build only the examples you need (`--target example-<folder>`): all of them take some 20 GB in a Debug build |

Screen readers come through AccessKit, which builds with cargo: install Rust 1.87 or later with
[rustup](https://rustup.rs) (CI's runners have it), and `rustup target add` the targets a build
makes code for, such as `x86_64-apple-darwin` for a universal Mac. Without them, builds and
exports leave AccessKit out and say so when configuring.

The loop, the same everywhere (add `-DCMAKE_C_COMPILER_LAUNCHER=ccache
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` to a first configure
to cache compiles):

```bash
cmake --preset dev && cmake --build --preset dev   # build
ctest --preset dev                                 # every test
ctest --preset dev -R 'unit_|example_physics-2d'   # some, by ctest name
./build/dev/tests/slash_tests -tc='*tile*'         # unit test cases by name (doctest flags)
./build/dev/tests/slash_tests -sf='*test_mesh*'    # a test file's cases
cmake --build --preset dev --target slash-cli      # one target
SLASH_UPDATE_EXAMPLES=1 ctest --preset dev -R example_  # rewrite examples' expected output
```

Before pushing:

```bash
cmake --preset ci && cmake --build --preset ci && ctest --preset ci   # warnings as errors
git diff --name-only --diff-filter=d origin/main -- '*.cpp' '*.hpp' | xargs clang-format-18 -i
.github/scripts/check-libcxx.py                    # changed files against libc++
# clang-tidy as the lint job runs it: a build configured with Clang 18, then the changed files
cmake --preset ci -B build/tidy -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
cmake --build build/tidy --target slash_api_hash
git diff --name-only --diff-filter=d origin/main -- '*.cpp' | grep -E '^(engine|tools|examples)/' \
    | xargs clang-tidy-18 -p build/tidy --quiet
```

`check-libcxx.py` syntax-checks the `.cpp` files changed since `origin/main` (or the files it is
given) with Emscripten's Clang against libc++, using the flags in `build/dev`'s
`compile_commands.json`. The web, Android and llvm-mingw builds use libc++ and Windows builds use
MSVC's STL; in both, `std::array`, `std::string_view` and `std::span` iterators are classes rather
than pointers and headers include fewer others, so code GCC accepts can fail there. Run it before
every push that changes C++. It needs `slash-cli toolchain install web`.

The unit tests run as seven ctest shards (`unit_1` to `unit_7`, by test file name). A test case
that runs over eight minutes is stuck: the watchdog in `tests/test_main.cpp` names it and stops
the shard, and `tests/retry_stuck.cmake` runs the shard once more, so a hang that comes back
still fails. Each case of 5 s or more is written to `build/<preset>/tests/unit_<n>-times.txt`
(`SLASH_TEST_TIMES`), which CI prints after the tests, slowest first.

Claude Code sessions run `.claude/hooks/task_nudge.py` after each tool call. It says nothing
until one task has taken 30 minutes, then adds a `[task-nudge]` line asking the agent to check
whether it is going in circles (a subagent is told to hand back what it has), and again every
30 minutes after. Waiting on a long run you started is fine. `TASK_NUDGE_MIN` changes the limit,
and `TASK_NUDGE_OFF=1` turns it off.

## Headless tests

`engine/command/src/test.cpp` is `test.run` (and `$cli test`): it runs `.test.json` files
(`$cli schema test`; tests by name, each a scene and steps of command calls with what each reply
must hold in `expect`) and returns only the checks that failed, with the JSON Pointer of each
into its test file. Each test opens its scene file in a new `Session` over the same registries,
with `SessionOptions::write_files` off (commands write files only through `Session::write_file`,
which refuses then) and no `build` capability. A test fails at a step whose command fails, a
reply that doesn't hold what it expects, or a warning or error logged while it runs, which it
sees by wrapping the log sink (`log::sink`). Run with no `paths` or `tests`, it also runs
`storyboard.check`, `soundtrack.check` where it is registered, and `l10n.check` once the project lists translations. Games made by `$cli init` start with `tests/main.test.json`, and their
workflow runs `$cli test`. `examples/headless-tests` breaks a scene and reads the failures.

## Examples

`examples/` holds one small working example per feature, listed in
[examples/README.md](../../examples/README.md). An example is a folder with a `README.md` and a
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
find engine tools examples -name '*.cpp' -not -path '*/unreal/Source/*' | xargs clang-tidy -p build/dev
```

CI rejects unformatted code and any clang-tidy finding.

CI's lint job also runs `scripts/check-owner-name.sh --pr` (the owner's name in files, the PR's title,
body, commit messages and commit authors) and `scripts/check-secrets.sh --range`: gitleaks
(`.gitleaks.toml`: its own rules plus the APPLE_\*, BENCH_\*, store and `SLASH_*_PASSWORD` variables)
over the new commits, tracked files that hold credentials (`.env`, `*.p12`, `*.p8`, keystores,
provisioning profiles, steamcmd's `config.vdf`, ...; `.gitignore` keeps them out too), and commit
e-mails that name a machine (`login@host.local`). Run `scripts/check-secrets.sh` before pushing to
check `origin/main..HEAD`, or `--all` for the whole history. Findings print redacted; a real secret
is rotated first, then dropped from the branch.
