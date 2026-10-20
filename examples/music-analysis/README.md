# Music analysis

Judging music without listening. [main.cpp](main.cpp) writes a four-bar groove in D minor at
100 beats per minute (kick, hi-hat, bass and chords, made in code; no audio files are
committed), a copy turned up until it clips, and two `.audio.json` files that loop it: one over
the whole groove and one that ends a tenth of a beat early. Then it measures each with
`audio.analyze`, as an agent does over MCP (tool `audio_analyze`) or JSON-RPC, and what the
Master bus played while the groove looped. `expected.txt` holds each call and its reply.

- `path` names a sound file (`.wav`, `.ogg`, `.mp3`) or a `.audio.json` file with a `source`,
  measured as that file loops it. `bus` measures what a bus played instead, up to the ten
  seconds sessions keep.
- Loudness is EBU R 128's, measured with libebur128: `lufs` (integrated), `loudness_range` and
  `max_short_term_lufs` (the loudest three seconds), with `true_peak_db` between samples,
  `peak_db` and `rms_db`.
- `clips` counts runs of samples at full scale; `silence` gives the quiet start and end, and
  gaps of half a second or more.
- `bpm`, `beat_offset` (the first beat, in seconds) and `beats` (the length in beats) come from
  where notes start, so an agent can check a file's `bpm` and `beat_count` against its sound.
  `key` comes from the notes' pitch classes against Krumhansl and Kessler's key profiles.
- `bands_db` is each octave band's share of the energy, centred on 31.5, 63, 125, 250, 500 Hz
  and 1, 2, 4, 8 and 16 kHz, in dB.
- `loop` says how the loop joins: its length in seconds and beats, the jump across the join,
  the level after it against before it, and whether it `clicks`.
- `problems` lists what needs fixing: clipping, a true peak above -1 dBTP, a loop that clicks
  or a silent sound.
