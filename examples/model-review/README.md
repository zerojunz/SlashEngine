# Model review

A generated or imported model can pass every number and still be the wrong thing: a chest with
no hinges, a sign whose back is missing. `asset.review` looks at a model the way a person would
before it goes into a game, with no window and no person:

1. Its geometry is checked against the storyboard's budgets that cover its path (triangles,
   texture size and texel density) and the checks the library keeps with it; a size outside 1 cm
   to 100 m, holes and triangles with no area are warnings.
2. It is drawn from four sides, a little above, on a plain backdrop, lit by a key and a fill
   light that turn with the camera. The views are drawn on the software device, so they are the
   same pixels on every machine and a second look at the same model is the same request.
3. A vision-capable model from `providers.json` is sent the views, the request and what the
   checks found, and says whether the views show what was asked for, naming the view that shows
   each problem. A problem naming no real view is left out.
4. The verdict is recorded as the asset's feedback, so search, candidates and later prompts learn
   from it.

[main.cpp](main.cpp) starts a stand-in for the vision model on the loopback interface, so the
example needs no network or key; it decodes the views it is sent and looks for brass-colored
pixels, as a real model would look for the hinges. [game/storyboard/style.json](game/storyboard/style.json)
gives props a budget of 200 triangles and 128 texels a meter. [expected.txt](expected.txt) holds
the output:

- The oak chest from [game/props/chest.model.json](game/props/chest.model.json) is added to the
  library and reviewed against "An oak chest with brass hinges on its lid". No brass shows in any
  view, so it is `rejected` with the model's problem, and `out` writes the views as PNG files an
  agent can open.
- The agent adds two brass hinges to the recipe and keeps it as a new version. They show in views
  2 and 3, so the new version is `accepted`, and `asset.info` lists that feedback.
- A sign written as an `.obj` file is reviewed where it is with `"ask": false`: its back face is
  missing (a hole of four open edges) and its 16-pixel texture is stretched over a meter, 20 texels
  a meter where the budget asks for 128, so it is `rejected` without a model looking.

With no `providers.json`, `asset.review` makes the checks and the views and says no model looked.
See [asset.review](../../docs/reference/commands.md#assetreview).
