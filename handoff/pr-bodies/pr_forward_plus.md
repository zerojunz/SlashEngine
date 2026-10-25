Before: every device lit 3D meshes as Godot's Compatibility renderer does. Each pixel added every light, so a scene took 64 lights at most, and a 65th failed the frame.

After: meshes are lit as Godot's Forward+ renderer lights them, by default. A scene takes 8 directional lights and 512 omni and spot lights. Each pixel adds only the omni and spot lights whose range reaches its tile and depth slice. `rendering.rendering_method` in project.json picks `forward_plus` or `gl_compatibility`, as Godot's `rendering/renderer/rendering_method` does.

```json
"rendering": {"rendering_method": "gl_compatibility"}
```

How:

- **Clusters** (`engine/render/src/clusters.cpp`)
  - This is Godot's `ClusterBuilderRD` layout, built on the CPU, so the software device and every GPU read the same clusters.
  - The screen is cut into 32-pixel tiles and the depth out to the far plane into 32 slices.
  - Omni and spot lights are sorted nearest first. Each tile gets a bit per light whose range sphere, rounded out, may reach it. Each slice holds the range of lights in it.
  - A light culled from a pixel would have added exactly 0, so Forward+ matches Compatibility up to the order lights are added in.
- **Shaders**
  - `mesh_clustered.hlsl` is `mesh.hlsl` with `CLUSTERED` defined, as a fragment stage only.
  - It reads the lights and cells from two storage buffers and walks a tile's bits in the slice's range, lowest first. The software device's twin walks them in the same order.
  - Directional lights stay in the scene uniforms.
  - `compile_shaders.cmake` gains `FRAGMENT_ONLY` and `STORAGE_BUFFERS`. Storage buffers are numbered after the uniforms in Metal (from SPIR-V built with `SLASH_MSL`) and are WebGPU's otherwise empty bind group 0 (`SLASH_WGSL`). Includes inside included files are brought in too.
- **Devices**
  - The SDL GPU device (Vulkan, Metal, Direct3D 12) and the WebGPU device upload the clusters each frame and key mesh pipelines on the method.
  - The software device builds the same clusters.
  - The OpenGL devices (including WebGL 2) always draw Compatibility, as Godot's do, which needs no storage buffers.
- **Limits**
  - `check_scene` now refuses a 9th directional light or a 513th omni or spot light.
  - `scene3d::lights` puts the directional lights first, so Compatibility's first 64 keep them all, and caps each kind in tree order.
  - `shadow_slots` returns one slot per light.
- **Settings**
  - `rendering.rendering_method` is in project.json's schema, `runtime::Project`, `canvas::Settings` (sessions and games) and the renderer's frames.
  - The Godot importer writes `gl_compatibility` for Godot's Compatibility renderer.
- **Tests**
  - New `tests/test_clusters.cpp` checks:
    - tiles and slices for lights ahead, behind and around the eye;
    - Forward+ against Compatibility with 64 lights;
    - a grid of 100 lamps fully lit on the software device, Vulkan and WebGPU, with only the first 64 on Compatibility and the OpenGL devices.
  - Further checks: scene light caps and order, the project setting, the importer, the new check_scene messages, and the clustered shader in the Windows HLSL compile test.
- **Example:** new `examples/clustered-lights`, a plaza of 100 lanterns read by `frame.capture`, past the 64th, then the last one turned red.
- **Docs**
  - `project-format.md` gains "Rendering methods", and `scene-format.md`'s lights section gives the new limits.
  - The Godot import table is updated.
  - The roadmap ticks the clustered forward+ renderer.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
