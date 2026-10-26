# Tileable materials

Walls, floors and roads are drawn with materials that repeat across them, and repeated textures
show their seams. `asset.generate` with `"type": "material"` makes a Godot `StandardMaterial3D`
that tiles: it generates an image asking for a seamless texture (or takes a library image as
`image`), measures how plainly its edges show where it wraps (`seam_ratio`), blends four copies
of it moved by half when they do, then works out a normal map and a roughness map from its
brightness, wrapping round the edges so they tile with it. The image, the three maps and the
`.standard_material.json` file naming them are kept in the asset library, and `path` copies them
into the project.

A `.standard_material.json` file is one `StandardMaterial3D` in a file of its own, as Godot
saves one as a `.tres` resource. Model recipes name it as a material, as it is or with fields
changed through `extends`, so every model drawn with the same stone shares one material and its
textures.

[main.cpp](main.cpp) uses the built-in `test` generator, whose textures are rows of stones of
random shades. [expected.txt](expected.txt) holds the output:

- The cobblestone image doesn't tile as drawn (`image_seam` 2.36), so it is made to (`seam`
  0.58), and the albedo, normal and roughness maps are copied beside the material file.
- A wall recipe names the material as it is; a steps recipe `extends` it with its texture tiling
  twice as often and has a mossy variant tinted green. `model.inspect` of `steps.model.json::mossy`
  shows the three textures and both changes.
- `asset.add` keeps both recipes, each with the material and its textures as library parts, and
  `asset.info` shows the material used by both and made from the generated image.
- A material asked for from a sound is refused at `/image`.

See [asset.generate](../../docs/reference/commands.md#tileable-materials) and
[material files](../../docs/reference/scene-format.md#material-files).
