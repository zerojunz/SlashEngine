# Physics debugger

Physics' visual debugger and caches, after Unreal's Chaos Visual Debugger and Chaos Cache
Manager. [main.scene.json](main.scene.json) has a stack of three crates on a floor, a blast
field beside them that starts off, a lamp hanging from a `PinJoint3D` and a 2D puck resting on
a 2D floor.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- After the crates settle, a snapshot keeps the scene, `physics.debug` starts recording and
  `physics.cache` starts recording the crates.
- The blast goes on for two ticks. `physics.debug` on `top` shows the step: the force the
  field put on it and its contact with `middle`, the normal pointing up from `middle`.
- Half a second later the middle crate leans on the bottom one; the lamp's joint holds its
  2 kilograms with 19.6 newtons and doesn't stretch; the puck's 2D step has its contact with
  the ground and the impulse holding it up.
- Two seconds in, `physics.cache` saves the collapse as `anims/collapse.anim.json`, keyed only
  where the crates' motion bends.
- Back at the snapshot, the director's `AnimationPlayer` plays the recording: the crates fall
  as they fell, frozen kinematic, and `top` lands where it landed, to a tenth of a millimeter.
- A misspelled ID is an error with the closest one.
