# Post-processing 3D

What happens to a 3D world's colors after it is lit, as Godot's `Environment` does it in its
Compatibility renderer, which [project.json](project.json)'s `rendering.rendering_method` picks
(`gl_compatibility`; [glow-3d](../glow-3d/) has Forward+'s). [main.scene.json](main.scene.json)
is a crate and a glowing lantern on the ground at night, seen through a `Camera3D` in a 320x180
window. Its `environment` entity's `WorldEnvironment` sets:

- `tonemap_mode` `"aces"` with `tonemap_exposure` 1.2: lit colors are brightened, then mapped
  into the screen's range by Godot's ACES curve.
- `glow_enabled`, with `glow_hdr_threshold` 0.6 and `glow_hdr_scale` 0.5: what is brighter than
  0.6 bleeds into what is around it. The lantern's emission is three times its color, so it glows.
  The textures glow is made from hold 8 bits a channel, as Godot's Compatibility renderer's do,
  so nothing is brighter than 1 and the threshold has to be below it.
- `adjustment_enabled`, with `adjustment_contrast` 1.1 and `adjustment_saturation` 1.2.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[188,67],[198,67],[122,100],[190,106],[40,20]]}
{"colors":[[1,1,0.898,1],[0.067,0.082,0.125,1],[0.514,0.18,0.039,1],[0.82,0.635,0.541,1],
 [0,0,0.016,1]],"drawn":{"meshes":3},"height":180,"width":320}
```

- `frame.capture` draws the frame on the CPU, post-processing included, so it needs no GPU and
  gives the same pixels on every machine. `points` reads the lantern, the night sky just beside it
  lit by its glow, the crate, the ground under the lantern and the sky far from it.
- `component.set` turns `glow_enabled` off, and the sky beside the lantern goes as dark as the
  rest.
- Setting `tonemap_mode` to `"linear"` leaves lit colors as they are; ACES lifted the middle
  tones, so the crate and the ground darken.
- AgX, glow back on and `adjustment_saturation` 0.3 turn the scene almost gray. `path` saves the
  frame as `shots/night.png`.

`adjustment_color_correction` names a lookup texture colors pass through after that: one pixel
tall to map each channel along it, or as wide as its height squared for a 3D LUT.
