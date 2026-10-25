# Fog volumes 3D

A cellar filled with fog volumes, as Godot 4.6's `FogVolume` and `FogMaterial` in its Forward+
renderer. [main.scene.json](main.scene.json) is a floor and a wall in a 160x90 window, lit by an
orange lamp, in a thin volumetric fog (`volumetric_fog_density` 0.01). Three `FogVolume`s add to
it, each filled as the `FogMaterial` on its entity says:

- `mist` is a box of pale fog over the floor, thinning with height by its `height_falloff` and
  fading toward its edges by its `edge_fade`.
- `clearing` is a cylinder whose density of -1 clears the mist near the lamp.
- `wisp` is a ball of fog that glows green by its `emission`.

[project.json](project.json) makes the fog's volume 32 froxels across and 32 deep, so the example
draws fast on the CPU.

[calls.jsonl](calls.jsonl) drives the scene the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[56,25],[40,55],[110,52],[80,85]],"frames":8,"path":"shots/cellar.png"}
{"colors":[[0.196,0.275,0.212,1],[0.18,0.192,0.29,1],[0.631,0.471,0.404,1],
 [0.188,0.141,0.137,1]],"drawn":{"meshes":2},"height":90,"path":"shots/cellar.png","width":160}
```

- The points read the wisp, the mist on the left of the floor, the wall lit by the lamp and the
  floor near the camera. `frames` draws 8 frames and returns the last, so the fog settles.
- One `component.set` makes the wisp thicker and red, and only its point changes.
- Another makes the mist a `"world"` volume, a thin fog everywhere, with no edge fade or falloff.
  A world volume is as big as twice the froxels in each direction, as Godot sizes it, so its
  `edge_fade` would thin it everywhere.
- The last turns the volumetric fog off, and the volumes go with it.

As in Godot, fog volumes add to each froxel in fixed point: the density in 1024ths, and the light
each scatters and gives off held to 11, 11 and 10 bits a channel. A frame takes up to 32 volumes.
Godot's Compatibility renderer draws no volumetric fog, so it draws no fog volumes either.
