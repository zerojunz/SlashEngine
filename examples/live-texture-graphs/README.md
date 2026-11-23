# Live texture graphs

A texture graph worked out as it draws, for textures that move. [lava.material.json](lava.material.json)'s
`"shader"` names [textures/lava.texture.json](textures/lava.texture.json) in place of an `.hlsl`
file: noise warped through a hot ramp, under a crust of bricks. The material's params move the
graph's parameters of the same names, and its `time` parameter follows the shader's `TIME`, so
`"offset": ["=time * 6", 0, 0]` makes the noise flow. [calls.jsonl](calls.jsonl) checks it as an
agent would:

1. `material.eval` reads three pixels of the lava at rest,
2. then two seconds on, where the noise has moved,
3. then with `glow` set blue, as `material_params` or an animation would set it.
4. `texture.edit` makes the flow's offset `time * time`,
5. which `material.eval` refuses: a moved parameter must move each setting in a straight line,
   so the shader can work it out per pixel.
6. Another edit puts it back.

- The graph compiles to a shader with a function per node, each node's op and size fixed, which
  compiles, exports and reloads as any shader does. Each output is filtered as a texture of the
  graph's size, so at texel centers it draws what the graph bakes to.
- A canvas material multiplies `COLOR` by the first output; a spatial one sets `ALBEDO`,
  `NORMAL_MAP`, `ROUGHNESS`, `METALLIC`, `AO`, `EMISSION` and `ALPHA` from outputs of those
  names.
- Every pixel works its nodes out again, so wide blurs cost too much and are refused; bake those.
- [Live texture graphs](../../docs/reference/scene-format.md#live-texture-graphs) in the scene
  format reference has the rules.
