# SlashEngine documentation

SlashEngine is a lightweight, free and open-source C++20 game engine whose primary user is an AI
agent. These docs are written for both people and agents: plain Markdown, one topic per file,
real commands and real output, and no screenshots.

Status: current · Updated: 2026-11-20

## Where to start

| You want to | Read |
| --- | --- |
| Know where the engine is headed and what is done | [roadmap.md](roadmap.md) |
| Build the engine and drive your first scene | [getting-started.md](getting-started.md) |
| Understand how the pieces fit together | [concepts.md](concepts.md) |
| Look up a command, its params and what it returns | [reference/commands.md](reference/commands.md) |
| Read every component's fields and every command's params, as this engine describes them | `slash-cli docs` ([reference/cli.md](reference/cli.md#docs)) |
| Copy code or command calls that are known to work | [../examples/](../examples/README.md) |
| Look up a `slash-cli`, `slash-mcp` or `slash-game` flag | [reference/cli.md](reference/cli.md) |
| Write or check a `.scene.json` file | [reference/scene-format.md](reference/scene-format.md) |
| Find the new name of a component an older scene, doc or model answer uses | [reference/renamed-names.md](reference/renamed-names.md) |
| See how the world tools fit together: recipes, 2D and 3D, one simulation, procedural graphs | [concepts.md](concepts.md#building-worlds) |
| Make characters, faces, animals or vehicles from recipes | [Characters](reference/scene-format.md#characters), [Faces](reference/scene-format.md#faces), [Creatures](reference/scene-format.md#creatures), [Vehicle recipes](reference/scene-format.md#vehicle-recipes) |
| Paint foliage, grow hair, blow wind or break things | [Foliage](reference/scene-format.md#foliage), [Hair and fur](reference/scene-format.md#hair-and-fur), [Wind](reference/scene-format.md#wind), [Destruction](reference/scene-format.md#destruction) |
| Build effects, fluids, smoke and water | [Visual effects](reference/scene-format.md#visual-effects), [Fluids](reference/scene-format.md#fluids), [Water](reference/scene-format.md#water) |
| Fill a world from rules | [Procedural graphs](reference/scene-format.md#procedural-graphs), [Scatter rules](reference/scene-format.md#scatter-rules) |
| Give NPCs plans, quests and dialogue | [NPCs](reference/scene-format.md#npcs), [Quests](reference/scene-format.md#quests), [Dialogue](reference/scene-format.md#dialogue) |
| Record a game's characters, story, rules or decisions | [reference/storyboard-format.md](reference/storyboard-format.md) |
| Set up a game's `project.json` | [reference/project-format.md](reference/project-format.md) |
| Add a closed vendor SDK (ads, a store, Steamworks) to a game | [reference/sdk-format.md](reference/sdk-format.md) |
| Show ads, log analytics events, sell in-app purchases, or unlock achievements and submit scores | [reference/services.md](reference/services.md) |
| Send packets between the machines in a multiplayer game | [reference/networking.md](reference/networking.md) |
| Build the engine with double-precision positions for worlds far from the origin | [reference/precision.md](reference/precision.md) |
| Try virtualized geometry (Nanite-style cluster hierarchies), a research feature | [reference/virtualized-geometry.md](reference/virtualized-geometry.md) |
| Try SDFGI, Godot's fully dynamic global illumination, a research feature | [reference/sdfgi.md](reference/sdfgi.md) |
| Light scenes of many shadowed lights with ReSTIR, a research feature | [reference/restir.md](reference/restir.md) |
| Draw scenery captured from photos as Gaussian splats, a research feature | [reference/gaussian-splats.md](reference/gaussian-splats.md) |
| Read what digital humans needed and the order they came in, a research note | [reference/digital-humans.md](reference/digital-humans.md) |
| Call a language model from a game, a tool or a test | [reference/providers.md](reference/providers.md) |
| Have Autopilot's agent play a game to find bugs, verify a claim or reach a goal | [reference/autopilot.md](reference/autopilot.md) |
| Bring a game over from Godot | [reference/godot-import.md](reference/godot-import.md) |
| Bring a game over from Unity | [reference/unity-import.md](reference/unity-import.md) |
| Bring a game over from Unreal Engine | [reference/unreal-import.md](reference/unreal-import.md) |
| Call the engine over HTTP, JSON-RPC or MCP | [reference/protocols.md](reference/protocols.md) |
| See what any agent is doing in a project, or did | [reference/watching-agents.md](reference/watching-agents.md) |
| Measure what agent tasks cost in calls and tokens | [reference/benchmark.md](reference/benchmark.md) |
| Check that each coding agent edits a scene over MCP | [exit-check/README.md](exit-check/README.md) |
| Read the full design and the reasons behind it | [design.md](design.md) |
| Work on the engine's own code | [../AGENTS.md](../AGENTS.md), then its [reference index](../AGENTS.md#reference-index) |

## All docs

Every doc in this folder. Each new doc gets a row in the same PR ([DOCS-STANDARD.md](DOCS-STANDARD.md)). Pages under `reference/` marked "engine notes" hold the subsystem detail that used to be in AGENTS.md.

| Doc | What it covers | Status |
| --- | --- | --- |
| [getting-started.md](getting-started.md) | Build SlashEngine, run a scene, change it with commands, connect an agent. | current |
| [concepts.md](concepts.md) | How the pieces fit together and the rules every command follows. | current |
| [design.md](design.md) | The approved design and the reasons behind it; changes only when a design decision changes. | current |
| [roadmap.md](roadmap.md) | The ordered plan to launch and beyond, with a status per item. | current |
| [DOCS-STANDARD.md](DOCS-STANDARD.md) | Docs standard: templates, length caps and rules for every doc. | current |
| [reference/animation.md](reference/animation.md) | Keyframe animation and animation trees. | current |
| [reference/asset-library.md](reference/asset-library.md) | The asset library: generators, commands and storage. | current |
| [reference/assets.md](reference/assets.md) | Asset loading and hot reload. | current |
| [reference/audio.md](reference/audio.md) | Sound files, the mixer, buses and effects. | current |
| [reference/autopilot.md](reference/autopilot.md) | Autopilot, an agent that plays a game to find bugs or reach a goal; plus engine notes. | current |
| [reference/benchmark.md](reference/benchmark.md) | What standard agent tasks cost in calls and tokens, and the CI baseline. | current |
| [reference/build-and-test.md](reference/build-and-test.md) | Full build, test, lint and CI-parity commands for engine work, per platform. | current |
| [reference/cli.md](reference/cli.md) | `slash-cli`, `slash-mcp` and `slash-game`: every flag and subcommand. | current |
| [reference/commands.md](reference/commands.md) | Every command: params, capabilities and a real example. | current |
| [reference/components.md](reference/components.md) | How to add a reflected component and register its docs. | current |
| [reference/debug-overlay.md](reference/debug-overlay.md) | The debug overlay over a running game. | current |
| [reference/digital-humans.md](reference/digital-humans.md) | Research note: what digital humans needed and the order they came in. | current |
| [reference/engine-modules.md](reference/engine-modules.md) | What each engine module under engine/ holds. | current |
| [reference/game-projects.md](reference/game-projects.md) | Game projects and exports: presets and building for each platform (engine notes). | current |
| [reference/gameplay-code.md](reference/gameplay-code.md) | Writing and reloading a game's own C++ code. | current |
| [reference/gaussian-splats.md](reference/gaussian-splats.md) | Research: scenery captured from photos as Gaussian splats. | current |
| [reference/godot-import.md](reference/godot-import.md) | Importing a Godot 4 project. | current |
| [reference/importing.md](reference/importing.md) | How the Godot, Unity and Unreal importers work (engine notes). | current |
| [reference/input.md](reference/input.md) | Input events, action maps and input state. | current |
| [reference/jobs.md](reference/jobs.md) | The job system. | current |
| [reference/localization.md](reference/localization.md) | Translation files, locales and plural rules. | current |
| [reference/navigation.md](reference/navigation.md) | Navigation meshes, maps, paths and agents. | current |
| [reference/networking.md](reference/networking.md) | Multiplayer packets between machines, as Godot's high-level multiplayer; plus engine notes. | current |
| [reference/npcs.md](reference/npcs.md) | NPC brains, planning, quests, dialogue and fauna. | current |
| [reference/particles.md](reference/particles.md) | CPU and GPU particles. | current |
| [reference/physics-2d.md](reference/physics-2d.md) | 2D physics on Box2D. | current |
| [reference/physics-3d.md](reference/physics-3d.md) | 3D physics on Jolt. | current |
| [reference/precision.md](reference/precision.md) | Building with double-precision positions. | current |
| [reference/procedural.md](reference/procedural.md) | Procedural generation: noise, textures, scatter rules and PCG graphs. | current |
| [reference/profiling.md](reference/profiling.md) | Profiling a running game. | current |
| [reference/project-format.md](reference/project-format.md) | A game's `project.json` and folder layout. | current |
| [reference/protocols.md](reference/protocols.md) | The engine's commands over CLI, JSON-RPC, MCP and REST; plus engine notes on the command registry and HTTP server. | current |
| [reference/providers.md](reference/providers.md) | `ai::Provider`: calling a language model from a game, tool or test; plus engine notes. | current |
| [reference/rendering.md](reference/rendering.md) | The renderer: devices, passes, materials, lighting and ray tracing (engine notes). | current |
| [reference/replays.md](reference/replays.md) | Recording and replaying a game session. | current |
| [reference/restir.md](reference/restir.md) | Research: many shadowed lights with ReSTIR. | current |
| [reference/scene-format.md](reference/scene-format.md) | `.scene.json` and `.prefab.json` files. | current |
| [reference/scenes.md](reference/scenes.md) | Scene files and the commands that edit them (engine notes). | current |
| [reference/sdfgi.md](reference/sdfgi.md) | Research: SDFGI, fully dynamic global illumination. | current |
| [reference/sdk-format.md](reference/sdk-format.md) | An SDK package adds a closed vendor SDK (an ad network, a store, analytics, Steamworks, an upscaler) to a game without changing the... | current |
| [reference/services.md](reference/services.md) | Ads, analytics, purchases, achievements and leaderboards; plus engine notes. | current |
| [reference/soundtracks.md](reference/soundtracks.md) | Scores, MIDI, stems and cue sheets. | current |
| [reference/storyboard-format.md](reference/storyboard-format.md) | The storyboard: a game's characters, story, rules and decisions; plus engine notes. | current |
| [reference/ui-text.md](reference/ui-text.md) | UI controls, themes and text. | current |
| [reference/unity-import.md](reference/unity-import.md) | Importing a Unity project. | current |
| [reference/unreal-import.md](reference/unreal-import.md) | Importing an Unreal Engine 5 project. | current |
| [reference/video.md](reference/video.md) | AV1 video decoding, encoding and playback. | current |
| [reference/virtualized-geometry.md](reference/virtualized-geometry.md) | Research: Nanite-style cluster hierarchies. | current |
| [reference/watching-agents.md](reference/watching-agents.md) | Watching agents work: the work journal, watch servers and `slash-cli watch`. | current |
| [reference/xr.md](reference/xr.md) | The XR server, interfaces, trackers and OpenXR. | current |
| [exit-check/README.md](exit-check/README.md) | Milestone 3 exit check: each coding agent edits a scene over MCP. | current |
| [exit-check/claude-code.md](exit-check/claude-code.md) | Generated result of the exit check for Claude Code. | historical |

## Reading order for agents

An agent new to the engine needs only three things, in this order:

1. Call `engine.describe` (MCP tool `engine_describe`). The engine reports its own components,
   default values, commands and the open scene, and that answer always matches the build you are
   talking to. These docs describe the same things but can lag behind a change.
2. Skim [concepts.md](concepts.md) for the rules every command follows: capabilities, undo,
   all-or-nothing batches and error shapes.
3. Use [reference/commands.md](reference/commands.md) when you need an example of a call and its
   result, and [examples/](../examples/README.md) for whole tasks: C++ components, systems and
   commands, and sequences of calls, each run by CI with its output checked.

An agent changing the engine itself starts with [AGENTS.md](../AGENTS.md), then reads the section
of [design.md](design.md) for the area it is changing, then checks [roadmap.md](roadmap.md) to see
whether that work is already planned for a milestone.

## How these docs are kept true

- **The engine is the source of truth.** `engine.describe`, `schema.get` and
  `GET /v1/openapi.json` are generated from the code. When a doc and the engine disagree, the
  engine is right and the doc is a bug.
- **Examples are real.** Every JSON result shown in these docs was produced by running the command
  against `tests/data/basic.scene.json`, `tests/data/game` or a project made by `slash-cli init`.
  Re-run an example when you change the command it shows.
- **A change updates its docs.** A pull request that adds or changes a command, flag, component or
  file format updates the matching reference page, and a pull request that finishes roadmap work
  updates [roadmap.md](roadmap.md) in the same change.
- **No dates.** The roadmap is ordered, not scheduled. Status words (Done, In progress, Next,
  Planned, After launch, Research) say where each item stands.
- **Plain style.** Short sentences, one idea per paragraph, commands in code blocks, every term
  defined once in [concepts.md](concepts.md). Agents and people read the same text.

[design.md](design.md) is the approved design and changes only when a design decision changes;
everything else in this folder follows the code.
