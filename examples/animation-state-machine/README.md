# Animation state machine

A hero whose `AnimationTree` is a state machine, as Godot's `AnimationNodeStateMachine`: it moves,
jumps when a condition says so and attacks when asked, going back to moving on its own after
each. [main.scene.json](main.scene.json) gives the hero the tree and its animations:

```json
"AnimationTree": {"libraries": {"": "hero.anim.json"}, "tree_root": "hero.animtree.json"}
```

[hero.animtree.json](hero.animtree.json) has three states. `move` is a `BlendSpace1D` of idle,
walk and run; `jump` plays once; `attack` is a grouped state machine of a wind-up and a strike.
The transitions say how the hero gets from one to another:

```json
{"from": "Start", "to": "move", "advance_mode": "auto"},
{"from": "move", "to": "jump", "advance_mode": "auto", "advance_condition": "jumping",
 "xfade_time": 0.1},
{"from": "jump", "to": "move", "advance_mode": "auto", "switch_mode": "at_end", "xfade_time": 0.1},
{"from": "move", "to": "attack", "xfade_time": 0.1},
{"from": "attack", "to": "move", "advance_mode": "auto", "switch_mode": "at_end", "xfade_time": 0.1}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `animation.tree` on the file lists what a game sets: `travel`, `start`, `next`, `stop`,
  `conditions/jumping` and the blend space's `move/blend_position`.
- Setting `move/blend_position` to 2 runs: `animation.tree` shows `run` playing and the
  machine's `playback` in `move`.
- `conditions/jumping` sends it to `jump`, cross-fading over 0.1 seconds; `entity.get` catches
  the hero going up. At the end of the jump it goes back to `move` on its own, and
  `events.read` has the jump's `animation_started` and `animation_finished`.
- A misspelled `travel` fails with "did you mean". A `travel` to `attack` goes into the group
  through its `Start`: `animation.tree` shows both cross-fades, `move` out and the wind-up in.
  The wind-up goes on to the strike when it ends, the strike to the group's `End`, and the
  machine back to `move`; the last `events.read` has each one's signals.
