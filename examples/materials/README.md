# Materials

Materials color a sprite's pixels, as Godot's canvas shaders do, written as a node graph in a
`.material.json` file. [main.scene.json](main.scene.json) has three sprites, each with its own
material: [hit_flash](hit_flash.material.json) mixes the hero toward white by a `flash` param,
[water](water.material.json) scrolls a second, repeating texture over the pond with the time,
and [glow](glow.material.json) only sets the blend mode, so the spark adds its light.
[main.cpp](main.cpp) writes the textures, checks colors with `material.eval` as an agent would,
then draws the scene into a texture.

- `"color"` is the pixel's color, a vec4, from ops (`mul`, `mix`, `texture`, `sin`,
  `smoothstep`, `rgb_to_hsv` and more) whose args are numbers, arrays, nodes written in place, or
  names: a node in `"nodes"`, a param, or an input (`uv`, `color`, `time`, `texture`,
  `texture_pixel_size`, `fragcoord`). `"base.rgb"` picks components. Name a value used twice in
  `"nodes"` and it is computed once.
- `"params"` are what each sprite sets in `Sprite2D`'s `material_params`; one material serves
  every sprite. Number params can have a `min` and `max`. Texture params name a file, which can
  `repeat` past its edges.
- `"blend"` is Godot's blend mode: `mix`, `add`, `sub`, `mul` or `premul_alpha`.
- `material.eval` returns the colors a material draws at texture coordinates, computed on the CPU
  as the GPU computes them, so an agent checks a material without a screenshot. Given an entity,
  it uses the sprite's texture, params and modulate; `params` and `time` try other values.
- Mistakes are found when the file loads, each with its JSON Pointer and the closest name:
  `slash-cli scene validate hit_flash.material.json` checks a file, and a sprite whose material
  or params are wrong is left out with one warning.
- Materials reload when their files change, like textures.
- The output only shows what is the same on every device; `tests/test_canvas.cpp` checks the
  drawn pixels on a GPU.
