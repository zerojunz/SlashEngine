# Particle attractors in 3D

Attractors pull `GpuEmitter3D`s' particles. In [main.scene.json](main.scene.json) a sheet of
dust hangs over a 6 by 6 meter square: a sphere, the well, draws the dust near it in; a box, the
gust, blows the dust inside it along its -z; and a vector field, the swirl, turns the dust
inside it round the y axis by a 3D texture of directions. [main.cpp](main.cpp) makes the swirl's
texture, checks the dust with `particles.eval` before and after the attractors act, takes the
well off the dust's layer and cuts the swirl's texture wrong. [expected.txt](expected.txt) holds
its output.

- Each tick the `gpu_particles_3d` system pairs each emitter with the attractors visible in the
  tree whose bounds touch its visibility box and whose `layer_filter` shares a bit with its
  `layers`, at most 32 (7 of them vector fields). Every device then moves the particles by
  one formula: `strength` toward the center (or, by `directionality`, along the attractor's
  -z), falling off toward its edge by `attenuation`.
- A vector field's `texture` is an image cut into `slices_horizontal` by `slices_vertical`
  slices, left to right then top to bottom, stacked front to back; each texel's color `c` pushes
  along `c * 2 - 1` in the box's space. Here an 8x8 PNG holds four 4x4 slices. A texture that
  can't be read or cut pushes as white, and scene validation reports it, as the last lines show.
- `particles.eval` with a `time` runs the emitter's settings apart from the world, where no
  attractor acts; stepping the world shows what they did.
- A process material's `attractor_interaction_enabled`, on by default, lets its particles feel
  attractors.
