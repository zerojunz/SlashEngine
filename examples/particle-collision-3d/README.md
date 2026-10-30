# Particle collision in 3D

`GPUParticles3D`s' particles collide with spheres and boxes, as Godot 4.6's
`GPUParticlesCollisionSphere3D` and `GPUParticlesCollisionBox3D` nodes. In
[main.scene.json](main.scene.json) a fountain of sparks falls onto a floor, a collision box, and
bounces; rain falls round an umbrella, a collision sphere, which hides each drop it touches, so
the ground under it stays dry. [main.cpp](main.cpp) steps the world, checks the sparks with
`particles.eval` and counts where the sparks and the rain are, then takes the umbrella off the
rain's layer. [expected.txt](expected.txt) holds its output.

- What a particle does on touching a collider is its process material's `collision_mode`:
  `rigid` pushes it out and bounces it off by `collision_bounce` (the share of its speed into
  the surface sent back) and `collision_friction` (the share of its speed along the surface
  lost), as [sparks.process_material.json](sparks.process_material.json) does;
  `hide_on_contact` kills it, as [rain.process_material.json](rain.process_material.json) does.
- A particle touches a collider when it comes within the emitter's `collision_base_size`
  (times its own scale with `collision_use_scale`), here 5 centimeters for the sparks.
- Each tick the `gpu_particles_3d` system pairs each emitter with the colliders visible in the
  tree whose bounds touch its visibility box and whose `cull_mask` shares a bit with its
  `layers`, at most 32, as Godot's scene cull pairs them; every device then collides the
  particles by Godot's formulas, the same on each.
