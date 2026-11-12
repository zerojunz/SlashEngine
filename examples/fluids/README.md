# Fluids

Liquids and smoke as particles with no grid, after Niagara Fluids' particle simulations (Godot
has none). A `.fluid.json` recipe (`slash-cli schema fluid`) says what a fluid is; a `Fluid3D` or
`Fluid2D` steps one, and `FluidEmitter3D`/`2D` and `FluidSink3D`/`2D` pour into and drain it.

- [liquids/water.fluid.json](liquids/water.fluid.json) is water: position-based fluids (a
  density constraint each particle keeps with its neighbors, XSPH viscosity, cohesion for
  surface tension and vorticity confinement), with a budget `fluid.check` holds it to.
- [gases/fire.fluid.json](gases/fire.fluid.json) is a campfire: particles carrying heat, smoke
  and fuel that rise by buoyancy, burn above ignition into heat and smoke, cool, share heat with
  their neighbors and swirl round vortex particles.
- [liquids/water2d.fluid.json](liquids/water2d.fluid.json) is the same water seen from the side
  in pixels.

[calls.jsonl](calls.jsonl) checks the water on a stage of its own, drops a block of it beside a
crate in [yard.scene.json](yard.scene.json) (it splashes over the ground and shoves the crate),
lights the campfire with `fluid.pour`, reads both with `fluid.state` and draws the frame, then
splashes 2D water over a rock between two walls in [side.scene.json](side.scene.json) and checks the world's
fluids. [results.jsonl](results.jsonl) holds the replies.

- Particles find their neighbors along a Morton curve, never in a grid, and each step's work is
  split over the job pool by particle, so a fluid steps the same bits on any number of threads.
- Particles collide with 3D and 2D physics bodies (each a sphere or circle against the shapes
  near it) and push rigid bodies back with the impulse that took.
- They draw as discs facing the camera: a liquid's lit and whiter as foam where it runs fast, a
  gas's blended by its smoke and glowing toward the fire color as it burns.
