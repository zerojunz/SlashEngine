# Destruction profiles

How things take damage and break, written down once per kind of thing. Godot has no
destruction, so this follows Unreal's Chaos Destruction: a `.damage.json` profile names a
material, whose defaults fill in what the file leaves out, its health, what resists which kind of
damage, the damage states that change its looks, how it breaks and whether its pieces stand
linked. A `Destructible3D` or `Destructible2D` names a profile.

[profiles/brick.damage.json](profiles/brick.damage.json) is a garden wall:

```json
{
  "format": 1,
  "name": "garden wall",
  "material": "brick",
  "health": 120,
  "states": [
    {"below": 0.6, "tint": [0.8, 0.75, 0.7, 1.0]},
    {"below": 0.3, "tint": [0.6, 0.55, 0.5, 1.0]}
  ],
  "fracture": {"pattern": "clusters", "pieces": 10, "seed": 4, "levels": 2, "level_pieces": 3, "fine": 0.9},
  "anchors": "bottom",
  "debris": {"lifetime": 0, "push": 1}
}
```

Brick is `connected` by default, so its pieces stand frozen, linked to the pieces they share
faces with, the bottom ones anchored to the ground. Only the pieces near a hit come loose; the
rest hold until the load through their links is more than brick's `strength` can carry, as
Chaos's connection graph sends strain to its anchors.
[profiles/glass.damage.json](profiles/glass.damage.json) is a window that shatters in rings and
spokes (`radial`), and in 2D [profiles/crate.damage.json](profiles/crate.damage.json) is a wooden
crate that splinters, which [profiles/old_crate.damage.json](profiles/old_crate.damage.json)
`extends` with less health and more fear of fire.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `damage.check` reads the wall's profile as brick fills it in, and how many hits of 25 of each
  kind it takes: five blunt, four explosive, and never by fire.
- [main.scene.json](main.scene.json) has the wall and the window. A blunt hit of 4 takes 6 off
  the window (glass takes blunt hits one and a half times over) and puts it in its first damage
  state, which `damage.state` reports; a pierce of 4 then shatters it into twelve pieces.
- Two hits on the wall's top corner: the first leaves it at 70, tinted; the second, explosive,
  breaks it into ten pieces. After 30 ticks `damage.state` shows its group: the two pieces near
  the hit came loose and the rest still stand on their anchors.
- An explosion at the wall's foot damages every piece in reach, less with distance: pieces
  break again into three (their second level), and pieces that can break no more fall loose.
- `events.read` has the `damaged` and `broken` events, and `undo` twice takes back the last step and
  the explosion.
- [crates.scene.json](crates.scene.json) has two crates in 2D. `damage.check` with no profile
  reads every profile the scene names. Fire burns the old crate at once; a blunt blast breaks the
  other, each piece a `RigidBody2D` with a `CollisionPolygon2D` cut from the crate's outline and
  a `Polygon2D` drawing its part.
- `physics.break` breaks a 2D piece by hand, and a kind of damage that doesn't exist is refused.
