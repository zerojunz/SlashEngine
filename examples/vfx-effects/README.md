# VFX effects

Visual effects as `.vfx.json` recipes, after Unreal's Niagara systems: layers on one timeline,
each an entity of components (particles, lights, decals, screen flashes, camera shakes) that
appears at its start, has fields moved along curves and goes at its end, its particles finishing
first. Values may be expressions over the recipe's parameters.

- `fx/explosion.vfx.json`: a flash of light that fades, a fireball, smoke only up close, a scorch
  left where it went off and a camera shake, sized by its `size` parameter, with levels of detail
  and a budget.
- `fx/campfire.vfx.json`: a looping fire whose light flickers along a curve.
- `fx/sparks.vfx.json`: what `profiles/barrel.damage.json` plays where the barrel is hit, as many
  sparks as the hit was hard.
- `fx/slash.vfx.json`: a 2D sword slash with sparks, a glow, a white flash over the screen and a
  shake of the 2D camera, played on the knight.

The calls check the explosion's cost against its budget (too many particles at full size, fine
at half), preview it alone as numbers and a PNG, near and far, then play it in the scene, hit the
barrel and slash with the knight, read what plays, and read the effects' events.

```bash
slash-cli rpc --project examples/vfx-effects < examples/vfx-effects/calls.jsonl
```
