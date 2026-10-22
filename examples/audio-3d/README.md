# Audio 3D

3D sound as Godot's `AudioStreamPlayer3D` and `AudioListener3D`. In
[main.scene.json](main.scene.json) a car drives past the listener along a road 10 meters ahead,
moved by [car.anim.json](car.anim.json) at 20 meters a second, and a stream trickles in a cave.
[project.json](project.json) names [audio/default.bus_layout.json](audio/default.bus_layout.json):
Sfx with a limiter, Cave with a low pass filter and Echo with a reverb. [main.cpp](main.cpp)
writes the sound files (no audio files are committed), then plays the game the way an agent does
over MCP (tools `step`, `audio_state`, `audio_capture`) or JSON-RPC. `expected.txt` holds each
call and its reply.

- The listener is the current `AudioListener3D`, facing its -z, as everything in Godot does.
  Without one, 3D sounds are heard from the current `Camera3D`.
- The car's engine fades with distance by its `attenuation_model` (`inverse_distance` from its
  `unit_size` of 5 meters) and fades out at `max_distance`, as Godot's `_update_panning`. It
  pans by where it is around the listener, at the project's `audio.panning_strength_3d`, and a
  high shelf muffles it as it fades: `audio.state` shows `volume` (left and right) and
  `filter_db`.
- Its `doppler_tracking` measures how fast it moves, so it plays higher coming and lower going:
  `audio.state` shows the `pitch_scale` it plays at.
- The cave is an `Area3D` with `audio_bus_override` and a reverb bus, so the trickle inside it
  plays on Cave and sends `reverb_bus_amount` of itself to Echo; `audio.state` lists it in
  `sends`, and `audio.capture` measures what Echo's reverb played.
