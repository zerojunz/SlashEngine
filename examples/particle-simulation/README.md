# Particle simulation

Simulation stages run over CPU particles in 2D and 3D, as Unreal's Niagara modules (Godot has
none): a `ParticleSimulation3D` or `ParticleSimulation2D` on the same entity as a
`CPUParticles3D` or `CPUParticles2D` works on its particles each tick after the emitter has moved
them. In [main.scene.json](main.scene.json):

- a fountain's drops bounce on a floor (a `GPUParticlesCollisionBox3D`, `collide: "colliders"`),
  fade by an `alpha` attribute, start two puffs in the `mist` emitter on a quarter of their
  landings and emit a `splash` world event on a few;
- fireflies flock (separation, cohesion and alignment within `neighbor_radius`), swirl in curl
  noise, keep between `min_speed` and `max_speed`, are sized by a `size` attribute worked out
  once from their `random`, and emit `lantern` events as they pass a box;
- in 2D, sand pours onto a dune (a `LightOccluder2D`, `collide: "occluders"`) and shrinks as it
  ages, and starlings flock in the plane.

[calls.jsonl](calls.jsonl) steps two seconds, checks each emitter with `particles.eval` (the
fountain's drops stay above the floor at y 0, the sand above the dune's slope), reads the
`particle_event`s, then turns the fountain's collision off and shows its drops falling through.
[results.jsonl](results.jsonl) holds the replies.

- Neighbors are found by sorting particles along a Morton curve by their cells and looking up
  each one's cell and those round it with binary searches, so no grid is kept; at most
  `max_neighbors`, the nearest, count.
- Attributes are Godot `Expression`s over the particle's values (`age`, `life`, `lifetime`,
  `speed`, `x`, `y`, `z`, `vx`, `vy`, `vz`, `neighbors`, `index`, `collided`, `random`, `time`,
  `delta`) and the attributes before them; `size`, `alpha` and `kill` change how it draws or
  whether it lives.
- Events answer `collision`, `death` and `location` (a box, or a rect in 2D, in the world): each
  can start `count` particles in another CPU particles emitter (`spawn`) and emit a
  `particle_event` world event (`name`), at most eight an emitter a tick.
- A 3D simulation can also collide with what the current camera sees (`collide: "depth"`), a
  depth image of the meshes in front of it, as Niagara's depth buffer collision.
- Every stage runs with the same results on any number of threads and on every platform.
