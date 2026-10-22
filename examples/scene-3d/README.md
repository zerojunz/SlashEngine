# Scene 3D

Godot's 3D nodes, as components in a scene. [main.scene.json](main.scene.json) is a table on a
lawn, seen through a `Camera3D` in a 160x90 window ([project.json](project.json)):

- `table` is a `BoxMesh`. Its `Transform3D` lists `crate`, `ball` and `ring` in `children`, so
  they sit on it and move with it, as child nodes of a `Node3D` do.
- Each mesh is a `MeshInstance3D` with one of Godot's primitive meshes (`BoxMesh`, `SphereMesh`,
  `TorusMesh`, `PlaneMesh` for the lawn) and a `StandardMaterial3D` for its color, roughness and
  metal.
- `sun` is a `DirectionalLight3D` shining down its -z. `lamp` is an orange `OmniLight3D`.
  `environment` is a `WorldEnvironment` with a sky-blue background and a soft ambient light.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[57,35],[80,33],[97,34],[80,48],[80,10],[80,75]]}
{"colors":[[0.933,0.204,0.137,1],[1,0.949,0.855,1],[1,0.769,0.129,1],[0.549,0.357,0.247,1],
 [0.451,0.651,0.902,1],[0.376,0.576,0.243,1]],"drawn":{"meshes":5},"height":90,"width":160}
```

- `world.query` with `MeshInstance3D` lists what draws.
- `frame.capture` draws the frame on the CPU, so it needs no GPU and gives the same pixels on
  every machine. `points` reads the crate, the ball's highlight, the gold ring, the table, the
  sky and the lawn. `drawn` counts the meshes.
- `component.set` moves the table 1.5 meters to the right, and its children go with it: the
  crate's old pixel now shows the lawn.
- `entity.create` adds a stone pillar, a `CylinderMesh` narrower at the top. Changing the
  `WorldEnvironment` turns the sky to sunset, and `path` saves the frame as `shots/table.png`.
- `ghost` has a `MeshInstance3D` but no mesh. `frame.summary` names the problem and the field
  to fix, and `slash-cli scene validate` would refuse it in a scene file.
