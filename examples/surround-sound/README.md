# Surround sound

Up to 7.1 speakers, as Godot's speaker modes. [project.json](project.json) sets
`audio.speaker_mode` to `surround_71`, so every bus mixes eight speakers: front left and right,
center, LFE, rear left and right, and side left and right. In [main.scene.json](main.scene.json)
a bee flies round the listener once every four seconds, moved by
[fly.anim.json](fly.anim.json). [main.cpp](main.cpp) writes the sound files (no audio files are
committed), then plays the game the way an agent does over MCP (tools `step`, `audio_state`,
`audio_capture`) or JSON-RPC. `expected.txt` holds each call and its reply.

- The bee's `AudioStreamPlayer3D` pans over the speakers by Godot's Spcap: each speaker's gain
  grows as it faces the sound, so the buzz moves from the front speakers to the right, the rear
  and the left as the bee goes round. `audio.state` shows `volume` (the front pair) and
  `speakers`: the `center`, the `lfe` and the `rear` and `side` pairs.
- The music's `AudioStreamPlayer` has `mix_target` `surround`, so it plays on every speaker; the
  announcer's has `center`, so it plays on the center speaker and the LFE.
- `audio.capture` measures each speaker's level, and with `path` writes a `.wav` file with a
  channel for each speaker.
- `auto`, the default `speaker_mode`, mixes for the output device's speakers. SDL folds 7.1 for a
  device with fewer, so the game plays on headphones too.
