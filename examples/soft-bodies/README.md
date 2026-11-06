# Soft bodies

Cloth and soft bodies on Jolt Physics, as Godot 4.4's `SoftBody3D` on its Jolt module.
[main.scene.json](main.scene.json) drops a tablecloth onto a table, hangs a flag from a pole by
two pinned points and drops a ball full of air onto the floor. [main.cpp](main.cpp) steps the
world as an agent would, reads each soft body's points with `physics.soft_body`, then moves the
pole with `component.set`. [expected.txt](expected.txt) holds its output.

- A `SoftBody3D` simulates the primitive mesh its entity's `MeshInstance3D` draws. The mesh's
  vertices are welded where they meet into points, one for each place, and the faces between
  them hold them together with springs as stiff as `linear_stiffness` says. The mesh draws where
  the points are, so the tablecloth folds over the table's edges as it lands.
- `physics.soft_body` returns how many points there are (one for each vertex of the mesh),
  whether the body sleeps, their bounds `[x, y, z, w, h, d]` and, with `at`, where the vertices
  asked for are. Before the first step they are where the mesh and the entity's `Transform3D`
  put them; the entity itself never moves.
- The flag's two left corners (vertices 34 and 6 of its 7 by 5 plane) are pinned to the pole, at
  `offset`s in the pole's space. Moving the pole moves them, and the rest of the flag swings
  after them. `parent_collision_ignore` lets the flag pass through the pole it hangs from.
- The ball's `pressure_coefficient` fills it with air that pushes its faces out, so it stays
  round on the floor where an empty one would slump.
- Results are the same on every platform: Jolt is built cross-platform deterministic, and the
  points are welded and stepped in the same order every time.
