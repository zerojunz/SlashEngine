# Texture graph tools

An agent tunes a texture graph as text, without opening a picture.
[wall.texture.json](textures/wall.texture.json) is the brick wall from
[texture-graphs](../texture-graphs/), and [storyboard/style.json](storyboard/style.json) holds a
`budget` that allows wall textures 32 pixels on a side.

[calls.jsonl](calls.jsonl) works on the wall, and [results.jsonl](results.jsonl) holds the
replies:

1. `texture.inspect` describes the graph: each parameter's value and the nodes that read it
   (`across` is the expression `=rows / 2`), each node's op and inputs, and the nodes each output
   needs. With nodes that no output needs, it lists them as `unused`.
2. `texture.edit` applies a JSON merge patch to the file. This one thickens the mortar and adds
   `expect`, the ranges each output should keep. The reply lists the JSON Pointers that changed
   and each output's measures from 0 to 1: `value` (mean luminance), each channel's mean, `low`,
   `high`, `contrast` (luminance's standard deviation) and `tiles`.
3. `texture.check` finds two problems. The albedo's value is below its range, and the wall is
   twice the budget's size.
4. A second edit halves the size and brightens the gradient. The patch replaces the gradient's
   `points` array whole, as a merge patch replaces arrays.
5. `texture.check` now finds nothing. `test.run` and `slash-cli project validate` run the same
   check.
6. A misspelled input is refused, with the patch's JSON Pointer and the closest node name, and
   the file is left unchanged.
7. `dry_run` shows what removing the grain would change, without writing it.
8. `undo` puts the file back as the first edit left it, so `texture.check` finds both problems
   again.
