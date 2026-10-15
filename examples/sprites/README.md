# Sprites

Sprites and flipbook animation, as in Godot. [main.scene.json](main.scene.json) has three
sprites and [hero.frames.json](hero.frames.json) two animations of one sprite sheet.
[main.cpp](main.cpp) writes the sheet, plays the hero's animations with the commands an agent
sends, then draws the scene into a texture.

- `Sprite2D` draws a texture, or frame `frame` of a sheet split into `hframes` by `vframes`, or
  the part `region` picks. It is centered on the entity unless `centered` is false. `flip_h` and
  `flip_v` mirror the picture in place, `modulate` tints it, and `z_index` puts it above or below
  others; equal `z_index` values draw in the order the entities were made.
- `AnimatedSprite2D` plays an animation from a `.frames.json` file on the entity's `Sprite2D`,
  adding one if it has none. A frame is a texture path or a frame number in the animation's sheet;
  leave `frames` out to play the whole sheet. Animations loop unless `"loop": false`, which stops
  on the last frame, sets `playing` to false and emits `animation_finished`.
- Switch animations with `component.set`; `run.until` waits for one to finish in one call.
- Between fixed ticks, games draw sprites part of the way from where they were to where they are
  (`rendering.interpolation` in `project.json`), so motion is smooth at any frame rate.
- Pixel art wants `"texture_filter": "nearest"` on the sprite, or `rendering.texture_filter` in
  `project.json` for every sprite that says `inherit`.
- Sprites sharing a texture are drawn together: the sign and the hero make one GPU draw.
- The output only shows what is the same on every device; `tests/test_canvas.cpp` checks the
  drawn pixels on a GPU.
