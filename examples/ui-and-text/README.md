# UI and text

UI, as Godot's `Control`, `Label` and `ColorRect`. [main.scene.json](main.scene.json) is a pause
screen: a dark rect over the whole window, a panel centered on it holding a title in a distance
field font with an outline, a paragraph that wraps, a quest line cut to one line with an ellipsis
and a hint with a shadow in its bottom-right corner, plus a score and a sign in the window's
corners. [main.cpp](main.cpp) writes the font file, checks the screen with `ui.layout` and
`text.measure`, changes the score and the window size, catches a misspelled theme override, then
draws the screen into a texture. [expected.txt](expected.txt) holds its output.

```text
ui.layout {"ids":["hint"]}
{"controls":[{"id":"hint","lines":["Esc: resume"],"rect":[373,238,85,22]}],"problems":[
  {"id":"hint","message":"Label has no colors item \"font_colour\"; did you mean \"font_color\"?",
   "path":"/components/Control/theme_override_colors/font_colour"}],"viewport":[640,360]}
```

- A control's rect is placed in its parent's by anchors and offsets: each edge is at its
  `anchor_*` (0 is the parent's left or top, 1 its right or bottom) plus its `offset_*` in
  pixels. The panel's anchors are all 0.5 and its offsets ±150 and ±90, so it stays centered at
  300x180 on any window. Controls that no `children` list names are placed in the viewport, the
  project's `window` size.
- `children` lists the controls placed in a control's rect, drawn over it in that order. A child
  can be listed by one parent only.
- A control is at least big enough for its content: a label's text, or `custom_minimum_size`.
  When it is bigger than its anchors and offsets make it, `grow_horizontal` and `grow_vertical`
  say which way it grows. The hint and the sign have zero-width rects at the right edge and grow
  `begin`, to the left, so they end where their right edges are.
- `Label` draws text in the rect. With `autowrap_mode` `word` the body wraps to the panel's
  width less its margins. The quest also wraps, but `max_lines_visible` 1 shows one line, and
  `text_overrun_behavior` `trim_ellipsis` ends it with `…` because there is more.
- A label's font, size, colors, outline and shadow are theme items, set with the control's
  `theme_override_colors`, `theme_override_constants`, `theme_override_fonts` and
  `theme_override_font_sizes`. Names a label doesn't have are reported with the closest one, as
  above.
- Fonts are `.ttf` and `.otf` files, or `.font.json` files that name one with settings. The
  title's [fonts/title.font.json](fonts/title.font.json) draws from multichannel signed distance
  fields, which stay sharp at any size, scale and outline. Labels with no font use Roboto, built
  into the engine. `outline_size`, as Godot's, is four times the pixels the outline reaches.
- Characters a font doesn't have are counted in `missing_glyphs`; Roboto has no Chinese, so the
  sign's 出口 needs a font that does. Text is shaped with HarfBuzz, so scripts that join letters,
  such as Arabic, draw as they should.
- `text.measure` gives text's size in a font without changing anything, to check that a longer
  score fits before setting it.
- Controls draw above the world's sprites, by `z_index`, then parents before children. Each label
  draws its shadow, outline and text; glyphs from one atlas page drawn one after another share a
  GPU draw.
