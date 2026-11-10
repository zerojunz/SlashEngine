# Style memory

Keeping a game's look after weeks of autonomous work. The harbor at dusk
([levels/harbor.scene.json](levels/harbor.scene.json), 3D) and the title screen
([menus/title.scene.json](menus/title.scene.json), 2D: a gradient sky under a `CanvasModulate`)
are the looks to keep. [calls.jsonl](calls.jsonl) marks both as storyboard style references,
briefs the pier as an agent would before working on it, checks every level and menu, then plays
a later agent that turns the pier to noon and tints the title blue: `style.check` and `test.run`
catch both, and they pass again once the settings are put back. [results.jsonl](results.jsonl)
holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- `style.mark` captures a scene (or the open world, an image, a model or a material) as
  `style.check` will, keeps the frame as `storyboard/style/<name>.png` and writes the
  `style/<name>` entry: what was captured, the look's settings (lights, environment, materials,
  `CanvasModulate`, project.json's `rendering`) and its fingerprint: palette, values at the 5th,
  50th and 95th percentiles, contrast, saturation, warmth, edges, line weight and flatness.
  `links` say what else it governs: here every scene under `levels` and `menus`.
- `storyboard.brief` for the pier returns the harbor's reference, since its links cover the pier.
- `style.check` captures what each reference governs the way the reference was captured and
  reports each measure's `drift`; `over` names the measures past their tolerance (the entry's
  `tolerances`, else the defaults), and `changed` the settings that differ from the reference's,
  by JSON Pointer, for the entities both have.
- `test.run` with no paths runs it too, one failure a frame, pointing at the entry.
- With `"record": true`, checks are kept in `storyboard/style_journal.jsonl`, and a measure that
  drifted further at each of the last checks is reported as `creeping` before it fails. With
  `"review": true`, a vision model from `providers.json` compares each frame with the
  reference's (`storyboard.review`).
