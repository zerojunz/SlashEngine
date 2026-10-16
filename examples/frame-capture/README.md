# Frame capture

`frame.capture` draws the frame the window shows, on the CPU, so an agent can check what a
player would see with no window or GPU, and get the same pixels on every machine.
[main.scene.json](main.scene.json) is a small HUD in a 160x90 window
([project.json](project.json)): a sky, the ground and a health bar. [calls.jsonl](calls.jsonl)
captures it the way an agent does over MCP (the tool `frame_capture`) or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies.

```text
frame.capture {"points":[[80,30],[80,75],[20,14],[68,14]]}
{"colors":[[0.2,0.4,0.8,1],[0.302,0.6,0.2,1],[0.502,0.584,0.753,1],[0.169,0.31,0.588,1]],
 "drawn":{"controls":3},"height":90,"width":160}
```

- With no params it returns the image's size and what it drew (`sprites`, `tiles`,
  `particles`, `lights` and `controls`; counts of zero are left out), which is often enough to
  tell that something is missing.
- `points` returns the colors of window pixels as `[r, g, b, a]`, the cheapest way to check a
  change: after `component.set` turns the health bar down to 25, the pixel at `[40, 14]` shows
  the bar's background instead of its fill.
- `region` keeps only part of the window, `[x, y, width, height]` in window pixels, and `scale`
  sizes the image (0.5 is half size). Points are still window pixels.
- `"image": true` returns the image as a base64 PNG in `png`; over MCP it is sent as an image
  the model can look at. Keep it small with `region` and `scale`: a picture costs far more
  tokens than a few colors.
- `path` writes the PNG to a project file, here `shots/hud.png`, and needs the `filesystem`
  capability.
- The software device draws what the GPU device draws, with the same materials, lights
  and blending; the tests hold both to the same pixels.
