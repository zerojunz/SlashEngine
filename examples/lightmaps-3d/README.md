# Lightmaps 3D

Light baked into a lightmap by a `LightmapGI`, as Godot 4.6's: sunlight bouncing off a red wall
onto the floor and into a crate's shadow, worked out once by `lightmap.bake` rather than every
frame. [main.scene.json](main.scene.json) is a floor, a wall, a crate and a ball seen through a
`Camera3D` in a 160x90 window, lit by a sun with shadows and a blue-grey ambient light. The
floor's, wall's and crate's `gi_mode` is `"static"`, Godot's default, so the `lightmap` entity
bakes them. The ball's is `"dynamic"`, as for anything that moves: it isn't baked, and takes its
ambient light from the light probes the bake places over the others instead. It bakes at
`"low"` quality (32 rays a texel) with two bounces, which is enough for a scene this small and
takes well under a second.

The sun keeps its default `light_bake_mode`, `"dynamic"`, as in Godot: the bake takes only the
light it bounces, and the sun itself is still drawn every frame, so its shadows stay sharp. A
`"static"` light's direct light would go into the lightmap too.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
lightmap.bake {"id":"lightmap"}
{"file":"main.lightmap.json","lights":1,"meshes":3,"probes":234,"size":[128,64],"slices":1,
 "texture":"main.lightmap.hdr"}
```

- The first `frame.capture` reads the floor by the wall, the crate's side in shadow, the floor in
  its shadow, the open floor near the camera and the ball's underside, with only the sun and the
  ambient light. `path` saves the frames as PNGs in `shots/`.
- `lightmap.bake` unwraps the three static meshes with xatlas, packs them into one 128x64 slice
  and traces their light on the CPU, then traces 234 light probes on an octree over them (the
  LightmapGI's `generate_probes_subdiv`, `"8"` by default). It writes `main.lightmap.json`
  beside the scene, its texels in `main.lightmap.hdr`, and sets the LightmapGI's `light_data` to
  it.
- Drawn with the lightmap, the floor by the wall warms with the red light the wall throws on it,
  the crate's dark side brightens from 0.24 to 0.4 with the light off the floor, and the shadow
  turns from the ambient light's blue to mauve, with the wall's red in it. The lightmap stands in
  for the ambient light on these meshes. The ball's underside, blue-grey from the ambient light
  before, takes the four probes around it instead and warms from (0.66, 0.66, 0.7) to (0.72,
  0.71, 0.71) with the light off the floor; moved, it would take the probes' light where it
  goes.
- `component.set` paints the wall blue. The lightmap still holds the red light until it is baked
  again, as in Godot, and the next `lightmap.bake` turns the floor by the wall from warm to
  cool grey.
- A bake of a scene that hasn't changed since reports `"unchanged": true` and records no undo
  step.

`lightmap.bake` changes project files, so it needs the `edit` and `filesystem` capabilities.
Undo removes the files it made, or puts back the ones it replaced. `slash-cli scene validate`
checks a LightmapGI's `light_data`, and exports ship both files.
