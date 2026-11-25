# Video

AV1 video decoding, encoding and playback.

Status: current · Updated: 2026-11-20

Parts of this area are ported from [Godot Engine](https://godotengine.org) (MIT); see [THIRD_PARTY.md](../../THIRD_PARTY.md).

`engine/video` is video playback, with AV1 in place of Theora: `.ivf` files of AV1 temporal units
(`read_ivf`, `src/ivf.cpp`, which marks key frames from the OBU headers) load as `video::VideoFile`
assets, and `video::Decoder` (`src/decoder.cpp`) decodes a frame with dav1d, built from its C files
with no assembly and one thread (`cmake/Dependencies.cmake`), going on from the frame before or from
the key frame before a seek; `to_rgba` (`src/convert.cpp`) turns its YUV into RGBA8 with integer
math by the sequence header's matrix and range. `video::install` (called by `canvas::install`) adds
the `video` system (priority 1140, `src/video.cpp`), which plays each `Video` every frame and keeps
each player's decoder and frame by entity ID (`video::shown`). `ui::layout` sizes and draws a player
as a control, and the texture path `video:<id>` (`video::kTexturePrefix`) is that player's frame
wherever `CanvasRenderer` takes a texture, so sprites and materials show it too. `src/encoder.cpp`
is the engine's own AV1 encoder (`encode_frame`, `encode_ivf`): lossless key frames of palette
blocks with symbol adaptation off, written in the order dav1d's parser reads them and with integer
math only, so tests and examples make their videos in code. Lossy clips come from libaom, built for
the tools and tests only by its own CMake as a separate project (generic C, one thread;
`cmake/Dependencies.cmake` keeps its cache flags out of the engine's) into `slash::video_encoder`:
`video::encode_lossy` (`src/lossy_encoder.cpp`) codes key frames every `kf_interval` frames and
predicted frames between, whose bytes can differ a little between machines, so tests compare the
pictures dav1d decodes rather than bytes. `video.encode` (`src/commands.cpp`,
`slash::video_commands`, registered by slash-cli and slash-mcp) codes a project's image files, such
as `frame.capture`'s PNGs, that way and returns no bytes. `examples/video-playback` plays a clip in
a control and on a sprite, `examples/lossy-video` a libaom cutscene and `examples/captured-cutscene`
one captured from a scene with `frame.capture` and `video.encode`.

WebM and Matroska files (`.webm`, `.mkv`) load too: `src/matroska.cpp` reads their EBML
(`read_matroska`: the first `V_AV1` track, whose `CodecPrivate` sequence header the decoder
starts each key frame with as `VideoFile::config`, and the audio tracks, through laced blocks
and clusters of unknown size) and writes them (`write_matroska`, `encode_matroska`, with PCM
and Opus tracks). `decode_audio` turns a PCM or Opus track into float samples, mixing more than
two channels down as BS.775; `src/opus.cpp` decodes Opus with libopus, built as fixed point with
no intrinsics so samples are the same everywhere, after its `OpusHead`'s pre-skip and gain and
before the last block's `DiscardPadding`, turning family 1's Vorbis order of speakers into WAV's,
and `encode_opus` encodes it for tests and examples. The sound plays in the audio module, which links
the video module (not the other way round, since ui links video): `play_videos` in
`engine/audio/src/audio.cpp` starts each player's `audio_track` on its `bus` at `volume_db` and
`stream_position` through `video::playing`, follows `paused` and `speed_scale`, and starts it
again where the picture jumps, and `audio.state` lists it. `examples/video-sound` plays a
clip's sound on a bus.
