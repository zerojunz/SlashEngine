# Video sound

A video's sound played with its picture
([docs/reference/scene-format.md](../../docs/reference/scene-format.md#sound-in-videos)).
[main.cpp](main.cpp) makes `clip.mkv`: four colors, a quarter of a second each, written by the
engine's own AV1 encoder into a Matroska file (`video::encode_matroska`) with two audio tracks
of a square wave that halves in loudness with each color: 16-bit PCM, then the same at 48 kHz
encoded as Opus by libopus (`video::encode_opus`). In
[main.scene.json](main.scene.json) the `VideoStreamPlayer` `tv` plays it on the `Cutscenes` bus
of [default.bus_layout.json](default.bus_layout.json), 6 dB down, and the output is what
`audio.state` lists for `tv` and what `audio.capture` measures on that bus:

- `autoplay` starts the picture and its sound on the first tick; the sound is mixed a tick
  ahead of the frame shown, as the tick's share of samples is.
- A quarter of a second on, the second frame's sound is 6 dB quieter; `volume_db` takes off 10
  more.
- Paused, the sound fades out over a mix step, holds where it got to and the bus goes
  silent.
- Setting `stream_position` seeks the sound with the picture.
- At the end both stop. `audio_track` 1 plays the Opus track from the start; it peaks higher
  than the PCM did, since a lossy codec rings at a square wave's corners. An `audio_track` the
  clip doesn't have is the player's `error`.
