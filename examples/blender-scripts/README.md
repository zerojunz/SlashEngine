# Blender scripts

A model that is easier to write as code than to describe in words, such as a bookshelf with a
number of boards, can be made by a Blender script. The built-in `blender` generator runs the
artist's own Blender in the background, as `.blend` files are imported (see
[blend-models](../blend-models/)): it starts from an empty scene, runs the script with its
`params` and `seed`, and exports what the script leaves in the scene as a `.glb` file with
Godot's glTF options. Blender is GPL, so it only ever runs as a program of its own, and running it
needs the `build` capability.

[game/tools/shelf.py](game/tools/shelf.py) builds a bookshelf from its width, depth, height and
number of boards. [main.cpp](main.cpp) drives the game in [game](game/), and
[expected.txt](expected.txt) holds the output. The example runs a stand-in for Blender
(`tests/fake_blender.cpp`) that makes a box the size the parameters ask for, so it runs anywhere:

- `asset.generate` with `script` (a `.py` file in the project, or the script's text) and
  `parameters` builds the shelf and keeps it in the asset library, with `props/shelf.glb` in the
  game.
- `asset.info` shows the record: generator `blender`, the version of Blender that ran it, and the
  script's text in the request, so the asset can be made again on any machine.
- `asset.refine` with new `parameters` runs the same script again with the width doubled; the
  wide shelf is a new version whose parent is the first, and `model.inspect` shows its size.
- The same script and parameters are the same asset, so asking again reuses it and Blender doesn't
  run.
- A session without the `build` capability is refused before Blender runs.

See [asset.generate](../../docs/reference/commands.md#blender-scripts).
