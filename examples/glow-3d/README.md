# Glow 3D

Glow from colors brighter than white, as Godot 4.6's Forward+ renderer makes it, which is the
default `rendering.rendering_method`. [main.scene.json](main.scene.json) is a wall on a street at
night with two neon signs, seen through a `Camera3D` in a 320x180 window. The pink sign's
`emission_energy_multiplier` is 6 and the cyan one's 4, so they shine six and four times white.

Forward+ draws the world linear into a texture of half floats, so those colors stay above 1
until the tonemap. The `environment` entity's `WorldEnvironment` sets `tonemap_mode` `"agx"` and
`glow_enabled` with `glow_intensity` 0.8; everything else is Godot's default:

- `glow_hdr_threshold` 1: only what is brighter than white glows, and nothing else on the street
  is, so the wall and ground stay dark.
- `glow_levels` `[0, 0.8, 0.4, 0.1, 0, 0, 0]`: the blur is drawn into textures of half, a quarter
  and so on of the window's size, and the second, third and fourth are added, the softer ones
  less.
- `glow_blend_mode` `"screen"`.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[130,60],[130,45],[200,70],[215,70],[20,160]]}
{"colors":[[1,0.988,1,1],[0.941,0.537,0.733,1],[0.976,1,1,1],[0.478,0.749,0.792,1],
 [0.004,0.004,0.012,1]],"drawn":{"meshes":4},"height":180,"width":320}
```

- `frame.capture` draws the frame on the CPU, glow included, so it needs no GPU and gives the
  same pixels on every machine. `points` reads the pink sign, the wall just above it, the cyan
  sign, the wall beside it and the ground in a corner. Both signs burn nearly white, as bright
  neon does through AgX, and their glow colors the wall around them.
- `"softlight"` blends glow in after the tonemap, at most as bright as white, so the halos dim.
- Back to `"screen"`, `glow_levels` of only the sixth level (a 64th of the window's size) spreads
  the glow across the whole street as a haze, dimmer by the signs and brighter far from them.
  `path` saves the frame as `shots/soft.png`.
- A `glow_hdr_threshold` of 8 is above both signs, so nothing glows: the wall above the pink
  sign keeps only the light of the lamp beside it.

The same settings in Godot's Compatibility renderer (`"rendering_method": "gl_compatibility"`,
and always with the OpenGL drivers) glow nowhere: its colors stop at 1, so it needs a threshold
below 1. [post-processing-3d](../post-processing-3d/) shows that renderer.
