# IK

Two copies of the waving arm of [skinned-meshes](../skinned-meshes/) reach for a ball with
Godot 4.6's IK modifiers, which turn bones after the animation, so their hands keep waving.
[main.scene.json](main.scene.json) gives the left arm a `TwoBoneIK3D` that bends the shoulder
and elbow so the hand reaches the ball, with the elbow toward the `elbow_hint` entity:

```json
"TwoBoneIK3D": {"settings": [{"root_bone": "Shoulder", "middle_bone": "Elbow",
                              "end_bone": "Hand", "target": "ball", "pole": "elbow_hint"}]}
```

and the right arm a `FABRIK3D` that turns the chain from the shoulder to the hand toward it:

```json
"FABRIK3D": {"settings": [{"root_bone": "Shoulder", "end_bone": "Hand", "target": "ball"}]}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- Before any tick `skeleton.get` shows the left hand on the ball already: a two-bone modifier
  solves from the pose each time. The right hand is still where the wave puts it, since a
  `FABRIK3D` turns each joint at most two degrees a step and four steps a tick, as Godot's.
- After 60 ticks the right hand is on the ball too, and `frame.capture` reads the two arms, the
  hands meeting over the ball and the sky.
- `component.set` moves the ball down. A tick later the left hand is on it and the right hand
  on its way; 60 ticks more and both are there.
- At an `influence` of 0.5 the left arm goes half way between the wave and the reach.
