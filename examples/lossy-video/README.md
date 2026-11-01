# Lossy video

A cutscene coded lossy with libaom
([docs/reference/scene-format.md](../../docs/reference/scene-format.md#video)).
[main.cpp](main.cpp) makes 48 frames of a sunset, a sky that darkens as the sun sinks with a
ball rolling across it, which no palette of 8 colors a block can keep, and codes them twice:

- `video::encode_ivf`, the engine's own encoder, makes every frame a lossless key frame of
  palettes.
- `video::encode_lossy` (`slash/video/lossy_encoder.hpp`, linked from
  `slash::video_encoder` in the tools and tests) has libaom code key frames where `kf_interval`
  asks, here every 24 frames, and every other frame as changes to the ones before, at
  `cq_level` 28. The clip is under a tenth of the palette one's size, and dav1d decodes each
  frame within 3 levels of its source on average.

Then it writes `sunset.ivf` and opens [main.scene.json](main.scene.json), where the
`VideoStreamPlayer` `cutscene` plays it stretched over the whole window. It checks a few pixels
`frame.capture` draws against the source as the video plays, seeks to 1.5 seconds (decoding on
from the key frame at 1 second) and runs past the end.

libaom decides with floating point and the C library's math, so its bytes can differ a little
between machines; the output says how close each picture is rather than printing pixels or
sizes.
