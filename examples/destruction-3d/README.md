# Destruction 3D

Walls and crates that break into pieces. Godot has no destruction of its own, so this follows
Unreal's geometry collections: a `Destructible3D` body's mesh is cut into Voronoi cells with
Manifold's booleans (the same code as CSG shapes), and when it breaks each cell becomes a rigid
body of its own. [main.scene.json](main.scene.json) has a brick wall, a ball flying at it and a
crate on the grass.

```json
"wall": {"components": {
  "StaticBody3D": {},
  "Destructible3D": {"pieces": 6, "seed": 1, "damage_threshold": 10.0, "mass": 20.0,
                     "debris_lifetime": 3.0},
  "BoxMesh": {"size": [2.0, 2.0, 0.3]}, "MeshInstance3D": {}, ...
}}
```

A hit whose impulse (the two bodies' reduced mass times how fast they close) passes
`damage_threshold` breaks the body. The pieces share `mass` by volume, start with the body's
velocity, and the body that hit them goes on through. Each piece is `<id>_<n>`, with a convex
shape of its hull, a `FracturePiece3D` saying which cell of which cut it draws, and, with
`debris_lifetime`, is removed that many seconds later. The same `seed` cuts the same mesh the
same way on every machine.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `frame.capture` reads the wall, the crate and the sky at a few points.
- 50 ticks later the ball has broken the wall: `events.read` has the `broken` event at tick
  16 with its six pieces, and `world.query` shows their masses, which add up to the wall's 20.
- `frame.capture` at the same points now sees sky and grass where the wall stood, and a piece
  falling.
- `physics.break` breaks the crate, which has no `damage_threshold`, pushing its pieces out
  from below at 3 m/s.
- After 150 more ticks the wall's pieces are gone (their 3 seconds are up) and the crate's,
  with no lifetime, stay.
- `undo` twice takes back the last step and the break: the crate is whole again.
- A `PlaneMesh` has no inside, so `physics.break` refuses to break a sign drawn with one.
  `slash-cli scene validate` reports the same.
