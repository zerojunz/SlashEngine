# Skinned polygon 2D

A 2D skeleton bending one polygon: [main.scene.json](main.scene.json) holds a `Skeleton2D`
(`tentacle`) with a chain of three `Bone2D`s and a `Polygon2D` (`skin`) beside them.
[main.cpp](main.cpp) binds the skin with `skeleton.bind`, curls the bones a little each tick in a
game system, and reads where the skin's tip draws.

```text
skeleton.bind {"id":"tentacle","polygons":["skin"]}
{"polygons":[{"bones":["root_bone","mid_bone","tip_bone"],"id":"skin","points":10}],"rested":3}
tip at [120.0, -1.0]
...
```

- `skeleton.bind` sets each bone's rest to its pose and weights each of the skin's points to the
  two bones nearest it, filling the polygon's `skeleton` and `bone_weights`.
- Bones are posed by their `Transform2D`, so game code, animations and IK move them as any entity;
  the skin's points follow the way each bone moved from its rest.
- `world::skin_2d` gives the bent points the canvas draws; `skeleton.get` lists the bones with
  their rest, pose and tip, and `show_rest_only` draws the skin as it was bound.

See [scene-format.md](../../docs/reference/scene-format.md#2d-skeletons) and
[commands.md](../../docs/reference/commands.md#skeletonbind).
