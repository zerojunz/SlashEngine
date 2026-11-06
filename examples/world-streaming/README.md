# World streaming

A world too large to keep whole, kept in pieces. Godot has no world streaming, so this follows
Unreal's World Partition: each part of the world is a scene file of its own, and a
`StreamedScene3D` loads it while a `StreamingSource3D` (a player or a camera) is near and
unloads it when every source has gone. [main.scene.json](main.scene.json) holds only a player and
three cells along a road; the farms, the mill and the tower are in
[cells/](cells/).

```json
"middle": {"components": {
  "StreamedScene3D": {"load_radius": 30.0, "scene": "cells/middle.scene.json",
                      "size": [100.0, 20.0, 100.0], "unload_margin": 10.0},
  "Transform3D": {"position": [100.0, -0.5, 0.0]}
}}
```

A cell loads when a source comes within `load_radius` of its box, and unloads only when every
source is `unload_margin` farther than that, so a player standing on the edge doesn't load and
unload it each tick. Its scene's root becomes `<id>_scene` under the cell, and the scene's other
entities `<id>_scene/<their id>`, as an instanced scene's do.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `streaming.state` lists the cells, whether each is loaded and how far the player is from
  each. Nothing loads until the world steps.
- After a tick, the west cell (the player stands in it) is loaded and the middle one, 50 meters
  off, isn't. `world.query` finds the barn the west cell brought.
- The player walks to 70 meters: the middle cell loads, and the west one, 20 meters behind,
  stays. At 150 meters the east cell loads and the west one, 100 meters behind, unloads.
  `events.read` has a `streamed_in` or `streamed_out` event for each.
- `physics.raycast` lands on the east cell's ground, which collides once it is loaded.
- A cell whose scene file doesn't exist is a `stream_failed` event, once, saying why; walking
  there unloads the others. `slash-cli scene validate` reports such a cell too.
- `undo` takes back the walk, the step and the broken cell, and the world is as it was at 150
  meters.
