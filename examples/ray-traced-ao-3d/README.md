# Ray-traced AO 3D

The room of [ssao-3d](../ssao-3d/), its ambient occlusion traced by rays instead of gathered from
the screen, as `rendering.ray_tracing`'s `ambient_occlusion` turns it on in Forward+.
[main.scene.json](main.scene.json) is the same: a floor, a back wall and a side wall, a crate
against the back wall and a small blue box on the open floor, lit by warm ambient light and a dim
sun, with the `WorldEnvironment`'s `ssao_enabled` on.

[project.json](project.json) sets `rendering.ray_tracing`: `ambient_occlusion` on, with 16 rays a
pixel. Each opaque pixel sends them over the half of the sphere it faces, up to `ssao_radius`, and
the share that something stops, times `ssao_intensity` (at most 0.98) and to `ssao_power`, darkens
its ambient light as SSAO's occlusion does. Rays find what the screen can't show: the back of the
box, and what is off the screen or behind something nearer.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[96,20],[272,90],[208,130],[160,165]],"path":"shots/room.png"}
{"colors":[[0.529,0.522,0.51,1],[0.506,0.506,0.506,1],[0.663,0.639,0.58,1],[0.78,0.741,0.667,1]],
 "drawn":{"meshes":5},"height":180,"path":"shots/room.png","width":320}
```

- `frame.capture` draws the frame on the CPU, rays included, so it needs no GPU and gives the same
  pixels on every machine. `points` reads the corner where the side wall meets the back wall, the
  line where the back wall meets the floor, the floor just below the blue box, and the open floor
  in front. The first three are darker than with SSAO (0.667, 0.714 and 0.737 red in ssao-3d):
  where the back wall meets the floor, half of the floor's rays hit the wall, which at the
  default `ssao_intensity` 2 takes all of its ambient light, leaving the sun's. The open floor is
  the same as without occlusion. `path` saves the frame as `shots/room.png`.
- `component.set` makes the occlusion wider and darker: `ssao_radius` 2 and `ssao_intensity` 4.
  The corner darkens further (0.447), as does the floor below the box (0.58).
- Setting the box's `cast_shadow` to `"off"` takes it out of the rays, as it is out of shadows:
  the floor below it is as bright as it is with no occlusion (0.788).
- Turning `ssao_enabled` off turns the traced occlusion off too, so the last capture matches
  ssao-3d's last.

Run as a game (`slash-cli run`), the rays are traced on the GPU with Vulkan's ray queries where
the GPU has them, matching the capture to a few levels. GPUs that can't trace rays yet draw the
room with SSAO.
