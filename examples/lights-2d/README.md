# 2D lights

Lights, shadows and normal maps, as Godot's 2D lighting. In [main.scene.json](main.scene.json),
a `CanvasModulate` darkens everything to a night blue, a torch (`PointLight2D`) and the moon
(`DirectionalLight2D`) light it, the crate's `LightOccluder2D` casts a shadow from the torch and
its `normal_texture` bevels its edges, and the neon sign's [material](neon.material.json) is
`"light_mode": "unshaded"`, so it glows at full brightness. [main.cpp](main.cpp) makes the
textures, asks `light.sample` how lit some points are, changes the torch's color and asks again,
then draws the scene into a texture. [expected.txt](expected.txt) holds its output.

```text
light.sample {"points":[[96,60],[200,90],[300,170]]}
{"ambient":[0.12,0.12,0.2],"lit":true,"points":[
  {"color":[1.244,1.02,0.9],"lights":[{"color":[0.125,0.15,0.25],"id":"moon"},
                                      {"color":[0.999,0.75,0.45],"id":"torch","shadow":0}]},
  {"color":[0.245,0.27,0.45],"lights":[{"color":[0.125,0.15,0.25],"id":"moon"},
                                       {"color":[0,0,0],"id":"torch","shadow":1}]},
  {"color":[0.245,0.27,0.45],"lights":[{"color":[0.125,0.15,0.25],"id":"moon"}]}]}
```

- Draws are unshaded until the scene has an enabled light or a `CanvasModulate`. Then sprites
  and tile layers start from the `CanvasModulate` color (the ambient light) and each light adds
  its color on top, as `blend_mode` says: `add`, `sub` or `mix`.
- A `PointLight2D` shines through its `texture`, centered on it and `texture_scale` times as
  big; with none it uses a round light 256 pixels across that fades to its edge. Its `color`
  times its `energy` is what it adds where the texture is white. A `DirectionalLight2D` lights
  everything from its `Transform2D` rotation: straight down the screen at 0.
- `light.sample` returns the color a white pixel gets at each point, as the GPU shades it, and
  each light that reaches it with its color there (times its alpha). A light with
  `shadow_enabled` also says how much of it occluders hide, from 0 to 1. The second point is
  behind the crate from the torch, so the torch adds nothing there; the third is past the
  torch's reach, so only the moon lights it.
- `light.sample` with an `id` samples at that entity, with its sprite's `light_mask` and
  `z_index`, so one call answers "is the hero in the dark?". A light reaches draws whose
  `light_mask` shares a bit with its `range_item_cull_mask` and whose `z_index` is within its
  `range_z_min` and `range_z_max`.
- Occluders cast shadows only from lights with `shadow_enabled`, when the occluder's
  `occluder_light_mask` shares a bit with the light's `shadow_item_cull_mask`. `shadow_filter`
  `pcf5` or `pcf13` softens the edges, and `shadow_color` is what shadowed pixels get instead
  of the light.
- The crate's polygon runs clockwise on screen, and its `cull_mode` is `counter_clockwise`: the
  edges facing the torch are left out, so the shadow starts at the crate's far side and the
  crate's own sprite is lit (`light.sample` with `"id": "crate"`). With `disabled`, the crate
  would sit in its own shadow, as in Godot.
- A normal map makes each pixel face its own way, so light falls on it by its angle. The crate's
  rim faces outward, so its side toward the torch is bright and the far side is dark. Normal
  maps are OpenGL style, as Godot reads them: green points up. `specular_texture` adds
  highlights.
- `slash-cli scene validate main.scene.json` checks the lights' fields, such as `blend_mode` and
  `shadow_filter`. A light that still can't shine, such as one whose texture is missing, is left
  out of the frame with a warning, and `light.sample` lists it under `warnings`.
