# Asset library

Images, sounds and models kept once on the machine and reused by every project, so agents stop making
the same asset again. [main.cpp](main.cpp) has two projects, `game/` and `sequel/`, share one
library and calls the commands as an agent does over MCP (tools `asset_search`,
`asset_generate` and the rest) or JSON-RPC. `expected.txt` holds each call and its reply.

- `asset.generate` makes an image or sound with a generator from `providers.json` and keeps it;
  `path` copies it into the project at once. The built-in `test` generator, used here, makes
  placeholders with no model or key. A real project names its own, and the calls stay the same:

  ```json
  {"generators": {
    "art": {"type": "openai-image", "api_key_env": "OPENAI_API_KEY", "license": "OpenAI terms of use"},
    "sfx": {"type": "elevenlabs-sound", "api_key_env": "ELEVENLABS_API_KEY"}
  }}
  ```

  Images come back fitted to the `width` and `height` asked for, whatever size the service drew.
- The same request again, from any project, returns the stored asset with `"reused": true` and
  calls no model. A close one returns the matches as `candidates` and makes nothing until asked
  with `"similar": false`.
- `asset.refine` makes a new version linked to its parent; `asset.info` lists the versions.
- `asset.add` puts a project's own file in, with its author and license, and `asset.use` copies an
  asset into another project.
- Each project's `library.json` records where its files came from and under what license.
- `asset.feedback` records whether an asset worked and why. Search ranks by it, assets rejected
  more than accepted stop coming back as candidates, and the reasons go into later prompts for
  assets like them, along with the storyboard's tone and palette.
- `asset.check` records feedback with no one asking. It opens each scene that draws a library
  file, here the sequel's night level, and looks at it as `frame.summary` does with
  `contrast`: the red slime's dark outline is lost against the night sky, so it is rejected
  with the reason, and the green slime and the heart are accepted. Every whole-project
  `test.run` runs it.
- Models go in as [`.model.json` recipes](../../docs/reference/scene-format.md#model-recipes),
  kept with their size and triangles. `asset.refine` makes a version of the bench with its
  `length` parameter doubled: no generator is called, and the legs move out with the seat.
- The library is `~/.slash/library`, or `SLASH_LIBRARY_DIR`; this example keeps its own in
  `library/`.
