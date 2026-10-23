# Bone attachment

The waving arm of [ragdoll](../ragdoll/), on a pedestal, holding a sword through a bone
attachment as Godot's `BoneAttachment3D`. [main.scene.json](main.scene.json) puts an attachment
in the arm's `Transform3D` children that follows the `Hand` bone, and the sword's blade and hilt
in the attachment's, placed in the hand's space:

```json
"grip": {"components": {
  "Transform3D": {"children": ["blade", "hilt"]},
  "BoneAttachment3D": {"bone_name": "Hand"}
}}
```

Each tick, after the arm's animation, the `skeleton_modifiers` system sets the grip's
`Transform3D` to where the hand is posed, so the blade, an `AnimatableBody3D`, reaches physics
where the hand put it.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- After 30 ticks the wave has turned the hand to the right: `skeleton.get` finds it at x 0.28,
  and `entity.get` finds the grip at the same place in the arm's space, turned with the hand.
- `physics.raycast` straight down at x 0.4 hits the blade, which leans out to the right.
- After 30 more ticks the hand has waved to the left, and the grip with it.
- `frame.capture` reads the blade above the hand, the hilt, the hand and the sky.
