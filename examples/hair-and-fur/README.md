# Hair and fur

Grooms grown over models and simulated in the one physics step, after Unreal's Groom and its
Chaos-driven strand simulation (Godot has none), in 3D and in 2D. A `.groom.json` file says how
strands grow, how they draw and how they move: [long.groom.json](long.groom.json) is long hair
drawn as thin strands, [fur.groom.json](fur.groom.json) short fur drawn as shells, and
[mane.groom.json](mane.groom.json) a 2D mane in pixels.

```json
{"format": 1, "render": "strands", "guides": 24, "density": 4000, "segments": 6,
 "length": 0.22, "lift": 0.1, "comb": [0, -1, -0.4], "clumping": 0.5,
 "curl_radius": 0.004, "curl_turns": 2,
 "root_color": [0.18, 0.11, 0.06, 1], "tip_color": [0.42, 0.28, 0.15, 1],
 "simulation": {"stiffness": 0.15, "tip_stiffness": 0.02, "damping": 2}}
```

A `HairInstance3D` grows its groom over the model its entity's `MeshInstance3D` draws: the
surfaces drawn with its `section` material, here the `scalp` of
[head.model.json](head.model.json), or all of them, as on the furry ball of
[creature.model.json](creature.model.json). A few guide strands are spread over the surface
by area and simulated; the strands drawn follow the nearest guide, clumped and curled about it.
A `Hair2D` grows a 2D groom along its `root_line`, standing to the line's left as drawn, so the
doll's line, drawn right to left, hangs its mane down.

[main.scene.json](main.scene.json) has the head (with a sphere `CollisionShape3D` its hair
keeps off), the creature, the doll, a breeze and a 2D draft. [calls.jsonl](calls.jsonl) looks
at them the way an agent does over MCP or JSON-RPC, and [results.jsonl](results.jsonl) holds
the replies:

- `hair.get` says what grew: guides, strands, points along each guide and the area grown on,
  where the guides are (`bounds`, `tips`) and how far the tips are from the groomed shape
  (`tip_moved`). Before physics steps it, the hair is as groomed and `simulated` is false.
- After a second, gravity and the breeze have moved the head's tips about five centimeters
  downwind, and the draft has blown the mane over.
- `frame.capture` reads the skin under the head's hair, the fur and the mane.
- With the breeze off, the head is moved 20 centimeters: four ticks later its hair trails
  behind by up to 30 centimeters, and two seconds later it has settled back on its groomed
  shape. The short, stiff fur hardly moves at all.
