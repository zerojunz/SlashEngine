# Shadows 3D

3D lights casting shadows, as Godot's do. [main.scene.json](main.scene.json) is a courtyard with
a crate, a pillar and a ball, seen through a `Camera3D` in a 160x90 window:

- `sun` is a `DirectionalLight3D` with `shadow_enabled`. Its shadow map comes in Godot's default
  four cascades, sharper near the camera, so the pillar and the crate cast long shadows.
- `lamp` is an orange `OmniLight3D` with `shadow_enabled`. Its shadow map is a cube around it, so
  the ball shades the ground in front of it.
- [project.json](project.json) sets `rendering.lights_and_shadows`: a 2048-texel atlas for the
  sun's cascades with soft edges, and a 1024-texel atlas with hard edges for the lamp.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[87,37],[72,46],[95,57],[95,63],[37,75]]}
{"colors":[[0.384,0.318,0.31,1],[0.412,0.333,0.314,1],[0.682,0.635,0.553,1],
 [0.827,0.706,0.573,1],[0.678,0.631,0.549,1]],"drawn":{"meshes":4},"height":90,"width":160}
```

- `frame.capture` draws the frame on the CPU, shadow maps included, so it needs no GPU and gives
  the same pixels on every machine. `points` reads the pillar's shadow and the crate's, both
  dark; the ball's shadow from the lamp, still lit by the sun; the lamp's light beside it; and
  the ground in the sun.
- `component.set` halves the sun's `shadow_opacity`, and its shadows lighten.
- Setting the crate's `cast_shadow` to `"shadows_only"` hides the crate, but its shadow stays.
  `drawn` still counts it, as it is drawn into the shadow maps.
- Turning off the lamp's `shadow_enabled` lights the ground behind the ball.
- `entity.create` adds `flashlight`, a `SpotLight3D` with a shadow, aimed at the pillar from the
  front left. The hidden crate stands in its beam, so the pillar's foot is darker than its middle.
  `path` saves the frame as `shots/courtyard.png`.
