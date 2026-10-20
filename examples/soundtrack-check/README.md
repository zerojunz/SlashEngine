# Soundtrack check

Music checked without listening. The project has three looping cues written as scores, the
harbor and the storm in E minor and the reef in B flat major, a bell, and
[chime.audio.json](music/chime.audio.json), which plays the bell's stem but says it lasts 16
beats. [music.json](storyboard/music.json) asks the loops for -20 LUFS, give or take 3.
[calls.jsonl](calls.jsonl) renders the scores, arranges the cues into interactive music,
checks it all with `soundtrack.check`, checks the chime alone with `paths`, and runs
`test.run`. [results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- The chime's 16 beats at 96 bpm last 10 seconds, but its sound lasts 3.34, so music timed by
  its beats would wait for beats that never come. That is an error, with the beats the sound
  holds.
- The reef is played softly, so it measures -25.9 LUFS, outside the target, and the check says
  how much to raise it. Each piece is measured as it plays: a clip of interactive music, a
  playlist's stream, or music nothing else plays.
- The arranged music cuts straight between the reef and the other two cues, and B flat major is
  three steps from E minor round the circle of fifths. Those are warnings: a filler clip between
  them, or a nearer key, would join them. Keys come from the scores the music was rendered from,
  or from measuring it.
- `test.run` with no tests still runs the project's checks, and fails on the music's errors but
  not its warnings.
