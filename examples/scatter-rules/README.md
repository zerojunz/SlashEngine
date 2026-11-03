# Scatter rules

A forest grown over a field and a hill by [scatter rules](../../docs/reference/scene-format.md#scatter-rules),
as Unreal's PCG graphs and Godot's MultiMesh *Populate Surface* place things.

[forest.scatter.json](forest.scatter.json) has two layers:

- `trees`: [models/tree.model.json](models/tree.model.json) drawn as one `MultiMeshInstance3D`
  (`"as": "multimesh"`), over the `ground` plane and the `hill` box, 0.15 a square meter, 1.2
  meters apart, only where the ground slopes 20 degrees or less (so the hill's top but not its
  sides) and only where the noise is above -0.1, turned any way and scaled by up to a quarter
  either way.
- `rocks`: ten entities extending [props/rock.prefab.json](props/rock.prefab.json), a meter from
  each other and from every tree (`"avoid": ["trees"]`).

[calls.jsonl](calls.jsonl) then:

1. opens [main.scene.json](main.scene.json): a camera, the sun, a 20 meter field and a hill;
2. runs the rules into `forest`: 78 trees and 10 rocks (`forest_trees` and `forest_rocks_1` to
   `_10`);
3. finds the rocks with `world.query`;
4. captures the frame: the field's green, the hill's brown top and the sky;
5. runs other rules inline into the same `forest`, replacing it: 200 trees 2.5 meters apart don't
   fit, so `scatter.run` says it placed 52 and that tries failed on `slope` and `spacing`;
6. undoes that run, putting the forest back;
7. names a missing ground and gets the closest ID.

The same seed and rules over the same ground place the same trees and rocks on every machine.
