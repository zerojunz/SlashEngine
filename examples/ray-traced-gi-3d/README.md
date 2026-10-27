# Ray-traced GI 3D

A room's light bouncing off its walls, traced by rays, as `rendering.ray_tracing`'s
`global_illumination` turns it on in Forward+. [main.scene.json](main.scene.json) is a room with
no ceiling seen through a `Camera3D` in a 320x180 window: a white floor and back wall, a red wall
on the left, a green wall on the right and a white block, lit by a sun from above the right wall
and a dim blue ambient light.

[project.json](project.json) sets `rendering.ray_tracing`: `global_illumination` on, with 16 rays
a pixel. Each opaque pixel sends them over the half of the sphere it faces. What a ray meets is
lit as a path tracer's bounce lights it (what it gives off, the sun through its shadows, and the
ambient light); a ray that meets nothing brings the ambient light. Their average, smoothed over
the neighboring pixels on the same surface, is the pixel's ambient light in place of the
`WorldEnvironment`'s.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[60,150],[175,100],[250,140],[160,40]],"path":"shots/room.png"}
{"colors":[[0.671,0.651,0.667,1],[0.573,0.584,0.573,1],[0.173,0.227,0.247,1],[0.498,0.502,0.529,1]],
 "drawn":{"meshes":5},"height":180,"path":"shots/room.png","width":320}
```

- `frame.capture` draws the frame on the CPU, rays included, so it needs no GPU and gives the same
  pixels on every machine. `points` reads the floor beside the red wall, the block's face toward
  the camera, the floor in the shade by the green wall and the back wall. With the ambient light
  alone they are `[0.655,0.667,0.698]`, `[0.51,0.529,0.537]`, `[0.216,0.259,0.341]` and
  `[0.475,0.494,0.537]`: the floor by the red wall turns pink with its bounce, the block's face
  is lit by the sunlit floor in front of it, and the shade by the green wall is darker, as the
  walls hide most of the sky's blue from it.
- `component.set` paints the red wall blue: the floor beside it turns blue.
- Turning the sun off leaves only the ambient light, which the rays still bring in over the walls.

Run as a game (`slash-cli run`) on Vulkan, a GPU with ray queries traces the same light; other
GPUs light the room with the ambient light.
