# Isometric and hexagonal tiles

Tile maps of diamonds and hexagons, as Godot's TileSet shapes and layouts.
[iso.tileset.json](iso.tileset.json) cuts `iso.png` into 32x16 diamonds in Godot's
`diamond_down` layout, where columns run down to the right and rows down to the left, with grass
and water tiles whose `peering` names a diamond's sides (`ne`, `se`, `sw`, `nw`) and corners
(`n`, `e`, `s`, `w`). [hex.tileset.json](hex.tileset.json) cuts `hex.png` into 32x28 hexagons in
offset rows. [main.scene.json](main.scene.json) has an empty layer of each. [main.cpp](main.cpp)
draws the atlases, then makes the calls an agent would make over MCP or JSON-RPC;
[expected.txt](expected.txt) holds the replies.

```text
tilemap.convert {"id":"board","points":[[136,149],[136,156]]}
{"cells":[[3,0],[3,1]]}
```

- `tile_shape`, `tile_layout` and `tile_offset_axis` are Godot's: `isometric` diamonds overlap
  by half a cell, `hexagon`s by a quarter, and `stacked` rows sit half a cell over every other
  row. Cells are placed, and their neighbors found, exactly as in Godot.
- `tilemap.paint` connects each diamond by its own sides and corners: the island's shore and the
  pond in it draw themselves, with inner corners where water meets a diamond only at a corner.
  The same tiles in a square tile set would be named by `n`, `e`, `s` and `w` sides; in a
  diamond layout the map is a square map turned 45 degrees.
- `tilemap.convert` turns cells into the points at their centers and points into the cells
  they are in, through the layer's `Transform2D`, as Godot's `map_to_local` and `local_to_map`.
  A point in the corner of a diamond's box is in the diamond beside it, and clicks either side
  of a hexagon's slanted edge land in the two hexagons that share it.
- Cells draw down the screen, so a tile taller than its cell covers the ones behind it.
- `frame.capture` confirms it: the colors at those points are the pond's water, the island's
  grass, the forest and hill hexagons, and the grass and forest either side of the edge.
- `slash-cli scene validate iso.tileset.json` checks that peering names only the sides and
  corners a diamond has.
