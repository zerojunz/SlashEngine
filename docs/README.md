# SlashEngine documentation

SlashEngine is a lightweight, free and open-source C++20 game engine whose primary user is an AI
agent. These docs are written for both people and agents: plain Markdown, one topic per file,
real commands and real output, and no screenshots.

## Where to start

| You want to | Read |
| --- | --- |
| Know where the engine is headed and what is done | [roadmap.md](roadmap.md) |
| Build the engine and drive your first scene | [getting-started.md](getting-started.md) |
| Understand how the pieces fit together | [concepts.md](concepts.md) |
| Look up a command, its params and what it returns | [reference/commands.md](reference/commands.md) |
| Copy code or command calls that are known to work | [../examples/](../examples/README.md) |
| Look up a `slash-cli`, `slash-mcp` or `slash-game` flag | [reference/cli.md](reference/cli.md) |
| Write or check a `.scene.json` file | [reference/scene-format.md](reference/scene-format.md) |
| Record a game's characters, story, rules or decisions | [reference/storyboard-format.md](reference/storyboard-format.md) |
| Set up a game's `project.json` | [reference/project-format.md](reference/project-format.md) |
| Add a closed vendor SDK (ads, a store, Steamworks) to a game | [reference/sdk-format.md](reference/sdk-format.md) |
| Show ads, log analytics events, sell in-app purchases, or unlock achievements and submit scores | [reference/services.md](reference/services.md) |
| Call a language model from a game, a tool or a test | [reference/providers.md](reference/providers.md) |
| Bring a game over from Godot | [reference/godot-import.md](reference/godot-import.md) |
| Bring a game over from Unity | [reference/unity-import.md](reference/unity-import.md) |
| Bring a game over from Unreal Engine | [reference/unreal-import.md](reference/unreal-import.md) |
| Call the engine over HTTP, JSON-RPC or MCP | [reference/protocols.md](reference/protocols.md) |
| Measure what agent tasks cost in calls and tokens | [reference/benchmark.md](reference/benchmark.md) |
| Read the full design and the reasons behind it | [design.md](design.md) |
| Work on the engine's own code | [../AGENTS.md](../AGENTS.md) |

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
