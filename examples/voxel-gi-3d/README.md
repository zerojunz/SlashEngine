# VoxelGI 3D

Light bouncing around a room through a `VoxelGI`, as Godot 4.6's: the floor's sunlight lights a
crate's shadowed side and the ball's underside, and the red wall tints the floor beside it.
[main.scene.json](main.scene.json) is a floor, a red wall, a back wall, a crate and a ball seen
through a `Camera3D` in a 160x90 window, lit by a sun with shadows and a blue-grey ambient light.
The `gi` entity's box covers the room at 64 cells along its longest side. `voxel_gi.bake`
voxelizes the meshes whose `gi_mode` is `"static"`, Godot's default, once; the renderer injects
the sun into the cells and traces cones through them every frame, so the bounce follows the
lights as the game runs. The ball's `gi_mode` is `"dynamic"`, as for anything that moves: it
isn't voxelized, but the renderer plots it into the cells' light each frame it moves or
changes, so it lights the room too.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
voxel_gi.bake {"id":"gi"}
{"cells":12104,"file":"main.gi.voxelgi.json","levels":7,"meshes":4,"size":[64,32,64]}
```

- The first `frame.capture` reads the crate's shadowed side, the floor in its shadow, the floor by
  the red wall, the open floor and the ball's underside, with only the sun and the ambient light.
  The VoxelGI has no data yet, so it gives nothing. `path` saves the frames as PNGs in `shots/`.
- `voxel_gi.bake` voxelizes the four static meshes into 12104 cells of a 64x32x64 grid, in an
  octree of 7 levels, writes them to `main.gi.voxelgi.json` beside the scene and sets the
  VoxelGI's `data` to it.
- Traced, the crate's shadowed side turns from the ambient light's blue (0.17, 0.22, 0.37) to grey
  (0.36, 0.34, 0.36) with the light off the floor, and so does the floor in its shadow. The
  ball's underside warms from (0.63, 0.62, 0.64) to (0.76, 0.72, 0.67). The floor by the red wall
  comes out redder than the open floor: (0.84, 0.77, 0.71) against (0.83, 0.79, 0.73).
- `component.set` turns the sun orange. Lights aren't baked, so the next frame's bounce is orange
  too, with no bake: the crate's shadowed side goes to (0.36, 0.24, 0.26).
- A bake after that reports `"unchanged": true` and records no undo step, since the meshes
  haven't changed.
- `component.set` makes the ball glow green. It's plotted into the cells with its emission, with
  no bake, so the floor beside it comes out green, (0.82, 0.64, 0.34), while the floor by the
  crate keeps the sunset's (0.83, 0.51, 0.36).
- Moved by the crate, the ball takes its glow with it: the floor where it was goes to
  (0.84, 0.51, 0.35), and the floor by the crate to (0.81, 0.72, 0.35).

`voxel_gi.bake` changes project files, so it needs the `edit` and `filesystem` capabilities.
Undo removes the file it made, or puts back the one it replaced. `slash-cli scene validate`
checks a VoxelGI's `data`, and exports ship the file. VoxelGIs draw only in Forward+, the default
`rendering.rendering_method`; `rendering.global_illumination.voxel_gi.quality` in `project.json`
picks Godot's four cones or six narrower ones.
