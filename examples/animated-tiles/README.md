# Animated tiles

Tiles that play frames, and a tile two cells wide, as Godot's TileSetAtlasSource.
[pond.tileset.json](pond.tileset.json) cuts `pond.png` into 16x16 cells: water that plays three
frames, a torch that plays four from a random point of its loop in each cell, a bridge two cells
wide and a stone. [main.scene.json](main.scene.json) has a layer for the pond and one above it.
[main.cpp](main.cpp) draws the atlas, then makes the calls an agent would make over MCP or
JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
frame.capture {"points":[[8,8]],"time":1.0}
{"colors":[[0.565,0.792,0.976,1]],"drawn":{"tiles":30},"height":64,"width":128}
```

- `animation_frames` lists each frame's duration in seconds, and its length is the number of
  frames. The frames are the cells after the tile in its texture, as in Godot: the water's are
  the two to its right. `animation_columns` wraps them after that many columns, so the torch's
  four frames are a 2x2 block, and `animation_separation` skips cells between them.
- `animation_speed` multiplies the speed, and `animation_mode` `random_start_times` starts each
  cell at its own point of the loop, picked from the layer's ID and the cell, so the three torches
  show three different flames and stay out of step.
- Tiles play on the game's clock: after 30 ticks, half a second, the water shows its second
  frame. `frame.capture`'s `time` draws any moment without stepping, here one second, the third.
- `size_in_atlas` `[2, 1]` makes the bridge two texture cells wide. A large tile draws centered
  on its one cell of the layer; its `texture_origin` `[-8, 0]` moves it half a cell right, so it
  covers its own cell and the next and leaves the water before it.
- A tile, its size and its frames cover texture cells no other tile does: `slash-cli scene
  validate pond.tileset.json` reports a tile placed on another's frame.
