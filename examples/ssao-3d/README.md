# SSAO 3D

A room's corners darkened by screen-space ambient occlusion, as Godot 4.6's SSAO in its Forward+
renderer, the default `rendering.rendering_method`. [main.scene.json](main.scene.json) is a room
seen through a `Camera3D` in a 320x180 window: a floor, a back wall and a side wall, a brown crate
against the back wall and a small blue box on the open floor, lit by warm ambient light and a dim
sun.

The `WorldEnvironment`'s `ssao_enabled` turns SSAO on with Godot's defaults: occlusion from
within `ssao_radius` 1 meter of each pixel, at `ssao_intensity` 2. [project.json](project.json)
sets `rendering.environment.ssao`, as Godot's project settings: `high` quality, which takes more
samples than the default `medium` and reads depth further out at lower resolutions.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[96,20],[272,90],[208,130],[160,165]],"path":"shots/room.png"}
{"colors":[[0.667,0.647,0.627,1],[0.714,0.694,0.671,1],[0.737,0.702,0.635,1],[0.78,0.741,0.667,1]],
 "drawn":{"meshes":5},"height":180,"path":"shots/room.png","width":320}
```

- `frame.capture` draws the frame on the CPU, SSAO included, so it needs no GPU and gives the
  same pixels on every machine. `points` reads the corner where the side wall meets the back
  wall, the line where the back wall meets the floor, the floor just below the blue box, and the
  open floor in front. The first three are darker than they are without SSAO (the last call
  shows them at 0.729, 0.765 and 0.788 red); the open floor is the same. `path` saves the frame
  as `shots/room.png`.
- `component.set` makes the occlusion wider and darker: `ssao_radius` 2 and `ssao_intensity` 4.
  The corners darken further (0.549 and 0.627) and the shadow under the box grows; the open floor
  stays as it was.
- `ssao_light_affect` and `ssao_ao_channel_affect` 1 let the occlusion darken the sun's light
  too, not only the ambient light, so the sunlit corners darken most (0.416 and 0.506).
- Turning `ssao_enabled` off leaves the room lit evenly again.

SSAO darkens opaque meshes only, as in Godot: a transparent material neither casts occlusion nor
takes it. Godot's Compatibility renderer (`"rendering_method": "gl_compatibility"`, and always
with the OpenGL drivers) has no SSAO.
