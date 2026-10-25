Before: a state whose motion was a Unity blend tree played only the tree's first clip, and the tree was reported.

After: a blend tree becomes a blend space in the state's place in the `.animtree.json` state machine, as Godot's `BlendSpace1D` and `BlendSpace2D`, over the animations of its clips. Gameplay code sets each blend position from the parameter Unity's tree read, as the controller's storyboard task says.

```json
"Walk": {"node": {"type": "BlendSpace1D", "sync": true, "points": [
  {"position": 0.0, "node": {"type": "Animation", "animation": "Idle"}},
  {"position": 1.0, "node": {"type": "Animation", "animation": "Walk Jump"}}]}}
```

How:

- **1D and 2D.** A 1D tree's thresholds and a 2D tree's positions become the points' positions. `min_space` and `max_space` widen to hold positions beyond -1 to 1. `sync` is on, since Unity keeps every motion of a tree playing.
- **Nesting.** A tree inside a tree becomes a blend space at its point. A tree with one motion is that motion's `Animation` node.
- **Speeds.** Each motion plays its clip at the state's speed times the motion's time scale. When another state already plays the clip at another speed, the animation is named after the state and the clip (`Walk Jump`).
- **Parameters.** A blend space's position is a parameter of the tree: `Walk/blend_position`, or `Walk/1/blend_position` for one at a tree's second point. The task lists which controller parameter sets each. The `AnimationTree` component's `parameters` start them at those parameters' defaults.
- **Reported differences:**
  - Unity's 2D types weigh every motion by gradient bands, where Godot blends the three points around the position in triangles.
  - Motions all in one row or column blend in 1D along it. Motions on one slanted line make no triangles, so they become a `discrete` space where the closest plays.
  - A direct tree plays its first motion.
  - Motions of different lengths play at their own lengths; Unity stretches them to one cycle.
  - Motion cycle offsets aren't converted.
- **Tests.** A new test covers 1D and 2D trees, a tree nested in a tree, 2D trees collapsed to 1D along a row and a column, a slanted line, a direct tree, wide spaces, time scales and the task and component parameters. The Frog test now expects its `Walk` blend space. `examples/import-unity` is unchanged, since its controller has no blend tree.
- **Docs.** `docs/reference/unity-import.md` and the roadmap.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
