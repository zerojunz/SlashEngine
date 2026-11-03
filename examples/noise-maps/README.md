# Noise maps

Tile maps and terrains made from noise, as an agent tunes a FastNoiseLite once and uses it for
both a texture and a map ([`tilemap.generate`](../../docs/reference/commands.md#tilemapgenerate)
and [`terrain.generate`](../../docs/reference/commands.md#terraingenerate)'s `noise`).

- `islands.texture.json` is a `NoiseTexture2D`. [`texture.generate`](../../docs/reference/commands.md#texturegenerate)
  measures it and reads two of its pixels, as an agent would while tuning it.
- `tilemap.generate` samples the same noise at each cell of a 24 by 12 rect and paints water
  where it is under 0.5 and grass above, with the tile set's terrain painting picking the shore
  tiles. The rows `tilemap.get` returns show an archipelago. Running it again changes nothing:
  the same noise always fills the same cells.
- A band with `"tile": null` erases the cells under 0.3, and `undo` puts them back.
- `dunes.texture.json` is a ridged, warped noise. `terrain.generate` puts it in place of the
  HTerrain generator's Perlin noise, normalized to 0 to 12 meters and curved, then again with
  another seed and shaped into an island, whose corner falls to 0.
- The tile set's own texture, `ground.texture.json`, is procedural too.
- The last call names a terrain the tile set lacks and fails with the band's path.

Run it with `ctest --preset dev -R example_noise-maps`.
