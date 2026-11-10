# ReSTIR: many shadowed lights (research)

A research feature with no commitment to ship ([roadmap](../roadmap.md)). Godot has nothing like
it; it follows Bitterli, Wyman, Pharr, Shirley, Lefohn and Jarosz's *Spatiotemporal reservoir
resampling for real-time ray tracing with dynamic direct lighting* (SIGGRAPH 2020), the method
behind the many-light shadows of recent games. Only the software device draws it for now. Read
here what it does now, and what it does not do yet.

## What it does

Forward+ already culls lights into clusters, so each pixel only loops over the omni and spot
lights that reach it. With many shadowed lights, that loop and the shadow maps are what cost:
Forward+ has 16 positional shadow slots and ray-traced shadows 8. ReSTIR makes each pixel pay for
a few lights whatever the count, and gives every light with `shadow` set a ray-traced shadow.

Turn it on in [project.json](project-format.md#ray-tracing):

```json
"rendering": {"ray_tracing": {"many_lights": true, "many_light_candidates": 8,
                              "many_light_samples": 2}}
```

After the depth and normal prepass, each opaque pixel keeps `many_light_samples` **reservoirs**,
each holding one light and a weight. Each frame:

1. **Candidates.** The pixel draws `many_light_candidates` lights at random from its cluster's,
   and keeps one by weighted reservoir sampling, weighed by how much light each would bring with
   no shadow (its brightness, its range and cone falloff, and how squarely the surface faces it).
   A shadow ray to the light kept clears its weight if something is in the way, so blocked lights
   aren't passed on.
2. **Last frame.** The pixel's place in the world is projected through last frame's camera; if
   the surface there is alike (normals within about 25 degrees, places within a tenth of the view
   depth), its reservoir from last frame is merged in, counted for at most 20 times the
   candidates.
3. **Neighbors.** Three pixels within 12 pixels, picked at random, merge their reservoirs in
   when their surfaces are alike.
4. **Shadow.** One ray to the light the reservoir ends with, through every mesh the frame draws
   (`path_blocked`, as ray-traced shadows), by the light's `shadow_caster_mask` and opacity.

The mesh pass then lights the pixel with each reservoir's light, weighed by its reservoir's
weight and divided by the reservoirs, in place of its cluster's omni and spot lights; directional
lights are lit as before. Merging is the paper's biased variant: reservoirs combine by their
candidate counts, with no visibility check between pixels.

Random numbers hash the pixel, the frame's number, the reservoir and the step, so a frame is the
same on any number of threads and every machine. A target keeps its reservoirs, the surfaces
they were for and the camera between frames; changing the omni and spot lights starts them over.

## What it does not do yet

- **Only the software device.** GPU devices, and the OpenGL and WebGPU devices, light every light
  in the cluster as before, with their shadow maps.
- **No denoiser.** With one reservoir a pixel the image is grainy where many lights overlap; TAA
  smooths it over frames, and `many_light_samples` up to 4 trades speed for less noise.
- **Biased merging.** Reservoirs combine without the paper's MIS weights or visibility checks
  between pixels, so light can bleed a little across shadow edges.
- **Point lights only.** Shadow rays go to the light's center, so a light's `size` makes no soft
  shadow here, as it does with ray-traced shadows.
- **Omni and spot lights only.** Area and emissive lights would need light sampling of their own.

[examples/many-lights](../../examples/many-lights/) lights a hall with 64 shadowed lamps.
