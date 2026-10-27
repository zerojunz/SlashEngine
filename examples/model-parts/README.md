# Model parts

A model's named parts move on their own and its attachment points carry other things, so a door
opens and a lamp stands on top without either being drawn into one mesh.
[props/cabinet.model.json](props/cabinet.model.json) is a cabinet recipe with two parts and two
points:

```json
"parts": [
  {"name": "body", "shape": "box", "size": ["=width", "=height", "=depth"], ...},
  {"name": "door", "position": ["=-width / 2", "=height / 2", "=depth / 2"],
   "children": [{"shape": "box", "position": ["=width / 2", 0, 0.01], ...}, ...]}
],
"points": [
  {"name": "handle", "part": "door", "position": ["=width - 0.07", 0, 0.075]},
  {"name": "top", "position": [0, "=height", 0]}
]
```

The door part is placed at its hinge and its board sits beside it, so turning the part turns the
door on the hinge. Each point is a node of the model with no mesh, as Godot's `Marker3D` in an
imported scene, placed in its part (or the model) and moving with it. In
[main.scene.json](main.scene.json) the cabinet's `Skeleton3D` opens the door by its node name, and
a `BoneAttachment3D` on the `top` point carries a lamp, another recipe:

```json
"cabinet": {"components": {
  "Transform3D": {"children": ["on_top"]},
  "MeshInstance3D": {"mesh": "props/cabinet.model.json"},
  "Skeleton3D": {"bone_rotations": {"door": [0, -1.2, 0]}}
}},
"on_top": {"components": {
  "Transform3D": {"children": ["lamp"]},
  "BoneAttachment3D": {"bone_name": "top"}
}}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` with `brief` describes the cabinet in a few numbers and names, so an agent can
  place it without drawing it: its size (0.8 by 1 by 0.525 meters, the door's handle included),
  its pivot (`[0.5, 0, 0.43]`: the origin is on the floor, centered across, a little behind the
  middle), each part's center, size, materials and triangles, and the points.
- After a tick, `skeleton.get` poses the door and the handle: the handle has swung out with the
  door, to z 0.93.
- `entity.get` shows the attachment at the top point, where the lamp stands.
- `frame.capture` samples the lamp's shade, the inside of the cabinet, the open door and the
  background.
