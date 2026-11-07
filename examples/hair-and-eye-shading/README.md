# Hair and eye shading

Godot's `StandardMaterial3D` rim, clearcoat and anisotropy, which it shades skin, eyes and hair
cards with. [main.scene.json](main.scene.json) has three spheres under a sun and a procedural sky,
seen in a 160x90 window ([project.json](project.json)):

- `cheek` has a rim (`rim_enabled`, `rim`, `rim_tint`): each light also lights the edges that
  face away from the camera, part way toward the skin's own color, as skin catches light.
- `eye` has a clearcoat (`clearcoat_enabled`, `clearcoat`, `clearcoat_roughness`): a glossy coat
  over a matte surface, with its own sharp highlight and its own reflection of the sky, as the wet
  layer of an eye.
- `hair` has anisotropy (`anisotropy_enabled`, `anisotropy`): its highlight stretches along the
  sphere's tangents, which run around it, as hair cards and brushed metal shine.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[22,45],[39,45],[83,40],[125,40],[125,34]]}
{"colors":[[0.698,0.498,0.467,1],[0.855,0.62,0.561,1],[1,1,1,1],[0.663,0.616,0.608,1],
 [0.361,0.243,0.216,1]],"drawn":{"meshes":3},"height":90,"width":160}
```

- `frame.capture` reads the cheek's edge and middle, the eye's highlight, and the hair's highlight
  and a pixel above it.
- `component.set` turns the cheek's rim and the eye's clearcoat off and turns the hair's
  anisotropy to -0.8. The second `frame.capture` sees the cheek's edge darken while its middle
  stays the same, the eye's highlight go from white to its blue, and the hair's highlight turn to
  run up and down the sphere: dimmer where it was, brighter above.

The textures Godot also takes for these (`rim_texture`, `clearcoat_texture`,
`anisotropy_flowmap`) aren't read yet.
