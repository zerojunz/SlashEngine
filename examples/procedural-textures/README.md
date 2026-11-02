# Procedural textures

Textures made from settings rather than painted, as Godot's `NoiseTexture2D`,
`GradientTexture1D` and `GradientTexture2D` resources. The project commits no images: its three
textures are `.texture.json` files in [textures/](textures/), made into pixels as they load, the
same on every machine.

- [water.texture.json](textures/water.texture.json) is cellular noise (Godot's `FastNoiseLite`)
  colored by a gradient from deep to shallow blue, made `seamless` so it tiles.
- [glow.texture.json](textures/glow.texture.json) is a radial gradient from warm yellow to
  clear orange.
- [dusk.texture.json](textures/dusk.texture.json) is a gradient one pixel high from night to
  sunset, blended in Oklab.

[main.scene.json](main.scene.json) names them where it would name `.png` files: the water and the
glow are `Sprite2D`s and the dusk sky is a `TextureRect` stretched across the top.

[calls.jsonl](calls.jsonl) tunes them the way an agent does, with no image to look at, and
[results.jsonl](results.jsonl) holds the replies:

- `texture.generate` with the water's `path` measures it: each channel's lowest, mean and
  highest byte, and `wrap`, how far apart neighboring pixels are inside the texture and across
  its edges when it repeats. The water's `x` and `y` are close to `inside`, so it tiles.
- The same noise inline, gray with no `color_ramp` and with `seamless` left out, jumps where it
  repeats: `x` and `y` are about three times `inside`. `seamless` is what makes it tile.
- `pixels` reads the glow's middle, edge and corner: nearly opaque yellow in the middle, clear
  orange at its radius and past it.
- `frame.capture` reads the water, the glow and both ends of the sky as the scene draws them.
- `texture.generate` with `out` writes a normal map of the water's noise to a PNG, for a
  `StandardMaterial3D`'s `normal_texture` or a person to look at.
- A misspelled `noise_type` fails at its JSON Pointer, listing the types there are.

`slash-cli scene validate textures/water.texture.json` checks a file and `slash-cli schema
texture` prints every setting.
