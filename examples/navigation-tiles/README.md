# Navigation on tiles

Paths over a tile map, as Godot 4.6's `TileMapLayer` makes navigation regions of its cells.
[maze.tileset.json](maze.tileset.json) gives its tiles navigation outlines on two navigation
layers: floor on the first, whose regions are in navigation layer 1, and water on the second,
in navigation layer 2. Walls have collision polygons instead, and the rock has both.

```json
"navigation_layers": [{}, {"layers": 2}],
"0,0": {"name": "floor", "navigation": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}]},
"2,0": {"name": "water", "navigation": [{"layer": 1, "points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}]}
```

The `maze` layer in [main.scene.json](main.scene.json) is a maze round a pond, with no
navigation file at all: each cell is a region of the 2D map in its tile's polygons, and the
regions join where the cells meet.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `navigation.map` counts a region for each floor and water cell.
- A mouse's path from the bottom left corner to the bottom right goes the long way round the
  pond, since a wall stands in the bottom row; every point's owner is the `maze` layer. A
  duck's path, on navigation layer 2, stays in the pond, and one to a target on land ends at
  the pond's nearest corner.
- `tilemap.set` turns the wall in the bottom row into floor, the map is rebuilt, and the mouse
  goes straight along it. `simplify_epsilon` drops the points where the path crosses from cell
  to cell.
- The `lawn` region bakes its file, [nav/lawn.navpoly.json](nav/lawn.navpoly.json), from the
  `grass` layer under it, as Godot bakes a `NavigationRegion2D` over a `TileMapLayer`: the
  tiles' outlines are where agents may walk and the rock's collision polygon is cut out, with
  an `agent_radius` of 2. The grass layer's `navigation_enabled` is `false`, so its cells
  aren't regions as well, and the path across the lawn goes round the rock.

Layers only filter paths: as in Godot, polygons that share an edge join whatever their layers,
so keep each cell to one layer's polygons where it meets its neighbors.
