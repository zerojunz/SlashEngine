# Auto exposure 3D

Eyes adjusting to light, as Godot 4.6's auto exposure in its Forward+ renderer, the default
`rendering.rendering_method`. [main.scene.json](main.scene.json) is a tunnel lit by a torch, with
daylight at its far end, seen through a `Camera3D` in a 320x180 window. The daylight is a wall
whose `emission_energy_multiplier` is 6, so it shines six times its color, and a lamp just inside
the mouth lights the last of the tunnel.

The camera's `CameraAttributesPractical` sets `auto_exposure_enabled`, as Godot's camera
`attributes` do (a `WorldEnvironment`'s entity can hold one too, for every camera without its
own):

- Each frame is measured: Forward+ averages the brightest channel of each pixel, in blocks of 8 by
  8, down to one value, and the tonemap multiplies the frame by `auto_exposure_scale` (Godot's
  0.4) over that brightness, so a dim frame is brightened and a bright one darkened.
- `auto_exposure_speed` 1 moves the measured brightness that part of the way to the new frame's
  each second of game time, so the change is gradual.
- `auto_exposure_max_sensitivity` 3200 lets it adapt to frames up to four times white on average
  (a sensitivity is turned into a brightness by 0.125 over `exposure_sensitivity`, 100), so
  bright daylight stays bright, as Godot's.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"time":0,"points":[[160,90],[40,90],[160,170]],"path":"shots/tunnel.png"}
{"colors":[[1,1,1,1],[0.325,0.149,0.047,1],[0.427,0.2,0.067,1]],"drawn":{"meshes":5},
 "height":180,"path":"shots/tunnel.png","width":320}
```

- `frame.capture` draws the frame on the CPU, auto exposure included, so it needs no GPU and gives
  the same pixels on every machine. `time` is the game time it is drawn at, in seconds: captures
  carry the measured brightness from one to the next, and the first takes its own. `points` reads
  the daylight, the left wall by the torch and the floor.
- `component.set` moves the camera to the tunnel's mouth. A quarter of a second later the daylight
  is white and the walls by the mouth are bright: the exposure is still the tunnel's.
- Six seconds later the eyes have adjusted: the daylight shows its blue and the walls are darker.
  `path` saves the frame as `shots/daylight.png`.
- Back in the tunnel, a quarter of a second later, the walls are nearly black; six seconds later
  they are as they were at first.
- `auto_exposure_scale` 0.8 brightens the adjusted frame at once: the scale is applied after the
  measurement, which goes on unchanged.
- `auto_exposure_metering` `"log_average"`, with the scale back at 0.4, measures the mean of each
  pixel's logarithm instead, as Unreal's auto exposure meters (SlashEngine's own; Godot has only
  the average). The daylight at the far end counts for less, so six seconds later the tunnel's
  walls and floor are about twice as bright as at the start. `shots/log_average.png` saves it.

Godot's Compatibility renderer (`"rendering_method": "gl_compatibility"`, and always with the
OpenGL drivers) has no auto exposure: it uses `exposure_multiplier` alone, which every renderer
applies to lights, skies, the background and emission.
