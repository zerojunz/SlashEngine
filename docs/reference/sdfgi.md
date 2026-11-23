# SDFGI: fully dynamic global illumination (research)

A research feature with no commitment to ship ([roadmap](../roadmap.md#research)). It ports
Godot 4.6's SDFGI, Godot's own fully dynamic GI, to the CPU and to compute shaders, following
RendererRD's `GI::SDFGI` (`servers/rendering/renderer_rd/environment/gi.cpp`) and its shaders
(`sdfgi_preprocess.glsl`, `sdfgi_direct_light.glsl`, `sdfgi_integrate.glsl` and `gi.glsl`'s
`sdfgi_process`), all MIT. The software device, the GPU device (Vulkan, Metal and
Direct3D 12) and the WebGPU device draw it; the OpenGL devices never will, as Godot's
Compatibility renderer has none. Read here what it does now, and what it does not do yet.

Status: current · Updated: 2026-11-20

## What it does

SDFGI lights a scene with no baking. Lights, meshes and the camera can all move.

Around the camera sit **cascades**, each a box of `cascade_size` cells a side (128, as Godot's).
The first cascade's cells are `field_lighting_min_cell_size` meters across, and each next one's are
twice as large, so it reaches twice as far. Heights count `y_scale` times as much (Godot's
`field_lighting_y_scale`: 2, 1.5 or 1), so cascades reach less far up and down than across. Each frame:

1. **The cascades follow the camera.** A cascade moves once the camera is more than half a probe
   spacing from its centre, by whole probe spacings (8 cells at 128), as Godot's `sdfgi_update`.
   Its probes move with it, keeping their history; probes that come in new start empty.
2. **Cells are made from the frame's triangles** when a cascade first appears, moves, or the
   frame's geometry changes. Every cell a triangle touches is solid, with its albedo (5 bits a
   channel, as Godot's), the light it gives off and the sides its triangles face. Godot draws the
   meshes into the cascade with the GPU's rasterizer; here a triangle and box overlap test does
   it, and the surface is read from the path tracer's scene (`path_surface_at`). Each cell sums
   its samples' albedo and emission in fixed point (8 bits under the point), so the sums come
   out the same in any order, as a GPU's atomic adds do.
3. **A distance field.** Each empty cell keeps its distance to the nearest solid cell, 1 more and
   rounded, up to 255, in a byte, as Godot's R8 texels. Godot finds it by jump flooding; here an
   exact Euclidean distance transform does, which jump flooding approximates, in whole numbers
   (squared distances, three passes of lower envelopes of parabolas), and the root is rounded
   up in whole numbers too. Each empty cell next to a solid one names the nearest of them, the
   first in `MODE_STORE`'s order, whose light it takes.
   With `field_lighting_use_occlusion`, each probe also floods out through the empty cells within a probe
   spacing of it, a step of Manhattan distance at a time, to say how much of each cell it sees:
   solid cells take their empty neighbors' on the sides they face, and empty cells next to solid
   ones see it by how many of those face it (`sdfgi_preprocess.glsl`'s `MODE_OCCLUSION`). The
   result is kept as 4 bits per probe parity, as Godot's occlusion texture. Its values are bytes
   and averages round half up in whole numbers.
4. **The solid cells' light.** Each light is traced through the distance field from the cell, with
   Godot's bias, and lights the sides the cell faces by how much they face it, times its albedo
   (`sdfgi_direct_light.glsl`). Emission and `field_lighting_bounce_feedback` of the probes' light from
   the frame before go in too, which makes more than one bounce. A `frames_to_update_light` share
   of the cells is lit a frame, by cell index (all of them after a cascade is made again), and
   the empty cells that name one take its light after it. The light is stored
   as Godot's RGB9E5 and its split over the six sides as bytes. Each light counts its
   `light_indirect_energy`, and lights whose `light_bake_mode` is `"disabled"` are left out.
5. **The probes trace rays.** A probe sits every 16th of a cascade (17 a side). Each frame each
   traces `ray_count` rays, spread by Vogel's spiral and turned each frame, through the cascades'
   fields: a ray that hits takes the cell's light toward it; one that leaves the last cascade takes
   the sky's light (`field_lighting_read_sky_light`). Each probe keeps 16 spherical harmonics coefficients
   over `frames_to_converge` frames as integers with 10 bits under the point, and their average,
   and stores octahedral maps of its irradiance and radiance, 6 texels a side with a border, as
   Godot's lightprobe texture (`sdfgi_integrate.glsl`).
6. **Pixels read the probes.** After the depth and normal prepass, each opaque pixel finds the
   finest cascade that holds it, blends into the next one near its edge, and takes the eight
   probes around it, each weighted by distance and by how much it faces the pixel, along its
   normal for ambient light and, when it is smooth, along its reflection, and with occlusion by
   how much it sees of the pixel's place. That is `gi.glsl`'s
   `sdfgi_process`, and the result covers the sky's ambient light and reflections by how much the
   cascades hold the pixel, as Godot's GI buffers do. VoxelGIs and reflection probes still go over
   it.

The work is shared out over the job pool's threads. Rays and samples are numbered, so a frame is
the same on any number of threads. Each cascade is kept as the 32-bit words a GPU's storage
buffers hold (`SdfgiCascade` in `src/sdfgi.hpp`): two a cell for the cell (albedo, facing, or
the neighbor it takes light from; its emission) and two for the field (the six shares and the
distance), one for the light and one for the occlusion, and the probes' history as pairs of
16-bit halves.

## On the GPU

The GPU device runs the same frame as compute shaders over storage buffers, word for word the
layout above, so it makes the same cells, fields, light and probes as the CPU twin
(`src/sdfgi_gpu.hpp` says what they take and in what order):

- `shaders/sdfgi_voxelize.hlsl` sums each draw's triangles into the cells, a thread for each of a
  triangle's z slices, with atomic adds of the same fixed point sums, sampling the albedo and
  emission textures as `path_surface_at` does;
- `shaders/sdfgi.hlsl` runs every other step as a pass of one dispatch: clears, the resolve into
  cells, the distance transform's passes, the occlusion flood, the light, the probes' rays,
  their history and their maps, and a moved cascade's scroll;
- `shaders/sdfgi_gather.hlsl` is `sdfgi_process`, drawn after the prepass into two layers of the
  screen texture (ambient light and reflections), which `mesh.hlsl` puts under VoxelGIs' light
  as `shade_mesh` does.

Each render target keeps its own buffers and `SdfgiState` (the cascades' places and what
changed), made again when the settings change; buffers past 4 GiB are refused with a warning
and the frame drawn without it. Rays that leave the cascades take the sky from the radiance cube
where the CPU reads the sky directly, so the two differ there by its filtering; elsewhere they
round alike to within a few levels, which `tests/test_sdfgi.cpp` holds them to.

## Using it

A `Environment3D`'s `sdfgi_*` fields turn it on ([scene-format.md](scene-format.md#worldenvironment)),
with Godot's names and defaults. A frame in `forward_plus` drawn by the software or GPU device (as
`frame.capture` draws it) then lights its opaque meshes with it:

```json
"environment": {"components": {
  "Environment3D": {"background_mode": "sky", "sdfgi_enabled": true, "sdfgi_cascades": 4}
}}
```

Light settles over `frames_to_converge` frames (30), as Godot's, so a capture of a scene's first
frame sees little of it. project.json's `rendering.field_lighting` holds Godot's
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
`tests/test_sdfgi.cpp` checks the light bounces, the cascades move and scroll, walls occlude probes and the result is the same on
any number of threads.

## Not done yet

This is a spike to measure the idea. It still does these things the simple way:

- **Large cascades on WebGPU.** Each of SDFGI's buffers must fit one storage binding of the
  browser's WebGPU. `web/webgpu.js` asks for the adapter's largest, and even the default
  128 MiB holds four cascades of 128 cells; past it the frame draws without SDFGI and logs why
  once, where splitting cascades over several buffers would draw it.
- **Only cascades over unchanged triangles scroll.** A moved cascade keeps the cells it still
  holds and makes only the slab that came in, as Godot's scroll does, but any change to the
  frame's triangles (and so a camera turn that culls others) makes every cascade again, where
  Godot voxelizes only the meshes that moved.
- **Cells are made from the frame's triangles,** which are what the camera sees through its
  culling, where Godot draws every mesh in a cascade's box.
- **No textures' changes.** A texture written again (a video or a viewport) doesn't make the
  cells again.
- **No debug views.** Godot's probe and cascade views are not ported.
