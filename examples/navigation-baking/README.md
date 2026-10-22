# Navigation baking

Navigation meshes baked from a scene, as Godot 4.6's `NavigationRegion3D.bake_navigation_mesh`
and `NavigationRegion2D.bake_navigation_polygon` bake them. [main.scene.json](main.scene.json)
has a courtyard on the 3D map: a 12 by 12 meter ground, a wall, a well, and a platform a meter
up with a ramp to it. They are under the `courtyard` region in its `Transform3D` children, as
Godot bakes the nodes under a region:

```json
"courtyard": {"components": {
  "NavigationRegion3D": {"navigation_mesh": "nav/courtyard.navmesh.json"},
  "Transform3D": {"children": ["ground", "wall", "well", "platform", "ramp"]}
}}
```

[nav/courtyard.navmesh.json](nav/courtyard.navmesh.json) holds only the bake's settings at
first, with Godot's names and defaults (`cell_size`, `agent_radius`, `agent_max_slope` and the
rest; here a taller `agent_height`). The ground, wall, platform and ramp are static bodies,
whose shapes bake; the well is a `MeshInstance3D`, whose mesh bakes. Recast marks the triangles
flat enough to walk, voxelizes them, keeps agents `agent_radius` from every edge and gives back
the walkable area as triangles, which the bake writes to the file's `vertices` and `polygons`.

On the 2D map, [nav/garden.navpoly.json](nav/garden.navpoly.json) has an outline of a garden
where agents may walk, and the `garden` region has a pond (a `CollisionPolygon2D`) and a tree
(a circle `CollisionShape2D`) on static bodies under it. Clipper2 takes the obstructions out of
the outline and shrinks what is left by `agent_radius`, and PolyPartition splits it into convex
polygons.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `navigation.bake` bakes the courtyard and says how many vertices and polygons it wrote.
  `navigation.path` then goes round the end of the wall and up the ramp onto the platform, and
  `navigation.closest` to a point above the well finds the ground beside it, since the well is
  cut out.
- The garden bakes the same way, and a path across it goes round the tree and the pond.
- The tree moves with `component.set`; a dry run says what a new bake would write and writes
  nothing, then the bake rewrites the file and the path goes round the tree where it is now.
- `undo` puts the file back as it was, then the tree, and baking again finds nothing to change.

A bake keeps everything else in the file as written, so settings and outlines stay where the
agent put them. Run `slash-cli scene validate` on a baked file to check it; it is already in
canonical form.
