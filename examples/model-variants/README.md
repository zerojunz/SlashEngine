# Model variants

One recipe, [props/crate.model.json](props/crate.model.json), makes three crates: as written, a
burnt one and a big one. Its `variants` are named forms of it, each changing what it names, as
Godot's inherited scenes override their nodes:

```json
"variants": {
  "big": {"parameters": {"size": 0.8}},
  "burnt": {
    "materials": {"pine": {"albedo_color": [0.16, 0.12, 0.1, 1]}},
    "parts": {"knot": {"radius": 0.12}, "lid": null}
  }
}
```

`burnt` darkens the pine, burns the knot in its side into a hole and leaves the lid out; `big`
sets one parameter. A model path picks a variant after `::`, so
[main.scene.json](main.scene.json) draws all three from the one file:

```json
"MeshInstance3D": {"mesh": "props/crate.model.json::burnt"}
```

The recipe's `quality` tiers work the same way for exports. Its `phone` tier leaves the knot
out, and the `phone` preset in [export_presets.json](export_presets.json) names it with
`model_quality`, so an Android export ships the crate with the knot gone and its variants applied
on top. `slash-cli project validate` checks every variant and tier as the recipe it makes.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` with `brief` describes the plain crate (a box of 174 triangles and a lid), the
  burnt one (no lid, the bigger hole making 204 triangles) and, by path, the big one, 0.82 m
  across.
- `frame.capture` samples the plain crate's pine, the burnt crate and the big crate.
- `model.inspect` of `::brunt` fails, naming the variant meant.
