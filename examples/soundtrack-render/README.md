# Soundtrack render

A score played into stems a game plays, then checked by measurement instead of by ear.
[battle.score.json](music/battle.score.json) is a four-bar battle loop in D minor at 140 beats per
minute: bass, strings, brass stabs and drums. [calls.jsonl](calls.jsonl) renders it with
`soundtrack.render`, measures two of the stems with `audio.analyze`, and renders the brass again
on its own as a WAV file. [results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- The instruments are the engine's built-in General MIDI sounds, made in code, so the example
  needs no SoundFont file; `soundfont` names an `.sf2` file to use instead.
- Each track becomes an Ogg Vorbis stem in `music/battle/` with a `.audio.json` file giving its
  `bpm` and `beat_count`, and `music/battle.audio.json` plays the four together as a synchronized
  stream, so a game's `AudioStreamPlayer` plays the music and can fade one layer at a time.
- With `"loop": true` each stem is exactly 16 beats long, and what rings past the end (the
  brass's last note) is added onto the start, so the loop joins without a click.
- `audio.analyze` finds the drums at 140 beats per minute with a clean loop, and the strings in
  D minor, as the score says. (A drum stem's key means little.)
- `peak_db` is each stem's loudest sample, and the music's when every track renders; a stem or
  the mix above full scale is listed under `problems` with what to lower.
