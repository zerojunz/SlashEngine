# Occlusion culling

Meshes hidden behind walls aren't drawn, as Godot 4.6's occlusion culling leaves them out.
[main.scene.json](main.scene.json) is a street: a wall in front of the camera, three houses behind
it and a tall tower behind them. The wall has an `OccluderInstance3D` box a little smaller than
the wall itself, and [project.json](project.json) turns culling on:

```json
"rendering": {"occlusion_culling": {"use_occlusion_culling": true, "jitter_projection": false}}
```

Each frame, the renderer casts a ray through each texel of a small depth buffer into the
occluders' triangles, as Godot's raycast occlusion culler does, and leaves out every mesh whose
bounds are behind that depth all over. `jitter_projection` is off so the example's counts don't
depend on which frame of Godot's jitter pattern it is.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `frame.capture` draws the street into `shots/street.png` and counts the meshes it drew, and in
  `occluded` how many of them the wall hid: the three houses. The tower rises above the wall, so
  it is drawn.
- With the camera raised over the wall, every mesh shows and `occluded` drops out.
- Back behind the wall, `house_b`'s `ignore_occlusion_culling` keeps it drawn, so two are hidden.
- `occluder.bake` fills the `tower_occluder`'s triangles from the meshes on its `bake_mask`
  layer, here only the tower: 8 corners and 12 triangles, simplified as Godot's bake does. The
  tower is too far back to hide anything from here, so the counts stay the same.

Occluders hide meshes from the camera only: shadow maps and reflection probes still draw them,
and a hidden mesh still casts its shadow, as in Godot.
