# Run until

Running a game headless until something happens, instead of stepping and querying after every
tick. [main.cpp](main.cpp) adds a falling `ball` with `gravity` and `despawn` systems, then calls
`run.until` the way an agent does over MCP (tool `run_until`) or JSON-RPC. `expected.txt` holds
each call and its reply.

- `run.until` takes `with`, `without` and `where` like `world.query`, and checks them before each
  tick. It replies with `matched`, the `ticks` it ran, the world's `tick` and the matching
  entities: the ball passes the floor at tick 69.
- `"until": "none"` waits until nothing matches: the ball despawns at tick 89.
- `ticks` caps the run (default 600). A condition that never holds stops there with
  `"matched": false`.
- A run is one undo step, like `step`, and takes `dry_run`.
- The ticks here are 1/64 s so every value is exact; the default `tick_seconds` is 1/60.
