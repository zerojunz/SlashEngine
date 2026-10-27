# Volumetric fog 3D

A misty courtyard, as Godot 4.6's volumetric fog in its Forward+ renderer, the default
`rendering.rendering_method`. [main.scene.json](main.scene.json) is a floor, a wall and a pillar
seen through a `Camera3D` in a 160x90 window under a procedural sky, lit by a shadowed sun and an
orange lamp. The `WorldEnvironment` turns on `volumetric_fog_enabled` with a `density` of 0.06
over a `length` of 24 meters, and the lamp's `light_volumetric_fog_energy` of 4 makes it glow in
the mist. [project.json](project.json) makes the fog's volume 32 froxels across and 32 deep
(`rendering.environment.volumetric_fog`), smaller than Godot's 64 by 64, so the example draws
fast on the CPU.

[calls.jsonl](calls.jsonl) drives the scene the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[10,10],[45,25],[110,35],[80,85]],"frames":8,"path":"shots/misty.png"}
{"colors":[[0.533,0.525,0.502,1],[0.447,0.38,0.329,1],[0.871,0.569,0.373,1],
 [0.478,0.439,0.369,1]],"drawn":{"meshes":3},"height":90,"path":"shots/misty.png","width":160}
```

- The points read the sky, the wall in the pillar's shadow, the wall by the lamp and the floor
  near the camera. `frames` draws 8 frames and returns the last: each frame jitters the froxels
  and keeps 90% of the frame before's density (`volumetric_fog_temporal_reprojection_amount`),
  so the fog settles as Godot's does over its first frames.
- One `component.set` raises the density to 0.2 and tints the fog blue. The sky and the shadowed
  wall go greyer, since less of them gets through, and the lamp's glow spreads.
- Another turns the fog off. The wall's colors come back, the shadow is sharp, and the sky is
  clear.

The fog lights each froxel by the lights through their shadows, so the mist in the pillar's
shadow is darker than the mist in the sun. The sky is seen through the fog's whole length by
`volumetric_fog_sky_affect`. As in Godot, a `"color"` background is drawn without the fog, and
Godot's Compatibility renderer (`"rendering_method": "gl_compatibility"`, and always with the
OpenGL drivers) draws no volumetric fog.
