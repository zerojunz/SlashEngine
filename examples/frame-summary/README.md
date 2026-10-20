# Frame summary

`frame.summary` says what the window shows as data, so an agent can check what a player sees
without an image or a model that reads one. [main.scene.json](main.scene.json) is a level in a
320x180 window ([project.json](project.json)): a row of ground tiles, a player beside a bush, a
coin past the right edge, a ghost faded out, sparks, a sign whose texture is missing, and a HUD.
[main.cpp](main.cpp) draws the textures, then makes the calls an agent would make over MCP (the
tool `frame_summary`) or JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
frame.summary {"ids":["player","coin","ghost","pause"]}
{"clear":["ghost"],"drawn":[{"id":"player","kind":"Sprite2D","rect":[82,136,16,24],"shown":0.54},
 {"focused":true,"id":"pause","kind":"Button","rect":[130,4,60,30],"text":"Pause"}],
 "offscreen":["coin"],"viewport":[320,180]}
```

- `drawn` lists what shows, bottom first, with its `rect` in window pixels and, when not all of
  it shows, `shown`: the player is half behind the bush. Tile map layers say how many `tiles`
  drew, emitters how many `particles`, and controls their `text`, `draw_mode` and `focused`.
- `covered`, `offscreen` and `clear` say why something doesn't show: under what was drawn after
  it, outside the window, or transparent (the ghost's `modulate` alpha is 0). `problems` says
  what can't draw, with the field's JSON Pointer: the sign's texture is missing.
- `"map": [40, 12]` adds a text picture: each cell is the key of what shows at its center, from
  `legend`, and `.` is the sky. `region` maps part of the window more closely.
- `ids` keeps only the entities asked about, the cheapest way to ask "can the player see it?".
  When the player walks behind the bush it moves to `covered`, and off the edge to `offscreen`.
- `"contrast": true` adds each thing's `contrast` with what is around its edge, as WCAG's
  contrast ratio from 1 to 21, and lists those under 1.5 as `hard_to_see`. The score's white
  text is 2.1:1 on the sky, readable but weak, and the health bar's pale fill nearly vanishes.
  It draws the frame, lights and materials included, so it costs more than a summary alone.
- A thing shows where at least a quarter of a pixel's color comes from it, so a translucent
  panel and what is under it both show. Materials and lights aren't run, except for contrast.
