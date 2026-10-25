# FSR 3 3D

A card drawn at half the window's size and rebuilt at full size by the upscaler of AMD's
FidelityFX Super Resolution 3.1 (FidelityFX SDK 1.1.4). This is the scaling mode `fsr3`, which
Godot doesn't have: it takes what Godot 4.6's `fsr2` takes, from the same jittered Forward+ frame.
[main.scene.json](main.scene.json) is the cream card of [fsr2-3d](../fsr2-3d/), turned 0.5
radians, seen through a `Camera3D` in a 160x90 window against a dark blue background.
[project.json](project.json) sets `rendering.scaling` to `fsr3` at a `scale` of `0.5`, so the 3D
world draws at 80x45.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[77,28],[88,28],[62,45],[98,45]],"path":"shots/rebuilt.png","frames":15}
{"colors":[[0.682,0.647,0.58,1],[0.78,0.737,0.659,1],[0.6,0.569,0.514,1],[0.592,0.565,0.51,1]],
 "drawn":{"meshes":1},"height":90,"path":"shots/rebuilt.png","render_size":[80,45],
 "upscaler":"fsr3","width":160}
```

- The first `frame.capture` draws one frame: the card at 80x45, its pixels made four times larger
  by FSR 3's Lanczos filter and sharpened by RCAS, so its edges step two pixels at a time
  (`shots/first.png`). `upscaler` and `render_size` say what drew it.
- With `frames` 15, the capture draws 15 more frames and returns the last. Each frame moves the
  camera by less than a render pixel, along the same Halton sequence as FSR 2's, and FSR 3
  gathers the frames into the window's pixels: the four points on the card's edges read between
  its cream and the background's blue, as FSR 2 has them (`shots/rebuilt.png`).
- `component.set` turns the card to 0.6 radians. The prepass's motion vectors say how far each of
  the card's pixels moved, and FSR 3 reads the frames before from there, so the edges stay smooth
  in the first frame after the turn (`shots/turned.png`) and settle over the next 15
  (`shots/settled.png`).
- `component.set` makes the card red without moving it. FSR 3 compares each pixel's luma with
  the frame before's in a pyramid at half the render size, finds the shading changed and takes
  the new color: inside the card, `[80,45]` is the new red at once (`shots/recolored.png`). Where
  a pixel's neighbors are the background, which didn't change, FSR 3.1 keeps the least change
  around it, so on the edges the cream fades over the next frames instead of at once
  (`shots/recolored_settled.png`, 31 frames later).

Captures keep FSR 3's history between them, as a running game's frames do. FSR 3 needs what
FSR 2 needs: Forward+, a `scale` below `1`, and a device that draws Forward+, so the canvas and
controls draw at the window's size, and Godot's Compatibility renderer and the OpenGL drivers
scale the world with `bilinear` instead. The sky's motion vectors are worked out from the depth,
as FSR 2's are, and TAA is off while FSR 3 runs.

In place of FSR 2's depth clip and locks, FSR 3 measures how much each pixel's shading changed,
counts the frames each render pixel has gathered, and keeps four frames of luma to tell a flicker
from a change. Where it has gathered many frames it clamps the frames before to three times the
spread of the colors around each pixel, where FSR 2 clamps them to those colors' range: AMD made
it so to keep detail steadier, and old colors fade more slowly.
