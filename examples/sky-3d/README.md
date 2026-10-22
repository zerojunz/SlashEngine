# Sky 3D

Skies and fog, as Godot's `Environment` and sky materials draw them in its Compatibility
renderer. [main.scene.json](main.scene.json) is a meadow with hills, a rock and a tree under the
sun, seen through a `Camera3D` in a 320x180 window. Its `environment` entity has:

- A `ProceduralSkyMaterial`: the sky runs from `sky_top_color` overhead to `sky_horizon_color`,
  and the ground half from `ground_horizon_color` to `ground_bottom_color`. The `sun` entity's
  `DirectionalLight3D` shows in it as a disk `light_angular_distance` degrees across, with a glow
  reaching `sun_angle_max` degrees past it.
- A `WorldEnvironment` with `background_mode` `"sky"` and depth fog (`fog_mode` `"depth"`) from 8
  to 60 units away, so the hills fade toward the fog's light blue. `fog_sky_affect` 0 leaves the
  sky clear of it.

The sky lights the meshes too, as the `WorldEnvironment`'s default `ambient_light_source` and
`reflected_light_source` say; [sky-light-3d](../sky-light-3d/) shows how.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[160,4],[20,76],[228,17],[150,92],[160,172]],"path":"shots/day.png"}
{"colors":[[0.267,0.463,0.788,1],[0.667,0.761,0.886,1],[1,1,1,1],[0.318,0.431,0.353,1],
 [0.247,0.404,0.282,1]],"drawn":{"meshes":6},"height":180,"path":"shots/day.png","width":320}
```

- `frame.capture` draws the frame on the CPU, sky and fog included, so it needs no GPU and gives
  the same pixels on every machine. `points` reads the sky overhead, the sky at the horizon, the
  sun, the far ground in the fog and the near ground clear of it. `path` saves the frame as
  `shots/day.png`.
- `fog_height` 0.6 with `fog_height_density` 0.8 adds height fog: everything below 0.6 thickens
  into mist, the near ground and the foot of the rock too.
- Swapping in a `PhysicalSkyMaterial` scatters the sun's light through the air, by Rayleigh and
  Mie scattering, instead of painting gradients. With the sun dropped to 7 degrees above the
  horizon, the sky is dusk gray with a warm glow round the sun. `energy_multiplier` 6 brightens
  it: like Godot's, a physical sky is dim in a renderer without HDR.
- A `PanoramaSkyMaterial` wraps an equirectangular image round the camera: here the day frame
  saved first, so blue sky shows overhead again.

Each material has Godot's fields: `PanoramaSkyMaterial`'s `panorama`, `ProceduralSkyMaterial`'s
`sky_cover` and `PhysicalSkyMaterial`'s `night_sky` name textures by project path.
`WorldEnvironment`'s `sky_rotation` turns the sky, and fog with a `"color"` background tints the
background too, as Godot's does.
