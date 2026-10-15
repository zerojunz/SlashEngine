# UI buttons

Buttons, focus and hover, as Godot's. [main.scene.json](main.scene.json) is a title menu: a
column holds a Play button, three toggle buttons in one `button_group` (Easy, Normal and Hard; at
most one is on), a `TextureButton` that mutes the sound, a disabled Online button and a Quit
button whose `shortcut` is `ui_cancel` (Escape). [main.cpp](main.cpp) adds a `menu` system that
reads the buttons' UI events, writes the speaker's three textures, then works the menu the way an
agent does over MCP (tools `input_inject`, `ui_press`, `ui_focus`, `ui_layout`, `events_read`) or
JSON-RPC, and draws it into a texture. [expected.txt](expected.txt) holds each call and its reply.

```text
events.read {"subscription":"events_1"}
{"dropped":0,"events":[{"id":"hard","kind":"mouse_entered","seq":1,"tick":0},{"id":"hard","kind":"focus_entered","seq":2,"tick":0},
  {"id":"hard","kind":"button_down","seq":3,"tick":0},{"data":{"toggled_on":false},"id":"normal","kind":"toggled","seq":4,"tick":0},
  {"data":{"toggled_on":true},"id":"hard","kind":"toggled","seq":5,"tick":0},{"id":"hard","kind":"pressed","seq":6,"tick":0},
  {"id":"hard","kind":"button_up","seq":7,"tick":0}],"more":false}
```

- A button is pressed when a click or tap that started on it ends over it (`action_mode`
  `press` presses as it starts), when `ui_accept` (Enter or Space) goes up while it has focus,
  or when its `shortcut` is pressed. It emits `button_down`, `pressed`, `toggled` (data
  `{"toggled_on": bool}`) and `button_up` as world events, in Godot's order; gameplay code reads
  them with `ui::events(world)` or `ui::was_pressed`, and agents with `events.subscribe`.
- Any control emits `mouse_entered`, `mouse_exited`, `focus_entered` and `focus_exited`.
- A click gives focus without showing it; the arrow keys, Tab and a gamepad's d-pad or stick
  move it to the closest button that way, or to a control's `focus_neighbor_*`, `focus_next` and
  `focus_previous`, and show it.
- `ui.layout` reports each button's `draw_mode` (`normal`, `hover`, `pressed`, `hover_pressed`
  or `disabled`) and which control is `focused` and `hovered`, so a check needs no screenshot.
  A toggle button that is on draws `pressed`, and `hover_pressed` while hovered or for a moment
  after its shortcut pressed it.
- A touch also clicks the mouse (Godot's `emulate_mouse_from_touch`): the click hovers and
  focuses the speaker, the touch presses it, and it isn't pressed twice.
- `ui.press` presses a button at the next tick, as a click would; `ui.focus` gives a control
  focus. A disabled button can't be pressed, and controls other than buttons take no focus
  unless their `focus_mode` says so.
- Which control a pointer reaches is `Control.mouse_filter`: a `Label` lets it through to the
  button under it (`ignore`), containers pass it on to their parents (`pass`), and buttons and
  the rest stop it (`stop`).
