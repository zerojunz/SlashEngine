# Asset library

The asset library: generators, commands and storage.

Status: current · Updated: 2026-11-20

`engine/library` keeps generated and added images, sounds, music and model recipes once per machine
(`~/.slash/library`, or `SLASH_LIBRARY_DIR`; `SessionOptions::library_dir` in tests), stored by
the SHA-256 of their bytes (`slash/core/sha256.hpp`) with a record of how each was made: request,
generator, model, seed, license, tags, a short description and the parent it was refined from.
Files are written whole and moved into place (`platform::rename_file`), so agents can share a
library. Generators are `library::Generator` adapters named in `providers.json`'s `generators`,
as models are `ai::Provider`s: `openai-image`, `gemini-image`, `automatic1111`, `openai-speech`,
`elevenlabs-sound`, `elevenlabs-music`, `stability-audio`, `stability-3d`, `fal-trellis` (Microsoft's TRELLIS on fal.ai) and `meshy-rig` (`src/adapters.cpp`, through `ai::post_with_retries`, with images fitted
to the size asked for by `fit_image`), and the built-in `test` generator, which draws
placeholders with integer math only, so they are the same bytes everywhere. `asset.search`, `asset.generate` (an exact repeat
calls no generator, close matches come back as candidates), `asset.refine`, `asset.info`,
`asset.use`, `asset.add`, `asset.feedback`, `asset.check`, `asset.style` and `asset.sync` are in `slash_library_commands`, registered by
slash-cli and slash-mcp; using an asset copies it into the project, records its source and
license in the project's `library.json` and checks an image or model against the storyboard's
palettes and budgets. Feedback (accepted or rejected, and why) ranks search results, keeps
rejected assets out of candidates and, with the storyboard's tone, palette and references, goes
into later prompts (`prompt_context` in `src/commands.cpp`). `asset.style` refines the style
from what was accepted: `storyboard::style_edits` drafts palette colors, budgets and references
from the project's accepted assets as edits for `storyboard.edit`, and prompts carry the
references of other assets of the type asked for; `examples/asset-style` drafts a cafe's style. `asset.check`, which `test.run` runs on a whole
project, records feedback by itself: it opens each scene that draws a library file and checks
what `canvas::summarize` sees with `SummaryOptions::contrast` (the edge contrast of each thing
in the frame `canvas::capture` draws, `Shown::contrast`) and how small each file is drawn, from
the texture paths on `FramePiece`s. Models are `.model.json` recipes kept with their `size`,
`triangles`, `pivot`, parts and points from `scene3d::summarize`, and `.glb` files that hold
their own buffers and images (`describe_recipe` and `describe_glb` in `src/library.cpp`; recipes
naming texture files are refused,
whose paths are a project's); `asset.refine` with `parameters` makes a version with new parameter
values and no generator (`refine_model` in `src/commands.cpp`). `asset.add` of a file the
project's `library.json` says came from an asset, changed since, keeps it as a new version of
that asset (`used_version`). A model in another format (`.gltf`, `.obj`, `.fbx`, `.blend`), or
one with `cleanup`, is imported (`import_model`): loaded through the session's assets, cleaned up
with `model.cleanup`'s options (`command::read_cleanup` and `clean_up` in
`slash/command/model_cleanup.hpp`) and kept as the `.glb` file `write_glb` makes, its record
keeping the file's name, the SHA-256 of its bytes and the cleanup;
`library.json` keeps the hash, so `asset.check` reports an edited source, and adding it again
makes a new version with the same cleanup. `review` reviews what was kept as `asset.review`
does; `examples/imported-assets` imports a CAD crate and an edit of it. A recipe made of other model files (its model parts) is kept with
each part as a library reference, `"asset:<id>.glb"` (`library_form` and `add_part` put the part
files in first; `library::part_reader` reads references back, and `describe_file` and the team
check recipes with it), its record's `uses` listing them; `copy_into_project` copies a recipe's
parts into a `parts/` folder beside it (`project_form`), `share` sends parts to the team first,
`asset.info` gives `uses` and `used_by`, and `asset.refine`'s `parts` swaps one model for
another (`swap_parts`). `asset.review` (`src/review.cpp`) looks at a model before a game takes
it: `scene3d::check_geometry` (`src/geometry.cpp`: holes, triangles with no area, texel density)
and the storyboard's budgets, then `review::turntable` draws it from four sides on the software
device and `review::ask` sends the views to a vision model from `providers.json`, recording the
verdict as feedback; `examples/model-review` rejects a chest with no hinges. Models are generated
concept first: `asset.generate`'s `image` names a library image the model is made from and kept
under as its parent (`generate_model` in `src/commands.cpp`; the test generator makes a relief of
it, `make_model` in `src/generator.cpp`), and `review` reviews each and makes it again with the
reasons added to the prompt until one is accepted; `examples/concept-models` makes a stool. Models
can also be built by a Blender script: `asset.generate`'s `script` (a project `.py` file or its
text) and `parameters` go to the built-in `blender` generator (`src/blender.cpp`), which runs the
artist's Blender through `scene3d::run_blender_script` (`engine/scene3d/src/blend.cpp`: an empty
scene, the script with `params` and `seed`, exported as `.blend` imports are) and only with the
`build` capability; the record keeps the script's text, so `asset.refine` with new `parameters`
builds it again (`generate_scripted` and `refine_scripted` in `src/commands.cpp`).
`examples/blender-scripts` builds a bookshelf.

Materials are `.standard_material.json` files (`$cli schema standard_material`;
`slash/scene3d/material.hpp`), one `Surface3D` each, which a recipe's `materials` name by path
or `extends` with fields over them (`resolve_materials` in `engine/scene3d/src/recipe.cpp`,
reading through the same `ModelReader` as model parts). `asset.generate`'s `material` type
(`generate_material` in `src/commands.cpp`) makes one that tiles from a generated or library
image: `seam_ratio`, `make_seamless`, `normal_map` and `roughness_map`
(`slash/library/material.hpp`, integer math only) make the albedo tile and its maps, kept as
images and the material as type `material` naming them by `asset:` reference; `asset_refs` lists
what a recipe or material names, which `library_form`, `project_form` and `describe_file`
follow. `examples/tileable-materials` shares cobblestones between a wall and steps.

`entity.place` (`engine/command/src/place.cpp`) assembles scenes of such assets by constraints:
each entity goes to a point (`at`) or along a `Spline3D` (`along`, turned to face it with
`rotate`), is dropped onto what is below (a ray from
a meter above the bottom of its bounds; `on` limits what it may stand on), and is moved to the
nearest free spot in rings around it when its bounds overlap anything but what it stands on.
What it stands on and avoids is `navigation::world_geometry`, each entity's own geometry as a
navigation bake reads it, in world space. `examples/scene-assembly` lays out a yard.

A team shares a library it hosts (`slash/library/team.hpp`): `providers.json`'s `library`, or
`SLASH_LIBRARY_TEAM`, names a folder every teammate reaches or a library server
(`slash-cli library serve`, `library::TeamServer`, answering `POST /v1/library/<op>` with a
bearer token through `answer_team_request`). `library::Team` is the one interface over both;
the commands open it through `Libraries` in `src/commands.cpp` when they first need it, search
and answer exact repeats from it, bring assets the machine lacks from it, put what they make
and the feedback they record there, and report a team that can't be used as `team_error`
while working on the machine's library alone. `asset.sync` shares what was made before.

Music is type `music`, a sound kept with its `bpm`, `beat_count`, `bar_beats` and `key`
(`slash/library/music.hpp`): `describe_music` measures it with `audio::analyze`, keeping a
composer's own facts and a generator's measured ones, and sets a tempo whose beats nearly fill
the sound so whole beats do. `asset.generate` takes `bpm`, `key`, `bar_beats`, `loop` and a
storyboard `theme` (its key, tempo, meter, instruments and mood); music services hear them in
words added to the prompt, and the `test` generator plays a chord progression in whole bars with
integer math. `asset.add` takes a composer's track with its `stems`, each a music record linked
by `track`; using music writes its `.audio.json` settings beside it, and `asset.use` with
`stems` lays the stems out with a synchronized stream, as `soundtrack.render` does.
`examples/generated-music` generates a theme's music and adds a track with stems.
