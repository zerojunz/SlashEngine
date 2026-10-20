# Memory report

Finding what keeps growing with `perf.stats`' memory report, then stopping it.
[main.cpp](main.cpp) flies 200 comets that each keep a trail of where they have been. The first
trail system never drops a point, so the trails grow every tick. Two memory reports ten seconds
apart show which part of the engine grew, `components`, and its items name the component,
`Trail`. A system that keeps the last 32 points stops the growth. [expected.txt](expected.txt)
holds its output, which prints what the reports say rather than the bytes, since sizes vary by
platform.

```json
{"memory": true, "top": 3}
```

```json
{"memory":{"parts":[{"bytes":3533880,"counts":{"buses":1,"players":0,"streams":0},"items":[{"bytes":3528000,"count":441000,"name":"history Master"}],"name":"audio"},{"bytes":219136,"counts":{"components":400,"types":2},"items":[{"bytes":144384,"count":200,"name":"Trail"},{"bytes":74752,"count":200,"name":"Transform2D"}],"name":"components"},{"bytes":81984,"counts":{"entities":200,"events":0,"prefabs":0},"name":"world"},{"bytes":0,"counts":{"emitters":0,"particles":0,"slots":0},"name":"cpu_particles_2d"},{"bytes":0,"counts":{"emitters":0,"particles":0,"steps":0},"name":"gpu_particles_2d"},{"bytes":0,"counts":{"bodies":0,"contacts":0,"joints":0,"shapes":0},"name":"physics2d"},{"bytes":0,"counts":{"pages":0},"name":"text"},{"bytes":0,"counts":{"assets":0},"name":"assets"}],"process":{"peak":23056384,"resident":23056384}},"systems":[{"avg":0.094,"max":0.255,"name":"trail","stage":3},{"avg":0.047,"max":0.193,"name":"audio","stage":10},{"avg":0.029,"max":0.061,"name":"camera_2d","stage":7}],"tick_ms":{"avg":0.297,"max":0.799}}
```

- `process` is what the operating system counts for the whole process; `parts` are the engine's
  own, the largest first, with what each holds (`counts`) and what its bytes are made of
  (`items`, `top` of them).
- Sessions keep each audio bus's last ten seconds for `audio.capture`, which is why `audio` is
  large even with nothing playing.
- Parts keep memory they have grown into, as a vector keeps its capacity, so the capped trails
  stay at the size they had reached; what matters is that they stop growing.
- Modules add parts with `World::add_memory_source`; see
  [perf.stats](../../docs/reference/commands.md#perfstats) for every part.
