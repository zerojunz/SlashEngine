# Gravity and conveyors

Gravity zones and conveyor belts, as Godot's `Area2D` space overrides and `StaticBody2D` constant
velocities. [main.scene.json](main.scene.json) has a factory with a conveyor belt, a crate and the
hero, and a patch of space with a planet, two rocks and a probe. [main.cpp](main.cpp) adds the
hero's controls as a plain C++ system, runs three seconds, asks `physics.gravity` what pulls each
body, then turns space off. [expected.txt](expected.txt) holds its output.

```text
physics.gravity {"ids":["hero","rock_below","probe"]}
{"bodies":{"hero":{"angular_damp":1,"gravity":[0,980],"linear_damp":0.1},"probe":{"angular_damp":1,
"areas":["space"],"gravity":[0,0],"linear_damp":0.1},"rock_below":{"angular_damp":1,
"areas":["planet_pull"],"gravity":[0,-440.894],"linear_damp":0.1}}}
```

- An `Area2D` changes gravity and damping for the bodies inside it through its
  `gravity_space_override`, `linear_damp_space_override` and `angular_damp_space_override`:
  `combine` adds its value, `replace` sets it and ignores areas of lower priority and the
  project's, and `combine_replace` and `replace_combine` mix the two, as in Godot. Areas apply
  from the highest `priority` down. Here `space` replaces gravity with none, and `planet_pull`,
  of priority 1, replaces it with a pull toward the planet first.
- With `gravity_point`, an area pulls toward `gravity_point_center` in its own space instead of
  along `gravity_direction`. `gravity_point_unit_distance` makes the pull weaken with distance
  squared: `gravity` at that distance, a quarter of it at twice that. The rocks beside and below
  the planet fall sideways and up onto its surface and stay there; the probe, outside the pull,
  floats until space is turned off.
- Overrides apply to the bodies in the area's `collision_mask`, whether or not it is
  `monitoring`, from the step after a body enters, as in Godot. So on the first step every body
  falls with the project's gravity, which is why the probe drifts slowly down through space.
- `physics.gravity` (`physics2d::gravity` in C++, Godot's `get_gravity`) returns the gravity and
  damping each body gets and the areas that set them. The hero's controls add that gravity to
  its velocity, so a character feels zones as rigid bodies do.
- A `StaticBody2D` with `constant_linear_velocity` or `constant_angular_velocity` doesn't move,
  but what touches it moves as if it did: the belt carries the crate off its end, and the hero,
  walking left at the belt's speed, stays in place. `platform_velocity` says how fast the floor
  carries the hero. An `AnimatableBody2D` adds its constant velocities to how it moves.
