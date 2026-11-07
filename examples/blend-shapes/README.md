# Blend shapes

A face whose mouth moves by blend shapes, as Godot imports a glTF file's morph targets.
[models/face.gltf](models/face.gltf) is one text file, its geometry in a base64 buffer: a head
and a mouth, whose mesh has two morph targets named by its `extras.targetNames`, `smile` (the
corners up) and `jawOpen` (the lower lip down). It holds one animation, `Talk-loop`, whose
morph target weights open and close the jaw twice a second. [main.scene.json](main.scene.json)
draws it with a `MeshInstance3D`:

```json
"face": {
  "components": {
    "MeshInstance3D": {"mesh": "models/face.gltf"},
    "Transform3D": {}
  }
}
```

A shape moves each vertex by its offset times its value, before any skin, as Godot's skeleton
shader does, on the CPU, into a mesh the render device refills when the values change.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` lists the mouth's shapes with their values at rest, and its `brief` their
  names.
- `frame.capture` reads four points: above the mouth's left corner, just above its top edge, its
  middle, and under its middle. At rest only the middle is lips.
- `MeshInstance3D`'s `blend_shapes` `{"smile": 1}` raises the corners, so the first point is
  lips and the second, which the mouth's edge has risen past, skin again.
- An `AnimationPlayer` playing the model's `Talk-loop` a quarter of the way in has the jaw fully
  open, so the point under the middle is lips. The clip keys every shape, as a glTF animation
  does, so while it plays it holds `smile` at 0 too, as Godot's mixer does: the corners drop.
- [anim/face.anim.json](anim/face.anim.json)'s `grin` animates the field itself, a value track
  on `MeshInstance3D/blend_shapes/smile` from 0 to 1 over a second. After 30 ticks `entity.get`
  says the smile is 0.5, and the corners have risen half way, over the second point.

`write_gltf` and `write_glb` keep blend shapes and their tracks, so `model.cleanup` and the
asset library do too.
