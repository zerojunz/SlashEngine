# FBX models

FBX models, read with ufbx as Godot 4.3's FBX importer reads them.
[models/windmill.fbx](models/windmill.fbx) is a windmill written as 3ds Max writes an ASCII FBX
file: z up, centimeters, the front toward -y, and faces counter-clockwise from outside.
[main.scene.json](main.scene.json) stands it in a field in the sun, with a `MeshInstance3D` whose
`mesh` names the file:

```json
"windmill": {
  "components": {
    "MeshInstance3D": {"mesh": "models/windmill.fbx"},
    "Transform3D": {}
  }
}
```

The file's lengths become meters and its axes y up, as Godot's importer converts them: the
tower, 240 units tall in the file, stands 2.4 meters tall, facing the camera. Its nodes keep
their tree: `Windmill`, an empty node, holds the `Tower` and the `Cap`, which holds the `Hub`,
which holds the `Sails`, and each mesh is drawn where those nodes place it. Each mesh has a
surface per material: the tower is stone with a wooden door and a lit window, and the sails are
canvas on a wooden hub. The roof leaves its normals out, so ufbx makes them; with no smoothing in
the file its faces stay flat.

Each material becomes a `StandardMaterial3D` as Godot's importer makes it: the diffuse color,
turned to sRGB, is the albedo, the window's emissive color and factor are its emission and
energy, and every material shows vertex colors, which the sails carry, dark red at the hub and
pale at the tips.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` reads the file without drawing it: its three meshes with their vertices,
  triangles and materials, each material's fields that differ from `StandardMaterial3D`'s
  defaults, the parts and the bounds, in meters.
- `frame.capture` reads the window, the door, the stone, the roof, the hub, a sail near the hub
  and at its tip, the sky and the field.
- A second windmill further off shares the model's meshes on the device; its shaded side faces
  the camera.

`slash-cli scene validate models/windmill.fbx` checks a model as `MeshInstance3D` loads it and
counts what it holds; it also reports textures that can't be found or read and the warnings
ufbx gives about the file.
