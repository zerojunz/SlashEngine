# Replays

Recording and replaying a game session.

Status: current · Updated: 2026-11-20

`engine/command/src/replay.cpp` records runs and plays them back. `replay.record` (or
`slash-cli run --record <file>`) listens through `World::set_step_listener` (ticks the host
steps), `input::set_recorder` (events queued between ticks) and `Session::set_call_listener`
(commands that change the world; undo, redo, `world.restore`, scene opens and hot reloads are
kept as the entities they left), and `replay.stop` writes a `.replay.json` file
(`$cli schema replay`) with a hash of the world every 60 host ticks and after each command.
`replay.run` puts the start back, steps the same ticks with the same input and commands, and
reports `matched`, the first tick that `diverged` and the `changes` at the end. The session keeps
the playback, so `replay.seek` goes on from where it stopped (to a tick, by `ticks`, or to the
first tick a `world.query` selection matches) when `Session::revision()` says nothing else
changed the world, and plays from the start otherwise, since Box2D's state can't be copied.
Recording and
playback start with `World::reset_state`, so a module that keeps state outside components
(input, gui, physics, particles, audio) adds a reset with `World::add_reset`, and anything that
iterates entities in an order that matters uses registry storage order, never handle values
(`EntityMap` in `engine/physics2d/src/space.hpp`), so a replay steps the same in every session.
`examples/replays` records a run, plays it back and seeks the moment the coin is taken.
