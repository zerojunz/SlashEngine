# Character anatomy

Bones and muscles under a character's skin, after Chaos Flesh.
[characters/bram.character.json](characters/bram.character.json) is a strong, lean man: his
body's `muscle` and `fat` size the muscles `world::make_anatomy` places on his body's frames, and
`character.place` with `flesh` gives him a `Flesh3D`, whose muscles are rings of points joined
into tetrahedra that an XPBD solver steps each tick, ends pinned to their bones.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `character.place` puts Bram in [main.scene.json](main.scene.json) with his flesh, and
  `flesh.get` reads his left biceps and triceps at rest: full length, no bulge.
- Bending his left elbow (`Skeleton3D`'s `bone_rotations`) shortens the biceps to 0.86 of its
  length, activates it and makes it bulge, while the triceps stretches and the right arm's biceps
  stays as it was.
- Setting the `core` group's activation to 1 by hand flexes his abdominals.
- `frame.capture` reads his skin, and the `::muscles` and `::skeleton` forms of his recipe draw
  his muscles and his bones on his rig beside him; `model.inspect` names their materials (bone,
  muscle and tendon).
- In 2D, `character.place` makes a side-on cut-out with a `Flesh2D`; flexing its `arm` group
  with its muscles drawn over it (`view`), `flesh.get` lists its bones and its flexed biceps, and
  `frame.capture` sees the red of the muscles.
- `ragdoll.generate` fits each of his ragdolls, in 3D and on the cut-out, to his anatomy's bones
  rather than his skin (`"fit": "anatomy"`): each body holds the skull, ribs, pelvis or long
  bone under it and weighs what the flesh round it would, and `ragdoll.hit` strikes the cut-out's
  arm.
