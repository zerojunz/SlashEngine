# Palette and budget checks

Holding a game's textures to the art style its storyboard records. "Moss Keep" is a pixel-art
game drawn in the PICO-8 colors. [main.cpp](main.cpp) draws its textures in code: a hero on the
palette, a crate another agent drew in colors close to the palette but not on it, and a title
screen wider than the budget. Then it records the style with `storyboard.edit`, briefs before
touching the crate, checks, fixes both textures and checks again. [expected.txt](expected.txt)
holds the replies.

```json
{"id": "palette/pico", "version": null, "set": {"name": "PICO-8", "links": ["art/sprites"],
 "colors": {"black": "#000000", "dark_blue": "#1d2b53", "brown": "#ab5236", ...}}}
```

- A `palette` checks the textures in its `links`, here the whole `art/sprites` folder: every pixel
  that isn't fully transparent must be one of its colors, or within its `tolerance`. A palette
  with no links still goes out with every brief, but checks nothing.
- A `budget`'s `texture_size` limits the larger side of the textures it covers, every texture in
  the project when it has neither `links` nor `refs`.
- Each error names the texture, how many pixels are off, the first few colors with where each
  first shows and the nearest palette color, so the fix needs no screenshot: here, snapping each
  pixel to the nearest color.
- `storyboard.brief` with `files` returns the palette for the crate, since a link to a folder
  covers everything in it. `slash-cli project validate` and `test.run` run the same check.
