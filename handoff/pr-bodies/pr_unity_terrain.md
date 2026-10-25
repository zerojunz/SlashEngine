Before: a Unity `Terrain` and its `TerrainCollider` were reported, and the ground they made was missing from the converted scene.

After: each terrain becomes an `HTerrain` on a `<object>_terrain` child entity, with its heights, alpha map, terrain layers, holes and collision, mirrored in x like the rest of a 3D scene. An agent can then read it with `terrain.get`, cast rays at it and see it in `frame.capture`.

```json
"island_terrain": {"components": {
  "Transform3D": {"position": [-64.0, 0.0, 0.0]},
  "HTerrain": {"heightmap": "unity/Assets/Terrain/Island.height.png", "height_range": [0.0, 8.0],
               "splatmap": "unity/Assets/Terrain/Island.splat.png",
               "colormap": "unity/Assets/Terrain/Island.color.png",
               "textures": [{"albedo_bump": "unity/Assets/Terrain/grass.png", "uv_scale": 8.0}, ...],
               "friction": 0.6, "collision_mask": 4294967295, "cast_shadow": "double_sided"}}}
```

How:

- **TerrainData.** The importer parses the `.asset` and decodes its hex heights, holes and alpha texture. Each TerrainData converts once, however many terrains use it.
  - **Heights** are written as a 16-bit `<terrain>.height.png` with columns flipped, since x mirrors. `height_range` is the whole meters that hold every height, and `map_scale` is Unity's cell size. A heightmap under 65 heights across is refined to 65, the smallest an `HTerrain` takes, and reported.
  - **The first alpha map** is resampled to one weight per height and written as `<terrain>.splat.png`.
  - **The first four terrain layers** become `textures`: the diffuse texture as `albedo_bump`, the normal map as `normal_roughness`, and the tile size in cells as `uv_scale`. Depth blending is off, because Unity's diffuse alpha is smoothness, not height.
  - **Holes** go into `<terrain>.color.png`, white with alpha 0 at the holes. A height is a hole where every cell around it is cut, which is reported.
- **Placement.** Unity's terrains reach toward +x and +z from their object. Once mirrored, the terrain would reach toward -x, so the child entity moves back the terrain's width and the ground stays where Unity draws it.
- **Terrain and TerrainCollider.**
  - The shadow casting mode becomes `cast_shadow` and the object's layer becomes `render_layers`.
  - A disabled terrain, or one that doesn't draw its heightmap, is hidden.
  - The collider gives the collision layer and mask, plus its physic material's friction and bounce.
  - With no collider, collision is off.
- **Reported:** more than four layers; tiles that aren't square; tile offsets; layer smoothness, metallic, mask maps and normal scale; trees and details; custom terrain materials; terrains on turned or scaled objects; a collider of another TerrainData or on a Rigidbody; a collider with no terrain.
- **Tests.** A new test imports a 65 by 65 terrain with 2 by 3 meter cells, three layers, a 4 by 4 alpha map, a hole, a tree and a physic material. It checks the scene, the decoded heights, the splat weights, the holes, the reports, and a `terrain.get` height read back through a session.
- **Example.** New `examples/import-unity-terrain`: a hill with rock above 4 meters and a hole in the flat ground. It is checked with `terrain.get`, `physics.raycast` (onto the summit and through the hole) and `frame.capture`.
- **Docs.** `docs/reference/unity-import.md` gains a Terrains section. The roadmap ticks Unity's terrains; skinned meshes and 3D animation stay open.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
