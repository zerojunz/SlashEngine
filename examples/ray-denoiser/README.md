# Ray denoiser

A game's own denoiser for the ray-traced passes, as DLSS Ray Reconstruction, NVIDIA's NRD or
AMD's FidelityFX denoisers come in from an SDK package. [main.cpp](main.cpp) adds `smooth` with
`render::add_denoiser`, then draws a floor and a cube on the software device with ray-traced
shadows from a light with a size (one ray a pixel) and ray-traced ambient occlusion (four), first
without a denoiser, then naming `smooth` as project.json's `rendering.ray_tracing.denoiser` would.

Each frame that traces rays, after the rays and before the frame is lit from them, the device
hands the denoiser a `render::DenoiseFrame`: the shadows, occlusion, reflections and ambient light
the frame traced (an empty texture for a pass it didn't), with the prepass's depth, normals and
roughness, the motion vectors where the frame draws them, whether its history starts over, and the
camera. It denoises them in place. `smooth` averages each pixel's shadows and occlusion with its
neighbors on the same surface and prints how grainy each was before and after:

```text
traced     77777777777772222222233333777777
denoising two frames:
  smooth: 32x32, reset
  shadows   grain   9 ->   5
  occlusion grain  23 ->   7
  smooth: 32x32
...
denoised   77777777777772222224444556677777
missing    as traced
```

- `traced` and `denoised` are the middle row of the picture, through the cube and its shadow, as
  the green channel in tenths.
- `reset` is set on the target's first frame with the denoiser, and after a change of size.
- A name no SDK package added (`missing`), or one whose `supports` says no on the device, leaves
  the passes as the engine traces and smooths them.
- On the GPU device (Vulkan, Direct3D 12 or Metal, where it traces rays) the textures are the
  native API's objects with the command buffer, the form vendors' libraries take.

See [rendering.md](../../docs/reference/rendering.md) and
[project-format.md](../../docs/reference/project-format.md#ray-tracing).
