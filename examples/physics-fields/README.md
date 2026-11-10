# Physics fields

One simulation for everything in a world, after Unreal's Chaos: bodies share physics materials,
and fields push, hold, break and remove whatever is in their reach, in 3D and 2D, particles
included.

A `.physics_material.json` file is Godot's `PhysicsMaterial` (`friction`, `rough`, `bounce`,
`absorbent`) with what Chaos's physical materials add: `density`, which weighs a rigid body by
its shapes' volume (in 2D, their area in square meters of 64 pixels), `strength`, which breaks a
`Destructible3D` whose `damage_threshold` is 0, and a `surface` name that ray hits report. A
body names one in `physics_material_override`, as Godot's do:

```json
"crate_low": {"components": {
  "RigidBody3D": {"physics_material_override": "materials/oak.physics_material.json"},
  "CollisionShape3D": {"shape": "box", "size": [0.6, 0.6, 0.6]}
}}
```

A `PhysicsField3D` (or `PhysicsField2D`) acts on what is in its sphere, box, the space below its
plane or everywhere, weakened by its falloff: forces out from it, along it or by noise,
velocities, strain, anchors, kill and sleep. [main.scene.json](main.scene.json) has two oak
crates on a stone floor, a blast beside them (off), a lantern an anchor holds in the air, a pit
that removes what falls below it, a glass window and a press of strain (off); in 2D, a puck
pushed along an ice rink by a draft and sparks a burst pushes apart.

[calls.jsonl](calls.jsonl) drives it as an agent does, and [results.jsonl](results.jsonl) holds
the replies:

- `physics.raycast` names the surface it hits: the crate's wood, the floor's stone and, in 2D,
  the puck's ice.
- After a second, the crates sit stacked, the lantern hangs where the anchor holds it, the puck
  slides along the rink and the sparks spread out from the burst.
- The blast goes on, the anchor and the press too: the press's strain is past the glass's
  strength, so the window breaks into four pieces, the blast throws the crates off the floor,
  the lantern falls, and the low crate falls into the pit, which removes it (`field_killed`).

`physics_3d` in [project.json](project.json) gives each tick two collision substeps
(`substeps`), as Chaos substeps.
