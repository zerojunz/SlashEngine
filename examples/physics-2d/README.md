# 2D physics

Bodies, shapes, areas and character movement on Box2D, as Godot's 2D physics.
[main.scene.json](main.scene.json) has ground and walls (`StaticBody2D`), a hill whose outline
is a concave `CollisionPolygon2D`, a one-way ledge, a crate (`RigidBody2D`), a lamp hanging from
a `PinJoint2D`, three coins (`Area2D`) and the hero (`CharacterBody2D`). [main.cpp](main.cpp)
adds the hero's controls as plain C++ systems, runs six seconds, then asks the physics commands
what is where, as an agent would. [expected.txt](expected.txt) holds its output.

```text
tick 60: hero at (125.4, 215.8), at a wall
tick 120: hero at (215.4, 170.0), on the floor
tick 180: hero at (303.9, 231.0), on the floor
tick 240: hero at (378.3, 170.3)
tick 300: hero at (420.3, 189.0), on the floor
physics.raycast {"exclude":["hero"],"from":[420,189],"to":[420,300]}
{"hit":{"fraction":0.1441,"id":"hill","normal":[0,-1],"point":[420,205]}}
```

- Lengths are pixels and y points down. Gravity is 980 pixels per second squared, as in Godot;
  `physics_2d` in `project.json` changes it. The `physics2d` system steps the world at priority
  950, after gameplay systems and keyframe animation.
- The hero moves as Godot's `move_and_slide`: `control_hero` sets its `velocity`, adding gravity
  in the air and a jump when `on_wall` says the way is blocked, as Godot's CharacterBody2D
  template script does. The physics2d system moves it, slides it along what it hits, snaps it
  to the floor and fills in `on_floor`, `on_wall`, `on_ceiling` and the floor's normal.
- The hero walks into the crate and jumps, up through the one-way ledge above it, and lands on
  the ledge. It walks off, climbs the hill's slope, jumps the step at its top and stops at the
  flag. A shape with `one_way_collision` stops only what comes down onto it.
- Collision layers and masks follow Godot: two bodies collide when either one's
  `collision_mask` has the other's `collision_layer`. The hero is on layer 2 and the coins
  watch only layer 2 (`collision_mask: 2`), so the crate and the lamp don't take coins.
- `CollisionShape2D` is a rectangle unless `shape` says `circle`, `capsule`, `segment` or
  `polygon`. `CollisionPolygon2D` takes any outline: the concave hill is split into convex
  pieces, and `build_mode: "segments"` would make only its outline solid.
- `take_coins` asks `physics2d::contacts` what each coin touches, as Godot's
  `get_overlapping_bodies`. Areas also emit `body_entered` and `body_exited` events, and a
  rigid body with `contact_monitor` reports what it hits the same way.
- `physics.raycast` returns the first hit's ID, point, normal and the fraction of the way it
  got. `physics.overlap` lists what is at a point or under a shape, sorted by ID, and
  `physics.contacts` what an entity touches. All three take `collision_mask`, `exclude` and
  whether to include bodies and areas, as Godot's query parameters.
- The pin joint lets the lamp swing but keeps it 40 pixels from the pin.
- The same scene and ticks give the same result on every platform, so this output is checked
  exactly on macOS, Windows and Linux.
