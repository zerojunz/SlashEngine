# Paths

A cart lapping a rail over a hill, built from Godot's `Path3D` and `PathFollow3D` and a
`CSGPolygon3D` in path mode, with a guard walking a `Path2D`. [main.scene.json](main.scene.json)
has the track: a closed curve of four points whose handles round it into an oval, the far
point raised.

```json
"track": {"components": {
  "Path3D": {"closed": true, "points": [
    {"in": [2.0, 0.0, 0.0], "out": [-2.0, 0.0, 0.0], "position": [0.0, 0.3, 3.0]},
    ...
  ]},
  "Transform3D": {"children": ["cart"]}
}}
```

The cart is the track's child, with a `PathFollow3D` that places it `progress` meters along
the curve, turned to face the way it goes (`"rotation_mode": "oriented"`). Nothing moves it but
an `AnimationPlayer` playing [cart.anim.json](cart.anim.json), whose one track runs
`PathFollow3D/progress` up by 4 meters a second; the `path_follow` system wraps it round the
loop and puts the cart there each tick. The rail is a `CSGPolygon3D` whose `mode` is `"path"`:
its polygon, a flat bar under the curve, is pushed along the track named by `path_node`, a ring
every `path_interval` meters, and `path_joined` closes it into a loop.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `path.get` returns the track's length, where it is and which way it faces at each quarter,
  with its up vectors, and the nearest point of the curve to a spot behind the hill.
- `csg.get` on the rail: its triangles, bounds and volume, the bar's area times the track's
  length.
- After 30 ticks the cart is 2 meters round, and `frame.capture` reads the cart, the rail in
  front and on the hill, the grass and the sky.
- The hill goes up with `component.set` on the track's points. The rail follows, `csg.get`
  shows it taller, the cart keeps to the new curve and the rail is now higher on screen.
- The guard's `PathFollow2D` is put 120 pixels along the patrol, turned along it, and
  `path.get` with `global` gives a point of the patrol in the world's space.

`slash-cli scene validate` reports followers that aren't children of a path, and a rail whose
`path_node` names no path.
