# Import Godot 3D animation tracks

`project.import` converts Godot 4's `position_3d`, `rotation_3d` and `scale_3d` animation tracks.
[godot/](godot/) holds "Signal Arm" as Godot saves it: [project.godot](godot/project.godot), the
skinned arm of [import-godot-character](../import-godot-character/) as a glTF file Godot imports
([models/arm.gltf](godot/models/arm.gltf)) and one scene, [main.tscn](godot/main.tscn), whose
`AnimationPlayer` signals with the arm and a red flag. [calls.jsonl](calls.jsonl) imports it and
plays the animation as an agent would over MCP or JSON-RPC; [results.jsonl](results.jsonl) holds
the replies.

- The tracks on `Arm/Rig/Skeleton3D:Shoulder` and `:Elbow` turn bones of the instanced model, so
  they become node tracks on the model's `Shoulder` and `Elbow` nodes, their rotations as Euler
  angles. Node tracks move the model their entity draws, so the player goes on `arm`, and the
  report says so. Its `targets` still reach the flag.
- The tracks on `Flag`, a `MeshInstance3D` of the scene, become `Transform3D` value tracks in
  `godot/main_animation_library_signal.anim.json`: the position keeps its ease in and out
  (`transition` -2) and the scale its `nearest` interpolation. Godot slerps the flag's half turn
  from key to key; turning each Euler angle the shorter way round would swing it back the other
  way, so the converter adds keys a quarter turn in, there and on the turn round the loop back to
  the first key.
- `animation.eval` returns the node tracks under `nodes` and the flag's tracks under `tracks` at
  four times through the second.
- Thirty ticks in, half a second, `skeleton.get` shows the shoulder leaning 45 degrees (-0.7854)
  and the elbow bent 60 (1.0472), and `entity.get` the flag raised, turned half round and grown by
  a quarter.
- `frame.capture` checks the flag and the bent arm against the sky.
