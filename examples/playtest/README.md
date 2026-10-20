# Playtest

Many headless runs of a game in one call, to tune a value: how high must the jump be to clear
the pit? [main.cpp](main.cpp) is a runner game whose code is linked in as its game module would
be, with a pit in its floor and coins to pick up. It calls `playtest.run` the way an agent does
over MCP (tool `playtest_run`) or JSON-RPC. `expected.txt` holds each call and its reply.

- `vary` names fields as `"<entity>/<Component>/<field>"` with the values to try; every
  combination runs. `"*/<Component>/<field>"` sets every entity with the component, and a
  `.prefab.json` path sets the prefab and so its instances, those made while the game runs too.
- The `bot` presses input actions: held for the whole run, `every` so many ticks, at random by
  `chance` (different in each of the `runs`, the same on every call) or `when` entities match.
- `end` names conditions, as `run.until`'s, that stop a run; `ends` counts how runs ended, and a
  run that meets none by `ticks` is a `timeout`.
- Each result has the `ticks` runs took and the `events` gameplay code emitted, both per run,
  and the `measure` values when they ended: numbers as min, mean and max, `"*/<Component>"` as
  how many entities have the component.
- Runs start from the world as it is, edits not yet saved included, each in a session of its own
  stepped on its own thread, so the session's world stays where it was.
- The ticks here are 1/64 s so every value is exact; the default `tick_seconds` is 1/60.
