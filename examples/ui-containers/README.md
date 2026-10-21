# UI containers and themes

Containers, themes and the controls that draw boxes and textures, as Godot's.
[main.scene.json](main.scene.json) is an inventory screen: a `CenterContainer` keeps a
`PanelContainer` in the middle of the window, and in it a `BoxContainer` stacks a title, a
`ProgressBar`, a `GridContainer` of item slots holding `TextureRect` icons, a `FlowContainer` of
tags and a hint. The first slot's tooltip, a `NinePatchRect` holding a `MarginContainer`, is
`top_level`, so it sits in the corner of the window rather than in the slot.
[ui/game.theme.json](ui/game.theme.json), the project's theme, styles all of it.
[main.cpp](main.cpp) writes the two textures, checks the screen with `ui.layout` and `ui.theme`,
changes the bar and the grid, catches a misspelled style override, then draws the screen into a
texture. [expected.txt](expected.txt) holds its output.

```text
ui.theme {"id":"slot_0","items":["styles"]}
{"items":[{"from":"ui/game.theme.json","item":"styles/panel","value":{"bg_color":[0.2,0.22,0.3,1.0],
  "content_margin":8.0,"corner_radius":6}}]}
```

- A container places its children and ignores their anchors and offsets. `BoxContainer` lines
  them up (`vertical` for a column) with the theme's `separation` between them; `GridContainer`
  puts them in `columns`, each column as wide as its widest child; `FlowContainer` wraps them
  into lines; `CenterContainer` centers them at their minimum size; `MarginContainer` and
  `PanelContainer` inset them by margins. A container is at least as big as its children need,
  so the frame grows to fit what is in it and the center container keeps it centered.
- Across a box, and in a grid cell, each child's `size_flags_horizontal` and
  `size_flags_vertical` place it: `fill` (the default) takes the whole space, `shrink_center` keeps
  its minimum size in the middle. Along a box, `expand` children share the space left over, by
  `size_flags_stretch_ratio`. The grid is `shrink_center`, so it stays 274 wide in the middle of
  the 328-pixel box.
- A theme is a `.theme.json` file of items by control type: colors, constants, fonts, font sizes
  and style boxes (`$cli schema theme`). A control uses its `theme_override_*` items, then the
  themes in its own and its parents' `Control.theme`, then the project's `gui.theme`, then
  Godot's default theme. `ui.theme` says which one each item came from.
- A `top_level` control is placed in the window by its own anchors and offsets, as Godot's: the
  slot's container leaves the tooltip out and goes on placing the gem, and the tooltip takes none
  of the slot's transform, tint, `z_index` or clip. It draws after the rest of the screen.
- A type with a `base_type` is a variation: controls whose `theme_type_variation` names it use
  its items first. The title is a `Header`, a bigger, gold `Label`; the slots are `Slot`s, smaller
  panels than the frame's.
- Style boxes are Godot's: `flat` ones are rects with `corner_radius`, `border_width` and a
  `shadow_size`, drawn with smooth edges as Godot's `StyleBoxFlat` draws them; `texture` ones cut
  a texture into nine parts; `empty` ones draw nothing. `content_margin` is the room a container
  leaves around its children. A style box set with `theme_override_styles` replaces the theme's.
- `ProgressBar` fills as far as its `value` between `min_value` and `max_value`, with its
  percentage on top; `ui.layout` reports the percentage as its line. `TextureRect` draws a texture
  as its `stretch_mode` says; `keep_aspect_centered` keeps the gems square. `NinePatchRect` keeps
  its texture's corners (`patch_margin_*`) and stretches the rest.
- Names a type doesn't have are reported with the closest one, in style overrides as in the
  rest. Draws in a row with the same texture are merged into one GPU draw.
