# Music timeline

Which clip played when, and every transition, so an agent can check the music follows the game
without listening. [main.cpp](main.cpp) is a boss fight whose code is linked in as its game
module would be: the boss arrives at tick 90 and loses the hero's `damage` each tick, and a
music director system asks [audio/score.audio.json](audio/score.audio.json) for the clip the
fight calls for through the player's `switch_to_clip` parameter. It writes the sound files (no
audio files are committed), then plays the game the way an agent does over MCP (tools
`run_until`, `step`, `playtest_run`) or JSON-RPC. `expected.txt` holds each call and its reply.

- Each clip the music starts is a `clip_changed` world event from the player. `step` and
  `run.until` list those of the ticks they ran as `music`: the `tick`, the player's `id`, the
  `clip`, the clip `from` before it, the `transition` it followed (an index into the file's
  `transitions`), and the seconds it `waited`.
- Boss music waits for explore's next bar (`from_time: next_bar`): the boss arrives 1.5 seconds
  in, and the boss clip starts half a second later, on the bar, in tick 119 (ticks count from 0,
  so that is the 120th, which ends two seconds in).
- The victory sting starts on the boss music's next beat, then goes back to explore at its end
  by itself: `automatic`, having waited the sting's length.
- `playtest.run` sums the music up per combination: how many runs each clip started in and how
  soon (`first_tick`, in ticks from the run's start), and each transition per run. With a
  damage of 0 the fight never ends, so no run hears the sting.
- `world.snapshot` keeps the arena as it started, and `world.restore` goes back to it, so the
  playtest's runs start there too.
