# Microphone

What the microphone records, as Godot's `AudioStreamMicrophone` and `AudioServer` input calls.
[main.cpp](main.cpp) writes a song (no audio files are committed) and adds a gameplay system,
then plays the game the way an agent does over MCP (tools `audio_input`, `audio_state`,
`audio_spectrum`) or JSON-RPC. `expected.txt` holds each call and its reply.

- [project.json](project.json) turns on `audio.enable_input`, as Godot's
  `audio/driver/enable_input`, and gives `audio.input_usage_description`, which exports put in
  the macOS and iOS apps for the system to show when it asks the player for the microphone.
  Android exports ask for `RECORD_AUDIO`.
- [audio/mic.audio.json](audio/mic.audio.json) is a `microphone`. The `voice` player in
  [main.scene.json](main.scene.json) plays it on the muted `Mic` bus
  ([audio/default.bus_layout.json](audio/default.bus_layout.json)), as Godot's microphone demo
  does, so the bus's spectrum analyzer and record effect hear the player but the speakers don't
  play them back. A playing microphone turns input on and opens the microphone.
- The `lanterns` system, plain C++, reads the same input with the server's `input_frames` and
  lights a lantern (a `lantern_lit` event) for each half second of loud singing.
- Headless there is no microphone, so the input is silent. `audio.input` plays a sound into it
  in place of the microphone, from the next tick: here the song, quiet, then loud for 1.2
  seconds, then quiet. The microphone player plays it 50 ms late.
- `audio.state`'s `input` shows that input is on, what feeds it and its peak over the last tick.
