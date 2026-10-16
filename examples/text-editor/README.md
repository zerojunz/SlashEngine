# Text editor

A text field of several lines, as Godot's `TextEdit`. [main.scene.json](main.scene.json) is a
letter to write: a `TextEdit` that wraps its lines at words (`wrap_mode` `boundary`) and
highlights the caret's line, with a word count under it. [main.cpp](main.cpp) adds a
`word_count` system that reads the TextEdit's `text_changed` events, then writes the letter the
way an agent does over MCP (tools `input_inject`, `ui_layout`, `events_read`) or JSON-RPC, and
draws it into a texture. [expected.txt](expected.txt) holds each call and its reply.

```text
ui.layout {"ids":["letter"]}
{"controls":[{"focused":true,"hovered":true,"id":"letter","lines":["Dear Mira,","The harbor bell rang twice tonight.",
  "Meet me at the old lighthouse before","the tide turns."],"rect":[16,38,288,124],
  "text_edit":{"caret":[1,20],"rows":4,"selection":[[1,16],[1,20]]}}],"viewport":[320,240]}
```

- A click, Tab or `ui.focus` puts the caret in it. `input.inject`'s `text` events type at the
  caret and `ime` events compose as an input method does, as in a `LineEdit`; Enter
  (`ui_text_newline`) starts a line, Ctrl+Enter a blank one below and Ctrl+Shift+Enter one
  above (Command for Ctrl on a Mac). Tab types a tab while `tab_input_mode` is on, and Ctrl+M (`ui_focus_mode`) turns that
  off so Tab moves focus.
- Keys and actions edit as in Godot: the arrows move by character and line (keeping the
  caret's x), `ui_text_caret_word_*` by word, Home and End to the row's and then the line's
  ends, Page Up and Page Down by a page and `ui_text_caret_document_*` to the ends; Shift
  selects. Clicks place the caret, double clicks select a word, triple clicks a line, and drags
  select. With nothing selected, `ui_copy` and `ui_cut` take the caret's line, and pasting it
  puts it back as a line.
- Undo takes back typing until a pause of three seconds, or another kind of edit, as Godot's.
  Changing `text` from outside (`entity.set`, gameplay code) is one undoable step, as Godot's
  `set_text`, and emits `text_set`.
- The wheel scrolls three lines a notch; a finger drags the text. The view follows the caret,
  and a scroll bar shows when the lines don't fit. `scroll_vertical` (rows) and
  `scroll_horizontal` (pixels) are in the component.
- It emits `text_changed` and `caret_changed` once a tick, `lines_edited_from`
  (`{"from_line", "to_line"}`) for each edit and `text_set`, as world events; gameplay code
  reads them with `ui::events(world)`.
- `ui.layout` reports each TextEdit's `text_edit`: its `caret` and `selection` as
  `[line, column]`, input method text, whether it shows its `placeholder`, the `visible_lines`
  when they aren't all of them, its `rows` when wrapping makes more rows than lines, and which
  scroll bars show. Its `lines` are the rows in view.
