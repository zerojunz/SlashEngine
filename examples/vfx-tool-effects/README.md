# VFX tool effects

Screen distortion and the effects the creature, vehicle and foliage tools play through `.vfx.json`
recipes, after Niagara's distortion materials and the Niagara components a Blueprint starts on
its events (Godot has neither: its distortion is a shader reading `SCREEN_TEXTURE`, and its
effects are scenes a script spawns).

- [fauna/dog.creature.json](fauna/dog.creature.json) names `fx/joy.vfx.json` for
  `creature_fed`: feeding the dog with `creature.interact` plays it as `rex_creature_fed`, a
  `ScreenDistortion3D` ripple that fades along a curve and a burst of sparkles.
- [vehicles/van.vehicle.json](vehicles/van.vehicle.json) names `fx/exhaust.vfx.json` for
  `running`: while its throttle is open `van_running` follows the van, a heat haze and smoke at
  its tail, and when it closes the effect stops, its smoke finishes and it goes.
- [flora/tuft.foliage.json](flora/tuft.foliage.json) names `fx/rustle.vfx.json` for
  `foliage_parted`: the kid beside the first tuft of [meadow.scene.json](meadow.scene.json)
  pushes it aside, which plays a rustle at its foot; the tuft far from the kid stays still.
- [fx/shockwave.vfx.json](fx/shockwave.vfx.json) is a 2D shockwave: a `ScreenDistortion2D`
  whose ring moves out along `progress` as it fades.

[calls.jsonl](calls.jsonl) feeds the dog, drives the van and stops it, opens the meadow, then
plays the shockwave and captures it: `frame.capture`'s `drawn` counts the `distortions`.
[results.jsonl](results.jsonl) holds the replies.

- The world, with the canvas layers below it, is drawn into a texture and shown through a grid
  of triangles whose texture coordinates each distortion moves, so every device draws the same
  pixels; controls and the canvases above the world are drawn over it unbent.
- A 3D distortion is placed and sized through the current `Camera3D` (`radius` in meters), a 2D
  one through its canvas (`radius` in pixels).
- A recipe's `vfx` names an effect per world event about the entity it draws; the effect plays
  at the event's `position`, or at the entity, as `<entity>_<event>`, and frees itself when done.
