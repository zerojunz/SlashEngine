# Fog lit by a VoxelGI

A `VoxelGI`'s bounced light shining in volumetric fog, as Godot 4.6's `volumetric_fog_gi_inject`.
[main.scene.json](main.scene.json) is the room of [voxel-gi-3d](../voxel-gi-3d/): a floor, a red
wall, a back wall, a crate and a ball glowing green, under a sun with shadows, seen in a 160x90
window. Its `WorldEnvironment` turns on volumetric fog (`volumetric_fog_density` 0.06, without
temporal reprojection, so each frame shows its own settings at once), and the `gi` entity's box
covers the room. The fog's density pass reads every level of the VoxelGI's light at each froxel in
its box, front to back as Godot's fog shader does, and adds it times `volumetric_fog_gi_inject`
(1 by default, up to 16) to the light the fog scatters.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies. Each `frame.capture` reads four places through
the fog: the red wall, high on the back wall, lower on the back wall and the floor on the right.

- Before a bake the VoxelGI has no data, so only the sun and its shadows light the fog: the red
  wall reads (0.61, 0.22, 0.19) and high on the back wall (0.58, 0.56, 0.55).
- `voxel_gi.bake` voxelizes the four static meshes. Now the bounce lights the meshes and the fog
  in front of them: (0.71, 0.28, 0.24) and (0.65, 0.62, 0.58).
- `component.set` raises `volumetric_fog_gi_inject` to 4, and the fog glows brighter with the
  bounce: (0.80, 0.41, 0.34) and (0.75, 0.71, 0.65).
- At 0 the fog takes none of the VoxelGI's light, as Godot's takes none at 0.001 or less, while
  the meshes keep theirs: (0.68, 0.22, 0.19) and (0.61, 0.58, 0.55).

The fog takes the light of the first eight VoxelGIs in the view whose boxes hold a froxel.
Volumetric fog and VoxelGIs draw only in Forward+, the default `rendering.rendering_method`.
