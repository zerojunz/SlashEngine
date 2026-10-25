# Import a Godot VoxelGI

`project.import` converts Godot's `VoxelGI` nodes with the cells Godot baked for them, so a room
imported from Godot is lit by its bounced light at once, with no new bake.
[godot/](godot/) holds "Gallery" as its maker keeps it in Godot 4:
[project.godot](godot/project.godot), with the VoxelGI quality set to high, and a scene
([main.tscn](godot/main.tscn)) of a room with a red wall and a crate, lit by the sun, with a
`VoxelGI` over it whose `data` names `main.gi_data.res`.

Godot's bake saves that `VoxelGIData` beside the scene in its binary resource format.
[main.cpp](main.cpp) first plays the part of the bake and writes it: it voxelizes the same
meshes with SlashEngine's port of Godot's voxelizer and saves the cells as Godot's
`VoxelGIData` keeps them, with an `energy` of 1.2. Then it makes the calls an agent would make
over MCP or JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
entity.get {"components":["VoxelGI"],"id":"gi"}
{"components":{"VoxelGI":{..., "data":"godot/main.gi_data.voxelgi.json", "energy":1.2,
 "size":[6.4,3.2,6.4], "subdiv":"64", ...}},"id":"gi"}
```

- The VoxelGI keeps its `subdiv` (Godot's `0` is `"64"`) and `size`, and the data gives its
  `energy` and the other settings.
- The cells become `main.gi_data.voxelgi.json`, the file `voxel_gi.bake` writes, named after
  the `.res` file. Data with no cells, or cells that don't make a VoxelGI's, would be reported.
- The quality setting becomes `rendering.global_illumination.voxel_gi.quality` in
  `project.json`: `"high"`.
- `frame.capture` reads the crate's shadowed side, the floor in its shadow, the floor by the red
  wall and the open floor. Lit by the imported cells, the floor in the crate's shadow is
  (0.47, 0.44, 0.42) and the floor by the wall takes the wall's red, (0.93, 0.83, 0.75) beside
  the open floor's (0.92, 0.88, 0.80).
- Without the cells (`component.set` clears `data`), only the sun and the ambient light are
  left: the floor in the crate's shadow drops to (0.17, 0.22, 0.37), and the floor by the wall
  and the open floor are the same, (0.72, 0.69, 0.68).
