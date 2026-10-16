# UI scrolling

Scroll containers, scroll bars and clipping, as Godot's `ScrollContainer`, `HScrollBar` and
`clip_contents`. [main.scene.json](main.scene.json) is a quest log in a 320x180 window
([project.json](project.json)): ten quest buttons in a `ScrollContainer` with `follow_focus`, a
map `Panel` with `clip_contents` and an island that sticks out of it, and a zoom `ScrollBar`.
[calls.jsonl](calls.jsonl) works it the way an agent does over MCP (tools `input_inject`,
`ui_focus`, `ui_layout`, `events_read`, `frame_summary`) or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies.

```text
input.inject {"events":[{"type":"mouse_button","button":"wheel_down","position":[80,80],"pressed":true},...]}
step {}
ui.layout {"ids":["log"]}
{"controls":[{"id":"log","rect":[8,8,160,164],"scroll":[0,41],"scroll_bars":["vertical"],"scroll_max":[0,172]}],...}
```

- The wheel scrolls the log an eighth of its height a turn, even over a button: buttons let
  wheel events pass to the container under them. Shift turns the wheel sideways.
- The log shows a vertical scroll bar because its quests are taller than it; a click on the
  bar's track below the grabber scrolls a page, as far as `scroll_max`.
- Giving the first quest focus with `ui.focus` scrolls back to show it, because of
  `follow_focus`.
- Dragging the zoom bar's grabber moves its value with the pointer and emits `value_changed`
  and `scrolling`.
- A scroll set from outside the UI, here with `component.set`, is held to `scroll_max` at the
  next step.
- `ui.layout` reports a container's `scroll`, `scroll_max` and visible `scroll_bars`, each
  control's `shown_rect` when part of it is clipped and `clipped` when none of it shows;
  `frame.summary` lists what is clipped away and draws only the island's visible part, so a
  check needs no screenshot.
