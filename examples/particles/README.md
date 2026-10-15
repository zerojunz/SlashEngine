# Particles

Particles, as Godot's `CPUParticles2D`. In [main.scene.json](main.scene.json), a fountain sprays
dots that shrink and fade as they fall, a one-shot burst of sparks goes off once in every
direction, smoke rises from a sphere and was already going when the scene opened, and a portal's
particles orbit in the entity's own coordinates. [main.cpp](main.cpp) makes the dot texture,
checks the particles with `particles.eval`, hears the sparks finish and sets them off again, then
draws the scene into a texture. [expected.txt](expected.txt) holds its output.

```text
particles.eval {"id":"sparks","list":true,"max_results":2}
{"alive":24,"bounds":[125.0635,58.4695,113.0968,114.781],"particles":[
  {"age":0.5,"color":[1,0.325,0.0833,0.1667],"position":[238.1602,134.3399],"rotation":-0.694,"scale":2.262},
  {"age":0.5,"color":[1,0.325,0.0833,0.1667],"position":[125.0635,136.8566],"rotation":0.6476,"scale":2.0231}]}
```

- Each emitter has `amount` slots and emits from each once every `lifetime` seconds, spread
  evenly over the cycle; `explosiveness` 1 emits them all at once and `randomness` moves each
  later by up to its share. A particle starts where `emission_shape` says (`point`, `sphere`,
  `sphere_surface`, `rectangle`, `ring`, `points` or `directed_points`) and heads along
  `direction`, up to `spread` degrees either side, at a speed between `initial_velocity_min` and
  `initial_velocity_max`.
- Each other `_min` / `_max` pair (`angle`, `angular_velocity`, `orbit_velocity`, `linear_accel`,
  `radial_accel`, `tangential_accel`, `damping`, `scale_amount`, `hue_variation`, `anim_speed`,
  `anim_offset`) gives each particle its own value between the two, times its `_curve` over the
  particle's life: points `[offset, value]` with straight lines between them. The fountain's
  `scale_amount_curve` shrinks its dots to 0.3 of their size by the end.
- `color` times `color_ramp`, a gradient over each particle's life, colors it; the sparks go
  from yellow to a transparent red. `color_initial_ramp` gives each particle a color from a
  gradient when it is emitted.
- Random values come from the entity's ID and `seed`, so the same scene makes the same particles
  every run, on every platform, and an agent can check them as numbers: `particles.eval` with a
  `time` runs the settings from the start apart from the world, and gives what that many ticks
  give.
- `one_shot` emitters run one cycle, then set `emitting` to false, and emit a `finished` event
  when their last particle dies. Setting `emitting` to true sets them off again, with new random
  values.
- `preprocess` runs that many seconds before the first tick, so the smoke is already rising.
  `draw_order` `"lifetime"` draws the oldest first, so the newest puffs draw on top.
- With `local_coords`, particles live in the entity's coordinates and move with it, as the
  portal's do; otherwise they are emitted through the entity's transform and stay in the world.
- Particles are not saved in the scene. Snapshots, undo, dry runs and failed commands leave them
  as they are.
