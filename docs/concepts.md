# Concepts

How SlashEngine is put together, and the rules every part follows. Read this once; the reference
pages assume it. Terms in **bold** are defined where they first appear and listed in the
[glossary](#glossary) at the end.

## The idea

SlashEngine is built for agents first. Four rules follow from that, and anything an agent cannot
do through them counts as a bug:

1. **The whole project is text.** Scenes are canonical JSON that any model can read and write.
2. **The engine describes itself.** Components and commands carry their own docs, defaults and
   JSON Schemas, and `engine.describe` returns them.
3. **Every action is a command.** One registry of commands is the only way to change a world, and
   it is served the same way over MCP, JSON-RPC, REST and the command line.
4. **An agent can observe the result.** Commands return structured JSON, logs are JSON lines, and
   later milestones add frame captures and frame summaries.

There is no GUI editor. People use a text editor, their agent and (later) a debug overlay.
Gameplay code is plain C++; there is no scripting language. Where a design choice is open, the
engine follows Godot's approach.

## The layers

```text
  Agents (MCP)   slash-cli   REST and JSON-RPC clients   Game code (later)
        |            |                  |                      |
        v            v                  v                      v
  +----------------------------------------------------------------+
  | Command layer: registry, sessions, undo, JSON-RPC, MCP, HTTP   |
  +----------------------------------------------------------------+
                                |
                                v
  +----------------------------------------------------------------+
  | World: EnTT entities, reflected components, JSON scenes        |
  +----------------------------------------------------------------+
                                |
                                v
  +----------------------------------------------------------------+
  | Platform and render: SDL3 window, run loop, files, GPU device  |
  +----------------------------------------------------------------+
```

The code follows the same split, one CMake library per module under `engine/`:

| Module | Holds |
| --- | --- |
| `core` | `Result<T>`, JSON logging, JSON parsing and schemas, reflection, math, the fixed-step clock |
| `platform` | The SDL3 window, the run loop and file access |
| `render` | The render device: SDL3's GPU API (Vulkan, Metal, Direct3D 12), or the null device for headless runs |
| `asset` | Asset handles, loaders and hot reload |
| `world` | The EnTT world, components, the component registry, scenes and prefabs |
| `storyboard` | Storyboard files: kinds, checks, versions, queries and edits |
| `command` | Commands, sessions with undo, JSON-RPC and MCP |
| `server` | The localhost HTTP server over a session |
| `runtime` | Project files and the game executable every export is built from |

The tools in `tools/` are thin front ends over these: `slash-cli` (run, validate, call, serve,
init, export), `slash-mcp` (MCP over stdio) and `slash-game` (runs a project as its export
would).

## World, entities and components

A **world** holds every entity in a running game. It is built on [EnTT](https://github.com/skypjack/entt),
an MIT-licensed entity component system.

An **entity** is a thing in the world, named by a stable **ID** such as `"player"` or
`"enemy_03"`: 1 to 64 letters, digits, `_` or `-`. IDs stay the same across saves, loads and
undo, so agents can refer to entities by name instead of by an index that changes.

A **component** is a plain C++ struct of data attached to an entity. An entity has at most one of
each component type. The engine ships two today:

| Component | Fields | Units |
| --- | --- | --- |
| `Transform2D` | `position` [x, y], `rotation`, `scale` [x, y] | Pixels; y points down; rotation in radians, clockwise |
| `Transform3D` | `position` [x, y, z], `rotation` quaternion [x, y, z, w], `scale` [x, y, z] | Meters; y points up |

Units and axes follow Godot. Components are **reflected**: one `SLASH_COMPONENT(Type, fields...)`
line lets the engine save, load, validate and describe the struct. Registering a component with
the **component registry** adds its docs and value ranges, and from then on it appears in scene
files, the scene schema, `engine.describe` and every command, with no other glue. Field types can
be `bool`, integers, `float`, `double`, `std::string`, `Vec2`, `Vec3`, `Vec4`, `Quat`, a
reference to another entity, a `std::vector` of these, or a nested struct declared with
`SLASH_REFLECT`.

A **system** is code that updates components every tick, added with `World::add_system` and an
optional doc string and priority. Systems run in order of priority, lowest first as with Godot's
`process_priority`, and in the order they were added when priorities are equal. `engine.describe`
lists them in that order. The engine has no built-in systems yet; they arrive with the feature
milestones (2D physics, animation, particles).

## Scenes

A **scene** is a world saved as a `.scene.json` file: entities keyed by ID, each with its
components, plus a `format` version. Fields equal to their defaults are left out, as in Godot, so
a scene holds only what differs.

Every scene has one **canonical form**: keys sorted, two-space indent, numbers normalized,
defaults removed. `slash-cli scene format` and `scene.save` both write it, so the same world
always produces the same bytes and diffs stay small.

The **scene schema** is a JSON Schema generated from the registered components, so it always
matches the build. Validation reports every error at once, each with the JSON Pointer of the bad
value and, for a misspelled name, the closest valid one. See
[reference/scene-format.md](reference/scene-format.md).

A **prefab** is one entity's components in a `.prefab.json` file, like a Godot scene that other
scenes instance. An entity that `"extends"` a prefab starts with its components and overrides
fields; a `null` component removes one. The entity stays linked to the prefab, so saving writes
only what differs and editing the prefab changes every instance. Prefabs can extend other
prefabs. Undo and snapshots keep the prefab each entity was made from, so they restore it exactly
even after the file changes.

## Assets

An **asset** is a file a game loads by project path, like a Godot resource: a JSON table, a CSV
file and, once the renderer and audio arrive, textures and sounds. Loading one returns a
**handle**; every handle to the same path shares one copy, which stays loaded while any handle
exists. `.json`, `.txt` and `.csv` files load out of the box, and a game can add its own types.

**Hot reload** keeps an agent's edit-and-check loop short. Before every command, and between
ticks in `slash-cli run` and `slash-cli serve`, the session checks the files it has loaded. A
changed asset reloads in place, so its handles see the new version. A changed prefab updates its
instances at once, keeping each instance's overrides, as Godot does for instanced scenes. A file
that no longer loads keeps its last good version, and the error is reported. `asset.list` shows
what is loaded, and `events.subscribe` delivers an `asset.reloaded` event per changed file plus a
`command` event named `asset.reload` with the changes to the world. A reload is not an undo step.

## Storyboard

The **storyboard** holds a game's facts that are not code: its style, world, story, design rules,
names, decisions and open work. Agents guess when they lack a fact, and the risk grows when
several agents share a project, so each fact is a typed **entry** under a stable ID such as
`character/mira`, in one of the `storyboard/*.json` files. The entry's **kind** (`character`,
`place`, `rule`, `task` and so on) decides its fields and its file; see the
[storyboard format](reference/storyboard-format.md).

`storyboard.query` finds entries by ID, kind, text or link. `storyboard.edit` creates, changes and
removes them in one undoable step, and checks fields and references: naming an entry that does not
exist is an error with a "did you mean" hint. Each entry has a **version** that changes with its
fields; an edit names the version it read, so a stale edit from another agent is refused instead
of overwriting a newer fact. The **journal**, `storyboard/journal.jsonl`, records who changed which
entries, when and why, and `task` entries say who is on what, so agents see each other's work.

Agents use it around every task. Before, `storyboard.brief` returns the facts the task touches:
the entries its scenes and entities stand for (through the `StoryRef` component), the entries
linked to its files, the rules that apply everywhere, and the open tasks and decisions about any
of them. After, `storyboard.check` finds references to missing entries, words the glossary says to
avoid, and `StoryRef` IDs that name nothing. `slash-cli project validate` runs the same check, so
CI catches what an agent missed.

## Game projects and exports

A **project** is a game's folder: a `project.json` naming the game, its version and its **main
scene**, plus its scene files. See [reference/project-format.md](reference/project-format.md).

An **export** is a build of a project for one platform, ready for players. Because gameplay is
compiled C++, an export is a real build of the game runtime for that platform, with the project's
data next to it, not a template plus scripts. `slash-cli export <platform>` validates the project
and its scenes, builds, signs, and with `--test` runs the result headless to prove it starts.
It covers macOS, Windows, Linux, the web, Android and iOS; building every platform from one Mac
is next on the [roadmap](roadmap.md).

An **export preset** is a named way to export, kept in the project's `export_presets.json` like
Godot's `export_presets.cfg`: a platform, an output folder, a debug flag and how to sign. Signing
secrets never go in the file; they come from environment variables. See
[reference/project-format.md](reference/project-format.md#export-presets).

## Ticks and the run loop

The world advances in fixed **ticks**: 60 per second by default. A fixed step keeps runs
deterministic, which lets agents and CI repeat a run and get the same result. `slash-cli run`
ticks the world in a window or headless; the `step` command advances it a given number of ticks
from any front end, and `run.until` until a condition on the entities holds.

## Rendering

Everything draws through a **render device**, about a dozen calls: make and update textures,
begin a frame, draw triangles, end it, and read a texture's pixels back. On a GPU it is SDL3's GPU
API, which uses Metal, Direct3D 12 or Vulkan, whichever the machine has. Headless runs, and
machines where no backend starts, use the **null device**: it keeps textures and checks every
call exactly as the GPU device does, but draws nothing. So code tested headless fails the same
way on a GPU, and `slash-cli run` and `slash-game` report which device drew in their summary's
`renderer`. For now a game's window shows its `rendering.clear_color`; sprites are next.

Shaders are written once in HLSL. Vulkan's SPIR-V and Metal's MSL are compiled ahead of time
and committed, so building the engine needs no shader tools, and Direct3D 12 compiles the HLSL at
startup with the compiler that comes with Windows. The
[draw-offscreen example](../examples/draw-offscreen/) draws into a texture and reads it back,
which is how tests check what a game draws.

## Commands

A **command** is a named action such as `entity.create`, `world.query` or `scene.save`. Each one
declares:

- a short **description**, sent to models as the tool description, so it stays brief;
- a **params schema** (JSON Schema, with `"additionalProperties": false`) that every call is
  checked against before it runs;
- the **capabilities** it needs;
- its **effect** on the world: read only, an edit (one undo step), a replace (opening or creating a
  scene, which clears undo history), a move through history (undo and redo), or compound (`batch`).

All commands live in one **command registry**. MCP, JSON-RPC, REST and `slash-cli call` are
generated from it, so they offer exactly the same commands with the same params and never drift.
The full list is in [reference/commands.md](reference/commands.md).

### Results are small on purpose

Commands are designed to cost agents as few calls and tokens as possible:

- Queries return entity IDs unless `include` asks for components.
- `component.set` returns only the fields whose values changed.
- `omit_defaults` drops fields equal to their defaults from an answer.
- `batch` runs up to 1000 commands in one call.
- `engine.describe` gives command names only; `full: true` or `schema.get` gives the details.

### Errors point at the problem

A failed command returns an error with a numeric code, a message and, where it applies, the JSON
Pointer of the bad value:

```json
{"code":-32001,"data":{"path":"/components/Transfrom2D"},"message":"/components/Transfrom2D: unknown component \"Transfrom2D\"; did you mean \"Transform2D\"?"}
```

The codes are shared by every front end and listed in
[reference/protocols.md](reference/protocols.md#error-codes).

## Sessions

A **session** is one editing context: a world, the open scene's path, the undo history and recent
log lines. Every front end wraps one session, and every command runs through `Session::call`,
which does the same four steps each time:

1. Check that the session grants the command's capabilities.
2. Validate the params against the command's schema.
3. Run the command.
4. If it changed the world, record one undo step labelled with the command's name.

**A failed command changes nothing.** If a command fails partway, the world is restored to how it
was before the call. `batch` extends this to many commands: all of them apply or none do, and one
undo reverts the whole batch.

### Capabilities

A **capability** is a permission level a session grants. Each command lists the ones it needs:

| Capability | Allows |
| --- | --- |
| `read` | Inspecting the world and the engine |
| `edit` | Changing the world, undo and redo |
| `run` | Advancing the simulation |
| `filesystem` | Reading and writing files in the project folder |
| `build` | Compiling and packaging the game (`export.run`), which runs compilers and other tools |

Sessions grant all five by default. `--allow read,run` (on `slash-cli` and `slash-mcp`) narrows
that, for example to let an agent observe a game without changing it.

### The project folder

Every session has a **project folder** (`--project`, default the current folder). File paths in
commands are relative to it. Absolute paths and `..` segments are rejected, so a command can never
read or write outside the project.

### Undo

The session keeps up to 100 undo steps. `undo` and `redo` move through them, and `history.get`
lists them by command name. Undo restores entities and components; the tick counter keeps
counting. Opening or creating a scene clears the history.

Commands that change project files, such as `storyboard.edit`, record how to reverse their change
and how to make it again, as Godot's UndoRedo does. Their files are written only when the whole
command succeeds, and not at all in a dry run; if another program changed a file meanwhile,
nothing is written and the command fails with `-32005`. Undo of such a step is refused the same
way when the files changed since.

### Dry runs and snapshots

Every command that changes the world takes `"dry_run": true`. The session runs the command, notes
what changed, and puts the world back exactly as it was, generated IDs and tick counter included,
with no undo step. The reply holds the command's own result and the changes:

```json
{"changes":{"changed":{"player":{"Transform2D":{"rotation":1.0}}}},"dry_run":true,"result":{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}}
```

`world.snapshot` saves a named copy of the entities. `world.diff` lists what changed since, in the
same form, and `world.restore` puts the entities back, as one undo step. Changes are listed by
entity ID: `added` and `removed` IDs, and under `changed` the new value of every field that
differs. A removed component is `null`.

### Events

`events.subscribe` starts queueing **events**, which `events.read` returns: entities created and
destroyed (by commands or by systems), a `command` event with the changes for each command that
changed the world, and events the game emits itself. Gameplay code emits one with
`world.emit("coin_collected", "coin", data)`, much as a Godot node emits a signal. Polling
`events.read` is cheaper than re-querying the world, and it works the same over every front end.

## Front ends

| Front end | Speaks | Use it for |
| --- | --- | --- |
| `slash-mcp` | MCP over stdio | Claude Code, Codex, Cursor and any other MCP client |
| `slash-mcp --live` | MCP over stdio, forwarded over HTTP | An agent driving a game that is already running |
| `slash-cli call` | One command per process | Scripts and shell use |
| `slash-cli rpc` | JSON-RPC 2.0, one message per line on stdio | Tools that keep one engine open |
| `slash-cli serve`, `slash-cli run --serve` | HTTP: REST, OpenAPI, JSON-RPC, MCP | Anything that speaks HTTP, and live games |

MCP **tool names** replace dots with underscores (`entity_create`), because some clients reject
dots. The MCP server also offers three **resources**: `slash://scene` (the open scene),
`slash://schema/scene` (the scene schema) and `slash://log` (recent logs).

## The HTTP server and live games

The **server** serves a session over HTTP so agents can work on a game while it runs. It is built
to be safe on a developer's machine:

- It listens on the loopback interface only (`127.0.0.1`, `::1` or `localhost`).
- Every request needs `Authorization: Bearer <token>`. The token is random unless given.
- Requests whose `Host` or `Origin` header names another site get 403, which blocks DNS rebinding
  attacks from web pages.
- Its URL and token are written to the **discovery file** `.slash/server.json` (owner-only on
  macOS and Linux, gitignored) and removed when it stops. `slash-mcp --live` reads it to connect.

Requests arrive on a background thread but never run there. They wait in a queue until the
session's owner calls `Server::poll()`, which the run loop does between ticks. So a command never
races the game loop, and every command sees the world at a tick boundary. A request that waits
more than 30 seconds fails with 503.

A running game (`slash-cli run --serve`) adds the `play` and `pause` commands. `pause` stops the
world, like a debugger's break, while the loop keeps serving requests, so an agent can inspect and
edit a frozen frame or advance it with `step`; `play` lets it run again.

## Agent setup files

`slash-cli init` writes everything an agent needs to work on a game: MCP configs for Claude Code
(`.mcp.json`), Codex (`.codex/config.toml`) and Cursor (`.cursor/mcp.json`), an `AGENTS.md` with
the working rules and a link to the engine's [examples](../examples/README.md) at the commit the
project pins, a `CLAUDE.md` that loads it, a Claude Code skill for editing scenes, a GitHub
Actions workflow that checks the project and exports and tests every platform
(`.github/workflows/export.yml`), an empty `scenes/main.scene.json`, a starter storyboard, and
`.gitignore` entries for `.slash/` and `build/`. The MCP server's own instructions repeat the key
rules (call `engine_describe` first, use `batch`, brief before a task and check after it, don't
hand-edit scenes while the server runs) to every client that connects.

## Model agnostic

Nothing in SlashEngine depends on one AI vendor. Agents connect through open protocols (MCP,
JSON-RPC, HTTP), and any model that can call tools can drive the engine. A later milestone adds an
`ai::Provider` interface for in-game features that call a model, with adapters for
OpenAI-compatible servers (including local ones), Anthropic and Gemini; see the
[design](design.md#model-agnostic-provider-interface).

## Glossary

| Term | Meaning |
| --- | --- |
| Asset | A file a game loads by project path into a shared, reference-counted handle |
| Canonical form | The one way a scene is written: sorted keys, two-space indent, defaults left out |
| Capability | A permission a session grants: `read`, `edit`, `run`, `filesystem` or `build` |
| Command | A named action with a params schema, capabilities and an effect |
| Command registry | The list of commands every front end is generated from |
| Component | A reflected C++ struct of data attached to an entity |
| Component registry | The list of component types a world knows, with their docs |
| Discovery file | `.slash/server.json`: a running server's URL and token |
| Effect | How a command changes the world: read, edit, replace, history or compound |
| Entity | A thing in the world, named by a stable ID |
| Example | A small program or sequence of calls in `examples/`, run by CI with its output checked |
| Export | A build of a project for one platform, ready for players |
| Front end | A way to reach the commands: MCP, JSON-RPC, REST or the CLI |
| Hot reload | Reloading edited files, and updating prefab instances, while the game runs |
| ID | An entity's stable name: 1 to 64 letters, digits, `_` or `-` |
| Journal | `storyboard/journal.jsonl`: who changed which storyboard entries, when and why |
| JSON Pointer | A path to a value inside JSON, such as `/entities/player/components` (RFC 6901) |
| Main scene | The scene a game loads at startup, named in `project.json` |
| Prefab | One entity's components in a `.prefab.json` file, which entities extend and override |
| Project | A game's folder: `project.json` plus its scenes |
| Project folder | The folder all command paths are relative to |
| Reflection | Field metadata from `SLASH_COMPONENT` or `SLASH_REFLECT` that drives saving, schemas and commands |
| Resource | Read-only data an MCP client can fetch: the scene, its schema or the logs |
| Scene | A world saved as a `.scene.json` file |
| Session | One world with its open scene path, undo history and recent logs |
| Storyboard | A game's facts that are not code, as typed entries in `storyboard/*.json` |
| StoryRef | The component that names the storyboard entries an entity stands for |
| System | Code that updates components every tick, in order of priority |
| Tick | One fixed simulation step, 1/60 s by default |
| Tool name | A command's MCP name, with `_` in place of `.` |
| World | Every entity in a running game, held in EnTT |
