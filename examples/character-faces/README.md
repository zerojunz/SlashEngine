# Character faces

Faces as recipes, after Unreal's MetaHuman Creator (Godot has none). A face is sculpted from 46
named controls (each -1 to 1, 0 for an average adult's), carves at its 27 landmarks, blends of
other faces and a style, and the same sculpt draws both a character's 3D head and its 2D
cut-out's side-on profile.

- [elder.face.json](faces/elder.face.json) is a preset: hollow cheeks, hooded eyes, a strong brow
  ridge, a longer nose, a thin upper lip and thick grey brows.
- [stern.face.json](faces/stern.face.json) `extends` it, a merge patch that tilts the brows down,
  angles them and pushes the chin out.
- [round.face.json](faces/round.face.json) is another preset: a wide face, full cheeks and a
  short chin.
- [ida.character.json](characters/ida.character.json) takes her face `from` the stern elder, `blend`s
  in 30% of the round face, carves a crease into her left cheek and is a little asymmetric:

```json
"face": {"from": "faces/stern.face.json",
         "blend": [{"face": "faces/round.face.json", "weight": 0.3}],
         "carves": [{"kind": "crease", "at": "cheek_left", "length": 0.12, "angle": 70, "depth": 0.6}],
         "asymmetry": 0.3, "seed": 7, "eyes": [0.25, 0.4, 0.3]}
```

- [pip.character.json](characters/pip.character.json) is sculpted `"style": "toon"`: big eyes,
  a small upturned nose and little fine detail.

[calls.jsonl](calls.jsonl) works with them the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `character.place` puts Ida and Pip in the 3D scene. `model.inspect` reads Ida's model: the
  sculpted head's skin, lips, the inside of the mouth and brows are materials of their own.
- `skeleton.get` reads each one's eye and jaw bones, which the sculpt places: Pip's toon eyes
  are further apart than Ida's.
- `frame.capture` samples each one's pupil and Ida's cheek.
- Placed at two numbers, Ida is a 2D cut-out whose head is the sculpt's profile; its eye sits
  where the sculpt's near eye is.
- A face with a misspelled control is an error naming the control it meant.
- Every sculpt carries ARKit's 52 expression shapes, made from it. Setting Ida's
  `MeshInstance3D.blend_shapes` makes her smile, squint her cheeks and close her eyes, which
  the second `frame.capture` sees as skin over the pupil; in 2D her `Character2D`'s
  `expressions` open her jaw, close her eyes and raise her brows.
