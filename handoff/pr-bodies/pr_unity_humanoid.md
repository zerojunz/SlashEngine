Before: a humanoid model playing another model's clip moved the nodes of the clip's model's names. A giant with its own bone names stood still, and a figure of another height slid its hips by the wrong amount. Root motion was only reported, so a walk cycle carried the model away from its object.

After: humanoid clips are retargeted through both models' Avatars, as Unity's Mecanim plays them. Root motion comes out of the pose through an `AnimationTree`, for gameplay code to move the object by.

```json
"giant": {"components": {
  "AnimationTree": {"tree_root": "unity/Assets/Animations/GiantWalker.animtree.json",
                    "libraries": {"": "unity/Assets/Animations/GiantWalker.anim.json"},
                    "root_motion_track": "pelvis", "deterministic": false}, ...}}
"GiantWalker.anim.json": {"Walk": {"length": 1.0, "loop": "linear", "tracks": [
  {"node": "pelvis", "property": "position", "keys": [...]},
  {"node": "thigh_l", "property": "rotation", "keys": [...]}, ...]}}
```

How:

- **Avatars.**
  - A model whose import settings make it humanoid (`animationType: 3`) has an Avatar.
  - The Avatar holds the model's node for each human bone, from `humanDescription.human`, and those bones' T-pose rotations, from `humanDescription.skeleton`, mirrored into the model's axes.
  - An Avatar copied from another model (`avatarSetup: 2`) is that model's.
  - Unity names are matched to the model's nodes as the model names them, or with the characters Godot's importer changes made `_`.
- **Retargeting.**
  - A humanoid take of one model, played by an instance of another humanoid model, is sampled at the take's frame rate.
  - Each of the target's human bones turns in the model as the source's bone of the same name turns from its T-pose.
  - The hips move as far as the source's, scaled by the ratio of the target's hips height to the source's.
  - Rotations become Euler node tracks, thinned to the keys that add something, with one at least every 64 frames.
  - Bones that don't move get no track. Human bones the source lacks hold their T-pose.
  - Models with the same rig still play the clip `"from"` its model, unchanged.
  - A controller of retargeted clips played by a second humanoid model is reported, since its tracks move the first model's nodes.
- **Root motion.**
  - With `m_ApplyRootMotion`, a model's instance gets an `AnimationTree` whose `root_motion_track` is the humanoid model's hips, or the node a generic model's `motionNodeName` names.
  - A controller with no transitions gets `<controller>.animtree.json`, a tree playing its first state.
  - The report now says that gameplay code applies the motion. Root motion on objects that aren't model instances is still reported as before.
- **Fixes found on the way.**
  - `mark_animated` converted an Animator's libraries before knowing which model plays them, so model clips always played `"from"`. It now uses the same clip target as `animator_component`.
  - The angle between two rotations now comes from `atan2` of the turn between them, so float quaternions at rest compare as equal where `acos` drifted.
  - When two validation errors named the same JSON Pointer, the importer removed it twice and emptied the scene. `pointer_within` now skips errors inside a value already removed.
- **Tests.** A new test imports "Gym", built from `tests/fbx.hpp`.
  - A leg model with a turned root bone plays a controller of the arm model's clips: the walk is retargeted, a looping `Idle` moves the root by root motion, and the tree has `root_motion_track` `Root`.
  - A twin of the arm plays its clips `"from"` the arm.
  - The arm's own instance, playing the leg's controller, is reported.
- **Example.** New `examples/import-unity-humanoid`.
  - A mannequin walks with its own `Walk` clip, and a giant twice its height, with other bone names and thighs half turned in its T-pose, walks with the same clip.
  - `animation.eval` shows the giant's pelvis going 2.4 m a cycle and its thighs keeping their half turn.
  - After 30 ticks, each tree's `root_motion_position` is 0.04 and 0.02 a tick.
  - `frame.capture` reads both figures.
- **Docs.** `docs/reference/unity-import.md` gains "Humanoid models and root motion". The roadmap ticks Unity's humanoid animation and root motion; skinned meshes stay open.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
