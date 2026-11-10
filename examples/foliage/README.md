# Foliage

A meadow painted by brush, after Unreal's Foliage tool (Godot has none), in 3D and in 2D. A
`.foliage.json` file says what one kind of plant or rock is: what it draws, how thickly a brush
paints it, how it bends in the wind and parts for characters, whether it blocks and what
harvesting it gives. [grass.foliage.json](grass.foliage.json) is a blade of grass,
[boulder.foliage.json](boulder.foliage.json) a rock that blocks and gives stone, and
[tuft.foliage.json](tuft.foliage.json) a 2D tuft drawn from a gradient texture.

```json
{"format": 1, "kind": "grass", "mesh": "blade.model.json", "density": 6, "spacing": 0.15,
 "scale": [0.8, 1.2], "cull_distance": [30, 40],
 "wind": {"bend": 0.08, "sway": 0.3}, "interaction": {"radius": 0.6, "bend": 0.9}}
```

Painted instances live in a `Foliage3D` (or `Foliage2D`) component as groups of one type each,
seven numbers an instance in 3D (place, rotation, scale) and four in 2D (place, rotation, scale,
negative when mirrored). [main.scene.json](main.scene.json) has the ground, a breeze, a draft,
a hiker walking east through where the grass will be and a 2D kid running through the tufts;
the meadows themselves are made by the brush.

[calls.jsonl](calls.jsonl) paints and tends it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `foliage.paint` makes the `meadow3d` entity and paints grass in a circle at six a square
  meter, then boulders, kept apart from each other by their `spacing`. Painting the grass again
  with another seed fills the gaps, and spacing turns most of it away. A brush on `meadow2d`
  paints 2D tufts in pixels.
- `foliage.get` with `at` finds the boulders near a point, nearest first.
- `physics.raycast` hits the boulder's sphere and says which instance (`[group, index]`);
  `foliage.harvest` takes it, gives three stone in a `foliage_harvested` event, and the ray then
  hits nothing.
- `frame.capture` reads a blade and a tuft. After 90 ticks the hiker and the kid have walked
  through: `foliage.get` counts the instances still bent by them (`pushed`), which spring back
  by their type's stiffness and damping.
- `foliage.erase` clears a circle of grass, and `undo` puts it back.
