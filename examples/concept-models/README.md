# Concept models

Organic shapes are made concept first, as the design asks: a cheap 2D image is generated and
checked, and only then is a 3D model made from it. The image stays in the asset library as the
model's parent, so where a model came from is always known, and the same image and prompt are a
repeat that calls no generator.

`asset.generate` with `"type": "model"` takes the concept's ID as `image`. With `review`, each
model made is reviewed as `asset.review` does it: its geometry checked, turntable views drawn
headless and a vision model from `providers.json` asked whether they show the prompt. While it is
rejected, it is made again with the reasons added to the prompt ("The last model was rejected:
..."), up to that many models in all, and it is copied to `path` only once accepted.

[main.cpp](main.cpp) uses the built-in `test` generator, whose models are reliefs of the concept
image: its shape in cells a meter across, pushed back into a slab one cell, a sixth or half its
width deep as the last word about depth says. Stability AI's `stability-3d` generator makes real
models from the same calls. The vision model is a stand-in on the loopback interface that reads
the size in the request and wants a stool at least 30 cm deep. [expected.txt](expected.txt) holds
the output:

- The concept, a round red cap on a transparent background, is generated into `concepts/` and
  accepted with `asset.feedback`.
- The model made from it is a sixth of a meter deep, so the review rejects it as too thin. The
  second try's prompt carries that reason, it comes out half a meter deep and is accepted, and it
  is copied to `props/stool.glb`. `attempts` lists both tries.
- `asset.info` shows the concept as the model's parent and the prompt the second try was made
  with.
- A model asked for with no image is refused at `/image`.

See [asset.generate](../../docs/reference/commands.md#models-from-a-concept-image).
