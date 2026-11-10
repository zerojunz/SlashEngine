# Reflections in 2D

A pond at dusk that mirrors the sky, the moon and a boat, through a `Reflection2D`:
SlashEngine's own 2D reflection, as Godot games draw water with a screen texture shader.

[pond.scene.json](pond.scene.json) is a 160x90 window ([project.json](project.json)): a sky
gradient, a moon and a boat above the water line at y 50, the `pond` below it and a reed in
front. The `pond`'s rect starts at its `Transform2D` and runs `size` across and down. Its top
edge is the mirror line. What its canvas drew before it shows upside down below the line,
lit where it is in the world. It is rippled sideways (`ripple_amplitude` pixels, a wave every
`ripple_wavelength` pixels, `ripple_speed` waves a second), faded with depth (`fade`) and tinted
blue (`tint`, `opacity`).

[calls.jsonl](calls.jsonl) checks it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `frame.capture` at the moon `[120, 20]` and 30 pixels below the line `[120, 80]` finds it
  there, dimmed and tinted blue; so are the boat and the sky. The reed draws after the pond
  (`z_index` 1), so it stands in front of the water and is not reflected.
- At the boat's left edge in the water, `[38, 53]` to `[38, 59]`, the ripples move the edge's
  reflection left and right, and after a second (`step` 60 ticks) the waves have moved on, so
  the same pixels change.
- With the ripples, fade and tint turned off, the moon's and boat's reflections are their own
  colors.
- Raising the boat over the pond (`z_index` 2) takes it out of the reflection, which then shows
  the sky behind it, as anything drawn after a reflection is.
- `frame.summary` lists the pond as a `Reflection2D` with its rect.

The reflection draws into a texture of its own each frame (its target, the rect's size in
pixels), so it costs one more pass over what draws before it. Draws of a `GPUParticles2D` are
left out of it. In 3D, the environment's screen-space reflections (`ssr_*`) do the same for
meshes (see `examples/ssr-3d`).
