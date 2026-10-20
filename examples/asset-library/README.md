# Asset library

Images and sounds kept once on the machine and reused by every project, so agents stop making
the same asset again. [main.cpp](main.cpp) has two projects, `game/` and `sequel/`, share one
library and calls the commands as an agent does over MCP (tools `asset_search`,
`asset_generate` and the rest) or JSON-RPC. `expected.txt` holds each call and its reply.

- `asset.generate` makes an image or sound with a generator from `providers.json` and keeps it;
  `path` copies it into the project at once. The built-in `test` generator, used here, makes
  placeholders with no model or key.
- The same request again, from any project, returns the stored asset with `"reused": true` and
  calls no model. A close one returns the matches as `candidates` and makes nothing until asked
  with `"similar": false`.
- `asset.refine` makes a new version linked to its parent; `asset.info` lists the versions.
- `asset.add` puts a project's own file in, with its author and license, and `asset.use` copies an
  asset into another project.
- Each project's `library.json` records where its files came from and under what license.
- The library is `~/.slash/library`, or `SLASH_LIBRARY_DIR`; this example keeps its own in
  `library/`.
