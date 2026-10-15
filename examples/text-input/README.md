# Text input

Text fields, as Godot's `LineEdit`. [main.scene.json](main.scene.json) is a sign-up form: a name
field with a clear button and at most 20 characters, a `secret` password field and an age field
that takes three characters and asks for the number keyboard. [main.cpp](main.cpp) adds a
`profile` system that reads the fields' UI events, writes the form's fonts, then fills the form
in the way an agent does over MCP (tools `input_inject`, `ui_layout`, `ui_clipboard`,
`events_read`) or JSON-RPC, and draws it into a texture. [expected.txt](expected.txt) holds each
call and its reply.

```text
input.inject {"events":[{"selection":[0,2],"text":"みら","type":"ime"}]}
ui.layout {"ids":["name"]}
{"controls":[{"focused":true,"hovered":true,"id":"name","line_edit":{"caret_column":5,"editing":true,"ime":"みら"},
  "lines":["Mira みら"],"rect":[40,42,240,30]}],"viewport":[320,240]}
```

- A click, Tab or `ui.focus` gives a field focus and starts editing it (focus moved by the
  arrow keys doesn't, as in Godot); Enter submits (`text_submitted`) and stops editing, unless
  `keep_editing_on_text_submit` is on, and Escape stops editing.
- `input.inject`'s `text` events type characters at the caret, as a keyboard's text input
  does; an `ime` event shows what an input method is composing at the caret until a `text`
  event commits it. Keys and actions edit: `ui_text_backspace`, `ui_text_caret_word_left`,
  `ui_text_select_all`, `ui_copy`, `ui_cut`, `ui_paste`, `ui_undo`, `ui_redo` and the rest of
  Godot's `ui_text_*` actions, bound to Ctrl on a PC and to Command and Option on a Mac.
- A field emits `text_changed` once a tick with its new text, `text_change_rejected` with what
  `max_length` left out, `text_submitted` and `editing_toggled` (`{"toggled_on": bool}`) as
  world events; gameplay code reads them with `ui::events(world)`.
- Undo takes back one tick's typing, as Godot takes back one frame's. Changing `text` from
  outside (`entity.set`, an animation) starts the history again.
- `ui.layout` reports each field's `line_edit`: whether it is `editing`, its `caret_column`,
  `selection` and input method text, whether it shows its `placeholder` and, once it scrolls,
  which characters are `visible`. A secret field's `lines` are its dots.
- `ui.clipboard` reads and sets the clipboard that cut, copy and paste use. Headless and in
  tests it is the session's own; a game in a window shares the system's.
- `ui::text_input(world)` says what the field being edited asks of the platform: where it is,
  where the caret is and which on-screen keyboard to show. Games and `slash-cli run` pass it to
  `SdlReader::set_text_input` each tick, which turns on the input method next to the caret and
  shows the keyboard on phones.
