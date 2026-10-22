# Models 3D

glTF 2.0 models, as Godot imports them. [models/house.gltf](models/house.gltf) is a little house
in one text file, its geometry in a base64 buffer: walls with normals, a roof with none (each
face gets its own, as the glTF specification asks) and corner colors darker at the eaves, and a
door its node places on the front wall. [main.scene.json](main.scene.json) puts it on a meadow
with a `MeshInstance3D` whose `mesh` names the file:

```json
"house": {
  "components": {
    "MeshInstance3D": {"mesh": "models/house.gltf"},
    "Transform3D": {"position": [-0.3, 0.0, 0.0], "rotation": [0.0, 0.55, 0.0]}
  }
}
```

The model's materials become `StandardMaterial3D`s as Godot's importer makes them: linear
colors turned to sRGB, `doubleSided` to `cull_mode` `"disabled"`, corner colors to
`vertex_color_use_as_albedo`, `.png`, `.jpg` and `.ktx2` textures beside the file or inside it.
Its triangles keep Godot's clockwise front faces, and MikkTSpace makes the tangents a model with
texture coordinates leaves out.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` reads the file without drawing it: its meshes with their vertices, triangles
  and materials, each material's fields that differ from `StandardMaterial3D`'s defaults, its
  images, the nodes that place its meshes and its bounds.
- `frame.capture` reads the roof, the side and front walls, the door, the sky and the ground.
- A second house, the shed, is smaller and turned the other way. Its `StandardMaterial3D` takes
  the place of the file's materials, as Godot's `material_override`, so it is all brown. Both
  houses share the model's meshes on the device.
- A `MeshInstance3D` naming `models/barn.obj` draws nothing, and `frame.summary` says why.

`slash-cli scene validate models/house.gltf` checks a model as `MeshInstance3D` loads it and
counts what it holds.
