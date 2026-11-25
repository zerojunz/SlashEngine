# Mesh maps

A texture that suits a model knows its shape: dirt gathers in crevices, paint wears off edges, the
top fades in the sun. `model.bake_maps` bakes what a model's shape says about each texel of its
texture into PNG files a texture graph's `image` nodes read, the way Substance Painter's mesh map
baker does, so an agent masks with them instead of painting by hand.

[props/crate_high.obj](props/crate_high.obj) is a detailed crate, planks inset 1.5 cm under twelve
iron beams along its edges. [props/crate.obj](props/crate.obj) is the box a game draws, its six
faces laid out apart in its texture coordinates. The calls in [calls.jsonl](calls.jsonl):

1. Bake every map of the box from the detailed crate at 256 texels into `props/crate.maps/`:
   `ao` darkens the planks beside the beams, `curvature` lights the beams' edges, `thickness` is
   low through the thin beams, `sky` is white on top and black underneath, `material_id` tells
   wood from iron, and `height` puts the planks 1.5 cm under the beams. `stats` gives each gray
   map's `[min, mean, max]`, so the agent sees what each holds without looking.
2. The same call again bakes nothing: `maps.json` in the folder keeps the bake's hash, and the
   result comes back with `"cached": true`.
3. The detailed crate's own ambient occlusion, curvature and ids, at 128 texels, with its two
   parts (`planks` and `frame`) in their own colors.
4. [textures/crate.texture.json](textures/crate.texture.json), a texture graph, multiplies wood
   grain by `ao.png` and screens the edges `curvature.png` finds over it, so the crate's albedo is
   dirty in its corners and worn on its edges.

Run it with `slash-cli rpc --project . < calls.jsonl`; [results.jsonl](results.jsonl) holds the
replies. With `"device": "gpu"`, a GPU that traces rays bakes the same maps.
