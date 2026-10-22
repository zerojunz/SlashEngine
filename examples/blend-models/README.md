# Blender models

Blender's `.blend` files, imported as Godot imports them: the engine never reads a `.blend` file
itself. It runs the artist's own Blender in the background, which opens the file and exports it
as glTF with Godot's default import options, and draws that. No Blender code is linked into the
engine or shipped with a game, so Blender's GPL stays with Blender.
[main.scene.json](main.scene.json) stands a toadstool on the grass, with a `MeshInstance3D`
whose `mesh` names the file:

```json
"toadstool": {
  "components": {
    "MeshInstance3D": {"mesh": "models/toadstool.blend"},
    "Transform3D": {}
  }
}
```

The first time a session reads the file it runs Blender, found as Godot finds it: the program
`SLASH_BLENDER_PATH` names, else `blender` on `PATH`, else where Blender's installers put it.
Blender must be 3.0 or later. The glTF it writes is kept in
`.slash/imported/models/toadstool.blend-<hash>.glb`, with Blender's output in a `.log` file
beside it, and used until the `.blend` file changes; then Blender converts it again and the old
files are removed. Running a program is the `build` capability's, so a session started with
`--allow read,edit` reads files converted before and asks for `--allow build` for the rest.
`slash-cli export` converts every `.blend` file in the project and ships the glTF in its place,
so a game never needs Blender.

This example runs without Blender: tests set `SLASH_BLENDER_PATH` to a stand-in
([tests/fake_blender.cpp](../../tests/fake_blender.cpp)), and
[models/toadstool.blend](models/toadstool.blend) is a stand-in `.blend` file holding the glTF
Blender would make of a toadstool. With Blender installed, put any `.blend` file in its place.

The model keeps Blender's objects as nodes: `Toadstool`, an empty, holds the `Stem` and the
`Cap`, which has two surfaces, the red cap and the gills under it. Each material becomes a
`StandardMaterial3D` as Godot's glTF importer makes it.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `scene.open` opens the scene, converting the toadstool.
- `model.inspect` returns its meshes, materials, parts and bounds, in meters.
- `frame.capture` reads both sides of the cap, the gills in its shade, the stem, the sky and the
  grass.
- A smaller toadstool further back uses the same converted file; Blender doesn't run again.

`slash-cli scene validate models/toadstool.blend` converts a `.blend` file, if it hasn't been,
and checks it as `MeshInstance3D` loads it.
