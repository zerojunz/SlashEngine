# Import a Godot terrain

`project.import` converts terrains made with the HTerrain plugin for Godot (zylann.hterrain),
which is what Godot games use for terrain, since Godot has no terrain node of its own.
[godot/](godot/) holds "Valley" as its maker keeps it in Godot 4: [project.godot](godot/project.godot),
a scene ([main.tscn](godot/main.tscn)) with an `HTerrain` node and a grass layer under it, the
`data.hterrain` file ([terrain/data.hterrain](godot/terrain/data.hterrain)) that lists the
terrain's maps, and the texture set ([terrain/textures.tres](godot/terrain/textures.tres)) that
names its grass and rock.

The plugin saves the heights as `height.res`, a Godot `Image` of 32-bit floats in Godot's binary
resource format, and its other maps as PNG files. [main.cpp](main.cpp) first plays the part of
the plugin and writes them: a hill 8 meters high in the middle of 64 by 64 meters of flat ground,
with rock above 5 meters in the splat map, and the two textures. Then it makes the calls an
agent would make over MCP or JSON-RPC; [expected.txt](expected.txt) holds the replies.

```text
entity.get {"components":["HTerrain"],"id":"hill"}
{"components":{"HTerrain":{..., "heightmap":"godot/terrain/terrain.height.png",
 "height_range":[0.0,8.0], "resolution":65, "splatmap":"godot/terrain/splat.png",
 "textures":[{"albedo_bump":"godot/terrain/grass_albedo_bump.png","uv_scale":8.0}, ...]}}}
```

- The heights become `terrain/terrain.height.png`, a 16-bit heightmap named after its folder,
  whose `height_range` is the whole meters that hold every height: here 0 to 8.
- The splat and color maps are used as they are. The texture set gives the two textures, and
  the Classic4Lite shader's one `u_ground_uv_scale` gives both their `uv_scale`. The physics
  material's friction, the chunk size and the shader's specular carry over.
- The grass layer (`HTerrainDetailLayer`) and the data's detail map are reported: SlashEngine's
  terrains don't have detail layers yet. The plugin's scripts are its nodes, not the game's code,
  so they get no C++ skeletons.
- `terrain.get` reads the ground at the summit (8 meters, rock), on the slope and on the flat,
  and `physics.raycast` straight down hits the terrain's height field at the summit.
- `frame.capture` reads the rock on the hilltop, the grassy slope, the flat ground at the
  terrain's edge, the ground past it and the sky.
