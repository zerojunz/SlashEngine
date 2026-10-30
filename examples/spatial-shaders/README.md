# Spatial shaders

A game's own 3D shaders, as Godot's `shader_type spatial`. A `.material.json` file with
`"shader_type": "spatial"` names an `.hlsl` file whose `vertex()` moves a mesh's corners and whose
`fragment()` gives its surface, and a `MeshInstance3D`'s `material_override` draws it. [main.scene.json](main.scene.json) has three:

- the [crystal](crystal.material.json) is tinted and glossy, and its edges glow brighter and
  dimmer with the time ([shaders/crystal.hlsl](shaders/crystal.hlsl));
- the [fence](fence.material.json) cuts gaps between its slats by writing `ALPHA` and
  `ALPHA_SCISSOR_THRESHOLD`, seen from both sides with the `cull_disabled` render mode, and
  leans in the wind as its `vertex()` moves its top ([shaders/fence.hlsl](shaders/fence.hlsl));
  its shadow has the same gaps and leans with it;
- the [glass](glass.material.json) ball writes `ALPHA`, so the ground shows through it
  ([shaders/glass.hlsl](shaders/glass.hlsl)).

```hlsl
void fragment() {
    ALBEDO = tint.rgb;
    ROUGHNESS = 0.15;
    float rim = pow(1.0 - saturate(dot(NORMAL, VIEW)), 2.0);
    EMISSION = glow.rgb * rim * (0.5 + 0.5 * sin(TIME * speed));
}
```

[calls.jsonl](calls.jsonl) checks them as an agent would: `frame.capture` at two times shows the
crystal's edge glow change, a point through a gap in the fence shows the ground behind it until
the wind moves a slat over it, two points on the ground swap between a slat's shadow and the
sun as the shadow leans, and `component.set` on the crystal's `material_params` recolors it.

- The engine lights the surface `fragment()` gives with every light, shadow, reflection and
  post effect, as it lights a `StandardMaterial3D`. Godot's built-ins are in view space.
- `render_mode` is Godot's render mode line: `blend_*`, `cull_*`, `unshaded`, `fog_disabled`,
  `shadows_disabled` and `world_vertex_coords`.
- `vertex()` runs in the shadow pass and in the motion prepass too, so shadows, TAA and FSR 2
  follow what it moves.
- [Spatial shaders](../../docs/reference/scene-format.md#spatial-shaders) in the scene format
  reference lists the built-ins and limits.
