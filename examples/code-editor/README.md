# Code editor

A `TextEdit` that colors code and marks what it is told to look for, as Godot's
`CodeHighlighter` and `TextEdit` search do. [main.scene.json](main.scene.json) is a script of
two C++ functions in a `TextEdit` with a `CodeHighlighter` beside it and `search_text`
`"velocity"`. [calls.jsonl](calls.jsonl) searches it and works its scroll bar the way an agent
does over MCP (tools `ui_search`, `input_inject`, `ui_layout`) or JSON-RPC;
[results.jsonl](results.jsonl) holds the replies.

```text
ui.search {"id":"script","text":"Player","all":true,"flags":["match_case","whole_words"]}
{"count":2,"matches":[[1,10],[10,10]]}
```

- The `CodeHighlighter` colors keywords (`void`, `if`, `float`), a word before `(` in
  `function_color`, a word after `.` in `member_variable_color`, symbols and numbers, and its
  color regions: `//` to the end of the line, `/* */` over several lines and strings between
  quotes, where a `\` and the character after it take `symbol_color`. The rest takes the
  `TextEdit`'s font color.
- `search_text` is marked wherever it occurs in view, and `ui.layout` lists those places as
  `search_results` (`[line, column]`, from 0). With `highlight_all_occurrences`, selecting a
  word marks its other copies.
- `ui.search` finds text from the caret (or `from`), going round past the end, ignoring case
  unless `flags` has `match_case`; `whole_words` skips matches inside longer words, `backwards`
  looks before the position, and `all` returns every match. With no `text` it looks for the
  `TextEdit`'s own `search_text`.
- The scroll bar works as a `ScrollBar`'s: a click below its grabber goes down a page, and
  dragging the grabber scrolls with it, by fractions of a row, without moving the caret.
  `ui.layout` gives the grabber's rect on screen as `v_scroll_bar` (and `h_scroll_bar`).
