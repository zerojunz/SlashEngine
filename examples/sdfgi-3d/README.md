# SDFGI 3D

A room's light bouncing off its walls with nothing baked, by Godot 4.6's SDFGI, which
SlashEngine has as a research spike on the CPU ([docs](../../docs/reference/sdfgi.md)).
[main.scene.json](main.scene.json) is the room of [ray-traced-gi-3d](../ray-traced-gi-3d/): no
ceiling, a white floor and back wall, a red wall on the left, a green wall on the right and a white
block, lit by a sun from above the right wall and a dim blue ambient light, seen through a
`Camera3D` in a 320x180 window. Its `WorldEnvironment` turns SDFGI on with one cascade of
0.15-meter cells, 19.2 meters across, around the camera.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[60,150],[175,100],[250,140],[160,40]],"frames":30,"path":"shots/room.png"}
{"colors":[[0.667,0.667,0.698,1],[0.584,0.584,0.565,1],[0.204,0.278,0.31,1],[0.525,0.522,0.537,1]],
 "drawn":{"meshes":5},"height":180,"path":"shots/room.png","width":320}
```

- `frame.capture` draws on the CPU, SDFGI included, so it needs no GPU and gives the same pixels
  on every machine. `points` reads the floor beside the red wall, the block's face toward the
  camera, the floor in the shade by the green wall and the back wall.
- The first capture draws one frame. Its probes have traced one frame's rays of the 30 they
  average, so the room is darker than with the ambient light alone.
- The second draws 30 more frames, and the light has settled. Next to the ambient light alone
  (the last capture: `[0.655,0.667,0.698]`, `[0.51,0.529,0.537]`, `[0.216,0.259,0.341]` and
  `[0.475,0.494,0.537]`), the floor by the red wall is warmer, the block's face is lit by the
  sunlit floor in front of it, the shade by the green wall is greener and the back wall takes the
  sun's bounce.
- `component.set` paints the red wall blue, and 30 frames later the floor beside it is bluer.
  Nothing was baked: the cells are made again from the frame's meshes when they change.
- With `sdfgi_enabled` off, the room has its ambient light again.

Only the software device draws SDFGI for now; run as a game on a GPU, the room is lit by the
ambient light.
