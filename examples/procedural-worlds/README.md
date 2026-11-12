# Procedural worlds

`.pcg.json` graphs fill worlds with what every other tool makes, as Unreal's PCG graphs do.
`valley.pcg.json` samples the valley's ground, keeps a noisy meadow for grass foliage and a
rocky patch for a multimesh of boulders, clears the meadow round the farm (a storyboard place,
found by its `StoryRef`), thins it into a herd that `creature.place` fills with deer, lines the
road with lamps whose energy it picks per lamp and routes a patrol through the herd.
`island.pcg.json` does the same in 2D: grass in a ring round a camp, thickest where the noise
says, and deer far from it.

The calls check the valley graph (budget, counts and that every deer can be walked to from the
farm), preview it as numbers and a top-down PNG, run it under `life`, read what it made, run it
again with another seed (which replaces what the first run made) and undo that, then run the
island. The last call shows an error: a grid doesn't take `keep`.

See [docs/reference/scene-format.md](../../docs/reference/scene-format.md#procedural-graphs) and
[docs/reference/commands.md](../../docs/reference/commands.md#pcgrun).
