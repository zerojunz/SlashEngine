# UI menus

Drop-down lists and menus, as Godot's `OptionButton`, `MenuButton` and `PopupMenu`.
[main.scene.json](main.scene.json) is a game's settings screen in a 320x180 window
([project.json](project.json)): a menu bar of two `MenuButton`s (Game and View, with a disabled
item, separators, a check item and radio items), a Quality `OptionButton` and a context menu, a
`PopupMenu` entity of its own. [calls.jsonl](calls.jsonl) works it the way an agent does over MCP
(tools `input_inject`, `ui_popup`, `ui_select`, `ui_focus`, `ui_layout`, `events_read`,
`frame_summary`) or JSON-RPC, and [results.jsonl](results.jsonl) holds the replies.

```text
ui.layout {"ids":["game"]}
{"controls":[...],"popups":[{"id":"game","items":[{"rect":[4,36,89,22],"text":"New Game"},
  {"rect":[4,62,89,22],"selectable":false,"text":"Load"},...],"rect":[0,30,97,90]}],...}
```

- A click on a menu button or option button opens its list below it, over every control; only
  one list is open at a time. A click on an item picks it; a click outside the list closes it
  and still reaches what it lands on. Holding the click from the button and letting go over an
  item picks it too, as long as the list has been open for 0.4 seconds.
- With `switch_on_hover`, moving onto the other menu button of the bar while a menu is open
  opens its menu instead, as in a menu bar.
- Picking a menu's item emits `id_pressed` and `index_pressed` (data `{"id", "index"}`) from the
  menu button or `PopupMenu`; opening and closing emit `about_to_popup` and `popup_hide`.
  Picking a check or radio item doesn't check it: as in Godot, the game sets `checked`.
- An option button opened by a key starts on the option shown. The arrow keys move through the
  items, skipping separators and disabled ones (`item_focused`), `ui_accept` picks one, which
  sets `selected` and emits `item_selected` (data `{"index"}`), and `ui_cancel` closes the list.
  Typing the start of an item's text moves to it.
- `ui.popup` opens or closes a list without a click, and `ui.select` picks an item by index,
  `item_id` or text, as a click would; a disabled item or a separator can't be picked.
- `ui.layout` reports each open list's rect and its items' rects, `frame.summary` lists it as a
  `PopupMenu` with its items, so a check needs no screenshot.
