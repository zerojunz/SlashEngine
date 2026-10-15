# Tile maps

Grids of tiles with terrain autotiling, as Godot's TileSet and TileMapLayer.
[ground.tileset.json](ground.tileset.json) cuts `ground.png` into 16x16 tiles: water, grass for
the middle of an area, its four edges, four outer corners and four inner corners, a flowery
grass and a rock. The grass tiles' `peering` says which sides and corners touch water, as
Godot's terrain peering bits. The `ground` entity in [main.scene.json](main.scene.json) has an
empty `TileMapLayer`. [main.cpp](main.cpp) draws the atlas from the tile set's own peering,
paints an island with a pond, reads the cells back and draws them into a texture.
[expected.txt](expected.txt) holds its output.

```text
  wwwwwwwwwwww
  wwdGGGGGGeww
  wwbjakggrcww
  wwbcwbgfgcww
  wwblGmgfgcww
  wwbggggfgcww
  wwhaaaaaaiww
  wwwwwwwwwwww
```

- A layer keeps its cells as text: `rows` of legend keys from `origin`, with `.` for an empty
  cell, and a `legend` naming the tile each key stands for. Agents read a map in one call, and
  a git diff of the scene shows the rows that changed.
- `tilemap.paint` paints a terrain on `cells` or a `rect` and picks the tile whose peering fits
  each cell's neighbors, for the painted cells and the ones around them. The shore draws itself,
  and the pond's neighbors turn to face it, with inner corners at its diagonals.
- Where several tiles fit equally well, the cell's position picks one, weighted by
  `probability`: the flowery grass (0.25) shows up in about one middle cell in five, and the
  same paint always gives the same tiles.
- `tilemap.set` places one tile by name, or as `"source:column,row"`, or erases cells with
  `null`; it doesn't change the tiles around it. `tilemap.get` with a `rect` returns a window of
  the map in the layer's own keys.
- New keys go to the most used tiles first, each the first letter of the tile's name where that
  is free. A key keeps standing for its tile as the map changes.
- The whole layer is one GPU draw, since all its tiles come from one atlas texture.
- `slash-cli scene validate main.scene.json` checks each layer's legend and rows against its
  tile set, and `slash-cli scene validate ground.tileset.json` checks the tile set.
