# Import Unity humanoid animation

`project.import` retargets Unity's humanoid animation: a clip made for one model's Avatar plays on
another humanoid model, its bones turned as the first model's are and its hips moved in proportion
to its height, and root motion comes out of the pose through the `AnimationTree`. [unity/](unity/)
holds "Parade" as Unity 2022.3 saves it: its project settings, two humanoid models, two Animator
controllers and a scene ([Parade.unity](unity/Assets/Scenes/Parade.unity)) with the camera, the sun
and an instance of each model walking. [calls.jsonl](calls.jsonl) imports it, opens the scene and
plays it, as an agent would over MCP or JSON-RPC; [results.jsonl](results.jsonl) holds the
replies.

- [Mannequin.fbx](unity/Assets/Models/Mannequin.fbx) is a figure a metre tall at the hips, with a
  `Walk` take: its hips go 1.2 metres forward in a second while its legs swing 25 degrees either
  way. Its [import settings](unity/Assets/Models/Mannequin.fbx.meta) make it a humanoid (its
  `humanDescription` names each bone's human bone, `Hips`, `Spine`, `LeftUpperLeg` and the rest)
  and cut a looping `Walk` clip from the take.
- [Giant.fbx](unity/Assets/Models/Giant.fbx) is twice as tall, its bones named `pelvis`, `spine_01`,
  `thigh_l` and so on, and its thighs turned half a turn in its T-pose. It has no takes of its own:
  [GiantWalker.controller](unity/Assets/Animations/GiantWalker.controller) plays the mannequin's
  `Walk` clip on it.
- The mannequin's own controller plays the clip `"from"` its model, as any model clip does. The
  giant's becomes `GiantWalker.anim.json` with the walk retargeted onto the giant's bones:
  `animation.eval` shows the pelvis going 2.4 metres a cycle, at its own height of 2, and each
  thigh swinging 25 degrees about x while it keeps its half turn about z.
- Both instances have Unity's `Apply Root Motion` on, so each gets an `AnimationTree` whose
  `root_motion_track` is its hips bone. The tree takes the hips' forward motion out of the pose,
  so the models walk on the spot, and puts it in `root_motion_position` for gameplay code to move
  them by, as Unity's Animator moved them: after 30 ticks the giant's is 0.04 a tick and the
  mannequin's 0.02. The import report says so.

`frame.capture` reads the sky and both figures' bodies and legs, with the sky between their legs.
