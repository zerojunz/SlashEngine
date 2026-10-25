# Clustered lights

3D lights lit as Godot's Forward+ renderer lights them. [main.scene.json](main.scene.json) is a
plaza at night under a dim `moon`, seen from above through a `Camera3D` in a 320x180 window, with
100 `OmniLight3D` lanterns in rows of 10, warm on the left and cool on the right, each reaching
0.9 meters.

That is more lights than Godot's Compatibility renderer draws: it adds the first 64 to every
pixel. [project.json](project.json) leaves `rendering.rendering_method` at its default,
`forward_plus`, so each frame sorts the lanterns into tiles of 32 pixels and slices of depth,
and a pixel adds only the moon and the few lanterns whose range reaches its tile and slice. All
100 light the plaza.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[96,33],[131,107],[150,107],[263,167],[160,115]]}
{"colors":[[1,0.616,0.4,1],[0.882,0.812,0.576,1],[0.812,0.812,0.647,1],[0.467,0.914,0.996,1],
 [0.184,0.212,0.282,1]],"drawn":{"meshes":1},"height":180,"width":320}
```

- `frame.capture` draws the frame on the CPU with the same clusters the GPU devices read, so it
  needs no GPU and gives the same pixels on every machine. `points` reads the plaza under
  `lantern001`, the orange one at the back left; under `lantern064` and `lantern065`, past the
  64 lights that Compatibility would draw (the moon and the first 63 lanterns); under
  `lantern100`, the blue one at the front right; and between lanterns, where only the moon and
  the ambient light reach.
- `component.set` turns `lantern100` red, and `frame.capture` reads it red. `path` saves the
  frame as `shots/plaza.png`.

With `"rendering_method": "gl_compatibility"` in project.json, as Godot's Compatibility renderer
and as the OpenGL drivers always draw, the plaza from `lantern064` on stays dark.
