# Layer stacks

A `.paint.json` file is a model's or a sprite's texturing as layers, bottom to top, the way
Substance Painter's layer stacks are: each layer a material laid over the ones below through a
mask, the masks reading the model's mesh maps, so an agent paints worn edges, dirt in the
crevices and dust on top as text and tunes them by the numbers each call returns.

[props/crate.obj](props/crate.obj) is a plain box and [props/crate_high.obj](props/crate_high.obj)
a detailed crate, planks inset under iron beams (the two models of the
[mesh-maps](../mesh-maps/) example). [props/crate.paint.json](props/crate.paint.json) paints the
box from the detailed crate's maps: wood with noise grain, iron where `height` finds the beams,
bare metal where `curvature` finds the beams' edges (more of it as `wear` rises), dirt where `ao`
is low and dust where the `world_normal` points up. The calls in [calls.jsonl](calls.jsonl):

1. `paint.bake` bakes the mesh maps the masks read, then the stack, into `props/crate_albedo.png`,
   `crate_orm.png` and `crate_normal.png`, and writes `props/crate.standard_material.json` to
   draw them. The reply measures each texture and each mask's coverage (`iron` 0.364: the beams),
   and the texture's `texel_density` in texels a meter.
2. The same call again writes nothing and comes back `"unchanged": true`.
3. `paint.edit` adds a `moss` layer by name, `below` the dust, that raises the surface where the
   occlusion is low; the reply lists the layers in order, and the stack now bakes a height map
   too, its slopes bent into the normal.
4. `paint.preview` draws the painted crate from two sides into `build/crate_preview.png`, with
   each view's measures.
5. [props/crate_rusty.paint.json](props/crate_rusty.paint.json) `extends` the crate, turning its
   iron to rust and its wear up; `parameters` turns `wear` to 1 for this one bake.
6. [sprites/shield.paint.json](sprites/shield.paint.json) paints a 2D sprite: a metal rim along
   its outline's edge, a scratch stroked along three points and grime from noise, keeping its
   alpha.
7. `texture.check` holds every stack to the ranges in its `expect`.

Run it with `slash-cli rpc --project . < calls.jsonl`; [results.jsonl](results.jsonl) holds the
replies. A stack bakes as a texture graph, in compute on GPUs and the same with the CPU twin.
