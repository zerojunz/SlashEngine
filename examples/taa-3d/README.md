# TAA 3D

A card's edges smoothed by temporal antialiasing, as Godot 4.6's TAA in its Forward+ renderer.
[main.scene.json](main.scene.json) is a cream card turned 0.5 radians, seen through a
`Camera3D` in a 160x90 window against a dark blue background, so each of its edges steps across
the pixels. [project.json](project.json) turns TAA on with
`rendering.anti_aliasing.quality.use_taa`, as Godot's project setting.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[77,28],[88,28],[62,45],[98,45]],"path":"shots/smooth.png","frames":15}
{"colors":[[0.627,0.596,0.545,1],[0.71,0.675,0.608,1],[0.882,0.839,0.753,1],[0.298,0.294,0.29,1]],
 "drawn":{"meshes":1},"height":90,"path":"shots/smooth.png","width":160}
```

- The first `frame.capture` draws the first frame with TAA, which has nothing to blend with: each
  pixel is the card's or the background's, as without TAA, and the four points on its edges read
  background, background, card and background. `path` saves it as `shots/first.png`.
- With `frames` 15, the capture draws 15 more frames and returns the last. Each frame moves the
  camera by less than a pixel, along the next of 16 points of a Halton sequence, and blends with
  the frames before it. After 16 frames, each pixel on an edge holds about the share of it the
  card covers, between the card's cream and the background's blue. `shots/smooth.png` shows the
  stair steps of `shots/first.png` smoothed out.
- `component.set` turns the card to 0.6 radians. The next capture's frame writes motion vectors
  for how far each of the card's pixels moved, and the blend reads the frames before from where
  they were, so the edges stay smooth in the first frame after the turn. Two of the points are
  inside the card now, and the second is still on its edge.
- 15 more frames settle the turned card's edges: the second point, now covered less, reads
  darker.

Captures keep TAA's history between them, as a running game's frames do, so the first capture
after a change shows what one frame of a game would. TAA blends only the 3D world, in Forward+:
the canvas, controls and Godot's Compatibility renderer (`"rendering_method":
"gl_compatibility"`, and always with the OpenGL drivers) draw without it. As in Godot, where a
background pixel was covered in the frame before, a little of the old color can trail for a
frame or two; and details thinner than a pixel, which most frames miss, fade rather than smooth.
