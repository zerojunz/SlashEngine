# Video playback

A video played in a control and on a sprite
([docs/reference/scene-format.md](../../docs/reference/scene-format.md#video)).
[main.cpp](main.cpp) makes `clip.ivf` with the engine's own AV1 encoder (`video::encode_ivf`):
four colors, a quarter of a second each, with a white bar that moves across. Then it opens
[main.scene.json](main.scene.json), where the `VideoStreamPlayer` `tv` plays the clip at its own
size in the top left corner and the `Sprite2D` `screen` shows the same frames at half size
through the texture `video:tv`, and reads the colors `frame.capture` draws:

- Before the first tick nothing is decoded and nothing draws. `autoplay` starts it on the first
  tick, which shows the first frame; the control's rect is the video's 64x32.
- Each tick after moves `stream_position` on by the tick's 0.05 seconds, so five ticks later the
  second frame shows.
- Paused, setting `stream_position` seeks: the last frame shows and holds.
- Unpaused, it runs past the end, stops on the last frame with `playing` false and emits
  `finished`.
- With `loop`, it starts again from the beginning at the end.
