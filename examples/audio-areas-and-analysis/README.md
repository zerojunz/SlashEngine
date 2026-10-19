# Audio areas and analysis

Areas that change a sound's bus, and effects that measure and keep what plays, as Godot's.
[audio/default.bus_layout.json](audio/default.bus_layout.json) puts a `spectrum_analyzer`, a
`capture` and a `record` effect on Master, a `pitch_shift` an octave up on Voice and a `reverb`
on Cave. In [main.scene.json](main.scene.json) a hum plays on Voice, a bat's chirp is an
`AudioStreamPlayer2D`, and the cave is an `Area2D` with `audio_bus_override` that sends 2D sounds
inside it to Cave. [main.cpp](main.cpp) writes the sound files (no audio files are committed),
then plays the game the way an agent does over MCP (tools `audio_spectrum`, `audio_record`,
`audio_state`) or JSON-RPC. `expected.txt` holds each call and its reply.

- `audio.spectrum` reads the analyzer: dB per frequency range, ten octave bands by default.
  The 220 Hz hum shows up in the 355 to 710 Hz band while the pitch shift doubles it, and in a
  200 to 240 Hz range once `audio.set_bus` turns the shift off.
- The bat plays on Master until `component.set` moves it into the cave, then `audio.state`
  shows it on Cave, as Godot's `_get_actual_bus`. Its `area_mask` picks which areas count.
- `audio.record` starts and stops the record effect at the next tick and writes what it
  recorded to a `.wav` file, 16-bit as its `format` says.
- The capture effect keeps a tenth of a second for gameplay code to read
  (`audio::capture_effect`, as Godot's `get_buffer`); `audio.state` shows how many frames it
  holds and how many it dropped while nothing read it.
- Validate the bus layout with `slash-cli scene validate`, and see each effect's settings with
  `slash-cli schema bus_layout`.
