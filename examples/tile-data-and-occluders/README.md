# Tile data and occluders

Custom data and occluders on tiles, as Godot's TileSet custom data and occlusion layers.
[dungeon.tileset.json](dungeon.tileset.json) has an occlusion layer and three custom data layers.
Its wall and pillar cast shadows; its lava does damage and slows. [main.scene.json](main.scene.json)
has a floor layer, a layer of walls above it and a torch. [main.cpp](main.cpp) draws the atlas,
then makes the calls an agent would make over MCP or JSON-RPC; [expected.txt](expected.txt)
holds the replies.

```text
tilemap.get {"cells":[[2,3],[5,3]],"id":"ground"}
{"cells":[{"cell":[2,3],"data":{"damage":10,"kind":"lava","speed":0.5},"tile":"lava"},...]}
```

- `custom_data_layers` names each value a tile can have and its type: `bool`, `int`, `float`,
  `string`, `vector2`, `vector2i` or `color`. A tile's or an alternative's `custom_data` sets
  values by layer name; a value it doesn't set is the type's default, as Godot's
  `get_custom_data` returns. `slash-cli scene validate` checks each value against its layer's
  type and suggests the closest layer name for a misspelled one.
- `tilemap.get` with `cells` returns each cell's tile and its value in every layer, so gameplay
  questions such as "does this cell hurt?" take one call. In C++, `TileSet::custom_data` answers
  the same.
- `occlusion_layers` lists occlusion layers with the `light_mask` of the lights they shadow. A
  tile's `occlusion` polygons sit at its cell's center and cast shadows from lights with
  `shadow_enabled` whose `shadow_item_cull_mask` shares a bit with that mask, as a
  `LightOccluder2D` does. An occluder is `closed` unless set otherwise, and its `cull_mode`
  shadows from one side only; turned cells turn their occluders.
- `TileMapLayer`'s `occlusion_enabled` turns a layer's occluders off, as Godot's does; a hidden
  layer casts no shadows either.
