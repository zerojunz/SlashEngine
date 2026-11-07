# Many lights

64 shadowed lamps lit by ReSTIR, a [research feature](../../docs/reference/restir.md).
[main.scene.json](main.scene.json) is a hall of six pillars under an 8 by 8 grid of colored
`OmniLight3D`s, each with `shadow_enabled`, seen through a `Camera3D` in a 160x90 window.
[project.json](project.json) turns on `rendering.ray_tracing.many_lights` with two reservoirs a
pixel.

Forward+ gives 16 positional lights shadow maps; ray-traced shadows take 8. With `many_lights`,
each pixel keeps two lights, resampled each frame from eight drawn from its cluster, from last
frame's and from its neighbors', and traces one shadow ray to each, so all 64 lamps cast shadows
for two rays a pixel. Only the software device draws it; GPUs light every lamp as before.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `frame.capture` with `frames` draws eight frames, so each pixel's reservoirs build up over
  them, as a running game's do, and saves the last as `shots/hall.png`.
- `lighting.check` with `"lights": false` reads the frame's exposure as numbers. One pixel's
  reservoirs show one or two lamps at a time, so the frame is grainy; the numbers average it out.
- `entity.destroy` takes a pillar away and the hall is drawn again: the reservoirs whose surfaces
  are still there carry on, and the floor where the pillar stood starts over.

With the reservoirs of frames before, fewer pixels are clipped than on a first frame: ReSTIR
spreads a bright lamp's light over the frames and neighbors that would otherwise catch it all at
once.
