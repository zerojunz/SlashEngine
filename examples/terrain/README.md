# Terrain

An island made on a heightmap terrain, as the HTerrain plugin for Godot (Godot has no terrain
node of its own). The `land` entity in [main.scene.json](main.scene.json) has an `HTerrain`
naming three maps that don't exist yet, so it starts as flat ground at the low end of its
`height_range`:

```json
"HTerrain": {
  "heightmap": "terrain/land.height.png",
  "splatmap": "terrain/land.splat.png",
  "colormap": "terrain/land.color.png",
  "resolution": 65,
  "height_range": [-10, 40],
  "map_scale": [2, 1, 2],
  "chunk_size": 16,
  "textures": [{}, {}]
}
```

Its 65 x 65 heights are two meters apart, so it covers 128 x 128 meters, with a sea plane at
height 0 around it. Texture 0 stands for grass and texture 1 for rock; with no images they are
white, tinted by the color map.

[calls.jsonl](calls.jsonl) builds it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies. Every map is written by a command, so the
project commits no images:

- `terrain.get` with no points returns the flat terrain's bounds.
- `terrain.generate` makes heights from fractal noise shaped into an island (`island_weight`),
  as HTerrain's generator does, and writes the heightmap.
- `terrain.brush` in `flatten` mode levels a plot at 5 meters for a house.
- Two `color` dabs as wide as the island paint it green, then rock gray where it slopes more
  than 25 degrees; a `splat` dab with the same slope paints texture 1 there.
- `terrain.get` with points reads the ground at a hillside, on the plot and on the sea floor:
  its height, normal, texture weights and color.
- `frame.capture` reads grass, rock, a hillside, the sea and the sky.
- `physics.raycast` straight down hits the terrain's height field.
- A `hole` dab cuts a hole through the hillside: the ray now passes through it, the camera sees
  the sea under it and `terrain.get` counts the cells cut.
- `undo` fills the hole, and the ray hits the ground again.

The terrain is drawn in chunks of 16 x 16 cells whose detail falls off with distance from the
camera, stitched where a fine chunk meets a coarser one. `slash-cli scene validate` reports a
resolution, chunk size or map HTerrain refuses.
