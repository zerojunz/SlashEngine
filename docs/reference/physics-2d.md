# 2D physics

2D physics on Box2D.

Status: current · Updated: 2026-11-20

Parts of this area are ported from [Godot Engine](https://godotengine.org) (MIT); see [THIRD_PARTY.md](../../THIRD_PARTY.md).

`engine/physics2d` is 2D physics on Box2D v3. Bodies and areas are components: `StaticBody2D`,
`MovingBody2D`, `RigidBody2D`, `CharacterMover2D` and `Zone2D`, with `Collider2D` and
`PolygonCollider2D` shapes and `PinJoint2D`, `SpringJoint2D` and `TrackJoint2D` joints. The ECS
holds the truth: each tick the `physics2d` system (priority 950) brings Box2D's world up to date
with the components, steps it, writes rigid bodies back, moves character bodies with
`move_and_slide` (`src/character.cpp`) and emits `body_entered`, `body_exited`, `area_entered` and
`area_exited` events. Box2D's lengths are pixels (`b2SetLengthUnitsPerMeter(64)`), and SlashEngine's
own rules apply where Box2D differs: layers and masks collide when either mask matches, one-way
shapes, and waking what rests on a moved body. `project.json`'s `physics_2d` sets gravity and
damping, and `Zone2D`s with space overrides change them for the bodies inside, by priority
(`Space::forces` and `apply_areas` in `src/space.cpp`, applied as forces with Box2D's gravity scale
at 0). A `StaticBody2D` with constant velocities is a kinematic Box2D body moved back after each
step, so it carries what touches it without moving. `CharacterMover2D` slides along floors, walls
and platforms with its floor, wall, platform and `safe_margin` options. `physics.raycast`,
`physics.overlap`, `physics.contacts` and `physics.gravity` (and `physics2d::raycast`, `gravity` and
the rest in C++) ask what is where. Each `TileLayer2D` with collision polygons is a static body:
physics2d asks for its shapes through the `TileShapeSource` that `canvas::add_systems` sets
(`set_tile_shape_source`), so physics2d doesn't depend on canvas, and rebuilds the body when they
change. Results must be the same on every platform: `physics2d` and the examples build with
`-ffp-contract=off`, and angles go through Box2D's own `b2ComputeCosSin`, not libm.
`examples/physics-2d` has every kind of body and `examples/gravity-and-conveyors` gravity zones and
a conveyor.
