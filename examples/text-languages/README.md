# Text in every language

Right-to-left text, right-to-left layout and fallback fonts, as Godot's.
[main.scene.json](main.scene.json) is a title menu in Arabic whose column has
`"layout_direction": "rtl"`, with a Hebrew greeting under it and an exit sign in Japanese and in
Chinese. [fonts/ui.font.json](fonts/ui.font.json) is Roboto with `fallbacks` for the characters
Roboto lacks, and the project's theme ([ui/game.theme.json](ui/game.theme.json)) makes it every
control's font. [main.cpp](main.cpp) writes the fonts, checks the menu with `ui.layout`, prints
which font drew each label, measures text with `text.measure`, lays the menu out left to right
for comparison, then draws it into a texture. [expected.txt](expected.txt) holds each call and
its reply.

```text
{"draw_mode":"normal","id":"start","lines":["ابدأ"],"rect":[336,53,48,30],"rtl":true},
{"draw_mode":"normal","id":"quit","lines":["خروج"],"rect":[284,53,48,30],"rtl":true}
```

- Text is ordered by the Unicode Bidirectional Algorithm (SheenBidi): a paragraph takes its
  direction from its first letter, Arabic and Hebrew runs draw right to left, and numbers and
  Latin words inside them still read left to right. `Label` and `Button` take a
  `text_direction` (`auto`, `ltr`, `rtl` or `inherited` from the layout) and a `language`.
  `ui.layout` and `text.measure` say `rtl` for lines that go right to left.
- `Control.layout_direction` `rtl` mirrors a control and the controls under it: anchors and
  offsets count from the right, a row's first child is at its right, a grid's first column too,
  `left` alignment is the right, a button's icon swaps sides and a progress bar fills from the
  right. The Hebrew greeting goes right to left, but its control is laid out left to right, so
  it stays on the left, as in Godot.
- Each character comes from the first font that has it: the label's font, then its `.font.json`
  file's `fallbacks` in order, then fonts installed on the machine. A fallback whose
  `language_support` or `script_support` says it isn't for the text is tried after the others,
  so the sign with `"language": "ja"` takes the Japanese font and the one with `"zh-Hant"` the
  Chinese one, although both fonts have 出口.
- Characters drawn from installed fonts are counted in `system_glyphs`, since other machines
  (and web exports, which have none) may lack them; characters no font has are counted in
  `missing_glyphs`. This example turns installed fonts off (`"gui": {"allow_system_fallback":
  false}` in [project.json](project.json)) so its output is the same everywhere; a `.font.json`
  file's `allow_system_fallback` does the same for one font.
- Games ship real fonts, such as Noto Sans Arabic and Noto Sans JP. This example has no binary
  file, so it writes Roboto (the engine's built-in font) and placeholder fonts made by
  `text::box_font`, whose glyphs are boxes. Their letters don't join, as an Arabic font's do;
  HarfBuzz shapes real fonts' joining forms.
