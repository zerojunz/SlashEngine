# Import Unity skinned meshes

`project.import` brings in what Unity scenes do to the nodes of a skinned model: nodes posed in
the scene become the model's `Skeleton3D` pose, objects added under a node follow its bone through
a `BoneAttachment3D`, and a model unpacked into objects becomes one model again. [unity/](unity/)
holds "Camp" as Unity 2022.3 saves it: its project settings, a skinned arm, two materials and a
scene ([Camp.unity](unity/Assets/Scenes/Camp.unity)) with the camera, the sun and two arms.
[calls.jsonl](calls.jsonl) imports it, opens the scene and plays a tick, as an agent would over
MCP or JSON-RPC; [results.jsonl](results.jsonl) holds the replies.

- [Arm.fbx](unity/Assets/Models/Arm.fbx) is the arm of [skinned-meshes](../skinned-meshes/): a
  sleeve and a glove skinned to `Shoulder`, `Elbow` and `Hand` bones under a `Rig` node, which
  Unity folds into the model's top object.
- "Torchbearer" is an instance of the model with its elbow turned 40 degrees and its hand 20 more,
  as overrides of the instance. It becomes a `MeshInstance3D` whose `Skeleton3D` turns `Elbow`
  and `Hand`, in the engine's axes. A torch added under the instance's `Hand` object becomes an
  entity under a `BoneAttachment3D` for `Hand`, placed where the posed hand is.
- "FlagBearer" is the same model unpacked in the scene: its `Shoulder`, `Elbow`, `Hand` and `Arm`
  objects are the scene's own, with a `SkinnedMeshRenderer` drawing the model's mesh. They fold
  back into one `MeshInstance3D` of the model, whose `Skeleton3D` turns the elbow the other way.
  The objects that hold nothing of their own leave no entity; the `Hand` object holding a flag
  becomes a `BoneAttachment3D`.
- `world.query` shows both poses and both attachments. After a tick the engine has placed each
  attachment on its bone itself, in the same place the importer wrote, to float precision.

`frame.capture` reads the sky, the torch, both gloves, the flag and both sleeves.
