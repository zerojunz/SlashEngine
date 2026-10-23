# GridMap

A level built from blocks in cells, as Godot 4.6's `GridMap` and `MeshLibrary`.
[blocks.meshlib.json](blocks.meshlib.json) is the library: four items, each a mesh (here a
primitive with a material), the shapes it collides with and a navigation mesh, all in the
space of one 2 x 2 x 2 meter cell.

```json
"ramp": {
  "primitive": {"PrismMesh": {"left_to_right": 0, "size": [2, 2, 2]}},
  "material": {"albedo_color": [0.85, 0.65, 0.3, 1]},
  "shapes": [{"shape": "convex_polygon", "points": [[-1, -1, -1], [1, -1, -1], ...]}],
  "navigation_mesh": "nav/ramp.navmesh.json"
}
```

The floor tile and the platform share [nav/square.navmesh.json](nav/square.navmesh.json),
moved to their tops by `navigation_mesh_transform`. The ramp has its own slope.

The `level` entity in [main.scene.json](main.scene.json) places them. Like a `TileMapLayer`, a
`GridMap` writes its cells as text: a `legend` of short keys, and per floor (`y`) rows of keys
going toward +z, each key a cell toward +x. `"r": "ramp:10"` is the ramp turned 180 degrees
about y by Godot's orthogonal index 10, so it rises toward the platform:

```json
"legend": {"f": "floor", "p": "platform", "r": "ramp:10", "w": "wall"},
"floors": [{"rows": ["ffffpp", "ffffrp", "ffwfff", "ffwfff"]}]
```

Cells are grouped into octants that draw as one mesh per material and collide as one static
body. With `bake_navigation`, each cell whose item has a navigation mesh is a navigation region,
and neighboring cells' edges join.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `gridmap.get` with no cells returns the legend, the floors and the library's items.
- `navigation.path` from the far floor corner to the top of the platform goes up the ramp.
- `physics.raycast` straight down hits the ramp's slope and names its cell and item.
- `frame.capture` reads the floor, the wall, the platform, the ramp, the wall's shadow and the
  sky.
- `gridmap.set` with a misspelled item says which one was meant. With `"wall"` it fills a box
  of cells, closing the gap between the two halves of the room, and the same path now ends at
  the nearest point it can reach.
- `gridmap.get` with `points` says which cell a point is in and what it holds; with `cells`, a
  cell's center and its item's orientation.
- `undo` takes the changes back, and the path climbs the ramp again.

`slash-cli scene validate` reports cells whose items the library doesn't have, and
`slash-cli scene validate blocks.meshlib.json` checks the library.
