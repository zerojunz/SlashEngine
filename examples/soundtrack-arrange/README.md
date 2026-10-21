# Soundtrack arrange

A cue sheet turned into music a game steers, with no audio editor. [music.json](storyboard/music.json)
has four cues: the harbor, in two layers (strings alone when calm, drums added when a storm is
near), the storm, a fanfare that plays once as the storm passes, and a bell that rings when the
lighthouse is lit. Each names its music: two stems of
[harbor.score.json](music/harbor.score.json), or a whole score.
[calls.jsonl](calls.jsonl) renders the scores, arranges the cues into one interactive stream with
`soundtrack.arrange`, then plays it and switches clips the way gameplay code would, by setting
the player's `switch_to_clip` parameter. [results.jsonl](results.jsonl) holds the responses, line
for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- Before the scores are rendered, `soundtrack.arrange` still writes the music but lists each
  score under `problems`, with the call that makes it.
- A cue becomes a clip named after it; a cue with layers becomes a clip per layer, each a
  synchronized stream of its file and those below it (`harbor_tense` is in `layers`).
- Switching between a cue's layers keeps the music's place and waits only for the next beat:
  `harbor_tense` picks up where `harbor_calm` was.
- Switching to another cue waits for the next bar: asked for a second in, the storm starts at
  the harbor's next bar line, two and a half seconds in.
- The fanfare's `after` names the storm, so leaving the storm plays it on the way out, as a
  filler clip, before the harbor comes back.
- The bell plays once and names no cue to follow, so it plays when asked for, on the next beat,
  and then the music goes back to the clip it interrupted, where that clip was.
- `"initial": "cue/harbor"` starts the music with the harbor's lowest layer.
