# Ray-traced reflections 3D

The hall of [ssr-3d](../ssr-3d/), its reflections traced by rays instead of found on the screen,
as `rendering.ray_tracing`'s `reflections` turns them on in Forward+.
[main.scene.json](main.scene.json) is the same, a polished metallic floor with a red crate, a blue
pillar, a rug and a warm back wall, with one thing added: a green `banner` high on the back wall,
above the top of the screen. The `WorldEnvironment`'s `ssr_enabled` is on.

[project.json](project.json) sets `rendering.ray_tracing`: `reflections` on, with 4 rays a pixel.
Each opaque pixel smoother than roughness 0.7 sends them off its surface, reflected about GGX's
microfacet normals for its roughness, through every mesh the frame draws. What a ray meets is lit
as a path tracer's bounce lights it (what it gives off, the lights through ray-traced shadows, and
the ambient light), and the share of rays that meet something covers the floor's reflection of
the environment, as SSR's reflections do.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[118,92],[200,88],[60,84],[160,147]],"path":"shots/hall.png"}
{"colors":[[0.502,0.322,0.349,1],[0.314,0.341,0.553,1],[0.498,0.435,0.4,1],[0.353,0.529,0.412,1]],
 "drawn":{"meshes":6},"height":180,"path":"shots/hall.png","width":320}
```

- `frame.capture` draws the frame on the CPU, rays included, so it needs no GPU and gives the same
  pixels on every machine. `points` reads the floor under the crate, under the pillar, where it
  reflects the back wall, and where it reflects the banner. The first three show the crate's red,
  the pillar's blue and the wall's warm color, stronger than SSR finds them (ssr-3d reads
  `[0.424,0.302,0.329]`, `[0.294,0.31,0.463]` and `[0.404,0.357,0.341]` there), as SSR's
  reflections fade in near the floor and out toward the screen's edges.
  The banner is off the screen, which SSR can't reflect; the rays find it, so the floor shows a
  green band. The pillar's reflection also goes on past the rug that hides it on the screen.
- `component.set` makes the floor rougher, at 0.4. Each pixel's rays spread out, so the
  reflections blur and the banner's fades into the lamp's highlight; with 4 rays a pixel they are
  grainy, and more `reflection_samples` smooth them.
- `batch` makes the floor polished again and turns `ssr_enabled` off, which turns the traced
  reflections off too: the floor is grey, as ssr-3d's last capture.

Run as a game (`slash-cli run`), GPUs draw the hall with SSR for now; tracing reflections on the
GPU comes next.
