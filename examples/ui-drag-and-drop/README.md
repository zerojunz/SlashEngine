# UI drag and drop

Tooltips and drag and drop, as Godot's `tooltip_text` and `_get_drag_data`, `_can_drop_data` and
`_drop_data`. [main.scene.json](main.scene.json) is an inventory in a 320x180 window
([project.json](project.json)): three slots, a sword in the first and a potion in the second,
and a trash bin. [main.cpp](main.cpp) adds the game's inventory system, which moves an item when
it is dropped, then works the inventory the way an agent does over MCP (tools `input_inject`,
`ui_drag`, `ui_tooltip`, `ui_layout`, `events_read`, `frame_summary`) or JSON-RPC.
[expected.txt](expected.txt) holds what it prints.

```text
ui.layout {"ids":["sword_ghost"]}
{"controls":[{"id":"sword_ghost","rect":[152,64,32,32]}],"drag":{"data":{"id":"sword","kind":"item"},
  "from":"sword","over":"slot_3","position":[152,64],"preview":"sword_ghost"},...}
```

- Each item has a `DragAndDrop` component: `drag_data` is what dragging it carries and
  `drag_preview` a hidden control shown at the pointer while it is dragged. The slots and the bin
  take whatever matches one of their `can_drop` patterns; `{"kind": "item"}` matches any object
  whose `kind` is `"item"`.
- Pressing on an item and moving more than 10 pixels (`gui.drag_threshold` in project.json)
  starts the drag: the item emits `drag_begin`. Letting go over a control that takes the data
  drops it there, which emits `drop_data` from that control with `{"data", "from", "position"}`,
  and the item emits `drag_end` with `{"successful"}`. A right click or Escape cancels it.
- The items pass the mouse on to their slots (`mouse_filter` `"pass"`), so dropping onto an
  occupied slot drops onto the slot, and the inventory system swaps the two items. Dropping on
  the bin destroys the item. The engine only reports the drop: what it means is the game's code.
- Resting the mouse on a control with a `tooltip_text` for half a second
  (`gui.tooltip_delay_sec`) shows its tooltip below and right of the pointer; a press, Escape or
  moving to another control hides it. `ui.tooltip` shows one without the mouse.
- `ui.drag` drags one control onto another in one call, as the mouse would from the middle of
  one to the middle of the other. `ui.layout` reports the tooltip shown and the drag going on,
  and `frame.summary` lists the tooltip as drawn, so a check needs no screenshot.
- Text fields drag their selection the same way: dropped on another field it moves there, with
  Ctrl (Command on a Mac) it is copied.
