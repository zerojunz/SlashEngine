# Profiling

Profiling a running game.

Status: current · Updated: 2026-11-20

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
`World::memory`: the world's own part, one for components (each type's storage plus `heap_bytes` of
its fields, through `ComponentType::memory`), and a part from each memory source. A module that
keeps state outside components adds a source when it installs (`World::add_memory_source`):
physics2d, audio, navigation, both particle systems and ui's glyph atlases do. Asset types say what
a loaded asset holds with `add_type`'s `bytes` function, else the file's size counts; a running game
adds the render device's texture bytes (`render::Stats::texture_bytes`). Count from capacities, and
keep a source cheap: agents call it between ticks. `examples/memory-report` finds a growing trail.
