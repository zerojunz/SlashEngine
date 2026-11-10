# Light tools

Light the way a lighting artist does, in numbers an agent can read: real units, color
temperatures and a tool that brings a frame to a brightness.

[project.json](project.json) turns on `rendering.lights_and_shadows.use_physical_light_units`,
as Godot's. [street.scene.json](street.scene.json) is a street at dusk seen through a camera
whose `CameraAttributesPractical` has an `exposure_sensitivity` of 6400 (ISO 6400):

- `dusk` is a `DirectionalLight3D` of 400 lux at 9000 K, the blue light after sunset.
- `lamp_west` is a sodium street lamp: an `OmniLight3D` of 6000 lumens at 2000 K, orange.
- `lamp_east` is a white one, 3000 lumens at 4000 K.
- The shop's `sign` glows at an `emission_intensity` of 200 nits, and the sky at a
  `background_intensity` of 200.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `lighting.check` finds the street lit and dim: a mean brightness of 0.15.
- `light.tune` with `"mean": 0.35` scales every light by one factor, learning from each step how
  the frame's brightness follows it, and stops once the mean is within 0.02 and no more than 2%
  of the frame clips. In physical units it changes the lights' lux and lumens, not their energy,
  so the numbers stay ones a lighting artist would write:

  ```text
  light.tune {"mean":0.35,"scale":1}
  {"after":{"clipped":0.0183,"mean":0.3603},"before":{"clipped":0.0,"mean":0.1498},
   "changes":[{"field":"light_intensity_lux","from":400.0,"id":"dusk","to":4189.112,...},
              {"field":"light_intensity_lumens","from":3000.0,"id":"lamp_east","to":31418.342,...}, ...],
   "converged":true,"iterations":2}
  ```

- `undo` puts every light back in one step.
- With `"shares": {"dusk": 0.5}` it also scales `dusk` until it gives half the frame's
  brightness, measured by drawing the frame without it, and the lamps the rest.

[alley.scene.json](alley.scene.json) is 2D: a wall under a dark blue `CanvasModulate`, a
`lantern` `PointLight2D` with a `temperature` of 1900 K and a `neon` sign at 12000 K.

- `light.sample` shows the lantern's light orange and the sign's blue, though both are white.
  A 2D light's `temperature` is SlashEngine's own (Godot's 2D lights have none); 0, the
  default, leaves its color alone.
- `lighting.check` measures the 2D lights as it does 3D ones: the lantern washes 5% of the frame
  out.
- `light.tune` with `"mean": 0.25` turns both lights down until the frame stops clipping.
- A misspelled light in `ids` gets a "did you mean".

Everything draws on the CPU, as `frame.capture` does, so the numbers are the same on every
machine.
