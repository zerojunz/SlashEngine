Before: a skinned model in a Unity scene came in standing in its rest pose. An instance whose nodes the scene turned (an arm bent at the elbow) was reported, a torch added under the instance's hand stood still where the scene first put it instead of following the hand, and a model unpacked into the scene's own objects was reported as a `SkinnedMeshRenderer` that doesn't draw.

After: what the scene does to a model's nodes converts onto the model, as Unity draws it.

```json
"torchbearer": {"components": {
  "MeshInstance3D": {"mesh": "unity/Assets/Models/Arm.fbx"},
  "Skeleton3D": {"bone_rotations": {"Elbow": [0.0, 0.0, -0.698132], "Hand": [0.0, 0.0, -0.349066]}},
  "Transform3D": {"children": ["torchbearer_hand"], "position": [0.8, 0.0, 0.0]}}},
"torchbearer_hand": {"components": {
  "BoneAttachment3D": {"bone_name": "Hand"},
  "Transform3D": {"children": ["torch"], "position": [0.321394, 1.183022, 0.0], "rotation": [0.0, 0.0, -1.047198]}}}
```

How:

- **Posed nodes.**
  - A model's instance now knows its nodes' objects: the IDs Unity's model importer gives them come from the `.meta` file's `internalIDToNameTable` or `fileIDToRecycleName`, or the hash of the node's name, as meshes and clips are found.
  - Unity folds a model's only top node into the instance's top object unless `preserveHierarchy` is set, so the importer does the same.
  - Positions, rotations and scales the instance changes become the model's `Skeleton3D` pose under the nodes' names in the file, mirrored in x, positions divided by the import scale factor. Nodes the instance's `Animator` moves are left to the animation.
  - `ModelNode` gains `file_name`, a node's name in the file before Godot's naming rules, which is how Unity names it (glTF and FBX readers fill it in).
- **Objects on nodes.**
  - Objects and components added under a node's object go on an entity with a `BoneAttachment3D` for the node, placed where the posed node is, holding the added objects; its components convert as any object's (a trigger on a hand is an `Area3D` that follows it).
  - On a model imported at a scale they go under the `model` child entity and scale with it, which is reported.
  - Inactive nodes, disabled renderers and other changes to node objects are reported, since the model draws whole.
- **Unpacked models.** A `SkinnedMeshRenderer` drawing a model's mesh, under objects that follow the model's nodes, folds those objects back into one `MeshInstance3D` of the model with the renderer's materials. Moved nodes become its pose, objects holding only the model's own components leave no entity, and the rest become attachments. Deleted nodes and skinned meshes whose objects don't follow their model are reported.
- **Tests.** A new test imports a scene of an arm built from `tests/fbx.hpp`: posed instance nodes, a lamp under a node and a trigger on another, an animated node left out of the pose, a scaled node next to an `Animator`, an FBX model with a turned top node read through `fileIDToRecycleName`, and an unpacked copy holding a sword.
- **Example.** New `examples/import-unity-skinned-meshes`: "Camp" has an arm instance bent at the elbow and wrist holding a torch, and an unpacked copy bent the other way holding a flag. `world.query` shows both poses and attachments, the engine places the attachments in the same spot after a tick, and `frame.capture` reads the torch, the flag, the gloves and the sleeves.
- **Docs.** `docs/reference/unity-import.md` gains "Model nodes and skinned meshes". The roadmap ticks Unity's skinned meshes.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
