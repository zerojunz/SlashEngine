# Imported assets

Models made in other tools, bought from asset stores or drawn by a team's own artists go into the
asset library as generated ones do. `asset.add` takes a `.gltf`, `.obj`, `.fbx` or `.blend` file
and keeps it as a self-contained `.glb` file, cleaned up with [`model.cleanup`](../model-cleanup/)'s
options, described, reviewed if asked, and recorded with its license, its author and the file it
came from. As Godot's `.import` files do, the library keeps the source file's hash and its
cleanup settings, so an edit to the file is noticed and imported again the same way.

[game/props/crate.obj](game/props/crate.obj) is a crate an artist exported from a CAD tool: a
metre wide in centimeters, with z up. [main.cpp](main.cpp) drives the warehouse game in
[game](game/), then a market town game in [town](town/), and
[expected.txt](expected.txt) holds the output:

- `asset.add` imports the crate turned to y up, scaled to meters, standing on its origin and with
  one convex hull for collision; its surface had no texture coordinates, so it is unwrapped.
  `review` looks at it as `asset.review` does: with no `providers.json`, no vision model looks at
  the turntable views, and the closed box with no holes is accepted.
- `asset.info` shows the record: a `.glb` model with its `source` file, the start of the file's
  SHA-256 and the cleanup it was imported with, and the review as its feedback.
  `library.json` keeps the whole hash beside the file.
- The artist makes the crate taller. `asset.check` says the file changed since it was imported,
  and adding it again keeps a new version 1.2 meters tall, its parent the first, imported with the
  same cleanup and with the same description, tags, license and author.
- The market town finds both versions with `asset.search` and takes the new one as
  `props/crate.glb`, which `model.inspect` shows with its material and convex collision.

See [asset.add](../../docs/reference/commands.md#importing-models).
