# Model collision

A model can say what it collides as, so a level made in a modeling tool needs no collision shapes
placed by hand. As Godot's scene importer, the engine reads its nodes' names for Godot's import
hints and makes `Model::collision` of them:

| Hint | Collides as | Draws |
| --- | --- | --- |
| `-col` | its triangles (a trimesh) | yes |
| `-convcol` | their convex hull | yes |
| `-colonly` | its triangles | no |
| `-convcolonly` | their convex hull | no |

The hint can also be written `_col` or `$col`, in any case, and is left out of the name.
A `MeshInstance3D` whose model has collision is a static body of those shapes, as the
`StaticBody3D` Godot's importer adds, unless it is a body itself or a child of one, which then
takes them as its shapes.

[source/yard.obj](source/yard.obj) has ground that only collides, a wall that collides as its
hull and a rock that only draws. `model.inspect`'s brief lists the collision apart from the parts:

```json
{"collision": [{"name": "Ground", "points": 4, "shape": "trimesh", "triangles": 2},
               {"name": "Wall", "points": 8, "shape": "convex", "triangles": 12}], ...}
```

A model without hints gets collision from `model.cleanup`'s `collision`, as Godot's mesh import
"Generate > Physics" options: `"trimesh"`, `"convex"` (one hull) or `"decompose"`, convex hulls
from V-HACD as Godot's `Mesh::convex_decompose`, at most `max_hulls` of them. One hull would fill
in [source/arch.model.json](source/arch.model.json)'s opening; decomposed, the arch keeps it:

```json
{"collision": {"convex": 6}, "out": "props/arch.glb", "pivot": [0.5, 0.0, 0.5], "size": [2.4, 2.4, 0.4], "triangles": [36, 36]}
```

The `.glb` file keeps the hulls as nodes named with Godot's hints, so Godot and the engine read
them back. Then three balls drop: onto the ground, the wall and the arch's beam. A ray through
the arch's opening hits nothing, one through its post stops at the post, and one down through
the rock reaches the ground below it.

Run it with `slash-cli rpc --project . < calls.jsonl`; [results.jsonl](results.jsonl) holds the
replies.
