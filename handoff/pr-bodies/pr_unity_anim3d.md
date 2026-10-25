Before: an `Animator` in a Unity 3D scene was reported and dropped. Its controller and clips never converted, so windmills stood still, doors stayed shut and animated model instances held their bind pose.

After: 3D Animators convert the way 2D ones do. The controller becomes an animation library and, when it has transitions, an animation tree. The clips' curves move `Transform3D`, mirrored in x like the rest of the scene. On an instance of a model file, curves move the model's own nodes, and clips cut from the model's takes play straight from the converted model.

```json
"windmill": {"components": {
  "AnimationPlayer": {"animation": "Spin", "library": "unity/Assets/Animations/Windmill.anim.json",
                      "targets": {"Blades": "blades"}},
  "Transform3D": {"children": ["tower", "blades"], "position": [-2.0, 0.0, 2.0]}}}
"Arm.anim.json": {"Idle": {"from": {"model": "unity/Assets/Models/Arm.fbx", "animation": "Idle-loop"},
                           "loop": "linear", "length": 1.0}, ...}
```

How:

- **Spaces.** A controller now converts for one of three kinds of object: 2D objects, 3D objects, or models' instances. The key it is stored under carries that kind. A controller played by two kinds of object is reported, because its tracks are written for the first kind.
- **Curves on 3D objects.**
  - Positions have x negated.
  - Euler angles become radians `[x, -y, -z]`.
  - Scales are kept as they are.
  - Quaternion curves have each part interpolated as Unity does, then mirrored. They become Euler angles with `linear_angle` interpolation, keyed wherever the turn between keys passes about 6 degrees.
  - Curves of one axis map to that axis's track.
  - `m_IsActive` becomes `Transform3D/visible`, which is reported.
- **Models' instances.**
  - A curve on a path below the Animator's object becomes a node track (`node`, `property`) naming the model's node.
  - Node tracks hold no key's value until the next key, so a stepped key is held by keying its value again 0.0001 s before the next one.
  - An Animator that a model instance's prefab modifications give to its root (Unity's model importer adds one there) plays on the instance. On a model imported at a scale, it plays on the `model` child entity instead, which is reported.
- **Model clips.**
  - A clip ID pointing into a model file is resolved as Unity's model importer makes it: from `internalIDToNameTable`, `fileIDToRecycleName` and `clipAnimations`, or else from the hash of a take's name.
  - It becomes an animation `"from"` the converted model's take, with the clip's loop and events, the event times scaled to the clip's length.
  - A clip that ends before its take does is cut there. One that starts past the take's start is reported. `ModelAnimation` gains `frame_rate`, the FBX file's frames per second, to turn the clip's frames into seconds.
  - A state's speed on a model clip, model clips on objects that aren't the model's instance, and `.blend` clips are reported.
- **Placement.** Cameras and lights on an animated object, or on an object one of its tracks moves, face -z through a child entity rather than turning the object itself, so the animation's turns don't undo the half turn. The importer keeps model files readable while it checks players, so `from` animations validate.
- **Reported:** root motion (in 3D wording), humanoid muscle and Animator parameter curves (grouped into one report), animated script values, and curves of fields with no counterpart.
- **Tests.** A new test imports "Shed", built around the FBX arm of `tests/fbx.hpp`.
  - A fan whose Animator moves it, turns its blade and hides the blade's tip, with a spot light on the fan.
  - An instance of the arm model with the model root's Animator given a controller of model clips (Idle, and Wave cut from the Bend take with an event) and a clip moving its bones by position and quaternion curves.
  - The test checks the libraries, players, node tracks with Unity's per-part quaternion lerp, the light's child entity and the reports.
  - `test_model.cpp` checks the FBX frame rate.
- **Example.** New `examples/import-unity-3d-animation`: a windmill whose `AnimationPlayer` spins its blades, and a gate whose `AnimationTree` swings it open on its `Open` trigger. It is checked with `animation.eval`, the transforms after stepping, and `frame.capture`.
- **Docs.** `docs/reference/unity-import.md` gains a 3D animation section. The roadmap ticks Unity's 3D animation; skinned meshes, humanoid animation and root motion stay open.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
