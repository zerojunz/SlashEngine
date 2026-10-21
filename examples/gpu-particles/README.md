# GPU particles

Particles run on the GPU, as Godot's `GPUParticles2D`. In [main.scene.json](main.scene.json), a
fire rises from a circle and fades from yellow to dark red, a one-shot burst of sparks goes off
the same way each time, snow was already falling when the scene opened, smoke curls as it rises
and rain lands on a ledge. How each one's particles start and move is a `.process_material.json`
file, Godot's `ParticleProcessMaterial` in 2D: [fire.process_material.json](fire.process_material.json),
[sparks.process_material.json](sparks.process_material.json),
[snow.process_material.json](snow.process_material.json),
[smoke.process_material.json](smoke.process_material.json) and
[rain.process_material.json](rain.process_material.json). [main.cpp](main.cpp) makes the dot
texture, checks the particles with `particles.eval` and `frame.capture`, hears the sparks finish
and sets them off again, then draws the scene. [expected.txt](expected.txt) holds its output.

```text
particles.eval {"id":"fire","time":0.5}
{"alive":24,"bounds":[74.5837,113.1188,14.8968,37.7536]}
step {"ticks":30}
{"tick":30,"ticks":30}
particles.eval {"id":"fire"}
{"alive":24,"bounds":[74.5837,113.1188,14.8968,37.7536]}
```

- The component holds what Godot's node holds: `amount`, `lifetime`, `one_shot`,
  `explosiveness`, `randomness`, `preprocess`, `speed_scale`, `fixed_fps`, `interpolate`,
  `local_coords`, `draw_order`, `texture` and its `hframes` and `vframes` for animated particles.
  The process material holds the rest, with Godot's property names: the `emission_shape`
  (`point`, `sphere`, `sphere_surface`, `box`, `points`, `directed_points`, `ring`),
  `direction`, `spread`, `gravity` (`[0, 98]` unless given, as in a 2D game made with Godot),
  each parameter's `_min`, `_max` and `_curve`, `color`, `color_ramp`, `color_initial_ramp`,
  `alpha_curve`, `emission_curve`, `velocity_limit_curve`, and the `turbulence_` and
  `collision_` settings. Angles are in degrees.
- Turbulence turns each particle's velocity along curl noise that moves with
  `turbulence_noise_speed`, by its `turbulence_influence`, and moves it by its
  `turbulence_initial_displacement` where it starts, as Godot's. The noise's gradients come from
  an integer hash, so every device makes the same noise. `turbulence_noise_scale` is Godot's
  slider, whose noise is sized for 3D meters: in pixels it wants to be close to 10 (the smoke's
  9.8 makes swirls about 50 pixels across), and its `velocity_limit_curve` keeps turbulence from
  speeding particles up as it turns them.
- With a `collision_mode`, particles collide with `LightOccluder2D` outlines (those with
  `sdf_collision`, as by default) and tile occluders on occlusion layers with `sdf_collision`, as
  Godot's collide with the distance field it draws from them: `rigid` ones bounce off by
  `collision_bounce` and slide with `collision_friction`, `hide_on_contact` ones die. A particle
  touches a shape when it comes within the emitter's `collision_base_size` of it, times half its
  texture's average side: 4 pixels for the rain's 8-pixel dot. The shapes are measured exactly,
  so every device and the CPU agree; `particles.eval` with a `time` runs apart from the world, with
  nothing to collide with.
- Each tick the `gpu_particles_2d` system works out each emitter's steps as Godot does: one each
  1/`fixed_fps` seconds (or one a tick with `fixed_fps` 0), with preprocess and the one-shot
  timing. The renderer runs the steps on the device and draws the particles there. On Metal,
  Direct3D 12 and Vulkan a compute shader runs them, so the CPU's work is the same for ten
  particles or a hundred thousand; web exports run them on the CPU, since WebGL 2 has no
  compute shaders.
- The steps make the same particles on every device, and on the CPU: `particles.eval` runs the
  steps kept on the CPU to list them, and with a `time` runs the settings from the start, apart
  from the world. A renderer that starts late, or misses frames, catches up from the steps kept.
- Random values come from `seed` with `use_fixed_seed`, the same each run and each burst;
  otherwise from the entity's ID and `seed`, varied by each one-shot restart.
- A `one_shot` emitter stops emitting after one `lifetime` and emits `finished` once its
  particles have had time to die: `lifetime` times 2 - `explosiveness` after it started.
  Setting `emitting` sets it off again.
- `slash-cli scene validate` checks a `.process_material.json` file and reports every mistake
  with its JSON Pointer; exports check every process material.
- Godot projects import with their `GPUParticles2D` nodes and `ParticleProcessMaterial`
  resources (`slash-cli import`), turbulence and collisions included; sub-emitters, trails and
  attractors are reported as not converted, since SlashEngine doesn't have them yet.
- Particles are not saved in the scene. Snapshots, undo, dry runs and failed commands leave them
  as they are.
