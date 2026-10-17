# Import Unity's isometric and hexagonal tile maps

`project.import` keeps each cell of a Unity isometric or hexagonal tile map where Unity draws it.
[unity/](unity/) holds "Hex Garden", a Unity 2D project as Unity 2022.3 saves it: a scene
([Garden.unity](unity/Assets/Scenes/Garden.unity)) with an isometric Grid of blocks, a garden
with a pond, and a hexagonal Grid, a board of grass and sand, with the sprites' import settings
and four tile assets. The blocks are 32x32 sprites that stand on their bottom middle, and the
garden's tile anchor is (0, 0), so each block's foot is on its cell's bottom corner. The
hexagons are centered on their cells. [main.cpp](main.cpp) draws the textures, then makes the
calls an agent would make over MCP or JSON-RPC; [expected.txt](expected.txt) holds the replies
and the tile sets the import wrote.

```text
tilemap.convert {"cells":[[2,-2]],"id":"garden"}
{"points":[[-64,0]]}
```

- Each Grid becomes a tile set of Godot's shape. Unity's isometric cells are an `isometric` tile
  set in the `diamond_right` layout, and its hexagons a `hexagon` tile set in `stacked_offset`
  rows. A Grid whose cell swizzle is YXZ, a hexagon map with flat tops, has the `vertical` offset
  axis.
- Unity's cell (x, y) is (x, -y - 1), as for rectangular maps: Unity's y points up. The pond, at
  Unity's (2, 1), is the garden's cell (2, -2), and the sand, at (1, 0), is the board's (1, -1).
- Unity and Godot place a map's cells differently, so each layer moves by a fixed offset that puts
  its cells' centers where Unity's are: the garden up half a cell, the board left half a cell and
  down a quarter. The layer's children move back by the same amount and stay where they were.
- Each tile's `texture_origin` puts its sprite's pivot at the tile anchor. The blocks'
  `[0, 8]` lifts them 8 pixels, so their bottom middles are on the cells' bottom corners.
- `frame.capture` checks the result: the pond's water 16 pixels above its center, where the top
  of its block is, the front block's grass top and its dark and light sides, the sand hexagon and
  the clear color between the maps.
- A hexagonal map whose tile anchor is off the cell's center, and turned tiles with an anchor off
  the center, go in the import's report.
