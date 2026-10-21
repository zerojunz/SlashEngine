# Music scores

Music as text an agent edits a bar at a time. [night_market.score.json](music/night_market.score.json)
is eight bars in A minor at 92 beats per minute: strings and a bass from the start, then a flute
melody and drums in the `loop` section from bar 4. [calls.jsonl](calls.jsonl) writes it as a
Standard MIDI File with `score.export`, reads that file back into a new score with
`score.import`, and tries to export [sketch.score.json](music/sketch.score.json), whose melody has
one note too many for its bar. [results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- Each bar is a line of notes, such as `"A1:1.5 A2:0.5 E2:1 A2"`: a pitch, then `:` and a length
  in beats that carries over to the notes after it. `+` plays notes together, `r` rests, `@`
  sets one note's velocity and `;` starts a second voice in the same bar.
- A `"drums"` track names General MIDI's drums, such as `kick`, `snare` and `closed_hat`.
- The import gives back the same score: the same tracks, instruments, tempo, key and sections,
  each bar written as the shortest line that plays its notes.
- An error names the bar by its JSON Pointer and the note that doesn't fit, as
  `slash-cli scene validate` does for the file.
