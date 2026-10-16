# Tile collision

Solid tile maps, as Godot's TileSet physics layers. [level.tileset.json](level.tileset.json) gives
each tile collision polygons around its center: a block, a slope, a one-way ledge and a spring on
a second physics layer that bounces. The block's alternative 1 is a see-through version with no
collision. The `level` entity in [main.scene.json](main.scene.json) is a `TileMapLayer` of those
tiles with four balls above it. [main.cpp](main.cpp) draws the atlas, turns a slope with
`tilemap.set`, steps two seconds and asks a ray what it hits. [expected.txt](expected.txt) holds
its output.

```text
tilemap.set {"cells":[[1,6]],"id":"level","tile":"slope:h"}
  ........===
  #
  #s..#.^.~../
  ############
tick 120:
  roller     at (58.0, 106.0)    rolled down the turned slope to the block
  jumper     at (104.0, 98.0)    bounced on the spring, now resting on it
  ghost_test at (136.0, 106.0)   fell through the see-through block
  riser      at (152.0, 26.0)    rose up through the ledge and landed on it
physics.raycast {"from":[72,0],"to":[72,128]}
{"hit":{"cell":[4,6],"fraction":0.75,"id":"level","normal":[0,-1],"point":[72,96]}}
```

- A tile's `collision` lists polygons in pixels around the tile's center, each on one of the
  tile set's `physics_layers`. A layer gives its polygons a collision layer and mask, a friction
  and a bounce, as Godot's physics layers with a physics material. Concave polygons are split
  into convex pieces.
- A polygon with `one_way` collides only with what comes down onto it, so the riser passes up
  through the ledge and lands on top.
- Each `TileMapLayer` whose tiles have polygons is one static body in the `physics2d` system,
  moving with the layer's `Transform2D`. Editing cells, the layer or the tile set rebuilds it, and
  `collision_enabled: false` turns it off.
- A cell can turn its tile: `:h` flips it left to right, `:v` top to bottom and `:t` swaps its
  axes, in that order, as Godot's cell transform flags. The tile draws turned and its polygons
  turn with it, so one slope tile serves both directions. `tilemap.set` gives `slope:h` a new
  legend key, `s`.
- Alternative tiles are other versions of a tile, numbered from 1 (`block:1`), each with its own
  flips, color and collision, as Godot's alternative tiles. The see-through block has a faded
  `modulate` and no polygons.
- `physics.raycast` reports the cell a ray hit on a tile map layer, beside the layer's ID.
- `slash-cli scene validate level.tileset.json` checks that each polygon has an area, doesn't
  cross itself and names a physics layer that exists.
