# FSR 2 3D

A card drawn at half the window's size and rebuilt at full size by AMD's FidelityFX Super
Resolution 2.2, as Godot 4.6's `scaling_3d` mode `fsr2` in its Forward+ renderer.
[main.scene.json](main.scene.json) is the cream card of [taa-3d](../taa-3d/), turned 0.5 radians,
seen through a `Camera3D` in a 160x90 window against a dark blue background.
[project.json](project.json) sets `rendering.scaling` to `fsr2` at a `scale` of `0.5`, as Godot's
project settings, so the 3D world draws at 80x45.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[77,28],[88,28],[62,45],[98,45]],"path":"shots/rebuilt.png","frames":15}
{"colors":[[0.675,0.639,0.573,1],[0.765,0.722,0.647,1],[0.678,0.643,0.576,1],[0.6,0.569,0.514,1]],
 "drawn":{"meshes":1},"height":90,"path":"shots/rebuilt.png","render_size":[80,45],
 "upscaler":"fsr2","width":160}
```

- The first `frame.capture` draws one frame: the card at 80x45, scaled up to 160x90 and sharpened
  by RCAS. Each render pixel covers four of the window's, so the card's edges step two pixels at a
  time, as `shots/first.png` shows. `upscaler` and `render_size` say what drew it.
- With `frames` 15, the capture draws 15 more frames and returns the last. Each frame moves the
  camera by less than a render pixel, along the next of 32 points of a Halton sequence (8 times
  the window's size over the render size, squared), so the frames sample the card at points
  between the render pixels. FSR 2 gathers them into the window's pixels, and after 16 frames
  each pixel on an edge holds about the share of it the card covers: the four points, all on the
  card's edges, read between its cream and the background's blue. `shots/rebuilt.png` shows edges
  as smooth as the window's own size would draw them.
- `component.set` turns the card to 0.6 radians. The next capture's frame writes motion vectors
  for how far each of the card's pixels moved, and FSR 2 reads the frames before from where they
  were, so the edges stay smooth in the first frame after the turn (`shots/turned.png`).
- 15 more frames settle the turned card's edges in `shots/settled.png`.

Captures keep FSR 2's history between them, as a running game's frames do. FSR 2 scales only the
3D world, in Forward+, at a `scale` below `1`: the canvas and controls draw at the window's size,
and Godot's Compatibility renderer (`"rendering_method": "gl_compatibility"`, and always with the
OpenGL drivers) scales the world with `bilinear` instead. TAA is off while FSR 2 runs, since it
does TAA's job too. As in Godot, FSR 2 has no reactive mask, so transparent meshes, which write
no depth, can trail for a frame or two.
