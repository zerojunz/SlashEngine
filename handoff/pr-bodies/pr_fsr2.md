Before: `rendering.scaling` could only stretch the world with `bilinear` or FSR 1. `project validate` refused Godot's `fsr2` mode, and `project.import` dropped a Godot project's `scaling_3d` settings.

After: `"rendering": {"scaling": {"mode": "fsr2", "scale": 0.5}}` draws the 3D world at half the window's size and rebuilds it at full size with AMD's FidelityFX Super Resolution 2.2, as Godot 4.6's `scaling_3d` mode `fsr2` does in Forward+. It runs on every Forward+ device: the SDL GPU device (Metal, Direct3D 12, Vulkan), WebGPU and the software device that `frame.capture` draws on.

- **Jitter:** each frame moves the camera by less than a render pixel, along a Halton sequence of `8 × (display ÷ render)²` points, as AMD's `ffxFsr2GetJitterPhaseCount` and Godot's `RendererSceneCull` work it out.
- **Motion vectors:** the Forward+ prepass writes them as it does for TAA, from transforms, skins and the camera.
- **Passes:** FSR 2.2's seven passes rebuild the frame at the window's size from the jittered frames, the depth and the motion vectors, then RCAS sharpens it by `fsr_sharpness`:
  1. luminance;
  2. dilated depth and motion;
  3. depth reconstructed from the frame before;
  4. depth clip;
  5. locks;
  6. accumulation;
  7. RCAS.

  The 3D post-processing then runs at the window's size.
- **What stays at full size:** the 2D canvas and controls draw at full size, as in Godot.
- **Fallbacks:** in Compatibility, with the OpenGL drivers, at a scale of 1 or above, or with no 3D in the world, the world scales with `bilinear` instead, as Godot falls back.
- **TAA:** TAA is off while FSR 2 runs.
- **Import:** Godot's `rendering/scaling_3d/mode`, `scale` and `fsr_sharpness` import into `rendering.scaling`. MetalFX isn't built, so its spatial mode becomes `fsr` and its temporal mode `fsr2`, and the import reports both.

How:

- **Shared code:** `engine/render/src/fsr2.hpp` and `fsr2.cpp` hold what every device shares: the render size, the phase count, the jitter, the passes' uniforms (AMD's dispatch constants from Godot's settings) and `run_fsr2`, the CPU twin of the seven passes.
- **Shaders:** `engine/render/shaders/fsr2*.hlsl` port AMD's `ffx_fsr2_*.h` (MIT, notice kept in every file). Each pass draws one triangle and loads texels, so every device draws the same values. The reconstruct pass is a compute shader, for its atomics.
- **Devices:**
  - A Forward+ frame with FSR 2 draws the meshes, depth and motion vectors at the render size.
  - Each target keeps FSR 2's history: color, locks, luma history and dilated motion, two of each.
  - Post-processing (SSAO, SSR and depth of field read the depth at the render size) runs after FSR 2.
- **Wiring:** `render::uses_fsr2` decides where it runs. `scene3d::Renderer::prepare` passes `Scene3D::fsr2`, and `CanvasRenderer` reports `upscaler` `"fsr2"` with the 3D world's `render_size` in `frame.capture` and `display.list`.
- **Tests:** `tests/test_fsr2.cpp` covers:
  - the render size and phases against Godot's;
  - the uniforms against AMD's dispatch;
  - phase changes on a resize;
  - a still square's edges rebuilt at full size;
  - moving meshes and a moving camera followed by their motion vectors;
  - the GPU device drawing as the software device does (Vulkan through lavapipe here, and Dawn in the WebGPU tests).

  `test_upscaler.cpp` and `test_import_godot.cpp` cover the mode and the import.
- **Example:** `examples/fsr2-3d` rebuilds a card from a half-size render through `frame.capture` and follows it as it turns. `examples/render-scale` now shows MetalFX's error where it showed `fsr2`'s.
- **Docs:** `docs/reference/project-format.md`, `commands.md`, `godot-import.md`, AGENTS.md, THIRD_PARTY.md and the examples index. The roadmap ticks FSR 2 and leaves FSR 3's upscaler, MetalFX and temporal upscalers a game adds through `render::Upscaler` open.

Left for later:

- No reactive mask, as in Godot, so transparent meshes can trail for a frame or two.
- The sky writes no motion vectors yet. That matters only when the camera turns.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
