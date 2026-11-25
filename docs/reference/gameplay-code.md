# Gameplay code

Writing and reloading a game's own C++ code.

Status: current · Updated: 2026-11-20

A game's own components and systems are plain C++ in the folders and files `project.json`'s
`gameplay.sources` names (`src` in games `slash-cli init` makes), registered in one
`SLASH_GAME_MODULE(game) { ... }` function (`engine/world/include/slash/world/game_module.hpp`).
Exports compile the sources into the game (`SLASH_GAME_SOURCES` in `export/CMakeLists.txt`). The
tools load them as a library instead, on macOS, Linux and Windows:

```bash
$cli build --project path/to/game     # or the gameplay.build command; compiler errors with file and line
$cli build --check                    # or build.check: compile what changed, nothing linked; errors and fixes as data
```

`exporter::build_gameplay` configures `cmake/gameplay/CMakeLists.txt` with the compiler, flags
and include folders of the engine build the program came from (`build/<preset>/gameplay-sdk.cmake`,
or an installed engine's `share/slashengine/gameplay-sdk.cmake`, which `cmake --install` writes
relocatable with `cmake/gameplay/install_sdk.cmake`: the compiler by name and headers from
outside `engine/` copied in) and builds `.slash/gameplay/game.so`, its engine symbols left for
the program to supply.
`exporter::check_gameplay` (`build.check`) builds only its `game_objects` target, so just the
files that changed compile and nothing links, and `read_compile_diagnostics` reads the errors,
warnings, notes and `-fdiagnostics-parseable-fixits` fixes from the log.
`gameplay::Host` (`engine/gameplay`) loads it from a copy with a name of its own, refuses one
built against other headers (`SLASH_API_HASH`, a hash of `engine/*/include` and the pinned
dependencies) or with another compiler family or standard library ABI (`SLASH_COMPILER_ID`; other
versions of the compiler are fine), and when the file changes swaps the code in: it saves
each entity's components of the old types as JSON, removes the old systems and their pools,
moves the new types into the world's `ComponentRegistry` and puts the values back field by
field, reporting what it had to drop. Libraries are never unloaded. Hosts link the engine
libraries whole and export their symbols (`slash_gameplay_host` in CMake). On Windows (MSVC)
that is a module-definition file `engine/gameplay/exports/write_def.cpp` writes from the
libraries' COFF objects (every external symbol but COMDATs any copy of which will do, data marked
`DATA`), and the game's `game.dll` links `slash-host.lib`, the import library of a program named
`slash-host.exe`, with `/DELAYLOAD`: `cmake/gameplay/delay_load.cpp`, compiled in, hands the
delay-load helper the running program, and its `slash_game_bind` binds every engine function
before the host runs the code, so a library calling one the program lacks is refused by name.
Engine variables can't be imported that way, so game code reaches them through functions.
`slash_gameplay_module` builds test libraries the same way. Sessions poll it
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

Plugins are libraries built apart from the engine against
`engine/plugin/include/slash/plugin/slash_plugin.h`, a C interface that only grows, so they load
into later engines and can be written in any language that calls C. `engine/plugin` loads them
(`plugin::Plugins`, `src/plugins.cpp`): project.json's `plugins` lists `.plugin.json` files
(`$cli schema plugin`, `parse_plugin_file`) naming the entry function and the library per
platform, and `runtime::load_plugins` loads them for slash-cli, slash-mcp, `scene validate` and
`runtime::Game` before scenes open; `SessionOptions::plugins` attaches a session's world, so
their systems run in it, and `engine.describe` lists them. The entry function is handed
`get_proc` (`detail::proc` in `src/interface.cpp`, functions only ever added). A plugin's
component is a C struct described by its fields (`SlashField`): `src/components.cpp` keeps its
`Layout` for the whole process (the same plugin loaded for another registry keeps its ID) and
stores the structs as `Blob<N>` in an EnTT storage named by the type's hash, with
`ComponentType::Ops` that save, assign, describe and rename them as reflected components are
(the world's `Ops` take the type's hash for this). A plugin that registers anything wrong adds
nothing. Exports ship them (`tools/exporter/src/plugins.cpp`: `plan_plugins`, then
`plugins.cmake`'s `slash_game_plugin()` calls in `export/CMakeLists.txt`): a plugin file's
`sources` are compiled into the game on every platform and entered through the
`slash_game_plugins` function the export writes (`GameOptions::plugins`, `LoadOptions::linked`);
otherwise its library ships in the data on Windows and Linux, in Frameworks on macOS and in
`lib/<abi>/` on Android (found by name through `LoadOptions::library_folders`), or links in as a
static library on iOS and the web. `examples/plugins` is a plugin in C, built by
`examples/CMakeLists.txt` from `addons/<p>/<p>.c`, which `tests/test_plugin.cpp` also loads and
links in, and CI's export jobs compile into the empty game.
