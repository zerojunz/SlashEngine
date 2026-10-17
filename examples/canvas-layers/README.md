# Canvas layers and parallax

A scrolling level with a parallax background and a HUD, as Godot's `CanvasLayer` and
`Parallax2D`. In [main.scene.json](main.scene.json) the level is 1600 pixels wide in a 320x180
window, and the player carries a camera. [main.cpp](main.cpp) draws the textures, then makes the
calls an agent would make over MCP or JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
frame.summary {"ids":["hills","cloud","player","heart","score"]}
{"camera":{"id":"cam","rect":[440,0,320,180]},"drawn":[{"id":"hills","kind":"Sprite2D",
 "rect":[-110,120,640,40],"shown":0.49},...,{"id":"heart","kind":"Sprite2D","layer":"hud",
 "rect":[294,10,12,12]},{"id":"score","kind":"Label","layer":"hud","rect":[8,8,100,22],
 "text":"Coins: 0"}],"viewport":[320,180]}
```

- `far` is a `Parallax2D` holding the hills. Its `scroll_scale` `[0.25, 1]` moves them across
  the window at a quarter of the camera's speed, so they look far away, and its `repeat_size`
  `[320, 0]` draws them again every 320 pixels, so two copies always fill the window. The
  `parallax_2d` system places it each tick from the current camera.
- `sky` holds a cloud that scrolls at half speed and drifts left on its own: `autoscroll`
  moves a repeating `Parallax2D` along by itself.
- `hud` is a `CanvasLayer`. What its `children` list draws in its own canvas, over the world and
  apart from the camera: the heart sprite and the coin counter stay put however the level
  scrolls. `frame.summary` and `ui.layout` name the layer each thing is in.
- `canvas.convert` turns window pixels into the world through the camera, as Godot's
  `get_global_mouse_position`, or back with `"to": "screen"`. With an `id` it uses the canvas
  that entity draws in, so a click in the HUD stays in window pixels.
