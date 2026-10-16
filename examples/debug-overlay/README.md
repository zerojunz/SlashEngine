# Debug overlay

What a person watching a running game sees, and what an agent sees of the same collision
shapes. [main.scene.json](main.scene.json) has ground, a hill whose outline is a concave
`CollisionPolygon2D`, a one-way ledge, a crate that falls onto it, a coin (an `Area2D`) and a
disabled gate, in a 640x360 window ([project.json](project.json)). [main.cpp](main.cpp) first
captures a frame with the shapes drawn, as an agent would, then runs the game the way
`slash-cli run --debug-overlay` does and works the panel. [expected.txt](expected.txt) holds its
output.

```text
frame.capture {"collisions":true,"points":[[360,204],[360,236],[480,300],[240,260],[600,280],[600,40]]}
{"colors":[[0.059,0.322,0.412,1],[0.478,0.239,0.243,1],...],"drawn":{"controls":1,"shapes":6},...}
-- the mouse moves over the crate
  | 60 fps · 16.7 ms · tick 47 · playing
  | 7 entities · 1 control
  | mouse 360, 204: crate
```

- `frame.capture`'s `"collisions": true` draws every `CollisionShape2D` and
  `CollisionPolygon2D` over the world as Godot's Visible Collision Shapes does: shapes fill with
  `Color(0.0, 0.6, 0.7, 0.42)` and have an opaque outline, polygons have a red one, the one-way
  ledge has an arrow down its +y in the inverted color, and the disabled gate is gray. `drawn`
  counts the `shapes`.
- Shapes are placed as 2D physics places them, so what is drawn is what collides.
- In `slash-cli run`, F3 shows and hides the panel. The host hands each window event to
  `Overlay::handle` first (`SdlReader::set_filter`), so the game never sees F3 or clicks on the
  panel, runs `Overlay::update` each tick before the game steps, and draws the panel with
  `CanvasOptions::overlay`.
- The panel shows the frame rate, the tick, whether the game plays, the entity count, what the
  last frame drew, and the mouse's position with the bodies and areas under it.
- Pause, Step and Play run the `pause`, `step` and `play` commands through the session, as an
  agent would: `history.get` lists the two steps, so `undo` can take one back, and a recording
  keeps them.
- Shapes toggles the collision shapes, which stay on when the panel hides.
- An agent shows or hides both with `debug.overlay`, which a game run with `--serve` takes.
