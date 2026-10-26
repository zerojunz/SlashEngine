# Model cleanup

Generated and imported models rarely arrive ready for a game: a CAD tool's crate is in
millimeters with z up and no texture coordinates, and a generated boulder has thousands of
triangles more than a prop needs. `model.cleanup` makes either game-ready in one call and writes
the result as a `.glb` file that holds everything, as Godot's glTF export does:

1. `up` turns the axis the file calls up to y, Godot's up.
2. `triangles` simplifies each surface to its share of the budget with meshoptimizer's
   simplifier, as Godot's importer simplifies LODs, then makes the result's LODs. Left out, the
   budget is the storyboard's tightest `budget` covering `out`.
3. `scale`, or `height` in meters, sizes it.
4. `pivot` puts its origin where scenes expect it, as a point in its box: `[0.5, 0, 0.5]` stands it
   on the ground, centered.
5. Surfaces whose vertices share one texture coordinate are unwrapped with xatlas, as lightmap
   UVs are, so textures can map onto them (`uvs: false` leaves them).

[source/crate.obj](source/crate.obj) is the CAD crate, turned, scaled from millimeters and stood
on its base:

```json
{"path": "source/crate.obj", "out": "props/crate.glb", "up": "z", "scale": 0.001, "pivot": [0.5, 0, 0.5]}
{"budget": "budget/props", "out": "props/crate.glb", "pivot": [0.5, 0, 0.5], "size": [0.6, 0.45, 0.4],
 "triangles": [12, 12], "unwrapped": 1}
```

[source/boulder.model.json](source/boulder.model.json) is a squashed sphere of 3968 triangles.
[storyboard/style.json](storyboard/style.json) gives everything in `props/` a budget of 600, so
the cleanup simplifies it to that without being told, and `storyboard.check` then passes. A
budget the simplifier can't reach is refused, saying how low it got.

Run it with `slash-cli rpc --project . < calls.jsonl`; [results.jsonl](results.jsonl) holds the
replies.
