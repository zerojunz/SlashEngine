# Meshes in 3D

Meshes lit, drawn by the render device. [main.cpp](main.cpp) makes a floor, a cube and a pane
with `create_mesh`, then draws a red cube, a yellow cube, the floor and a pane of blue glass in
one frame whose `scene` gives the camera, the sun, a warm lamp and the ambient light, and prints
what each pixel of the 60x20 target shows; [expected.txt](expected.txt) holds the output.

```text
..............GGGGGGGGGGGRRRRRrrr########...................
.........#####GGGGGGGGGGGGGGGGrrr###################........
++++++++++++++GGGGGGGGGGGGGGGGrr############################
+++++++++++++++GGGGGGGGGGGGGGGr###YYYYYYYYYYY###############
+++++++++++++++GGGGGGGGGGGGGGG####YYYYYYYYyyyy##############
```

- Y is up and the camera looks down its -Z. `render::look_at` places the camera
  and `render::perspective` (or `orthogonal`) projects with reversed depth. Front faces wind
  clockwise as the camera sees them; `cull` hides back faces by default.
- `Surface3D` is the material: albedo, metallic, roughness, specular, emission,
  a normal map and textures for each, `unshaded`, `transparency` (`Alpha` blends, after the
  opaque meshes and farthest first; `AlphaScissor` cuts), `cull` and `uv_scale`/`uv_offset`.
- A frame's `Scene3D` holds up to 64 lights, each a `SunLight3D`, `PointLight3D` or
  `SpotLight3D` with its color, energy, range, attenuation, spot angle and cull mask, and the
  ambient light. A mesh draw's `layers` choose which lights reach it.
- Meshes draw first, over a depth buffer, and a frame's 2D draws go over them.
- Every device draws meshes: Metal, Direct3D 12 and Vulkan, OpenGL 3.3 and OpenGL ES 3.0, WebGPU
  and the software device. This draws on the software device so the output is the same on
  every machine; `tests/test_mesh.cpp` holds the GPU devices to its pixels. Shadows, a sky and
  scenes with 3D nodes come in later milestone 7 work.
