# Navigation

Paths over navigation meshes, as Godot 4.6's `NavigationServer3D` and `NavigationServer2D`.
[main.scene.json](main.scene.json) has a house on the 3D map: two rooms joined the long way
round by a corridor and the short way by a hatch, a `NavigationLink3D` six meters long that
costs one and a half times as much to cross as to walk. The house's floor is
[nav/house.navmesh.json](nav/house.navmesh.json), seven convex polygons over shared vertices, as
a baked `NavigationMesh`:

```json
"hatch": {"components": {
  "Transform3D": {},
  "NavigationLink3D": {"start_position": [3, 0, 2], "end_position": [9, 0, 2], "travel_cost": 1.5}
}}
```

A guard with a `NavigationAgent3D` is sent to the far room, and on the 2D map a cat with a
`NavigationAgent2D` crosses a yard shaped like an L ([nav/yard.navpoly.json](nav/yard.navpoly.json),
two polygons). The navigation system finds each agent's path and keeps track of where it is
along it; [main.cpp](main.cpp) moves them with a plain C++ system that walks each one toward
`next_path_position_3d` or `next_path_position_2d`, as a Godot script calling
`get_next_path_position` would.

The program then asks the `navigation.*` commands what an agent would, printing each call and
its reply ([expected.txt](expected.txt)):

- `navigation.map` counts each map's regions, links, polygons and edges, and
  `navigation.agent` gives the guard's path through the hatch.
- Three seconds later the guard is in the far room, and `events.read` has its `link_reached`
  at the hatch and the cat's `target_reached` and `navigation_finished`.
- `navigation.path` with `metadata` names the hatch as the owner of the path's link point.
  With the hatch moved to navigation layer 2 the path goes the long way round, pulled tight round
  the corners, then through the middle of each edge with `"postprocessing": "edgecentered"`.
- `navigation.closest` finds the floor under a point off it, and `navigation.random` picks a
  point of the yard that is the same for the same seed.
