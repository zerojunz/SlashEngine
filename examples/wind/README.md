# Wind

A meadow in a breeze. Godot's only wind is an `Area3D`'s, for soft bodies, so this follows
Unreal's `WindDirectionalSource`: a `WindSource3D` blows along its entity's -z, or out from it
with `point_wind`, and everything that feels the wind asks the one field all the sources make.
[main.scene.json](main.scene.json) has a breeze from the west, a fan by the path, a banner, a
tuft of grass, falling leaves and, in 2D, petals in a draft.

```json
"breeze": {"components": {
  "Transform3D": {"rotation": [0, -1.5707964, 0]},
  "WindSource3D": {"strength": 3}
}}
```

Gusts travel downwind at the wind's speed and turbulence pushes across it, both from noise
seeded by each source's ID, so the same scene blows the same everywhere. What feels it:

- `SoftBody3D`s, by their `drag_coefficient`: the banner, pinned along one edge, streams out.
- `CPUParticles3D` and `GPUParticles3D` by their `wind_influence`, added to their gravity: the
  leaves drift east as they fall.
- Spatial shaders, which read the wind at the mesh's origin as `WIND`:
  [shaders/grass.hlsl](shaders/grass.hlsl) bends the grass's tip by it.
- In 2D, `WindSource2D` blows along its entity's x axis in pixels, which `CPUParticles2D` feel by
  `wind_influence` and canvas shaders read as `WIND`.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `wind.sample` reads the wind at three places: the grass and the banner feel the breeze,
  and a spot by the fan feels both. Four seconds on, a gust has changed it. Two-number
  points read the 2D wind, where the draft blows the petals.
- `frame.capture` reads three pixels across the grass's tip, leaning east.
- After two seconds, `physics.soft_body` shows the banner's free edge blown out to the east,
  and `particles.eval` the leaves and petals carried downwind.
- With the breeze turned off, only the fan blows: the wind at the grass drops, and the tip
  stands up again (the last capture's pixels move west).
