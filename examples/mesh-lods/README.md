# Mesh LODs

Models draw coarser versions of themselves far away, as Godot 4.6 does.
[models/boulder.obj](models/boulder.obj) is a bumpy boulder of 960 triangles. When it loads, its
surface gets LODs as Godot's importer generates them with meshoptimizer: each has about half
the triangles of the one before, with corners merged where their normals, texture coordinates
and colors agree. [main.scene.json](main.scene.json) puts three boulders on a meadow: one near
the camera, one 9 meters off and a large one 33 meters off.

Each frame, each surface draws the coarsest LOD whose error, seen from the camera at the mesh's
bounds, comes to no more than a pixel of the view's width (project.json's
`rendering.mesh_lod.lod_change.threshold_pixels`, Godot's
`rendering/mesh_lod/lod_change/threshold_pixels`). `MeshInstance3D`'s `lod_bias` picks as if the
entity were that many times larger, as Godot's `GeometryInstance3D.lod_bias`:

```json
"MeshInstance3D": {"mesh": "models/boulder.obj", "lod_bias": 20.0}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` gives the boulder's 960 triangles and its LODs': 480, 240, 120, 60, 30 and 14.
- `frame.capture` draws the meadow into `shots/boulders.png` and counts the meshes it drew, and in `lods` how many of
  them at a coarser LOD: the two far boulders.
- With a `lod_bias` of 20, the far boulder keeps its detail, and only the middle one draws a
  coarser LOD.
- Moved up beside the near one, the middle boulder draws in full too, and `lods` drops out.

Shadow maps draw the LODs the camera sees. Primitive meshes, CSG shapes, `GridMap`s and terrains
have none, as in Godot.
