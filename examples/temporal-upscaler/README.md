# Temporal upscaler

A game's own temporal upscaler, run in place of FSR 2, as DLSS, XeSS or FSR 4 come in from a
vendor's SDK package. Godot hands its `RenderingDevice`'s native objects to a
`CompositorEffect` for this; SlashEngine hands them to a `render::TemporalUpscaler`.

[main.cpp](main.cpp) adds `splat` with `render::add_upscaler`, marked `temporal` and made by
`make_temporal`. [project.json](project.json) sets `rendering.scaling` to `fsr2` at a `scale` of
`0.5` with `upscaler` `splat`, so wherever FSR 2 would run (Forward+, a scale under 1, the GPU
device on Vulkan, Direct3D 12 or Metal, or the software device) the device runs `splat` instead.
[main.scene.json](main.scene.json) is the cream card of [fsr2-3d](../fsr2-3d/) in a 160x90
window, so the 3D world draws at 80x45.

Each frame, `upscale` gets a `render::TemporalFrame`: the color, depth and motion vectors at the
render size, the output at the window's size, the camera's jitter in render pixels and whether
its history starts over. On a GPU those are the native API's objects (a `VkImage`, an
`ID3D12Resource`, an `MTLTexture`, with the device, queue and command buffer), which a vendor's
library takes; `frame.capture` draws on the software device, which hands over float pixels
instead, so the example runs the same on every machine.

`splat` moves its history along the motion vectors, then writes each render pixel's color into
the window pixel its sample fell in: the jitter moved the picture right and down by `jitter`
render pixels, so a render pixel's center saw the point that far to its left and above. Each
frame it prints what it was handed:

```text
  splat: frame 1, 80x45 to 160x90, jitter (-0.500, 0.500), moved 0.0, reset
frame.capture {"points": [[59, 45], [60, 45], [61, 45], [62, 45], [63, 45], [64, 45]]}
  -> {"colors":[[0.051,0.078,0.149,1],[0.949,0.898,0.8,1],...],"render_size":[80,45],"upscaler":"splat",...}
```

- The first capture is a first frame, so `reset` is set and `splat` stretches the render pixels:
  across the card's left edge on row 45 the cream starts at x 60, where the render pixel covering
  60 and 61 is cream. `upscaler` says `splat` ran.
- Seven more frames each move the camera along FSR 2's jitter sequence, and `splat` puts their
  samples between the render pixels' centers: the edge settles at x 62.
- At a `scale` of `1` nothing upscales and `splat` isn't called. The window's own pixels show the
  edge at x 62 too, where `splat` put it.
- Back at half size, the device makes a new `splat` for the new size, which starts over and
  settles again.
- `component.set` turns the card. Its motion vectors carry each pixel up to 2.2 window pixels
  (`moved`), so the history follows the card and the frame's samples land on the new edge, at
  x 61, as the full-size capture after it shows.

A failure from `upscale` is logged and the frame shows what the output held. A vendor's
upscaler checks the GPU in `supports` (on Vulkan, DLSS needs NVIDIA's device extensions, which
SDL doesn't turn on), and leaves the textures in the state shaders read them in, as
`render::TemporalFrame` says.
