# Replays

A run recorded into a `.replay.json` file plays back exactly, so a player's bug report or an
agent's play session can be stepped through again, looked at from any tick and kept as a test.
[main.cpp](main.cpp) is a hero who walks and jumps by input actions ([project.json](project.json))
past a crate to a coin ([main.scene.json](main.scene.json)). One session records a run while the
player presses keys between ticks, as a window's events arrive; a second plays it back, as an
agent would, and seeks the moment the coin is taken; a third has a lower jump.
[expected.txt](expected.txt) holds the output.

```text
replay.run {"path":"replays/run.replay.json"}
{"end":150,"matched":true,"start":0,"tick":150}
replay.run {"path":"replays/run.replay.json","tick":55}
{"end":150,"matched":true,"start":0,"tick":55}
replay.seek {"until":"none","with":["Area2D"]}
{"count":0,"end":150,"entities":[],"found":true,"matched":true,"start":0,"tick":96,"truncated":false}
replay.seek {"ticks":-1}
{"end":150,"matched":true,"start":0,"tick":95}
-- a session with a lower jump
replay.run {"path":"replays/run.replay.json"}
{"changes":{"added":["coin"],"changed":{...}},"diverged":60,"end":150,"matched":false,"start":0,"tick":150}
```

- `replay.record` starts a recording and `replay.stop` writes it; `slash-cli run --record
  replays/run.replay.json` records a whole played run. The file holds the entities at the start,
  the ticks the host stepped (in runs of one length), the input it queued and the commands that
  changed the world, in order, then a hash of the world every 60 ticks and the entities at the
  end. Prefabs are copied in, so it plays back after their files change.
- `replay.run` puts the start back in any session, steps the same ticks with the same input and
  commands, and compares the hashes and the end. `"tick"` stops early, here at the jump, so
  every query works on the world as it was then: `input.state` shows D still held.
- `replay.seek` moves the playback: to a `tick`, by `ticks`, or to the first tick where an entity
  matches `with`, `without` and `where`, here the first with no `Area2D` (the coin) left. Going
  forward goes on from where the playback is; going back, as `"ticks": -1` does here, plays the
  run again from its start, so the world is exactly as it was a tick before.
- When it goes another way, `diverged` is the first tick whose hash differs and `changes` is
  what differs from the recorded end, here the coin the hero no longer reaches.
- Recording starts the world afresh, as playback does: held input, focus, particles, physics
  contacts and sounds start over. Gameplay systems that keep state outside components add a
  reset with `World::add_reset` to replay exactly.
