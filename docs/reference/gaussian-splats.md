# Gaussian splats: captured scenery (research)

A research feature with no commitment to ship ([roadmap](../roadmap.md)). Godot has nothing like
it; it follows Kerbl, Kopanas, Leimkühler and Drettakis's *3D Gaussian Splatting for Real-Time
Radiance Field Rendering* (SIGGRAPH 2023), the way photos of a real place are turned into
scenery that looks like the photos. Only the software device draws it for now. Read here what it
does now, and what it does not do yet.

## What it does

A capture made with 3D Gaussian Splatting's trainer, or a tool that writes its files (Postshot,
Polycam, Luma, nerfstudio's splatfacto, gsplat), is a `.ply` file of **splats**: small,
see-through 3D Gaussians, each with a place, a size along three axes (a standard deviation in
meters), a turn, an opacity and a color that changes with the side it is seen from (spherical
harmonics, up to degree 3). Detail an agent could not model by hand (bark, moss, a cluttered
shelf, a whole street) comes in as millions of them.

A [`GaussianSplat3D`](scene-format.md#gaussian-splats) names the file and places it with its
entity's `Transform3D`:

```json
"boulder": {"components": {
  "GaussianSplat3D": {"splats": "captures/boulder.ply", "sh_degree": 3},
  "Transform3D": {"position": [0, 0.5, 0], "rotation": [3.14159, 0, 0]}}}
```

Trainers work in their cameras' axes, often with y down, so a capture usually needs a turn half
way round x to stand up in Godot's y-up space. The file loads as an asset, so editing or
replacing it reloads the cloud.

In Forward+, after the opaque meshes (and subsurface scattering) and before the transparent
ones, the software device blends each frame's clouds over the world as the trainer's rasterizer
does:

1. **Project.** Each splat's covariance is turned and scaled by its cloud's transform, then
   projected to the screen through the Jacobian of the frame's view projection (Zwicker et al.'s
   EWA splatting), with 0.3 pixels squared added, as the trainer's low-pass filter. The Jacobian
   comes from the whole projection, so orthogonal cameras and TAA's and FSR 2's jitter work.
2. **Color.** Its spherical harmonics are evaluated toward the camera, in the cloud's own space,
   up to `sh_degree`, and 0.5 added, as the trainer's colors; they are sRGB, so they are made
   linear for Forward+'s world.
3. **Sort.** Splats are sorted by their distance along the view, nearest first (ties by cloud,
   then splat), and put in the tiles of 16 pixels their three standard deviations reach.
4. **Blend.** Each pixel blends its tile's splats front to back, each by its opacity times its
   Gaussian (at most 0.99, and none under 1/255), until under 1/10000 of the light behind shows.
   A splat whose center is behind what the opaque meshes drew at a pixel leaves it alone, so
   meshes in front hide captured scenery and the scenery hides meshes behind it.

The tiles run on the job pool, built with `-ffp-contract=off` and with `slash::exp`, so a frame
is the same on any number of threads and every machine. `frame.capture` and `lighting.check`
see the splats, so agents check a capture's placement by its pixels.

Exports ship `.ply` files and check that each reads as splats; `scene validate` reports a
`GaussianSplat3D` whose file isn't a `.ply` file, can't be read or isn't splats, and an
`sh_degree` outside 0 to 3. `examples/gaussian-splats` makes a boulder in code, puts it behind a
post, turns its shine off and fades it.

## What it does not do yet

- **Only the software device.** GPU devices, and the OpenGL and WebGPU devices, draw no splats;
  they need a compute sort and a tiled blend of their own.
- **Forward+ only.** Compatibility frames draw none.
- **Captured light only.** Splats show the light they were captured in: the scene's lights,
  shadows, fog, GI and reflections don't change them, and they cast no shadows.
- **Not in the depth buffer.** They write no depth, so SSAO, SSR, depth of field, decals and
  ray-traced effects don't see them, and transparent meshes draw over them whatever their depth.
- **No rays.** The path tracer and ray-traced effects don't meet them, and physics and
  navigation don't know they are there: give a capture collision with simple shapes or a mesh.
- **No compression or streaming.** A cloud is held as floats in memory (about 250 bytes a splat
  at degree 3); compressed formats such as SPZ and `.splat`, and loading parts of a large capture
  as the camera moves, would come next.
- **No anti-aliasing compensation.** The 0.3-pixel filter is the original trainer's;
  Mip-Splatting's opacity compensation for captures seen smaller than they were trained is not
  applied.
