# Cinematics

A cutscene as a timeline of tracks, as Godot builds one with an AnimationPlayer. The `director`
in [main.scene.json](main.scene.json) plays `intro` from [intro.anim.json](intro.anim.json): two
bezier tracks dolly the `wide` camera in and narrow its field of view, an animation track opens
the `door` by playing `open` on the door's own `AnimationPlayer` ([door.anim.json](door.anim.json)),
a value track cuts to the `close` camera, and an event track shows a title. Markers name the
`door` and `close_up` moments. [calls.jsonl](calls.jsonl) checks the curves, jumps to a marker,
then plays the scene; [results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- A track with `"type": "bezier"` sets one number along a curve, as Godot's bezier tracks: each
  key's `out` and the next key's `in` are handles, `[seconds, value]` from the key, so the dolly
  eases out of its first key and into its last. `animation.eval` returns the curve's values.
- A track with `"type": "animation"` plays its target's `AnimationPlayer`, as Godot's animation
  playback tracks: each key's `animation` starts there, and `"[stop]"` stops it.
- Setting a `Camera3D`'s `current` from a track cuts to it: as Godot's, every other camera's
  `current` turns off.
- `animation.seek` jumps to a time or a marker and sets the shot at once, as Godot's `seek`: the
  dolly's end, the cut, and the door where it would be two seconds after its key, fully open.
  Event tracks wait for playback, so the title doesn't show. `undo` takes the jump back, and
  `frame.capture` after a seek draws that moment.
- Playing it, the title event fires at one second (tick 59, the 60th tick) and the door finishes
  opening a second and a half after its key.
