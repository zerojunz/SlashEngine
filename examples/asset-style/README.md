# Asset style

A game keeps one style when the storyboard says what it is and every new asset is held to it.
That style should come from the art the game keeps, not only from what was written on the first
day. `asset.style` reads the library assets the project uses that a person or agent accepted
with `asset.feedback`, and drafts storyboard edits from them:

- a palette of the colors the accepted images use, named in words, or the colors an existing
  palette lacks;
- the triangle and texture budgets they fit, where no budget sets them;
- a `reference` for each asset accepted with a reason, which later generation requests carry.

Nothing is written: `storyboard.edit` keeps the edits an agent agrees with. From then on
`storyboard.check` (and `project validate`, `test.run` and CI) holds models to the triangle
budget, and `asset.generate` sends the palette and references along with the prompt.

The cafe in [game/](game/) starts with a tone and nothing else. [main.cpp](main.cpp) does what an
agent does, and [expected.txt](expected.txt) holds the output:

- `asset.add` keeps the [stool](game/props/stool.model.json), the
  [cup](game/props/cup.model.json) and a chalkboard sign drawn in code, and `asset.feedback`
  accepts each with a reason.
- `asset.style` drafts `palette/main` from the sign's two colors, `budget/assets` with 96
  triangles (the stool's) and 32 pixels a side, and a reference per asset; `storyboard.edit`
  keeps them.
- A [brass lamp](game/props/lamp.model.json) of 2208 triangles fails `storyboard.check`.
- A menu board generated under `art/` is asked for in the palette's colors and the sign's
  reference, and is checked against the palette as it lands.
