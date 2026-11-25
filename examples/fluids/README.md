# Fluids

Liquids and smoke as particles with no grid, after Niagara Fluids' particle simulations. A
`.fluid.json` recipe (`slash-cli schema fluid`) says what a fluid is; a `Fluid3D` or `Fluid2D` steps
one, and `FluidEmitter3D`/`2D` and `FluidSink3D`/`2D` pour into and drain it.

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
splashes 2D water over a rock between two walls in [side.scene.json](side.scene.json), checks
the world's fluids and previews each recipe on a stage of its own into `previews/`.
[results.jsonl](results.jsonl) holds the replies.

- Particles find their neighbors along a Morton curve, never in a grid, and each step's work is
  split over the job pool by particle, so a fluid steps the same bits on any number of threads.
- Particles collide with 3D and 2D physics bodies (each a sphere or circle against the shapes
  near it) and push rigid bodies back with the impulse that took.
- A liquid draws as one surface: its particles' spheres splatted into a depth image over the
  screen, smoothed with its edges kept, and meshed back in the world, lit, clearer where shallow
  and whiter as foam where it runs fast; drops flying apart draw on their own. In 2D, marching
  squares trace its outline, lighter at the surface.
- A gas draws as discs blended by their smoke, each lit by the sun through the smoke between it
  and the sun, and glowing toward the fire color as it burns.
