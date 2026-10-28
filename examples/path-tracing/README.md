# Path tracing

A reference still of a 3D world, path traced on the CPU: light bounced from surface to surface
and shadows traced by ray, for checking what the real-time renderer only estimates.
[main.scene.json](main.scene.json) is the room of [lightmaps-3d](../lightmaps-3d/) without its
`LightmapGI`: a floor, a red wall, a crate and a ball seen through a `Camera3D` in a 160x90
window, lit by a sun with shadows and a blue-grey ambient light.

`frame.capture`'s `path_trace` traces `samples` paths through each pixel. Each path meets the
nearest surface, shades it with the same material and light code the renderer draws with
(`StandardMaterial3D`'s albedo, metallic, roughness and emission, and every light), then
bounces `bounces` times more, each bounce taking the light that reaches the point it meets.
A bounce that meets nothing takes the `WorldEnvironment`'s ambient light, or its sky's when
meshes are lit by the sky, so the sky lights the room from every side it can reach. The frame is
post-processed as the live frame is.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[60,42],[89,42],[85,45],[80,65],[57,51]],"path_trace":{"samples":32},
 "path":"shots/traced.png"}
{"colors":[[0.851,0.784,0.757,1],[0.424,0.337,0.247,1],[0.475,0.302,0.353,1],
 [0.78,0.749,0.737,1],[0.804,0.773,0.765,1]],"drawn":{"meshes":4},"height":90,
 "path":"shots/traced.png","path_samples":32,"width":160}
```

- The first `frame.capture` draws the live frame and reads the floor by the wall, the crate's
  side in shadow, the floor in its shadow, the open floor near the camera and the ball's
  underside. `path` saves the frames as PNGs in `shots/`.
- Path traced with 32 samples and the default 4 bounces, the floor by the wall warms with the
  wall's red light, the crate's dark side brightens from 0.24 to 0.42 with the light off the
  floor, its shadow turns from the ambient light's blue to mauve, and the ball's underside
  brightens from 0.66 to 0.8 with the light the floor throws up at it. These are what
  `lightmaps-3d` bakes, worked out here for every pixel with no bake.
- The same capture again adds 32 more samples to the 32 before (`path_samples` 64): captures of
  a world that looks the same, at the same size, add up, so the noise fades call by call.
- `bounces` 0 is the lights alone, with no ambient light, so the crate's side the sun misses is black.
  It starts the sum over, as any change to the world, its lights or the settings does.
- Painted blue, the wall cools the floor beside it.

A path-traced capture turns off TAA, FSR 2 and occlusion culling and draws every mesh at its
full detail. It runs on the CPU's threads (`threading.max_threads`), and the pixels are the
same on every machine and any number of threads.
