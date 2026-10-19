# Code editor

A `TextEdit` that colors code, numbers its lines, marks breakpoints and what it is told to look
for, as Godot's `CodeEdit`, `CodeHighlighter` and `TextEdit` search do.
[main.scene.json](main.scene.json) is a script of two C++ functions in a `TextEdit` with a
`CodeHighlighter` and a `CodeEdit` beside it, a breakpoint on line 3 and `search_text`
`"velocity"`. [calls.jsonl](calls.jsonl) searches it, clicks its gutters, adds a line above the
breakpoints and works its scroll bar the way an agent does over MCP (tools `ui_search`,
`input_inject`, `ui_layout`) or JSON-RPC; [results.jsonl](results.jsonl) holds the replies.

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
- The `CodeEdit` draws its gutters left of the text: the main gutter, a row wide, with
  breakpoints (`breakpointed_lines`), then line numbers, a `0` wider than their digits. A line
  at `line_length_guidelines`' column, in widths of `0`, runs down the text. `ui.layout` gives
  each gutter's rect on screen from the first row's top, and `line_height`, so the row of a line
  in view is `(line - visible_lines[0]) * line_height` below that.
- A click in the main gutter toggles a breakpoint and emits `gutter_clicked` and
  `breakpoint_toggled` with the line; a click on a line number selects the line. Breakpoints
  stay on their lines as text is typed above them: the new line moves them from 1 and 3 to 2
  and 4, with a `breakpoint_toggled` for each line one left or came to.
- The scroll bar works as a `ScrollBar`'s: a click below its grabber goes down a page, and
  dragging the grabber scrolls with it, by fractions of a row, without moving the caret.
  `ui.layout` gives the grabber's rect on screen as `v_scroll_bar` (and `h_scroll_bar`).
