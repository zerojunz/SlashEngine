# Particle SDF and height field collision in 3D

`GPUParticles3D`s' particles collide with a signed distance field baked from meshes and with the
ground drawn from above, as Godot 4.6's `GPUParticlesCollisionSDF3D` and
`GPUParticlesCollisionHeightField3D` nodes. In [main.scene.json](main.scene.json) snow falls onto
a yard with a mound in it, and sparks fall onto a round statue. [main.cpp](main.cpp) bakes the
statue's field with `particles.bake_sdf`, steps the world and counts where the snow and sparks
are, takes the field's texture away, then has the height field follow the camera.
[expected.txt](expected.txt) holds its output.

- `particles.bake_sdf` bakes a `GPUParticlesCollisionSDF3D` as Godot's Bake SDF does: the
  visible meshes on layers of its `bake_mask` under its parent whose triangles touch its box,
  `resolution` cells along its longest side, each cell's distance to the nearest triangle, less
  than 0 within `thickness` cells behind one. It writes a `.sdf.png` file (16-bit samples of half
  floats, the slices of cells stacked down) and sets the collider's `texture` and
  `slices_vertical`. Here the statue is on layer 2 and the collider bakes only that layer.
- A `GPUParticlesCollisionHeightField3D` draws the highest point of what is on a layer of its
  `heightfield_mask` under each of its texels, from above, when it moves or its settings change
  (every tick with `update_mode` `"always"`), as Godot draws it. Particles collide with the
  first height field paired with their emitter, as Godot's.
- With `follow_camera_enabled` a height field steps after the current camera, a meter at a
  time along each of its x and z axes (its scale along them), so a small field can cover a big
  level around the player.
