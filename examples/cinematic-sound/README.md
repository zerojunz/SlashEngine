# Cinematic sound

A cutscene's sound as Godot's audio playback tracks. The `director` in
[main.scene.json](main.scene.json) plays `intro` from [intro.anim.json](intro.anim.json), whose
audio tracks start sounds on the `door`'s and the `narrator`'s `AudioStreamPlayer`s at their keys:
a creak at a quarter of a second, a line at one second and a sigh at two and a half.
[main.cpp](main.cpp) writes the sounds, plays the scene as an agent does over JSON-RPC and prints
each call and reply; [expected.txt](expected.txt) holds the output.

- A track with `"type": "audio"` names its player by `target`, as Godot's track names a node.
  Each key's `stream` plays on a voice of the player's own, beside the player's own `stream`, at
  the player's volume, pitch and bus: the door's -6 dB takes the creak's -6 dB peak to -12 dB.
- A key's `start_offset` and `end_offset` cut the sound, as Godot's: the line starts a quarter
  of a second in and stops half a second before its end.
- `audio.state` counts each player's `track_streams`, the sounds tracks are playing on it.
- `animation.seek` starts no sound, as Godot's update-only seek; playback after it starts the
  keys it passes, so the jump back to the `retake` marker plays the line again.
- The animation's end stops the sounds still playing, as Godot stops a finished animation's.
  In an `AnimationTree`, a sound's volume follows how much its animation counts in the blend
  unless the track's `use_blend` is off.
