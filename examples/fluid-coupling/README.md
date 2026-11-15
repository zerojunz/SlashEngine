# Fluid coupling

Fluids taking part in the rest of the world: soft bodies and cloth they land on and push, physics
fields that blow, hold and remove them, emitters riding any body, and visual effects that pour
them. See [fluids](../fluids/) for the solver itself.

[garden.scene.json](garden.scene.json) has:

- a hammock, a `SoftBody3D` sheet pinned at its corners, that water from a tap fills and sags
  under; the tap's `FluidEmitter3D` names no fluid, so it pours into the `Fluid3D` it is under
  (a hose in a hand, a creature's mouth or a vehicle's exhaust pours the same way),
- a steam vent, a `VisualEffect3D` whose [effect](effects/steam.vfx.json) has a layer holding its
  own `Fluid3D` and emitter: the emitter stops when the layer ends, and the part stays until its
  steam is gone, then frees the effect,
- a blower, a `directional_velocity` `PhysicsField3D` that carries the steam sideways, and a
  drain, a `kill` field that removes what reaches it.

[side.scene.json](side.scene.json) rains 2D water onto an awning, a `Cloth2D` held along its
left end, whose free end the rain weighs down.

[calls.jsonl](calls.jsonl) opens the garden, finds the effect's fluid part, steps until the
hammock is full, reads every fluid with `fluid.state` and the hammock's points with
`physics.soft_body`, steps past the steam's life and finds the part and the effect gone, draws
the frame, then rains on the awning and reads it. [results.jsonl](results.jsonl) holds the
replies.

- Soft bodies' and `Cloth3D`s' faces are a sheet as thick as their points' radius plus a
  centimeter; a `Cloth2D` is the outline of the triangles it draws. Each contact pushes the
  nearest face's or side's points back by their inverse masses.
- Fields act on each particle as a kilogram: forces add to gravity, velocities set it, `anchor`
  holds it and `kill` removes it. `particles: false` leaves fluids alone.
- `inherit_velocity` (1 by default) is the share of the emitter's own motion its particles take.
