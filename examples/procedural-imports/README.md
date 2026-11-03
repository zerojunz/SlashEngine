# Import procedural content

Procedural content made in other tools comes in as files the engine makes the same way.

[godot/pond.tscn](godot/pond.tscn) is a Godot 4 scene whose water is a `NoiseTexture2D` (a
`FastNoiseLite` with a color ramp, made seamless) and whose moon glow is a `GradientTexture2D`.
`project.import` writes each as a [`.texture.json`](../../docs/reference/scene-format.md#procedural-textures)
file beside the scene, which makes the same pixels when it loads, so the water still tiles and an
agent tunes it with `texture.generate`:

```text
project.import {"source":"godot"}
{"converted": {"procedural_textures": 2, "scenes": 1, ...}, "files": 5, ...}
texture.generate {"path":"godot/pond_noise_texture2d_water.texture.json"}
{"width": 64, "height": 64, "wrap": {"inside": 3.44, "x": 2.77, "y": 2.59}, ...}
```

Terrains come from World Machine, Gaea or World Creator as their heightmap exports: a 16-bit PNG,
`.r16` or `.raw` file, a `.r32` file of floats, or an EXR of meters, with splat and color maps as
PNGs. [main.cpp](main.cpp) writes a valley's `.r16` file and splat map as such a tool would, then
`terrain.import` sizes the `HTerrain` to the file as HTerrain's import dialog does (the smaller
side's next power of two plus one, here 129), maps the values from `min_height` to `max_height`
and writes the terrain's own maps:

```text
terrain.import {"id":"valley","heightmap":"terrain/valley.r16","min_height":-5,"max_height":120,
                "splatmap":"terrain/valley_splat.png"}
{"files": ["terrain/valley.height.png", "terrain/valley.splat.png"], "height_range": [-5, 120],
 "resolution": 129, ...}
```

[expected.txt](expected.txt) holds every reply and the files written.
