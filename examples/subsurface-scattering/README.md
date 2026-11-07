# Subsurface scattering

Godot's `StandardMaterial3D` subsurface scattering and transmittance, as its Forward+ renderer
draws them for skin, wax and leaves. [main.scene.json](main.scene.json) has two meshes in a
160x90 window, with the project's blur set to high quality ([project.json](project.json)):

- `wax` is a pale ball lit from the right (`subsurf_scatter_enabled`,
  `subsurf_scatter_strength`). Its diffuse light is blurred across and down the screen after it
  draws, so light creeps past the line where the lit half meets the dark half, as it does in
  wax.
- `ear` is a thin slab of skin (`subsurf_scatter_skin_mode`) lit only from behind by a light that
  casts shadows (`back_light`). With `subsurf_scatter_transmittance_enabled`, the side facing the
  camera glows red where the light's shadow map says the slab is thinner than
  `subsurf_scatter_transmittance_depth`, with skin's own colors, as an ear does against the sun.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[47,45],[53,45],[60,45],[105,45]]}
{"colors":[[0.071,0.063,0.047,1],[0.396,0.373,0.306,1],[0.659,0.624,0.522,1],
 [0.541,0.031,0.004,1]],"drawn":{"meshes":2},"height":90,"width":160}
```

- `frame.capture` reads the wax's dark side just past the line, the line itself and its lit
  side, and the middle of the ear.
- `component.set` turns the wax's scattering and the ear's transmittance off. The second
  `frame.capture` sees the dark side go black and the line darken a little while the lit side
  stays the same, and the ear go black, since nothing lights its front.

The blur's quality, width and narrowing with distance are project.json's
`rendering.environment.subsurface_scattering`. Compatibility draws neither effect, and the
textures Godot also takes for them (`subsurf_scatter_texture`,
`subsurf_scatter_transmittance_texture`) aren't read yet.
