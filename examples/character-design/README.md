# Character design

A cast of characters made as recipes, after Unreal's MetaHuman Creator (Godot has none), in 3D
and in 2D. A `.character.json` file gives a body's measures, a face, hair, clothing in layers and
props: [hero.character.json](characters/hero.character.json) is a player in a shirt, trousers,
boots and a cape that hangs as cloth, with a sword ([sword.model.json](props/sword.model.json))
in their right hand.

```json
{"format": 1, "name": "Hero", "role": "player",
 "body": {"height": 1.82, "build": 0.7, "shoulders": 1.15, "skin": [0.784, 0.596, 0.439]},
 "hair": {"style": "short", "color": [0.063, 0.063, 0.063]}, "face": {"beard": "stubble"},
 "clothing": [{"kind": "shirt", "color": [0.627, 0.125, 0.125]}, ...,
              {"kind": "cape", "color": [0.125, 0.251, 0.627], "cloth": true, "length": 0.8}],
 "props": [{"model": "props/sword.model.json", "point": "right_hand",
            "rotation": [1.5708, 0, 0]}]}
```

[hero_double.character.json](characters/hero_double.character.json) `extends` the hero and
changes only the cape, so it looks almost the same. The storyboard's
[design.json](storyboard/design.json) has the cast's `character_rules`: outlines at least 0.08
apart, main colors at least 6 apart, heights from 1.5 to 1.9 meters, one prop each and the
colors of the `palette/cast` in [style.json](storyboard/style.json).

[calls.jsonl](calls.jsonl) works on the cast the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `character.check` finds the double too like the hero: their outlines are the same and their
  colors only 1.62 apart. `measures` adds how far apart each pair is.
- `character.generate` writes Mira and Oren: from 24 seeded candidates within the rules each,
  it keeps the one least like the rest of the cast, and says which character is nearest and how
  far. Checked, they keep the rules.
- `character.place` puts the hero and Mira in the 3D scene: a `MeshInstance3D` drawing each one's
  rigged model and a `Cloth3D` on the hero's cape. `model.inspect` reads the hero's model: one
  skinned mesh with a material per thing it draws, and the points props hang from.
- Placed at two numbers, Oren is a 2D cut-out: after a tick the `character_2d` system has made a
  bone entity with a `Polygon2D` each, and `frame.summary` sees them drawn, the far arm and leg
  behind the body.
- A recipe that doesn't exist is an error at `/recipe`.
