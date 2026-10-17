# Lit tiles and canvas layers

A dungeon floor lit by a torch, and a HUD lit by a lantern of its own, as Godot's 2D lights. In
[main.scene.json](main.scene.json) the floor is a tile map layer, `night` darkens the world and a
`CanvasLayer` named `hud` holds a panel, its own `CanvasModulate` and a lantern.
[main.cpp](main.cpp) draws the textures, then makes the calls an agent would make over MCP or
JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
light.sample {"id":"panel"}
{"ambient":[0.35,0.35,0.35],"layer":"hud","lit":true,"points":[{"color":[1.131,1.014,0.741],
 "lights":[{"color":[0.781,0.664,0.391],"id":"lantern"}]}]}
```

- The `stone` source in [dungeon.tileset.json](dungeon.tileset.json) names a `normal_texture`
  and a `specular_texture`, as a Godot `CanvasTexture` gives an atlas source. The normal map
  bevels each block's edges, so the edge facing the torch is bright and the one facing away is
  dim; the specular map makes the middle of each block shine with `specular_color` and
  `specular_shininess`.
- Each canvas has its own `CanvasModulate`: `night` tints only the world and `hud_dim` only the
  HUD.
- A light reaches the canvases whose layer is in `range_layer_min` to `range_layer_max`: the
  world is layer 0 and `hud` is layer 1. The lantern's range is 1 to 1, so it lights the panel
  and not the floor; the torch's is 0 to 0 until `component.set` widens it, and then it lights
  the panel too. Lights in a `CanvasLayer` are placed in that layer's canvas.
- `light.sample` with an `id` samples the entity where it is in its own canvas and names the
  layer it is in.
