# Import Godot skeletons

`project.import` converts what a Godot 4 scene adds to the skeleton of a model it instances.
[godot/](godot/) holds "Arm Rig" as Godot saves it: [project.godot](godot/project.godot), the
skinned arm of [skinned-meshes](../skinned-meshes/), without its animations, as a glTF file
Godot imports ([models/arm.gltf](godot/models/arm.gltf)), and one scene,
[main.tscn](godot/main.tscn), with two instances of it. Godot's importer gives each a
`Skeleton3D` at `Rig/Skeleton3D` with the bones `Shoulder`, `Elbow` and `Hand`, and the scene
adds to them:

- Under `Reacher/Rig/Skeleton3D`, a `TwoBoneIK3D` that reaches the hand for the ball, bending
  the elbow toward a marker, and a `BoneAttachment3D` that holds a blade in the hand.
- On `Limp/Rig/Skeleton3D`, a pose (`bones/0/rotation` leans the shoulder, `bones/1/rotation`
  bends the elbow) and a `PhysicalBoneSimulator3D` with a `PhysicalBone3D` capsule over each
  arm bone, the forearm's held at the elbow by a hinge.

[calls.jsonl](calls.jsonl) imports it and drives it as an agent would over MCP or JSON-RPC;
[results.jsonl](results.jsonl) holds the replies.

- The import converts everything and reports nothing. `entity.get` shows where it went: the
  pose is `limp`'s `Skeleton3D` rotations, by bone; the modifiers and the attachment are
  entities in the `Transform3D` children of the entity that draws the model, so they run on
  its pose; the IK's bones are named and its target and pole nodes are entity IDs; and the
  forearm's hinge limits, 135 degrees in Godot, are radians.
- After a tick `skeleton.get` finds the reacher's hand at the ball, and the limp arm's elbow
  and hand where the pose put them.
- Starting the simulation drops the limp arm: 120 ticks later its bones lie on the ground.
- `frame.capture` reads the blade, the reaching glove, the fallen sleeve and glove, and the sky.
