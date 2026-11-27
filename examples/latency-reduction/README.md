# Latency reduction

A vendor's latency reduction, as NVIDIA Reflex, AMD Anti-Lag 2 or Intel XeLL come in from an SDK
package. [main.cpp](main.cpp) adds `printer` with `render::add_latency_reducer`, and
[project.json](project.json) names it in `rendering.latency_reduction`. The game makes it for
its render device's native objects when it starts, after the SDK packages register, then runs
two frames headless.

Each frame the game calls `sleep` before the frame starts, where a vendor's library waits so the
frame reads input as late as it can, then `mark` at each point the vendor times:

```text
made for the null device
frame 1: sleep
frame 1: simulation_start
frame 1: input_sample
frame 1: simulation_end
frame 1: render_submit_start
frame 1: render_submit_end
...
started: yes
```

- `simulation_start` and `simulation_end` wrap the frame's ticks, with `input_sample` where the
  loop reads the frame's events.
- `render_submit_start` and `render_submit_end` wrap the frame's drawing.
- The GPU device and the OpenGL devices also mark `present_start` and `present_end` around
  handing each frame to the window (`RenderDevice::on_present`). This game runs headless on the
  null device, which has no window, so there is no present to mark.
- `started` is the run's summary `latency_reduction`: whether the reducer the project names was
  made. Where it isn't added or its `supports` says no, the game logs a warning and runs without
  it.

See [project-format.md](../../docs/reference/project-format.md#latency-reduction).
