# UI settings

Check boxes, check buttons and sliders, as Godot's `CheckBox`, `CheckButton`, `HSlider` and
`VSlider`. [main.scene.json](main.scene.json) is a settings screen in a 320x180 window
([project.json](project.json)): Fullscreen and Subtitles check boxes, three check boxes in one
`button_group` (Low, Medium and High), a V-Sync check button, a volume slider with ticks and a
vertical brightness slider. [calls.jsonl](calls.jsonl) works it the way an agent does over MCP
(tools `input_inject`, `ui_press`, `ui_layout`, `events_read`, `frame_summary`) or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies.

```text
input.inject {"events":[{"type":"mouse_button","button":"left","position":[179,155],"pressed":true},
  {"type":"mouse_motion","position":[200,155]},{"type":"mouse_button","button":"left","position":[200,155],"pressed":false}]}
step {}
events.read {"subscription":"events_1"}
{"events":[{"data":{"value":50.0},"id":"volume","kind":"value_changed",...},
  {"data":{"value":60.0},"id":"volume","kind":"value_changed",...},
  {"data":{"value_changed":true},"id":"volume","kind":"drag_ended",...}],...}
```

- `CheckBox` and `CheckButton` are toggle buttons with every field of `Button`; they draw a box
  or a switch beside their text instead of a style box. Check boxes in a `button_group` draw as
  radio buttons, and clicking High turns Medium off.
- Their icons are theme items (`icons/checked`, `icons/unchecked` and the rest), Godot's
  default icons drawn by the engine (`builtin:toggle_on`); `ui.theme` says where each comes
  from, and a theme or `theme_override_icons` can name a PNG instead.
- A click on a slider moves the grabber's middle to the pointer and starts a drag; the drag
  moves it with the pointer, and letting go emits `drag_ended` with whether the value changed.
  With focus, the arrow keys (or a gamepad, repeating while held) step it by `step`, and Home
  and End go to the ends. The value is always a multiple of `step` from `min_value`.
- A value set from outside the UI, here with `component.set`, is snapped the same way (0.73
  becomes 0.7) and emits `value_changed` at the next step.
- `ui.layout` reports a slider's `value` and `ratio`, and `frame.summary` its `value`, so a
  check needs no screenshot.
