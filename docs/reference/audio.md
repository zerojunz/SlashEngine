# Audio

Sound files, the mixer, buses and effects.

Status: current · Updated: 2026-11-20

`engine/audio` is sound, as Godot's AudioServer, on miniaudio's decoders. `.wav` files decode to
samples when they load (integer samples scaled as Godot's importer scales them), and `.ogg` and
`.mp3` files decode as they play. A `.audio.json` file (`$cli schema audio`) gives a sound
Godot's loop settings, or picks one of several at random each play, as Godot's
`AudioStreamRandomizer` (`src/stream.cpp`, with Godot 4.4's cubic resampler). It can also be
music, ported from Godot's `modules/interactive_music` (`src/music.cpp`): a playlist, layers
played together, or interactive clips that switch by its transitions when a player's
`switch_to_clip` parameter asks; players hand their `parameters` to their playbacks
(`Playback::set_parameter`), and the audio system writes `switch_to_clip` back as the music
moves on. Generators and polyphonic streams, as Godot's, are what gameplay code pushes frames to
or starts sounds on, through `audio::stream_playback` (`generator()`, `polyphonic()`). A
microphone stream plays the server's input, which records only with project.json's
`audio.input_enabled`: each tick the audio system pushes the tick's frames from an
`audio::InputDevice` (`audio::SdlInput` in a window, silence headless) or the sound
`audio.input` plays in its place, before mixing. Exports of such games ask for the microphone
(`export/CMakeLists.txt`'s `SLASH_GAME_MICROPHONE`).
`audio::Server`
(`src/server.cpp`) is the mixer: playbacks mix into buses in steps of 512 frames, with a 64-frame
lookahead, volume ramps and stop fades; each bus runs its effects (`src/effects.cpp`: 24 of Godot's
`servers/audio/effects`, each ported from the one of the same name, the pitch shift and spectrum
analyzer with Bernsee's FFT code under its own notice) and sends to an earlier bus, down to
Master. `Server::effect_instance` and `spectrum_analyzer`, `capture_effect` and `record_effect`
reach a bus's analyzer, capture buffer and recorder from C++. A `.bus_layout.json` file
(`$cli schema bus_layout`; project.json's `audio.bus_layout`) is Godot's `default_bus_layout.tres`.
`SoundPlayer`, `SoundPlayer2D`, `SoundPlayer3D`, `Listener2D` and
`Listener3D` are components, played by the
`audio` system (priority 1200, last), which mixes each tick's share of frames, so the same ticks
make the same sound, and hands them to a sink: `audio::SdlOutput` in a window, nowhere headless.
An `Zone2D` with `bus_override` sends the 2D players inside it whose `zone_mask` it
matches to its `audio_bus_name`, as Godot's `_get_actual_bus`. A 3D player fades by Godot's
attenuation models, pans, is muffled by a high shelf with distance and outside its emission
angle, shifts pitch with the Doppler effect (velocity tracked by ticks) and follows an `Zone3D`'s
bus and reverb bus, as Godot's `_update_panning`: each playback mixes into several buses at once
(`Server::set_bus_volumes`) through one high shelf (`Server::set_highshelf`), as Godot's mixer.
Sound mixes for project.json's `audio.speaker_mode` (Godot's speaker modes, up to 7.1; `auto`
follows the sink's device, `SdlOutput::speaker_mode()`): each bus has a buffer and its own
effects per speaker pair (`audio::Volumes`, a gain per pair), 3D players pan by Godot's Spcap,
an `SoundPlayer`'s `mix_target` picks its speakers, and mixes and sinks interleave the
pairs. Sessions keep ten seconds of each bus for `audio.capture`, which measures it (peak, RMS,
slices and each speaker in dB) or writes a `.wav` file; `audio.spectrum` reads a bus's `spectrum_analyzer` in
frequency bands, `audio.record` starts and stops its `record` effect and writes the take, and
`audio.state`, `audio.play`, `audio.stop` and `audio.set_bus` do the rest. Sessions put the
buses back with the world when a command fails or is a dry run. `audio.analyze`
(`src/analysis.cpp`) judges music without listening: loudness and true peak as EBU R 128 measures
them (libebur128), clipping, silence, tempo and first beat, key, octave bands and whether a loop
clicks where it joins, for a sound, a `.audio.json` sound as it loops or what a bus played;
`examples/music-analysis` measures a groove. Example sounds are made in code
without libm, so outputs match everywhere. `examples/audio` plays music and footsteps panned
around a listener, `examples/audio-areas-and-analysis` a bat in a cave's reverb, measured
by its spectrum and recorded, `examples/interactive-music` a score that follows a fight and
`examples/surround-sound` a bee flying round a listener in 7.1.
