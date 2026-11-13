# Character rig

A character's model has the whole rig of Godot's humanoid profile: fingers, eyes and a jaw, and a
twist bone along each limb, so hands grip, eyes look and forearms twist without candy-wrapping.
[characters/ada.character.json](characters/ada.character.json) is an ordinary recipe; the rig
comes with every character.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `character.place` with `look_at` puts Ada in 3D with three modifier children: `ada_twists`, a
  `CopyTransformModifier3D` giving each arm's and thigh's twist bone half its limb's roll, and
  `ada_look_left` and `ada_look_right`, Godot's `LookAtModifier3D` on each eye, turned toward
  the lantern within their limits.
- `model.inspect` lists the model's 64 bones' worth of parts and its animations: a relaxed hand,
  a fist, a grip and a point for each hand, and an open jaw.
- `skeleton.get` shows both eyes turned up and to the side toward the lantern.
- An `AnimationPlayer` playing `fist_right` from the character's own file closes the right hand:
  each finger's phalanges and the thumb curl toward the palm.
- A `Skeleton3D` rolls the right hand about its length; its forearm's twist bone takes half the
  roll, so the skin near the wrist turns with the hand and the elbow stays put.
- `frame.capture` draws her.
- In 2D, `character.place` makes a cut-out whose head has a `jaw` and an `eye` and whose hands
  have `fingers`, an `index` and a `thumb`. Setting the `Character2D`'s `jaw_open`, `hand_front`
  (`"point"`) and `hand_back` (`"grip"`) poses them on the next tick, and `look_at` moves the
  eye's iris toward the lantern; `entity.get` reads where each part went.
