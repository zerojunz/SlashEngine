# Damage that deforms

Damage short of breaking leaves marks, as Chaos Destruction's deformation does (Godot has no
destruction). A profile's `deform` settings say how far each kind of hit dents, holes and carves
its material, per point of damage dealt: blunt and impact hits dent, pierce holes, explosions
carve a round bite or dig a crater in an `HTerrain`, and cuts, pierces and explosions tear cloth.
Every material has defaults, so [profiles/steel.damage.json](profiles/steel.damage.json) only
names its material:

```json
{
  "format": 1,
  "health": 5000,
  "material": "metal",
  "name": "steel door"
}
```

and [profiles/concrete.damage.json](profiles/concrete.damage.json) caps a carve at 0.6 meters
with `"deform": {"max_carve": 0.6}`.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- [yard.scene.json](yard.scene.json) has a steel door, a concrete wall, soil ground and a flag
  that is a `SoftBody3D`. Two blunt hits near each other on the door make one deeper dent, and a
  pierce makes a hole through it. `entity.get` shows them as the door's `dents` and `carves`, in
  its mesh's space; the door draws and collides as what is left.
- An explosion carves a bite out of the wall's top corner. An explosion over the yard digs a
  crater into the ground's heights (`craters`), and a cut tears the flag's triangles in reach.
- `damage.state` counts each body's dents, carves and craters and each cloth's torn parts, and
  `events.read` has the `damaged` events, each saying how it `deformed`, and the `torn` event.
- `undo` takes back the step and the tear, and the flag is whole again.
- [cave.scene.json](cave.scene.json) is 2D: an explosion in the middle of a cave's tile map
  carves out the cells in reach, and a boulder loses a round bite to an explosion, then a pierce
  holes it. Its `CollisionPolygon2D` and `Polygon2D` are cut with Clipper2, holes slotted open
  to the edge, so it collides and draws as what is left.
- A cut in reach of the banner, a `Cloth2D`, tears the edges it crosses; any part torn free of
  its held points falls.
