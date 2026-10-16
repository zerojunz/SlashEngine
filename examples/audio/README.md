# Audio

Sound as Godot's AudioServer. [main.scene.json](main.scene.json) has music on an
`AudioStreamPlayer`, footsteps on an `AudioStreamPlayer2D` and an `AudioListener2D` to hear them
from. [project.json](project.json) names [audio/default.bus_layout.json](audio/default.bus_layout.json),
which sets up the buses: Music at -6 dB with a low pass filter turned off, and Sfx with a hard
limiter, both mixing into Master. [audio/steps.audio.json](audio/steps.audio.json) takes turns
between two footstep sounds, as Godot's `AudioStreamRandomizer`. [main.cpp](main.cpp) writes the
sound files (no audio files are committed), then plays the game the way an agent does over MCP
(tools `audio_state`, `audio_play`, `audio_set_bus`, `audio_capture`) or JSON-RPC.
`expected.txt` holds each call and its reply.

- Mixing follows the ticks, not the sound card: each tick of 1/60 s mixes 735 frames at
  44100 Hz, so the same ticks always make the same sound and an agent can measure it.
- `autoplay` starts the music at the first tick. `audio.play` starts a player at the next
  tick, as Godot's `play()`, and `audio.stop` stops it.
- An `AudioStreamPlayer2D` fades over `max_distance` and pans by where it is on the screen
  from the listener, at half the project's `audio.panning_strength_2d`, as Godot's
  `_update_panning`. `audio.state` shows each player's `volume` after that, left and right.
- A player emits `finished` when its sound ends, as Godot's signal.
- `audio.state` gives each bus's peak in dB over its last mix step, `null` when silent.
  `audio.capture` measures what a bus played over the last seconds (sessions keep ten): peak
  and RMS dB, and the peak of each slice of that time. With `path`, it writes a `.wav` file.
- `audio.set_bus` changes a bus's volume, mute, solo, effect bypass or one effect's `enabled`
  while the game runs, as Godot's `AudioServer` setters; the layout file stays as it is.
- Validate the bus layout and stream files with `slash-cli scene validate`, and see every
  effect and its settings with `slash-cli schema bus_layout`.
- In a window, `slash-cli run` and exported games play the mix through SDL's default output
  device.
