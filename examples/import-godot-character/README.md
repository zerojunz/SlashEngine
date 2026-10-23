# Import Godot animation trees

`project.import` converts Godot 4's `AnimationTree` nodes: their tree of animation nodes becomes
a `.animtree.json` file and the node an `AnimationTree` component that plays it.
[godot/](godot/) holds "Waving Arm" as Godot saves it: [project.godot](godot/project.godot), the
skinned arm of [animation-tree](../animation-tree/) as a glTF file Godot imports
([models/arm.gltf](godot/models/arm.gltf)) and one scene, [main.tscn](godot/main.tscn), with two
trees. [calls.jsonl](calls.jsonl) imports it and drives both trees as an agent would over MCP or
JSON-RPC; [results.jsonl](results.jsonl) holds the replies.

- `ArmTree` plays the arm's own animations through the `AnimationPlayer` Godot's importer gives
  the model (`anim_player = NodePath("../Arm/AnimationPlayer")`). Its `AnimationNodeBlendTree`
  becomes `godot/main_arm_tree.animtree.json`: each `nodes/<name>/node` a node of the same name
  and type without the `AnimationNode` prefix, and `node_connections` each node's `inputs` and the
  tree's `output`. Since a tree poses the model its entity draws, the component goes on `arm`, and
  the report says so. Godot drops the `-loop` from the model's `Wave-loop` animation, which the
  tree calls `Wave`; the file names it `Wave-loop`, as the model does. The `OneShot`'s filter
  `Rig/Skeleton3D:Shoulder` becomes the bone it names, `Shoulder`.
- `LampTree` plays the animations of the lamp's own `AnimationPlayer`, whose library becomes
  `godot/main_animation_library_lamp.anim.json`; the tree takes its `targets` too. Its
  `AnimationNodeStateMachine` becomes `godot/main_lamp_tree.animtree.json`, its `transitions`
  triples each a transition from one state to the next: `Start` goes on to `bright` on its own,
  and `bright` to `dim` when the `night` condition is true, cross-fading over half a second.
  Godot's deterministic tree starts the lamp's `light_energy` from the library's `RESET`
  animation, and the library has none, so it gets one at zero, as the report says.
- `animation.tree` lists what the lamp's tree takes: `conditions/night` and the state machine's
  requests.
- `animation.set_parameters` fires the arm's shot: twelve ticks later `skeleton.get` shows
  `Point` turning the shoulder, at five sixths of its weight as it fades in, while the wave goes
  on bending the hand.
- Setting `conditions/night` sends the lamp to `dim`: fifteen ticks in, `light_energy` is part of
  the way from 4 to 0.5, and once the cross-fade is over it is 0.5 and `animation.tree` shows
  `dim` playing.
- `frame.capture` checks the arm, lit by the dimmed lamp, against the sky.
