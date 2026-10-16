# Keyframe animation

Animating any component field with keys, as Godot's AnimationPlayer. [hero.anim.json](hero.anim.json)
holds two animations: `walk`, a loop that bobs the sprite, steps through its frames and emits a
`footstep` event at each step, and `hit`, a one-shot red flash and squash that swings the hero's
sword. The hero in [main.scene.json](main.scene.json) has an `AnimationPlayer` playing `walk`.
[calls.jsonl](calls.jsonl) checks both with `animation.eval`, plays them with `step` and reads
their events. [results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- A track's `path` is a component, a field and optionally a part of it: `Sprite2D/offset/y`,
  `Sprite2D/modulate`, `Transform2D/scale`. Values are absolute, so the bob moves the sprite's
  offset rather than the entity's position.
- A track with a `target` acts on another entity, as a Godot track's node path does: the swing
  turns the `sword`, the hero's child, which the `AnimationPlayer`'s `targets` names. A target
  missing from `targets` is an error in `slash-cli scene validate main.scene.json`.
- `animation.eval` returns each track's values at the times asked, without playing anything.
  `tracks` limits it to the ones you need; a track with a target is `sword:Transform2D/rotation`,
  as Godot's `Node:property`.
- Float fields change smoothly between keys (`linear` by default, here `cubic` for the bob); a
  key's `transition` eases the way to the next key, as Godot's easing curve. Integer, string and
  boolean fields such as `Sprite2D/frame` jump from key to key.
- Event keys fire as playback reaches them: at 60 ticks a second, the `right` footstep at 0.25
  seconds comes on tick 14, the 15th tick. Looping back to the start fires the `left` one again.
- An animation that doesn't loop stops at its end, sets `playing` to false and emits
  `animation_finished`. Fields it doesn't animate keep their last values: the walk's offset stays
  where the hit interrupted it.
- `slash-cli scene validate hero.anim.json` checks every track's path and key values against the
  components, with a "did you mean" for misspelled names.
