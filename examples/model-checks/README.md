# Model checks

A complex asset is counted on by other files: a scene places the stool under a counter, a light
hangs from the lamp's `bulb`, an animation turns a door by its node's name. Checks say what a model
must be for them, and the engine holds every version to them, before the asset library keeps it
and again in CI.

A recipe carries its own, as [props/stool.model.json](game/props/stool.model.json) does:

```json
"checks": {"size": {"min": [0.3, 0.4, 0.3], "max": [0.4, 0.6, 0.4]}, "pivot": [0.5, 0, 0.5],
           "max_triangles": 2000, "materials": ["oak", "steel"], "nodes": ["seat", "post", "foot"]}
```

`size` bounds its box in meters, `pivot` puts its origin on the ground in the middle, and the
names are those other files use: materials, nodes (parts and attachment points), a skin's
`bones` and `animations`. `slash-cli scene validate`, `project validate` and exports report a
recipe that fails them at the check's JSON Pointer, and each variant and quality tier is checked
too, with its own `checks` over the recipe's.

Any model, a `.glb` included, gets checks from `asset.add`'s `checks`. The library keeps them
with the asset, every later version must pass them unless the call gives new ones, and the
project's `library.json` keeps them beside the file, so `asset.check` and `test.run` check the
project's copy where no library is, as on CI.

[main.cpp](main.cpp) does what an agent does, and [expected.txt](expected.txt) holds the output:

- `asset.add` keeps the stool, which passes its checks, and the lamp with checks for its height
  and its `bulb`; `library.json` keeps them, and `test.run` passes.
- The stool made 0.75 m tall is refused, its size outside the recipe's checks.
- The lamp's bulb renamed: `asset.check` reports the check it fails, `test.run` fails at it in
  `library.json`, and `asset.add` won't keep the edit as a version of the lamp.
