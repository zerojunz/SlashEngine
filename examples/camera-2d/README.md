# Camera 2D

The part of a level the window shows, as Godot's `Camera2D`. In
[main.scene.json](main.scene.json) the level is 1600 pixels wide in a 320x180 window, and the
player carries a camera 32 pixels above it as a child entity, so the view follows the player.
[main.cpp](main.cpp) draws the textures, then makes the calls an agent would make over MCP or
JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
frame.summary {"ids":["player","coin","score"]}
{"camera":{"id":"cam","rect":[1280,0,320,180]},"drawn":[{"id":"coin","kind":"Sprite2D",
 "rect":[216,116,8,8]},{"id":"player","kind":"Sprite2D","rect":[294,144,12,16]},
 {"id":"score","kind":"Label","rect":[8,8,100,22],"text":"Coins: 0"}],"viewport":[320,180]}
```

- The first enabled `Camera2D` in drawing order is current, and the world draws as seen from it:
  `anchor_mode` `drag_center` puts the middle of the screen at the camera. `frame.summary` gives
  the camera and the world rect it shows, and every rect in window pixels.
- `drag_horizontal_enabled` lets the player walk within 0.2 of half the screen before the camera
  moves; `position_smoothing_enabled` makes it glide there, closing
  `position_smoothing_speed` times the tick's seconds of the gap each tick.
- `limit_left`, `limit_top`, `limit_right` and `limit_bottom` keep the view inside the level, so
  at its end the player walks off the middle of the screen.
- `zoom` `[2, 2]` shows things twice as big. Controls outside a `CanvasLayer`, such as the coin
  counter, stay put on the screen; [canvas-layers](../canvas-layers/) puts a HUD in a layer.
