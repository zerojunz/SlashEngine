# Skinned meshes

A skinned arm, posed as Godot poses an imported glTF scene's `Skeleton3D`.
[models/arm.gltf](models/arm.gltf) is one text file, its geometry in a base64 buffer: a sleeve and
a glove deformed by a skin of three bones, `Shoulder`, `Elbow` and `Hand`, whose vertices are
weighted between the bones around each joint. It holds two animations: `Wave-loop`, which swings
the elbow and hand back and forth and loops because of its name, as Godot's import hint, and
`Point`, which turns the shoulder. [main.scene.json](main.scene.json) draws it with a
`MeshInstance3D` and plays it with an `AnimationPlayer` whose library is the model itself:

```json
"arm": {
  "components": {
    "AnimationPlayer": {"animation": "Wave-loop", "library": "models/arm.gltf", "playing": false},
    "MeshInstance3D": {"mesh": "models/arm.gltf"},
    "Transform3D": {}
  }
}
```

The skin moves each vertex by its bones as Godot's skeleton shader does, on the CPU, into a mesh
the render device refills each frame. The pose is worked out from the components each time, so
the paused player's `time` poses the arm too.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` lists the model's skin and its animations with their lengths and loops, and
  `skeleton.get` its bones with their parents.
- `frame.capture` reads five points: the glove where it is at rest (bent left at the start of the
  wave), where it swings to half way through, where it points, where the `Skeleton3D` puts it,
  and the bottom of the sleeve.
- Setting the player's `time` to 0.5 swings the arm the other way, and `skeleton.get` gives the
  hand's pose in its parent and its `world_position`.
- `Point` turns the arm from the shoulder, and a `Skeleton3D` turning `Shoulder` by 0.5 radians
  wins over it, as Godot's bone poses do over an animation.
- [anim/arm.anim.json](anim/arm.anim.json)'s `wave` comes `from` the model's `Wave-loop`, taking
  its length and loop, and adds an event track. Playing it for a second emits `wave_out` at each
  end of the swing, which `events.read` returns.

`slash-cli scene validate main.scene.json` checks a `Skeleton3D`'s bone names against the model,
with "did you mean".
