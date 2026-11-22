# Watch window

`slash-cli watch` opens a window on the session an agent is working in: its world as it is now,
redrawn whenever the agent changes it, with a bar naming the session and the agent's last command
and an outline round each entity the agent's commands touched, green round what it made and amber
round what it changed, fading a few seconds after
([watching agents](../../docs/reference/watching-agents.md#the-watch-window)).

In [main.cpp](main.cpp) an agent's session journals its commands and serves itself to watchers,
as slash-mcp's do. A watcher follows it as the window does, with `server::Follower` (the
journal's new lines, and the world through the session's watch server), draws that world on the
software device with a `debug::WatchView` over it, and prints the bar and where each outline went:

```text
agent: entity.create {"components":{"Polygon2D":{...},"Transform2D":{"position":[280,252]}},"id":"crate"}
agent: component.set {"components":{"Transform2D":{"position":[40,320]}},"id":"floor"}
window at 2.0 s: Watching 20261122-140000-w1n2  slash-mcp  |  component.set {...}  ~floor(Transform2D)
  green outline round crate at [277, 249] 54 x 54, 100% strong
  amber outline round floor at [37, 317] 566 x 26, 100% strong
window at 6.0 s: ...
  green outline round crate at [277, 249] 54 x 54, 50% strong
  amber outline round floor at [37, 317] 566 x 26, 50% strong
```

- The watcher never changes the agent's world: its requests run between the agent's commands,
  read only, and go in no journal.
- A 3D entity's outline covers its meshes and shapes, and its children's, as the current camera
  sees them; an entity that draws nothing gets a small square at its origin.
- `slash-cli watch --text` prints the journal's lines instead, as before.
