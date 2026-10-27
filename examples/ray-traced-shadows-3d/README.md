# Ray-traced shadows 3D

Shadows traced by rays from each pixel toward its lights, as `rendering.ray_tracing` turns them on
in Forward+. [main.scene.json](main.scene.json) is an avenue with two pillars, seen from above
through a `Camera3D` in a 160x90 window:

- `sun` is a `DirectionalLight3D` with `shadow_enabled`. Its `directional_shadow_max_distance` is
  14 meters, so its shadow maps end before `far_pillar`, 22 meters away; without ray tracing that
  pillar casts no shadow.
- [project.json](project.json) sets `rendering.ray_tracing`: `shadows` on, with 16 rays a pixel for
  each light. The lights with `shadow_enabled` take their shadows from the rays instead of their
  shadow maps, as far as the camera sees.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[75,20],[75,61],[100,40]]}
{"colors":[[0.2,0.235,0.29,1],[0.2,0.235,0.29,1],[0.651,0.608,0.533,1]],"drawn":{"meshes":3},
 "height":90,"width":160}
```

- `frame.capture` draws the frame on the CPU, rays included, so it needs no GPU and gives the same
  pixels on every machine. `points` reads the far pillar's shadow and the near one's, both dark,
  and the ground in the sun.
- `component.set` gives the sun a `light_angular_distance` of 10 degrees, so the rays spread over
  its disk. The shadow stays dark at the pillar's foot, and its far end turns to a penumbra.
- Setting the far pillar's `cast_shadow` to `"off"` lights the ground behind it: rays pass through
  meshes that cast no shadow, as shadow maps leave them out. `path` saves the frame as
  `shots/avenue.png`.

Run as a game (`slash-cli run`), the scene's rays are traced on the GPU with Vulkan's ray queries
where the GPU has them, matching the capture to a few levels. GPUs that can't trace rays yet draw
it with the sun's shadow maps.
