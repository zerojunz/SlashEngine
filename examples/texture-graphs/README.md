# Texture graphs

A brick wall made from a node graph, in the spirit of Substance Designer and Unreal's Texture
Graph, so an agent tunes materials as text. [wall.texture.json](textures/wall.texture.json) is a
`.texture.json` file of `"type": "graph"`: nodes over named `parameters`. Bricks are multiplied
by cellular grain, blurred into a height, and mapped through a gradient into an albedo; a normal
map comes from the same height. Its three `outputs` are `albedo`, `normal` and `height`. Graphs
bake in compute on the GPU devices and on the CPU everywhere else, and both give the same
pixels.

- `rows`, `mortar` and `across` are parameters. `across` is an expression, `=rows / 2`, so the
  bricks keep their shape when `rows` changes. Nodes read parameters as `"$rows"`.
- [tall_wall.texture.json](textures/tall_wall.texture.json) `extends` the wall and sets
  different parameters. This gives twice the rows with thinner mortar, from four lines of text.

[main.scene.json](main.scene.json) draws both walls as `Sprite2D`s. A texture field names a graph
the same way it names a `.png` file; it takes the first output, or another output after `::`
(`wall.texture.json::normal`).

[calls.jsonl](calls.jsonl) tunes the wall without looking at it, and
[results.jsonl](results.jsonl) holds the replies:

- `texture.generate` on the graph measures its first output. The reply covers the channel
  ranges, `wrap`, two picked pixels (a mortar pixel and a brick pixel) and the names of every
  output. Graphs tile unless `"tiling": false`. The edges here fall on brick joints, so `wrap`'s
  `x` and `y` are as large as a joint's, above the `inside` mean of the flat bricks.
- `::height` picks the height output, gray in every channel.
- `parameters` changes a value for one call and writes nothing. Here, thicker mortar darkens
  the mean.
- `output` with `out` writes the normal map to a PNG.
- `frame.capture` reads the middle of each wall as the scene draws it.
- A misspelled parameter fails at `/parameters`, and an unknown output lists the outputs
  there are.

`slash-cli scene validate textures/wall.texture.json` checks a graph, with the JSON Pointer
and a "did you mean" for each mistake. `slash-cli schema texture` lists every op and its
settings.
