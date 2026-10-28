# Model delighting

A scanned or generated model arrives with the light it was captured under painted into its albedo
texture: the top of a scanned crate is bright and its underside dark, and in a game's own lights it
looks lit twice. `model.cleanup` with `delight` takes that light back out, as Unity's De-Lighting
tool does for scans:

1. Each texel the model's surfaces cover is placed on the model, with the way it faces.
2. How much of the sky each place sees is traced against the model itself (16 rays, as far as half
   the model's size), so its own corners count as shade.
3. The light that best explains the texture, an ambient part and a direction, is fitted to the
   texels' brightness by least squares, and each texel is divided by it, so the faces even out and
   the texture's own pattern stays. `light` in the reply is that direction, in the cleaned model's
   space.

`delight` is how much to take out, from 0 to 1: 1 for scans and generated models lit in their
textures, less for a texture whose shading is partly the artist's. A texture lit evenly, or on
surfaces that all face one way, is left as it is.

[main.cpp](main.cpp) writes a crate whose texture is a gray checker lit from above and to the
right, then cleans it:

```json
{"path": "source/crate.obj", "out": "props/crate.glb", "delight": 1}
{"delit": 1, "light": [0.2859, 0.9383, 0.1944], "out": "props/crate.glb", ...}
```

The top went from 176 to 144 and the underside from 100 to 145: every face is within a color
code of the others, and each keeps its checker. The light it found is within a degree of the one
the texture was lit by. A texture that can't be read is named, at `/delight`.

Run it with `example-model-delighting`; [expected.txt](expected.txt) holds its output.
