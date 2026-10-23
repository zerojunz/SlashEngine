# Ragdoll

The waving arm of [skinned-meshes](../skinned-meshes/), on a pedestal, with a ragdoll as Godot
4.6's `PhysicalBoneSimulator3D` and `PhysicalBone3D`. [main.scene.json](main.scene.json) puts a
simulator in the arm's `Transform3D` children, and two physical bones in the simulator's: a
capsule over the upper arm and one over the forearm, held to it at the elbow by a cone joint
whose x axis `joint_rotation` turns along the forearm, so it swings up to 2.4 radians from
straight:

```json
"forearm": {"components": {
  "Transform3D": {},
  "PhysicalBone3D": {"bone_name": "Elbow", "body_offset_position": [0, 0.25, 0],
                     "joint_type": "cone", "joint_rotation": [0, 0, 1.5707964],
                     "swing_span": 2.4},
  "CollisionShape3D": {"shape": "capsule", "radius": 0.05, "height": 0.3}
}}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- While the simulator doesn't simulate, the bodies follow the wave: `entity.get` finds the
  forearm's capsule half way along the forearm.
- `component.set` starts simulating the elbow and the bones under it. The forearm goes limp and
  dangles from the elbow while the shoulder keeps waving, so after 90 ticks `skeleton.get` finds
  the hand below the elbow.
- With `simulated_bones` empty, the whole arm simulates: it falls off the pedestal, and after 120
  ticks the bones lie on the ground and `frame.capture` reads the pedestal, the sleeve and the
  hand beside it.
- Stopping the simulation puts the bodies back on the pose, and the arm waves again.
