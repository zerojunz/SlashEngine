# Animation tree

The skinned arm of [skinned-meshes](../skinned-meshes/), played by an `AnimationTree` instead of
an `AnimationPlayer`, as Godot's: it waves, and on request points with its shoulder while the
elbow and hand keep waving. [main.scene.json](main.scene.json) gives the arm the tree and the
model as its library:

```json
"AnimationTree": {"libraries": {"": "models/arm.gltf"}, "tree_root": "arm.animtree.json"}
```

[arm.animtree.json](arm.animtree.json) is a blend tree of four nodes: the model's `Wave-loop`
through a `TimeScale`, and a `OneShot` that fades `Point` in and out over it in 0.2 seconds. Its
filters name only the `Shoulder` bone, so the shot moves the shoulder and the wave goes on
moving the rest:

```json
"shot": {"type": "OneShot", "inputs": {"in": "speed", "shot": "point"},
         "fadein_time": 0.2, "fadeout_time": 0.2,
         "filter_enabled": true, "filters": ["Shoulder"]}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `animation.tree` on the file lists the parameters a game sets, `shot/request` and
  `speed/scale`, with their defaults and docs.
- After 30 ticks `skeleton.get` shows the wave alone posing the arm. `animation.set_parameters`
  fires the shot, and six ticks later `animation.tree` shows the tree set the request back to
  `"none"`, `shot/active` in its state and `Point` playing at a third of its weight as it fades
  in; `skeleton.get` lists both clips with their weights, the shoulder turning a little and the
  hand still waving, and `frame.capture` reads the arm's colors.
- `events.read` has the tree's `animation_started` and `animation_finished` signals for `Point`.
- A misspelled parameter fails with "did you mean". Doubling `speed/scale` makes the wave move
  on twice as fast: 15 ticks take it half a loop further.
