# 3D physics

Bodies, shapes, areas and character movement on Jolt Physics, as Godot 4.4's Jolt module.
[main.scene.json](main.scene.json) has a floor, a step, a ramp and a conveyor belt
(`StaticBody3D`), a ball, a parcel and three stacked crates (`RigidBody3D`), three coins
(`Area3D`) and the hero (`CharacterBody3D`). [main.cpp](main.cpp) adds the hero's controls as
plain C++ systems, runs six seconds, then asks the physics commands what is where, as an agent
would. [expected.txt](expected.txt) holds its output.

- Lengths are meters and y points up. Gravity is 9.8 meters per second squared, as in Godot;
  `physics_3d` in `project.json` changes it. The `physics3d` system steps the world at priority
  950, after gameplay systems and keyframe animation, beside 2D physics.
- The hero moves as Godot's `move_and_slide`, ported line for line onto Godot's Jolt motion
  tests: `control_hero` sets its `velocity`, adding gravity in the air and a jump when `on_wall`
  says the way is blocked. The physics3d system moves it, slides it along what it hits, snaps it
  to the floor and fills in `on_floor`, `on_wall`, `on_ceiling`, the floor's and wall's normals
  and the velocity of what it stands on.
- Collision layers and masks follow Godot: two bodies collide when either one's
  `collision_mask` has the other's `collision_layer`. The hero is on layer 2 and the coins
  watch only layer 2, so the crates and the ball don't take coins.
- `CollisionShape3D` is a box unless `shape` says `sphere`, `capsule`, `cylinder`,
  `convex_polygon`, `concave_polygon` or `world_boundary`. A body takes the shapes of child
  entities with no body of their own, as a Godot body takes its CollisionShape3D children.
- The belt's `constant_linear_velocity` carries what touches it without moving the belt, as
  Godot's: the parcel rides it to the far end and drops onto the floor. The parcel has
  `contact_monitor`, so it reports `body_entered` when it lands on the belt and on the floor.
- `take_coins` asks `physics3d::contacts` what each coin touches, as Godot's
  `get_overlapping_bodies`. Areas also emit `body_entered` and `body_exited` events.
- `physics.raycast`, `physics.overlap`, `physics.contacts` and `physics.gravity` are the 2D
  commands: points of three numbers, a `CollisionShape3D` shape or a 3D body make them ask 3D
  physics.
- Jolt is built cross-platform deterministic and steps on one thread, so this output is checked
  exactly on macOS, Windows and Linux.
