# Weathering

A `.paint.json` layer stack can age a whole model by one number. A `weather` layer stands for four
layers, each grown by its amount times `age` (0 new, 1 old) and kept inside the layer's own mask:

- `rain`: dark streaks running down the faces that stand, along `gravity`, out of the sky.
- `rust`: rust on the edges and in the crevices, with runs below.
- `dust`: dust on the faces turned up, under open sky, more on the side the `wind` comes from.
- `moss`: moss in the shade and on the tops, in patches.

[props/stall.obj](props/stall.obj) is a market stall: a counter, two posts, an awning, a sign and an
iron hinge, each its own part. [props/stall.paint.json](props/stall.paint.json) paints it: wood with
grain, the counter in a `paint` color, the canvas awning, then

- `hinge_rust`: rust round the hinge, its mask a `point` named after the model's `hinge_l` part,
  as strong as `age`;
- `sign`: a decal of `text` ("FRESH FISH") drawn with the engine's fonts, shaped by HarfBuzz, so a
  translation paints just as well;
- `rain`: the `weather` layer, its age the stack's `$age` parameter, the wind from +x.

Its `damage` block wears it as gameplay damages it: [props/stall.damage.json](props/stall.damage.json)
is the stall's damage profile, and each bake also bakes the stack with `age` at 0.85 and at 1 and
writes those looks into the profile's `states`, so a `Destructible3D` stall looks older below 60%
and 30% of its health. Its `variants` draw three stalls from it, each its own paint color and age, the same on every
machine. The calls in [calls.jsonl](calls.jsonl):

1. `paint.bake` bakes the stall's mesh maps, then the stack, into `props/stall_albedo.png`,
   `stall_orm.png`, `stall_height.png` and `stall_normal.png` and a material. Each part of the
   weather has its own mask in the reply: `rain.rain`, `rain.rust`, `rain.dust`, `rain.moss`.
   `damage` lists the two worn looks, `props/stall_d1.standard_material.json` and `_d2`, now
   named by the profile's states.
2. The same with `age` at 1 for the call: more rain streaks and rust.
3. `"variant": "all"` bakes the three variants as `props/stall_v1_albedo.png` and on, each with
   its own `.standard_material.json`; the reply lists the parameters each one drew.
4. `paint.preview` draws the stall from two sides into `build/stall_preview.png`.
5. [sprites/shield.paint.json](sprites/shield.paint.json) weathers a 2D sprite the same way, rain
   running down the screen, with a letter stamped on it.
6. `texture.check` holds both stacks to their `expect`.

Run it with `slash-cli rpc --project . < calls.jsonl`; [results.jsonl](results.jsonl) holds the
replies.
