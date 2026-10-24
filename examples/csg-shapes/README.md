# CSG shapes

Level geometry built from boxes, cylinders, tori and polygons combined with Godot 4.6's CSG
nodes, on Manifold's booleans. [main.scene.json](main.scene.json) has a hamlet: a house cut
hollow with a door and a round window, under a roof pushed out of a triangle; a tower with a
cone on top; a ring; a vase spun from its outline; and the ground they stand on.

A CSG shape's children change it in their order, as Godot's do. The house is a `CSGBox3D` with
four children:

```json
"house": {"components": {
  "CSGBox3D": {"size": [3.0, 2.4, 3.0], "use_collision": true},
  "StandardMaterial3D": {"albedo_color": [0.9, 0.85, 0.7, 1.0]},
  "Transform3D": {"children": ["inside", "door", "window", "roof"], "position": [0.0, 1.2, 0.0]}
}}
```

`inside`, `door` and `window` have `"operation": "subtraction"` and cut it; `roof` is a
`CSGPolygon3D` added on top. Only the root, the shape whose parent isn't one, draws and
collides: the shapes under it make one mesh with a surface per material. Faces keep the material
of the shape they came from, so the inside walls are the `inside` box's brown and the roof stays
red. With `use_collision`, a root is a static body, so the ground, the house and the tower are
solid.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `csg.get` on the house returns its triangles, its bounds and volume in its own space, and
  each surface's material. Asking for the door instead says which root makes the mesh.
- `physics.raycast` straight through the doorway hits the inside of the back wall; one 0.6
  meters to the side hits the front wall.
- `frame.capture` reads the roof, the wall, the dark doorway, the tower, the cone, the ring,
  the vase, the tower's shadow and the sky.
- The door widens with `component.set`. The same side ray now goes through, and `csg.get`
  shows the house lost volume.
- A hoop whose `inner_radius` equals its `outer_radius` makes nothing, and `csg.get` says why.
  `undo` takes it away, then puts the door back.

`slash-cli scene validate` reports CSG shapes that can't be made, the way `csg.get` does.
