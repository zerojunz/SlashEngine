# GPU particles in 3D

Particles in 3D, as Godot 4.6's `GPUParticles3D` with its `ParticleProcessMaterial`. In
[main.scene.json](main.scene.json), sparks of additive quads rise from a sphere, cool from
yellow to a fading red and drift in turbulence; leaves made of thin boxes circle a ring about y
in the emitter's own space, spinning as they go; and blue streaks thrown up from a box stretch
along their velocity as they fly and fall. Each emitter's
[`.process_material.json`](sparks.process_material.json) says how its particles move.
[calls.jsonl](calls.jsonl) checks them with `particles.eval`, draws the scene with
`frame.capture`, turns the sparks off and adds an emitter with nothing to draw;
[results.jsonl](results.jsonl) holds the replies.

```text
particles.eval {"id":"streaks","time":2,"list":true,"max_results":1}
{"alive":29,"bounds":[1.6083,0.1352,-0.5898,1.1237,0.9726,0.9954],"particles":[
  {"age":0.9667,"angle":0,"color":[0.499,0.799,1,1],"position":[1.9999,0.8741,0.1567],
   "scale":[1,1.9798,1],"velocity":[-0.0819,-1.9348,0.2996]}]}
```

- Process materials are the same files `GPUParticles2D` reads, with Godot's 3D fields: vectors
  of three numbers (a 2D one's two keep the 3D default's z), the `ring` shape with
  `emission_ring_axis`, `emission_ring_height` and `emission_ring_cone_angle`, `flatness`,
  `particle_flag_rotate_y` and `particle_flag_disable_z`, directional velocity and scale over
  velocity. `scale_curve`, `scale_over_velocity_curve`, `orbit_velocity_curve` and
  `directional_velocity_curve` take one curve or one per axis (`{"x": ..., "y": ..., "z": ...}`),
  as Godot's `CurveXYZTexture`: the leaves orbit about y only and the streaks stretch in y only.
- Emission is stepped as Godot's node and particles storage step it: `amount` slots,
  `lifetime`, `explosiveness`, `randomness`, `preprocess`, `fixed_fps` with `interpolate`,
  `speed_scale`, `amount_ratio` and `one_shot`, which emits `finished`. Renderers run the steps
  the device hasn't and draw each draw pass's mesh at every live particle.
- `draw_pass_1` to `draw_pass_4` name model files to draw, with `draw_passes` saying how many;
  with none, the entity's primitive mesh component and `StandardMaterial3D` draw, as the three
  emitters here do. The sparks' material has `billboard_mode` `"particles"`, so each quad faces
  the camera, and `draw_order` `"view_depth"` draws the farthest first so their additive light
  blends in order. An emitter with nothing to draw is a problem in `frame.summary` and
  `scene validate`, as the last call shows.
- `transform_align` `"y_to_velocity"` points each streak's y along its velocity;
  `"z_billboard"` and `"z_billboard_y_to_velocity"` face the camera too.
- Particles are culled by `visibility_aabb_position` and `visibility_aabb_size`, a box in the
  emitter's space, as Godot's `visibility_aabb`: an emitter whose box is out of view isn't drawn.
- Random values come from the entity's ID and `seed` (or `seed` alone with `use_fixed_seed`),
  so the same scene makes the same particles on every platform. `particles.eval` runs the steps
  kept on the CPU, and with a `time` runs an emitter's settings from the start apart from the
  world.
- Setting `emitting` to false lets the live particles finish: 90 ticks later the sparks have
  none. Particles are not saved in the scene; snapshots, undo, dry runs and failed commands
  leave them as they are.
