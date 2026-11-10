# Gaussian splats

Scenery captured from photos, drawn as 3D Gaussian Splatting draws it (a research feature: only
the software device draws it yet; see [gaussian-splats.md](../../docs/reference/gaussian-splats.md)).

A capture made with Postshot, Polycam, Luma or nerfstudio's splatfacto is a `.ply` file of
splats: small, see-through 3D Gaussians, each with a place, a size along three axes, a turn, an
opacity and a color that changes with the side it is seen from (spherical harmonics). A
`GaussianSplat3D` names the file and its entity's `Transform3D` places it:

```json
"boulder": {"components": {
  "GaussianSplat3D": {"splats": "boulder.ply"},
  "Transform3D": {"position": [0, 0.5, 0]}}}
```

[main.cpp](main.cpp) makes its boulder in code with `render::write_splat_ply`, so the repository
holds no binary file: 1500 splats over a squashed sphere, gray stone below and moss on top, the
moss wet so a first-degree spherical harmonic brightens it toward the camera. It opens
[main.scene.json](main.scene.json) (a floor, a sun and a white post), puts the boulder behind the
post, and reads the frame with `frame.capture`. [expected.txt](expected.txt) holds the output:

- The moss and the stone show their colors, the post in front hides the boulder's right side, and
  the sky is the background.
- `sh_degree` 0 leaves out the shine: the moss is duller, the stone (which has none) the same.
- Moved in front of the post, the boulder hides it. Its splats overlap many deep, as a capture's
  do, so the post shows through only at a faint `opacity` of 0.05.

Splats show the light they were captured in, so the sun doesn't light them, and they cast no
shadows.
