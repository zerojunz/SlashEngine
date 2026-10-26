# Scene assembly

`entity.place` puts things in a level with constraints instead of exact coordinates: dropped
onto what is below them, moved aside from what is in the way, and spread along a path. An agent
building a level of library assets says where each should roughly go and reads back where it
landed.

[main.scene.json](main.scene.json) is a yard: a ground plane, a stone platform, a fence path and
the props still stacked out of sight under the ground. [calls.jsonl](calls.jsonl) drives it the
way an agent does over MCP or JSON-RPC, and [results.jsonl](results.jsonl) holds the replies:

- Three crates are all asked to go to the same point in the air. The first drops onto the
  ground, as Godot's editor snaps a node to the floor; the other two are each moved aside to
  the nearest free spot, 0.8 meters away, where their boxes only touch.
- The lamp, asked to go above the platform, stands on the platform.
- Five fence posts are spread evenly along the fence's `Path3D`, from end to end, each turned to
  face along it (`"rotate": true`): the posts on the second side are turned a quarter turn.
- A kite asked to go past the edge of the yard has nothing below it. With `dry_run`, the reply
  says so and nothing moves.
- A crate asked to go onto the platform beside the lamp, with `"on": ["platform"]`, stands on
  the platform rather than on the lamp and is moved aside on the platform, keeping the 0.1 meter
  `margin`.
- `frame.capture` samples the crate on the ground, the lamp, the crate beside it and the grass.

What is below and what is in the way are the meshes and static bodies a navigation bake reads,
so a floor needs no collision shape; things are kept apart by their bounding boxes.
