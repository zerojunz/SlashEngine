# Job system

Threaded systems, as Godot runs a process group on its `WorkerThreadPool`. [main.cpp](main.cpp)
is a meadow: plants grow and drop seeds, grazers get hungry, wander and eat the nearest grown
plant. It prints which systems run together, then runs ten seconds of the meadow on no worker
threads and on four, and checks that both give the same world. [expected.txt](expected.txt)
holds its output.

```text
stage 0: grow, threaded, reads Transform2D, writes Plant
stage 0: hunger, threaded, writes Grazer
stage 1: wander, threaded, reads Grazer, writes Transform2D
stage 2: graze
```

- A system added with `.threaded = true` names the component types (or other shared state) it
  `reads` and `writes`. Threaded systems next to each other in the order share a stage and run
  at the same time, each on a thread of the world's pool, unless one writes what another reads
  or writes: `grow` and `hunger` touch different components, but `wander` writes the
  `Transform2D` that `grow` reads, so it waits for them. `graze` is an ordinary system, so it
  runs alone and may destroy entities as it goes.
- A threaded system emits events as usual; they are delivered once its stage is done, in system
  order. It creates and destroys entities, and adds and removes components, through
  `World::defer`, which runs the call on the thread stepping the world after the stage: `grow`
  plants its seeds that way.
- `world.pool().parallel_for` shares a system's own work out over the threads, with the thread
  running the system taking part; `grow` grows each plant on its own. Which thread runs which
  element varies, so each element's work touches only its own data.
- Results don't depend on the thread count: a stage runs the same way on one thread as on many.
  `project.json`'s `threading.max_threads` sets how many worker threads a game has (`-1`, the
  default, is one per CPU core); tests and examples give a world its own `jobs::Pool` with
  `set_pool`. Built in, `cpu_particles_2d` and `gpu_particles_2d` are threaded and share a stage,
  CPU particle emitters step in parallel, and Box2D steps physics on the same threads.
- `engine.describe` with `"full": true` lists each threaded system's `stage`, `reads` and
  `writes`.
