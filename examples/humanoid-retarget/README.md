# Humanoid retargeting

Characters rigged in different tools name and turn their bones differently, so an animation made
for one doesn't play on another. Godot fixes this on import: a `BoneMap` names which bone of the
model is each bone of its `SkeletonProfileHumanoid`, the bones take the profile's names, and each
bone's rest is turned to lie as the profile's does, with the skin and animations turned to match
(the "Overwrite Axis" rest fixer). `model.retarget` does the same to a model file and writes the
result as a `.glb`, guessing the bone map from the bones' names and places as Godot's
`BoneMapper` does.

[main.cpp](main.cpp) makes two rigged humanoids in code and writes them as `.glb` files:

- a hero with Mixamo's bone names (`mixamorig_Hips`, `mixamorig_LeftForeArm`), a meter to the
  hips, standing in a T-pose with every rest unturned, and a looping `wave` that raises its left
  arm, bends the elbow and bobs the hips up 10 centimeters;
- a knight with Blender's names (`hips`, `upper_arm_L`, `shin_R`), 80 centimeters to the hips,
  built in centimeters under an armature scaled by 0.01, its arms hanging in an A-pose and each
  bone turned to point along itself, as Blender's do.

[expected.txt](expected.txt) holds the output:

- `model.retarget` on the hero returns the bone map it guessed (`LeftUpperArm` is
  `mixamorig_LeftArm`) and its `motion_scale`, the hips' height. On the knight it maps Blender's
  names, folds the armature's scale into the bones and, with `fix_silhouette`, turns the arms out
  to the profile's T-pose; its motion scale is 0.8.
- Both play the hero's `wave-loop` (the `-loop` hint the file writer gives an animation that
  loops) half way through. `skeleton.get` shows the bones with the profile's names and the same
  turns on both, the knight's hand rising as the hero's does, and its hips bobbing 8 centimeters:
  position tracks scale by the ratio of the two models' motion scales.
- A bone map naming a bone the knight doesn't have is refused at `/bone_map/LeftHand`, with the
  bone it meant.

See [model.retarget](../../docs/reference/commands.md#modelretarget) and
[retargeting](../../docs/reference/scene-format.md#retargeting).
