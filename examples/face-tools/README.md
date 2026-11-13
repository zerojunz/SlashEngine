# Face tools

Agents design a cast's faces against the game's rules, without looking at them: `face.check`
measures how alike faces are, `face.generate` writes a new face from a brief, and
`face.portrait` draws one to a PNG file to look at. The rules are a `face_rules` entry in the
storyboard (Godot has none; after MetaHuman Creator's presets and the character rules of
[character-design](../character-design/)).

- [design.json](storyboard/design.json) holds `face_rules/cast`: faces at least 0.6% apart in
  shape and 2% in profile, the `stylized` look only, scars, freckles, moles and paint but no more
  than two marks a face, and a nose no longer than 0.8. It covers the `characters` and `faces`
  folders.
- [captain.face.json](faces/captain.face.json) and [mate.face.json](faces/mate.face.json) are near
  twins (the captain's scar shows on neither measure), and
  [ana.character.json](characters/ana.character.json) is a character whose own face is designed.

`face.check` sculpts every face the rules cover (face files, and recipes whose face is
designed; presets that other faces extend, blend or come `from` are left out) on the same head,
casts 990 rays at the front to measure its shape and reads 96 rows of its side profile, and
gives each pair's `shape` and `profile` differences, in percent of the head's size, as the mean
of the tenth of samples that differ most. The captain and the mate are 0.19% and 0% apart, so
both checks fail, with what to change:

```json
{"file": "faces/captain.face.json",
 "message": "its face is too like faces/mate.face.json's: 0.19% apart, and face_rules/cast asks 0.6%; change its sculpt controls or carves"}
```

`face.generate` reads a brief for traits (old, stern, sly, kind, round, ...), features with a
word before them (a strong jaw, high cheekbones, a button nose), marks, beards and styles, makes
seeded candidates around what it read within the rules' ranges, and keeps the first that is far
enough from the cast (else the farthest). It says which words it understood and which it
ignored, and how near the nearest face is. Moss's brief asks for a tattoo, which the rules
don't allow, so it is `marks_left_out`. Checking the three new faces passes.

`face.portrait` draws a face file or a character's face in a session of its own: `front`,
`three_quarter` or `side` in 3D, or the 2D `cutout`, with `expressions` (ARKit's names) set.
Brann frowns from the front, and Ana smiles as a cut-out:

```json
{"face": "faces/old_brann.face.json", "out": "portraits/brann.png", "view": "front",
 "size": [128, 128],
 "expressions": {"browDownLeft": 0.6, "browDownRight": 0.6, "mouthFrownLeft": 0.4, "mouthFrownRight": 0.4}}
```

`test.run` runs `face.check` over the project, so the twins fail it until one changes.
