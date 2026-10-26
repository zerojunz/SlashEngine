# 3D particles

Particles in 3D, as Godot 4.6's `CPUParticles3D`. In [main.scene.json](main.scene.json), a
fountain sprays small spheres that arc up and fall back, a fire's quads always face the camera
and burn from yellow to a dark red as they rise, and snow made of little boxes drifts down
through the whole view, already falling when the scene opens. [calls.jsonl](calls.jsonl) checks
them with `particles.eval`, draws the scene with `frame.capture`, turns the fountain off and adds
an emitter with nothing to draw; [results.jsonl](results.jsonl) holds the replies.

```text
particles.eval {"id":"fountain","list":true,"max_results":2}
{"alive":45,"bounds":[-2.7396,0.0276,-0.419,0.9195,0.9621,0.8001],"particles":[
  {"age":0.6667,"angle":0,"color":[0.55,0.8,1,1],"position":[-2.2196,0.7629,0.3811],
   "scale":[0.6667,0.6667,0.6667],"velocity":[-0.0294,-2.0406,0.5717]}, ...]}
```

- Each particle is drawn as the emitter's own mesh: a primitive mesh component on the entity
  (the fountain's `SphereMesh`, the fire's `QuadMesh`, the snow's `BoxMesh`) or a model file in
  `mesh`, with its `StandardMaterial3D`. An emitter with neither is a problem in `frame.summary`
  and `scene validate`, as the last call shows.
- Emission works as `CPUParticles2D`'s: `amount` slots, each emitted once every `lifetime`,
  with `explosiveness`, `randomness` and `preprocess`. Shapes are 3D (`point`, `sphere`,
  `sphere_surface`, `box`, `points`, `directed_points` and `ring` with its axis, height and cone
  angle), `spread` is a cone round `direction` that `flatness` squashes, and `gravity` is a
  vector in meters per second squared.
- The fire's material has `billboard_mode` `"particles"`, so each quad faces the camera and
  turns by its particle's `angle`; `"enabled"` faces the camera and `"fixed_y"` turns only about
  y. `particles_anim_h_frames` and `particles_anim_v_frames` play a sprite sheet over each
  particle's life. With `vertex_color_use_as_albedo`, `color` and `color_ramp` color it.
- `draw_order` `"view_depth"` draws the farthest particles first, so the fire's additive quads
  blend in order; `"lifetime"` draws the oldest first. `particle_flag_rotate_y` spins the snow's
  boxes about y, and `particle_flag_align_y` points a particle's y along its velocity.
- Random values come from the entity's ID and `seed`, so the same scene makes the same particles
  on every platform and any number of threads. `particles.eval` with a `time` runs an emitter's
  settings from the start apart from the world.
- Setting `emitting` to false lets the live particles finish: 90 ticks later the fountain has
  none. A `one_shot` emitter emits `finished` when its last particle dies.
- Particles are not saved in the scene. Snapshots, undo, dry runs and failed commands leave them
  as they are.
