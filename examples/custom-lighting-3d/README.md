# Custom lighting 3D

A spatial shader's `light()`, as Godot's: it lights a mesh once for each light, in place of the
engine's light model. [main.scene.json](main.scene.json) has two red balls under a sun and an
orange lamp:

- the [toon](toon.material.json) ball is cel-shaded by [shaders/toon.hlsl](shaders/toon.hlsl),
  whose `light()` cuts each light's diffuse light into `steps` flat bands, adds a sharp highlight
  and a rim of sunlight at its edge;
- the smooth ball has a `StandardMaterial3D` of the same color, lit by the engine's own light
  model, for comparison.

```hlsl
void light() {
    float lit = saturate(dot(NORMAL, LIGHT)) * ATTENUATION;
    float stepped = floor(lit * steps + 0.5) / steps;
    DIFFUSE_LIGHT += stepped * LIGHT_COLOR / PI;
    float3 half_way = normalize(LIGHT + VIEW);
    SPECULAR_LIGHT += step(0.97, dot(NORMAL, half_way)) * ATTENUATION * LIGHT_COLOR *
                      SPECULAR_AMOUNT * 0.25;
}
```

[calls.jsonl](calls.jsonl) checks it as an agent would, with `frame.capture` at points on both
balls:

- With 2 steps, two points on the toon ball's lit side have the same color, and so do two in its
  shade, where the same points on the smooth ball all differ.
- `component.set` of the toon ball's `material_params` to 4 steps splits each pair into two bands.
- Turning the lamp off with `light_energy` 0 takes away the orange band its light gave the toon
  ball's right side.

- `NORMAL`, `LIGHT` and `VIEW` are in view space, and `ATTENUATION` holds the light's falloff,
  a spot light's cone and the shadows that fall on the pixel.
- The engine multiplies `DIFFUSE_LIGHT` by `ALBEDO`, then adds the ambient light, reflections and
  `EMISSION`, which `light()` doesn't touch.
- [Spatial shaders](../../docs/reference/scene-format.md#spatial-shaders) in the scene format
  reference lists `light()`'s built-ins.
