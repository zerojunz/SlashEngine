# Render scale

The world drawn at a lower resolution and scaled up to the window, while controls draw at the
window's own size, as Godot's `rendering/scaling_3d` settings. A game that is slow to draw (lots
of lights, particles or shaders) draws fewer pixels and keeps its UI sharp.

[project.json](project.json) draws this game's world at half size and scales it up with AMD's
FidelityFX Super Resolution 1:

```json
"rendering": {"scaling": {"mode": "fsr", "scale": 0.5, "fsr_sharpness": 0.2}}
```

[main.cpp](main.cpp) changes the settings with `display.set`, which `slash-cli run --serve` adds
for a running game, and samples pixels across the planet's edge and the health bar's with
`frame.capture` after each change:

- `scale` is the world's size over the window's, from 0.25 to 2. At 1 with `bilinear` nothing is
  scaled; above 1 the world draws larger and is averaged down, always with `bilinear`.
- `bilinear` stretches the world with filtering, so edges blur over a couple of pixels.
- `fsr` runs FSR 1's two passes: EASU scales up along the edges it finds, then RCAS sharpens by
  `fsr_sharpness` stops (0 is the sharpest, 2 the softest).
- The health bar is a control, so its edge stays one sharp step whatever the scale. Canvas
  layers above the world stay sharp too; layers below it are scaled with it.
- `frame.capture` returns the `upscaler` that ran and the `render_size`, and `display.list` and
  `perf.stats` return the settings.
- A game adds its own upscaler with `render::add_upscaler`, here a nearest-neighbor one for pixel
  art, and names it in `scaling.upscaler`. Vendor upscalers (DLSS, XeSS) come the same way, from
  an SDK package's glue code, where the vendor's GPU runs them. Temporal ones take the depth and
  motion vectors of the 3D world instead, as [temporal-upscaler](../temporal-upscaler/) shows.
- `metalfx_spatial` is Apple's MetalFX spatial scaler, on Metal with macOS 13 or iOS 16. Here, on
  the CPU, it falls back to `fsr` as Godot's does, and `frame.capture` says so.
  Godot's `fsr2` scales only the 3D world, from jittered frames, as
  [fsr2-3d](../fsr2-3d/) shows; a world with no 3D in it scales with `bilinear` instead.

The planet's PNG is made by the program, so the example has no binary file, and every frame is
drawn on the CPU, so the output is the same on every machine.
