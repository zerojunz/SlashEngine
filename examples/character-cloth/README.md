# Character cloth

Cloth that a character wears, as Unreal's Chaos Cloth: a hero's cape on its skin in 3D
(`Cloth3D`) and a cut-out kid's cape in 2D (`Cloth2D`). [main.cpp](main.cpp) writes the hero's
model in code (`hero.glb`, a one-bone figure with a body and a cape behind it), opens
[main.scene.json](main.scene.json), then steps the world, moves the hero's spine and the kid with
`component.set`, turns a breeze on and reads the capes' points with `physics.soft_body`.
[expected.txt](expected.txt) holds its output.

- `Cloth3D`'s `section` names the material whose surfaces are cloth (`cape`). Its vertices are
  welded into the points of one Jolt soft body, each skinned as the model is and free to move up
  to its share of `max_distance` from where the skin puts it. With the default
  `"gradient"` map along `[0, -1, 0]`, the top row's share is 0, so the shoulders hold it
  exactly, and the hem's is 1.
- Moving the spine moves the cape's top at once; the hem trails behind and then swings back
  under the hero, never further than 0.4 meters from where the skin puts it. Long range
  attachments tether each point to the held top, so the cape can't stretch while it falls.
- `Cloth2D` is a sheet of `columns` by `rows` points (4 by 8 here) hanging from its entity's
  `Transform2D`, held, tethered and limited the same way, stepped by 2D physics after the bodies
  and drawn by the canvas with its `color` and `texture`. Its points keep `point_radius` from the
  2D bodies its `collision_mask` meets.
- The `WindSource2D` breeze carries the kid's cape by its `drag_coefficient`; a `Cloth3D` feels
  `WindSource3D`s the same way, and both feel gravity and physics fields.
- `physics.soft_body` reads either: a `Cloth3D`'s points in the world, a `Cloth2D`'s row by row
  from the top, with `bounds` `[x, y, w, h]` in pixels.
- Results are the same on every platform: Jolt is built cross-platform deterministic, and the 2D
  sheet's solver uses no libm but square roots and builds with `-ffp-contract=off`.
