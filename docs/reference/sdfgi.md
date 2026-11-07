# SDFGI: fully dynamic global illumination (research)

A research feature with no commitment to ship ([roadmap](../roadmap.md#research)). It ports
Godot 4.6's SDFGI, Godot's own fully dynamic GI, to the CPU, following RendererRD's
`GI::SDFGI` (`servers/rendering/renderer_rd/environment/gi.cpp`) and its shaders
(`sdfgi_preprocess.glsl`, `sdfgi_direct_light.glsl`, `sdfgi_integrate.glsl` and `gi.glsl`'s
`sdfgi_process`), all MIT. Only the software device draws it for now. Read here what it does now,
and what it does not do yet.

## What it does

SDFGI lights a scene with no baking. Lights, meshes and the camera can all move.

Around the camera sit **cascades**, each a box of `cascade_size` cells a side (128, as Godot's).
The first cascade's cells are `sdfgi_min_cell_size` meters across, and each next one's are
twice as large, so it reaches twice as far. Heights count `y_scale` times as much (Godot's
`sdfgi_y_scale`: 2, 1.5 or 1), so cascades reach less far up and down than across. Each frame:

1. **The cascades follow the camera.** A cascade moves once the camera is more than half a probe
   spacing from its centre, by whole probe spacings (8 cells at 128), as Godot's `sdfgi_update`.
   Its probes move with it, keeping their history; probes that come in new start empty.
2. **Cells are made from the frame's triangles** when a cascade first appears, moves, or the
   frame's geometry changes. Every cell a triangle touches is solid, with its albedo (5 bits a
   channel, as Godot's), the light it gives off and the sides its triangles face. Godot draws the
   meshes into the cascade with the GPU's rasterizer; here a triangle and box overlap test does
   it, and the surface is read from the path tracer's scene (`path_surface_at`).
3. **A distance field.** Each empty cell keeps its distance to the nearest solid cell, 1 more and
   rounded, up to 255, in a byte, as Godot's R8 texels. Godot finds it by jump flooding; here an
   exact Euclidean distance transform does, which jump flooding approximates.
4. **The solid cells' light.** Each light is traced through the distance field from the cell, with
   Godot's bias, and lights the sides the cell faces by how much they face it, times its albedo
   (`sdfgi_direct_light.glsl`). Emission and `sdfgi_bounce_feedback` of the probes' light from
   the frame before go in too, which makes more than one bounce. A `frames_to_update_light` share
   of the cells is lit a frame (all of them after a cascade is made again). The light is stored
   as Godot's RGB9E5 and its split over the six sides as bytes. Each light counts its
   `light_indirect_energy`, and lights whose `light_bake_mode` is `"disabled"` are left out.
5. **The probes trace rays.** A probe sits every 16th of a cascade (17 a side). Each frame each
   traces `ray_count` rays, spread by Vogel's spiral and turned each frame, through the cascades'
   fields: a ray that hits takes the cell's light toward it; one that leaves the last cascade takes
   the sky's light (`sdfgi_read_sky_light`). Each probe keeps 16 spherical harmonics coefficients
   over `frames_to_converge` frames as integers with 10 bits under the point, and their average,
   and stores octahedral maps of its irradiance and radiance, 6 texels a side with a border, as
   Godot's lightprobe texture (`sdfgi_integrate.glsl`).
6. **Pixels read the probes.** After the depth and normal prepass, each opaque pixel finds the
   finest cascade that holds it, blends into the next one near its edge, and takes the eight
   probes around it, each weighted by distance and by how much it faces the pixel, along its
   normal for ambient light and, when it is smooth, along its reflection. That is `gi.glsl`'s
   `sdfgi_process`, and the result covers the sky's ambient light and reflections by how much the
   cascades hold the pixel, as Godot's GI buffers do. VoxelGIs and reflection probes still go over
   it.

The work is shared out over the job pool's threads. Rays and samples are numbered, so a frame is
the same on any number of threads.

## Using it

A `WorldEnvironment`'s `sdfgi_*` fields turn it on ([scene-format.md](scene-format.md#worldenvironment)),
with Godot's names and defaults. A frame in `forward_plus` drawn by the software device (as
`frame.capture` draws it) then lights its opaque meshes with it:

```json
"environment": {"components": {
  "WorldEnvironment": {"background_mode": "sky", "sdfgi_enabled": true, "sdfgi_cascades": 4}
}}
```

Light settles over `frames_to_converge` frames (30), as Godot's, so a capture of a scene's first
frame sees little of it. project.json's `rendering.global_illumination.sdfgi` holds Godot's
project settings of the same names, for every SDFGI a frame draws:

| Setting | Default | What it does |
| --- | --- | --- |
| `probe_ray_count` | 8 | Rays each probe traces a frame: 4, 8, 16, 32, 64, 96 or 128 |
| `frames_to_converge` | 30 | Frames a probe averages: 5, 10, 15, 20, 25 or 30 |
| `frames_to_update_lights` | 4 | Frames over which the cells' light is updated: 1, 2, 4, 8 or 16 |

```json
"rendering": {"global_illumination": {"sdfgi": {"probe_ray_count": 16, "frames_to_converge": 20}}}
```

The Godot importer reads them from `project.godot`. In C++, `render::Scene3D::sdfgi`
(`render::Sdfgi3D`) has the same settings (`ray_count`, `frames_to_converge`,
`frames_to_update_light`), plus the cascade size (128), which only C++ sets.

`examples/sdfgi-3d` lights a room with it and repaints a wall.
`tests/test_sdfgi.cpp` checks the light bounces, the cascades move and scroll and the result is the same on
any number of threads.

## Not done yet

This is a spike to measure the idea. It still does these things the simple way:

- **Only the software device draws it.** The other devices draw without it. A port to compute
  shaders, as Godot runs it, is the next step if the spike holds up.
- **Only cascades over unchanged triangles scroll.** A moved cascade keeps the cells it still
  holds and makes only the slab that came in, as Godot's scroll does, but any change to the
  frame's triangles (and so a camera turn that culls others) makes every cascade again, where
  Godot voxelizes only the meshes that moved.
- **Cells are made from the frame's triangles,** which are what the camera sees through its
  culling, where Godot draws every mesh in a cascade's box.
- **No textures' changes.** A texture written again (a video or a viewport) doesn't make the
  cells again.
- **No probe occlusion.** `sdfgi_use_occlusion` is kept, but Godot's occlusion texture isn't
  made yet, so probes behind a wall can leak light through it.
- **No debug views.** Godot's probe and cascade views are not ported.
