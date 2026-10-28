# Humanoid rigging

A generated or modeled character usually comes as a mesh with no skeleton, so no animation can
play on it. Services such as Meshy's rig a standing humanoid figure: they place a skeleton by the
body's shape and weight the skin to it. `asset.refine` with `"rig": "humanoid"` has a generator
named in `providers.json` do that to a library model, then retargets the result to Godot's
`SkeletonProfileHumanoid` (as [`model.retarget`](../humanoid-retarget/) does) before the library
keeps it, so every rigged character plays every other's animations.

[main.cpp](main.cpp) makes two figures of boxes in code and writes them as `.glb` files:

- a knight 1.8 meters tall, standing with its arms at its sides, with no skeleton;
- a dancer 1.5 meters tall in a T-pose, rigged in code by `scene3d::rig_humanoid` (the rig the
  `test` generator gives), with a looping `wave` that raises its left arm and bobs its hips up
  10 centimeters.

[expected.txt](expected.txt) holds the output:

- `asset.add` keeps the knight, and `asset.refine` with `rig` makes a rigged version of it. The
  test generator places 23 of the profile's bones by a standing figure's proportions and weights
  each corner to its two nearest bones on its side of the body; the reply's `rig` says how many
  bones were mapped, that none the profile needs is missing, and the `motion_scale`, the hips'
  height. Retargeting turns the arms out to the profile's T-pose, so the rigged knight is wider.
  With Meshy's key in the environment, a `meshy-rig` generator rigs it instead.
- `asset.info` shows the rigged version made from the knight, with the request that made it.
- `model.retarget` maps the dancer's bones, which already have the profile's names.
- The knight plays the dancer's `wave-loop` half way through: `skeleton.get` shows its left arm
  raised 60 degrees as the dancer's is, its hand above its head, and its hips raised 12
  centimeters for the dancer's 10, as the knight's hips are higher (0.954 meters to 0.795).
- A model with a skeleton already isn't rigged again.

See [asset.refine](../../docs/reference/commands.md#rigging-a-figure) and
[meshy-rig](../../docs/reference/providers.md#generators).
