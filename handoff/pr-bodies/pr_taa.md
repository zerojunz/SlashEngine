Before: 3D meshes had hard, stair-stepped edges in every Forward+ frame. Godot's TAA setting had nothing to map to, so `project.import` dropped it.

After: `rendering.anti_aliasing.quality.use_taa` in `project.json` turns on temporal antialiasing for the 3D world, as Godot 4.6's `use_taa`. It runs on every Forward+ device: the SDL GPU device (Metal, Direct3D 12, Vulkan), WebGPU and the software device that `frame.capture` draws on.

- **Jitter:** each frame moves the camera by less than a pixel, along the next of 16 points of a Halton sequence (bases 2 and 3), as Godot's `RendererSceneCull` does.
- **Motion vectors:** the Forward+ prepass writes how far each opaque pixel moved since the frame before. The motion comes from the mesh's transform, a skinned mesh's vertices and the camera.
- **Resolve:** a pass blends the frame with the frames before it, moved by those motion vectors and clipped to the colors around each pixel. Edges smooth out over about 16 frames.
- **Where it stays off:** the 2D canvas, controls, the Compatibility renderer (which has no TAA in Godot either, so the OpenGL drivers never use it) and reflection probe captures.
- **Import:** Godot's `rendering/anti_aliasing/quality/use_taa` now imports.

How:

- **Shared code:** `engine/render/src/taa.hpp` and `taa.cpp` hold what every device shares: the jitter, the mesh passes' camera with the frame before's view-projection, and `run_taa`, the CPU twin of the resolve.
- **Shaders:**
  - `shaders/taa.hlsl` ports Godot's `taa_resolve.glsl` (MIT, from Spartan Engine's TAA). It covers the Catmull-Rom history, the closest-depth velocity, variance clipping, the off-screen and disocclusion factors, and the anti-flicker weight.
  - It runs as a pass that draws one triangle and loads texels, so every device draws the same values.
  - `shaders/mesh_motion.hlsl` is the prepass with a second, RG16F target.
- **Devices:**
  - `RenderDevice::draw_mesh` takes the mesh's previous transform.
  - A device keeps a skinned mesh's vertices from before its last `update_mesh`, and keeps each target's history (two color textures and two velocity textures).
  - A frame with no meshes starts the history again.
- **Previous transforms:** `scene3d::Renderer` remembers each draw's transform by entity and passes the old one when it changed.
- **Settings:** the setting is read by `slash-game`, sessions and `frame.capture`, and the project schema checks it.
- **`frame.capture`:** a new `frames` param draws several frames at once, since TAA builds up over frames. Captures keep the history between them, as a running game's frames do.
- **Tests:** `tests/test_taa.cpp` covers:
  - the Halton jitter and the jittered projection;
  - motion vectors from transforms and from `update_mesh`;
  - the resolve's blending, off-screen history and clipping;
  - the setting;
  - a still square's edges smoothing, with no effect in Compatibility and a reset after an empty frame;
  - squares moved by transform, by `update_mesh` and by the camera, each against a run without motion vectors;
  - the GPU device drawing as the software device does (run on Vulkan through lavapipe).

  The test also runs in the WebGPU tests.
- **Example:** `examples/taa-3d` smooths a card's edges through `frame.capture` and keeps them smooth as it turns.
- **Docs:** `docs/reference/project-format.md`, `commands.md`, `godot-import.md`, AGENTS.md and the examples index. The roadmap ticks TAA under the upscalers item, since FSR 2/3 and MetalFX build on the same jitter and motion vectors.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
