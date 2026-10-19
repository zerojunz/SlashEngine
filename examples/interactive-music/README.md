# Interactive music

Music that follows the game, as Godot's `AudioStreamInteractive`, `AudioStreamSynchronized` and
`AudioStreamPlaylist`. Each is an `.audio.json` file that an `AudioStreamPlayer` plays like any
sound, and the player's `parameters` steer it. [main.cpp](main.cpp) writes the sound files (no
audio files are committed), then plays the game the way an agent does over MCP (tools
`component_set`, `audio_state`, `entity_get`) or JSON-RPC. `expected.txt` holds each call and its
reply.

- [audio/score.audio.json](audio/score.audio.json) has three `clips`. Explore and combat are
  `.audio.json` sounds that give their `.wav` a `bpm` of 120 and a `beat_count` of 8, so
  transitions can wait for a beat or a bar; victory is a sting that goes on to explore at its
  end (`auto_advance`).
- Setting the player's `switch_to_clip` parameter with `component.set` asks for a clip. The
  `transitions` say when it changes and how: explore waits for its next bar and crossfades into
  combat over a beat, combat fades out under victory from its next beat, and anything going to
  explore starts it at the end of the last clip and fades it in.
- `audio.state` names the clip playing. `switch_to_clip` follows the music: while victory plays
  it already names explore, the clip it is heading to, so gameplay code and agents can read it.
- [audio/layers.audio.json](audio/layers.audio.json) plays drums and a melody together from the
  same start. The player's `stream_1_volume_db` parameter keeps the melody at -80 dB until
  `component.set` turns it up.
- [audio/radio.audio.json](audio/radio.audio.json) is a playlist: each stream plays to its end,
  or for its `beat_count` beats, then fades out over `fade_time` seconds under the next, and the
  list starts over (`loop`, true by default; `shuffle` plays it in a new order each time).
- Validate the files with `slash-cli scene validate`, and see every key with
  `slash-cli schema audio`. A misspelled clip name is an error with a suggestion.
