# Animation expressions

A hero whose `AnimationTree` state machine moves between idle, run and jump on its own, by
expressions of values the game sets, as Godot's `advance_expression`. A jump trigger waits until
the hero is on the ground, then the transition uses it up, as Unity's Animator triggers do.

[hero.animtree.json](hero.animtree.json) declares the values its expressions read, with their
defaults, and the triggers among them:

```json
"variables": {"grounded": true, "speed": 0.0},
"triggers": ["jump"]
```

Its auto transitions take them in Godot's expression syntax:

```json
{"from": "idle", "to": "run", "advance_mode": "auto", "advance_expression": "speed > 0.5"},
{"from": "run", "to": "idle", "advance_mode": "auto", "advance_expression": "speed <= 0.5"},
{"from": "run", "to": "jump", "advance_mode": "auto", "advance_expression": "jump and grounded"},
{"from": "jump", "to": "idle", "advance_mode": "auto", "switch_mode": "at_end"}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `animation.tree` on the file lists the variables and the trigger as parameters, beside the
  state machine's `travel`, `start`, `next` and `stop`. A misspelled `sped` fails with "did you
  mean".
- Setting `speed` to 2 sends the hero from idle to run.
- `jump` set while `grounded` is false waits: `animation.tree` shows it still `true` and `run`
  playing. Once `grounded` is true the hero jumps, `entity.get` catches it going up, and the
  tree has set `jump` back, so only `grounded` and `speed` are left in `parameters`.
- With `speed` at 0 the jump ends in idle and stays there; `events.read` has the jump's
  `animation_started` and `animation_finished`.
