# OBJ models

Wavefront OBJ models, as Godot's OBJ importer reads them. [models/lamp.obj](models/lamp.obj) is a
street lamp written as an exporter writes one, meters with y up and faces counter-clockwise from
outside, with its materials in [models/lamp.mtl](models/lamp.mtl), written as Blender writes
them. [main.scene.json](main.scene.json) stands it on a street at night, with an `OmniLight3D` in
its lantern, and a `MeshInstance3D` whose `mesh` names the file:

```json
"lamp": {
  "components": {
    "MeshInstance3D": {"mesh": "models/lamp.obj"},
    "Transform3D": {}
  }
}
```

Each `o` object becomes a mesh, and each `usemtl` a surface of it: the lantern is glass with an
iron roof. The base carries its own normals; the pole has none, so its sides get smooth normals
from their smoothing group and its top, after `s off`, a flat one. Each material becomes a
`StandardMaterial3D` as Godot's importer makes it: `Kd` is the albedo color, `Ks`'s brightest
channel the metallic (`Ns`, before it, is overruled; after it, Godot's importer would set the
metallic from it too), and `d` the albedo's alpha, so the glass is see-through.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` reads the file without drawing it: its three meshes with their vertices,
  triangles and materials, each material's fields that differ from `StandardMaterial3D`'s
  defaults, the parts and the bounds.
- `frame.capture` reads the glass, the roof, the pole, the base, the sky, the street under the
  lamp and the street beyond its light.
- A second lamp further down the street shares the model's meshes on the device.
- A bench naming `models/bench.obj`, which isn't there, draws nothing, and `frame.summary` says
  why. A file that is there but can't load is reported the same way, with the line at fault.

`slash-cli scene validate models/lamp.obj` checks a model as `MeshInstance3D` loads it and counts
what it holds; it also reports a material library that can't be read and textures that can't be
found, with their lines.
