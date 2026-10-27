# Parametric models

Props written as text: a `.model.json` recipe names its parameters, materials and parts, and the
engine builds the mesh when it loads the file, so a `MeshInstance3D` draws a recipe as it draws a
glTF file. Parts are Godot's CSG shapes (box, sphere, cylinder, torus, polygon pushed, spun or swept
along a path) and its capsule and prism meshes, combined with their children in order as Godot's
CSG nodes are, with bevels on boxes and cylinders and arrays of copies.

[props/table.model.json](props/table.model.json) is a table sized by six parameters. Any number can
be an expression over them, in Godot's `Expression` syntax:

```json
"parameters": {"width": 1.4, "depth": 0.8, "height": 0.75, "thickness": 0.05, "leg": 0.06, "inset": 0.1},
"parts": [
  {"name": "top", "shape": "box", "material": "oak", "size": ["=width", "=thickness", "=depth"],
   "bevel": 0.01, "bevel_segments": 2, "position": [0, "=height - thickness / 2", 0]},
  {"name": "legs", "material": "iron", "children": [
    {"position": [0, 0, "=-depth / 2 + inset"],
     "array": {"count": 2, "position": [0, 0, "=depth - inset * 2"]},
     "children": [{"shape": "cylinder", "radius": "=leg / 2", "height": "=height - thickness",
                   "position": ["=-width / 2 + inset", "=(height - thickness) / 2", 0],
                   "array": {"count": 2, "position": ["=width - inset * 2", 0, 0]}}]}]}
]
```

Each top-level part is a node of the model with a mesh of its own: the top, rounded by its bevel,
and the four legs, two arrays of two. Making the table longer is changing `width`; the legs move
with it. [props/vase.model.json](props/vase.model.json) spins a profile around y, as Godot's
`CSGPolygon3D` in spin mode, and cuts its mouth out with a cylinder child whose `operation` is
`subtraction`.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` lists what the table's recipe builds: a mesh per part, with its triangles and
  LODs, the two materials and the bounds, 1.4 by 0.75 by 0.8 meters as its parameters say.
- `model.inspect` with a `path` reads the vase's recipe without an entity.
- `frame.capture` draws the room into `shots/room.png` and samples the vase, the table top and the
  floor.

From the command line:

```bash
slash-cli schema model                              # the recipe format, with every field's doc
slash-cli scene validate props/table.model.json     # every mistake at its JSON Pointer
slash-cli scene format props/table.model.json       # canonical form
```

Validation reports a misspelled parameter or material with the closest name, a field the part's
shape doesn't have, a bevel too big for its box, parameters that depend on themselves and parts
the CSG can't make, such as one cut away to nothing.
