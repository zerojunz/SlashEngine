# Profiling

Finding the system that slows a game down, and checking a fix, with `perf.stats`.
[main.cpp](main.cpp) flies a flock of 600 boids whose `flock` system compares every boid with
every other. `perf.stats` names it as the slowest system; a version that sorts the boids into a
grid first, so each looks only at the cells around it, takes a fraction of the time.
[expected.txt](expected.txt) holds its output, which prints what the times say rather than the
times, since they vary by machine.

```json
{"top": 3}
```

```json
{"systems":[{"avg":2.871,"max":3.402,"name":"flock","stage":3},{"avg":0.018,"max":0.06,"name":"audio","stage":9},{"avg":0.004,"max":0.049,"name":"gui","stage":1}],"tick_ms":{"avg":2.95,"max":3.49}}
```

- Every world times each tick and each system over the last 120 ticks, and `perf.stats` returns
  the averages and maxima in milliseconds in any session, the slowest systems first; `top` says
  how many. A threaded system's time is its own, so systems sharing a stage can add up to more
  than the tick.
- In a running game (`slash-cli run --serve`), `perf.stats` also reports frame pacing: fps, frame
  times and catch-up frames ([frame-pacing](../frame-pacing/)).
- For a timeline of every thread, build with `cmake --preset tracy` and connect the
  [Tracy](https://github.com/wolfpld/tracy) profiler of the same release to `127.0.0.1`: each
  tick, system, command, physics step and frame drawn is a zone, and the job system's threads are
  named. Other builds compile the zones out. Add zones of your own with `SLASH_PROFILE_ZONE` from
  `slash/core/profile.hpp`.
