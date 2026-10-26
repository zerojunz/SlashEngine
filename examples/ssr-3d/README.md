# SSR 3D

A polished floor reflecting what stands on it with screen-space reflections, as Godot 4.6's SSR
in its Forward+ renderer, the default `rendering.rendering_method`.
[main.scene.json](main.scene.json) is a hall seen through a `Camera3D` in a 320x180 window: a
metallic floor with a roughness of 0.05, a warm back wall, a red crate, a blue pillar and a rug,
lit by grey ambient light and a lamp.

The `WorldEnvironment`'s `ssr_enabled` turns SSR on with Godot's defaults: rays of up to
`ssr_max_steps` 64 steps, fading in over `ssr_fade_in` 0.15 near where they leave the floor and
out over `ssr_fade_out` 2 toward the screen's edges, and hitting what they pass within
`ssr_depth_tolerance` 0.5 meters behind. [project.json](project.json) sets
`rendering.environment.screen_space_reflection.half_size` to false, as Godot's project setting,
so the rays trace at the frame's full size instead of half.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[118,92],[200,88],[60,84],[160,165]],"path":"shots/hall.png"}
{"colors":[[0.424,0.302,0.329,1],[0.294,0.31,0.463,1],[0.404,0.357,0.341,1],[0.318,0.318,0.349,1]],
 "drawn":{"meshes":5},"height":180,"path":"shots/hall.png","width":320}
```

- `frame.capture` draws the frame on the CPU, SSR included, so it needs no GPU and gives the same
  pixels on every machine. `points` reads the floor under the crate, under the pillar, where it
  reflects the back wall, and the open floor near the camera. The first three show the crate's
  red, the pillar's blue and the wall's warm color (without SSR, the last call, they are grey:
  0.294, 0.282 and 0.239 red); the open floor reflects nothing on screen and stays as it was.
  `path` saves the frame as `shots/hall.png`.
- `component.set` makes the floor rougher, at 0.4. Each pixel reads a blurrier level of the
  reflections, so they soften and spread; the lamp's highlight spreads across the open floor too.
- At roughness 0.8 the reflections are gone, as in Godot, which fades them out between roughness
  0.6 and 0.7; the floor shows only its lighting.
- `batch` makes the floor polished again and turns `ssr_enabled` off, in one call: the floor's
  reflections show only the dark background again.

SSR reflects only what is on screen, as in Godot: what is behind the camera or off the edges has
no reflection, and reflections fade toward the edges rather than end there. It reflects in opaque
meshes only. Godot's Compatibility renderer (`"rendering_method": "gl_compatibility"`, and always
with the OpenGL drivers) has no SSR.
