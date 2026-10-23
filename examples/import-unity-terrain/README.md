# Import a Unity terrain

`project.import` converts Unity's terrains into SlashEngine's `HTerrain`, the heightmap terrain
that follows the HTerrain plugin for Godot. [unity/](unity/) holds "Island" as Unity 2022.3 saves
it: its project settings, a scene ([Island.unity](unity/Assets/Scenes/Island.unity)) with the
camera, the sun and an `Island` object carrying a `Terrain` and a `TerrainCollider`, and the two
terrain layers it paints with ([Grass.terrainlayer](unity/Assets/Terrain/Grass.terrainlayer) and
[Rock.terrainlayer](unity/Assets/Terrain/Rock.terrainlayer)).

Unity keeps a terrain's heights, holes and alpha maps in its `TerrainData` asset, as hex numbers in
the `.asset` file, and its layers' textures as PNG files. [main.cpp](main.cpp) first plays the part
of Unity's terrain tools and writes them: `Island.asset` with a hill 8 meters high in the middle of
64 by 64 meters of flat ground, rock above 4 meters in its alpha map and a hole eight cells across
in the flat ground, then the two textures. Then it makes the calls an agent would make over MCP or
JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
entity.get {"id":"island_terrain","omit_defaults":true}
{"components":{"HTerrain":{..., "heightmap":"unity/Assets/Terrain/Island.height.png",
 "height_range":[0.0,8.0], "splatmap":"unity/Assets/Terrain/Island.splat.png",
 "textures":[{"albedo_bump":"unity/Assets/Terrain/grass.png","uv_scale":8.0}, ...]},
 "Transform3D":{"position":[-64.0,0.0,0.0]}}}
```

- The terrain becomes an entity under the `Island` object, `island_terrain`. Unity's terrains reach
  toward +x and +z from their object, and the import mirrors Unity's scenes in x, so the terrain
  would reach toward -x: its entity moves back the terrain's width, 64 meters, and the hill stays
  where Unity draws it.
- The heights become `Island.height.png`, a 16-bit heightmap whose `height_range` is the whole
  meters that hold every height, 0 to 8, with its columns flipped as x mirrors. Unity's cells of
  one meter are `map_scale`'s default.
- The alpha map becomes `Island.splat.png`, resampled to one weight a height, and the two layers
  become the terrain's `textures`. A layer's tile size is in meters and `uv_scale` in cells, so
  tiles of 8 meters over cells of 1 meter give 8.
- A height is a hole in an `HTerrain` where its color map's alpha is below a half, so the import
  writes `Island.color.png`, white with the holes cut. A height is cut where every cell around it
  is, which the report notes: the hole is seven heights across, half a cell smaller on each side.
- The `TerrainCollider` sets the terrain's collision: its layer and mask, and Unity's default
  friction of 0.6, as it has no physic material. `m_ShadowCastingMode` 2, Unity's two-sided
  shadows, is `cast_shadow` `double_sided`.
- `terrain.get` reads the rock at the summit, the grass on the slope toward the camera and the hole;
  `physics.raycast` straight down hits the summit at 8 meters and falls through the hole.
- `frame.capture` reads the rock on the hilltop, the grassy slope, the flat ground, the sky's ground
  color through the hole and the sky.
