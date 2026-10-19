# Generated audio

Sound a game makes as it runs, as Godot's `AudioStreamGenerator` and `AudioStreamPolyphonic`.
Each is an `.audio.json` file that an `AudioStreamPlayer` plays like any sound. [main.cpp](main.cpp)
writes the hit sound (no audio files are committed) and adds a gameplay system, then plays the
game the way an agent does over MCP (tools `audio_play`, `audio_state`, `audio_capture`) or
JSON-RPC. `expected.txt` holds each call and its reply.

- [audio/synth.audio.json](audio/synth.audio.json) is a `generator` at 22050 frames a second.
  The `synth` system, plain C++, gets the player's playback with `audio::stream_playback` and
  pushes a square-wave tune into it each tick with `push_buffer`, as many frames as
  `frames_available` says fit. Gameplay systems run before the audio system, so each tick's
  frames are there before they are mixed.
- `audio.state` shows a generator's `frames_available` and `skips`: mixes that found too few
  frames and played silence instead. The first tick skips (the voice starts after the systems
  ran), and so does every mix once the tune is over and nothing is pushed.
- [audio/sfx.audio.json](audio/sfx.audio.json) is `polyphonic`: up to `polyphony` streams at once
  on one player. `audio.play` with `stream` plays a sound on it at its own `volume_db` and
  `pitch_scale`, starting the player if it isn't playing. In C++, a playback's `polyphonic()`
  has Godot's `play_stream`, `set_stream_volume`, `stop_stream` and the rest.
- Generators, polyphonic streams and music are monophonic, as in Godot: playing the player again
  starts it over rather than adding a voice.
- `audio.capture` measures Master slice by slice: the tune, the two hits over it, then silence.
