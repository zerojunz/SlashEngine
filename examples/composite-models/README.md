# Composite models

A model recipe's part can be another model file, so a big asset is built from small ones that
stay files of their own: a market stall of a counter, posts and an awning, three crates from
[props/crate.model.json](props/crate.model.json) and a lantern from
[props/lantern.model.json](props/lantern.model.json). [props/stall.model.json](props/stall.model.json)
places them:

```json
{"model": "props/crate.model.json", "position": [-0.7, 0.9, 0.3],
 "array": {"count": 3, "position": [0.7, 0, 0]}},
{"name": "lamp", "model": "props/lantern.model.json", "position": [0, 2.05, 0.3]}
```

Each model part is a node of the stall, as Godot instances a scene under a node: the crate's
nodes go under `crate`, `crate2` and `crate3`, and the lantern's under `lamp`. Names that would
clash are numbered (`box2`, `lid2`), and the crates share one copy of their materials. Edit the
crate file and the stall reloads with it, in a running game or at the next command.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` with `brief` describes the whole stall: its size, its parts (the crates' and
  lantern's meshes among the stall's own), its six materials and the points, each crate's `lid`
  among them.
- `frame.capture` samples the awning, the lantern's glow, a crate and the counter.
- `component.set` swings the lantern by its `lamp` node through `Skeleton3D`, and
  `skeleton.get` shows its glass swinging with it.
