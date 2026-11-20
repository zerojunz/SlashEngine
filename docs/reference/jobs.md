# Job system

The job system.

Status: current · Updated: 2026-11-20

`engine/core`'s `jobs::Pool` (`slash/core/jobs.hpp`) is Godot's `WorkerThreadPool`: tasks, group
tasks and `parallel_for`, on `threading.max_threads` worker threads (project.json; -1 is one per
core, and the web has none). A system added with `.threaded = true` declares the component names
(or other shared state) it `reads` and `writes`; neighbouring threaded systems that don't clash
form a stage and run at once on the world's pool (`World::pool`, `set_pool`). A threaded system
emits events as usual (delivered after its stage, in system order) and creates or destroys
entities, or adds and removes components, through `World::defer`; anything else it touches must
be its own, or declared under a name (physics queries bring Box2D up to date first, so a system
making them writes `"physics2d"`). Results must not depend on the thread count: tests run with
`jobs::Pool(0)` and a pool with threads and compare. `cpu_particles_2d` shares its emitters out
with `parallel_for`, and Box2D steps on the pool through its task callbacks (`enqueue_task` in
`engine/physics2d/src/space.cpp`). `examples/job-system` is a meadow of threaded systems.
