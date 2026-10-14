# SlashEngine Design

Approved 2026-10-14. The living version is the [SlashEngine Design Doc](https://claude.ai/code/artifact/9bb8a68c-122e-47aa-b966-4556d10784af); this file is the copy the code is built against.

## Summary

SlashEngine is a small C++20 game engine whose primary user is an AI agent: every scene is plain text, every engine object is introspectable, and every action an editor could take is a command an agent can call over MCP. It builds with CMake, runs on SDL3 plus a thin renderer, and talks to any LLM through one provider interface. Where a choice is open, the default is to follow Godot's approach, adapted for agents.

**Goals**

- An agent can create, inspect, edit, run and test a game without a GUI editor.
- The engine and its tools run on macOS first, with Windows and Linux hosts after launch; games export to macOS, Windows, Linux, iOS, Android and web from day one.
- Feature coverage close to Godot 4 in both 2D and 3D, with UE5-class features as a longer-term track, while staying lightweight: every feature is a module, so a minimal 2D build stays under 10 MB.
- Model agnostic: no engine feature depends on a specific AI vendor; local models work as well as hosted ones.
- No proprietary code: the engine is Apache 2.0 and every dependency is under a permissive open-source license.

**Non-goals (for the first release)**

- A visual editor. Humans use their text editor, a debug overlay, and their agent.
- Matching UE5's flagship systems (Nanite, Lumen, MetaHuman) in the first releases. They are research items on the parity table.
- A custom scripting language. Gameplay is C++ plus a hot-reload layer, so agents can still edit it live.
- Bundling or hosting a model. The engine calls models; it never ships weights.

## Language and build

The engine is C++20, built with CMake 3.25+ and presets, with dependencies fetched through CPM.cmake at pinned versions. C++20 gives concepts, `std::span`, designated initializers and `<format>`-era ergonomics, and every target compiler (MSVC 19.3x, Clang 16+, GCC 12+, Emscripten) supports it fully. C++23 is allowed later behind feature checks; modules stay off until tooling is reliable on all three compilers.

| Decision | Choice | Why |
| --- | --- | --- |
| Standard | C++20 | Broad compiler support; concepts make the reflection layer readable |
| Build | CMake + `CMakePresets.json` | The default for C++; IDEs and agents both understand presets |
| Dependencies | CPM.cmake, pinned tags, no system packages | One-command clean build on any machine, reproducible for agents |
| Exceptions / RTTI | Off in engine core, allowed in tools | Smaller binaries; errors return `Result<T>` |
| Formatting / lint | clang-format, clang-tidy, enforced in CI | Agents get deterministic, machine-checkable style |
| Tests | doctest for units, headless scene tests via the command API | Fast, single-header; scene tests double as agent examples |
| CI | GitHub Actions on macOS, Windows, Linux, Emscripten | Every PR builds on macOS and exports and tests all six targets |

A single `cmake --preset dev && cmake --build --preset dev` must produce the engine, the sample game and the MCP server. That one command is what an agent will run first, so it never needs manual setup.

## Platform and renderer

SDL3 is the only platform layer: windowing, input, audio, file I/O, threads and timers all go through it. Rendering sits on SDL3's GPU API (Vulkan, Metal, Direct3D 12) behind a thin `RenderDevice` interface of roughly a dozen calls, so the engine never touches a native graphics API directly.

- **Renderer shape.** A frame graph with two full pipelines. 2D: batched sprites, tilemaps, 2D lights and shadows, canvas shaders. 3D: clustered forward+ with PBR materials, cascaded shadows and post-processing, and global illumination. Shaders are written once in HLSL and cross-compiled with SDL_shadercross at build time.
- **Headless backend.** A null device that runs the full simulation with no window or GPU. This is how agents and CI run games, tests and replays on a server.
- **Offscreen capture.** Any backend can render a frame to an image and return it as PNG. This is how an agent sees the game it is building.
- **Web.** Web export ships at launch. If SDL3's GPU API still has no web backend by then, SlashEngine adds its own WebGPU backend behind the same interface, with WebGL2 as the fallback for browsers without WebGPU.
- **Determinism.** Fixed-timestep simulation, seeded RNG per world, and input recorded as events, so a replay file reproduces a run exactly. Agents rely on this to verify fixes.

| Platform | Engine and tools run here | Export from a Mac uses |
| --- | --- | --- |
| macOS (Apple silicon and Intel) | At launch | Apple's command-line tools (needed for signing) |
| iOS | No | Apple's command-line tools |
| Windows x64 | After launch | llvm-mingw cross toolchain |
| Linux x64 and arm64 | After launch | Clang with a Linux sysroot |
| Android | No | Android NDK |
| Web | No | Emscripten |

**Exporting.** `slash-cli export <platform>` (or the `export.run` command an agent calls over MCP) builds the game module, packs assets and signs the result. Because gameplay is compiled C++, every export is a real build for that platform, not a template plus scripts.

- **Projects.** A game is a folder with a `project.json`, the equivalent of Godot's `project.godot`: name, identifier, version, main scene, window size and tick rate, checked against a schema the engine publishes.
- **One entry point.** Every game executable starts in the same `main` built on SDL's main callbacks, so the loop is driven by the host where it must be (the browser, iOS) and by SDL elsewhere. The same executable runs headless with `--headless --ticks N` and prints a JSON summary, which is how exports are tested.
- **How an export runs.** The project and every scene are validated, staged as the game's data, and built with CMake from the engine sources (`export/CMakeLists.txt`), then laid out for the platform: an app bundle on macOS (universal for release), an executable and `data` folder on Windows and Linux with the C++ runtime linked statically, and `index.html` with WebAssembly on the web. The full build log goes to a file; the caller gets a short JSON result, or the failing step with the end of the log.

- **Toolchains on demand.** `slash-cli toolchain install` downloads and pins the open-source cross toolchains above, so one Mac can produce every build with no manual setup.
- **CI as a second path.** `slash-cli init` also writes a GitHub Actions workflow that builds and runs headless tests for every platform on native runners, which is how Windows and Linux exports get tested before those hosts are supported.
- **Signing.** As in Godot, each developer brings their own Apple team ID and certificates, Android keystore and Windows code-signing certificate. Export presets live in a text file (`export_presets.json`) that agents can edit, with secrets read from environment variables. macOS exports are always signed, ad hoc unless a preset names a Developer ID identity. The engine generates a throwaway debug keystore for Android test builds, and the project's own accounts sign only its sample games in CI.

## Feature parity with Godot and UE5

SlashEngine covers nearly every Godot 4 feature area at launch and adds UE5 features wherever an open-source path exists; UE5's flagship systems stay research. Every feature is a module exposed through reflection and the command API, so an agent can use it without an editor.

| Area | Godot 4 | UE5 | SlashEngine approach | Target |
| --- | --- | --- | --- | --- |
| Editor | Full GUI editor | Full GUI editor | No GUI editor: MCP and REST command API, debug overlay, frame capture | Launch |
| Gameplay code | GDScript, C#, GDExtension | Blueprints, C++ | C++ with hot reload; plugins as C++ modules with a stable C ABI | Launch |
| Scenes and prefabs | Node scenes with inheritance | Levels, Blueprint actors | JSON scenes, prefabs with overrides, on EnTT | Launch |
| 2D sprites and animation | Sprite2D, AnimatedSprite2D | Paper2D | Batched sprites, sprite sheets, flipbooks | Launch |
| Tilemaps | TileMapLayer, terrains | Paper2D tilemaps | Tilemap layers with terrain autotiling, stored as text | Launch |
| 2D lighting | 2D lights, shadows, normal maps | None built in | 2D lights, occluders, normal-mapped sprites | Launch |
| 2D physics | Godot Physics 2D | None built in | Box2D v3 | Launch |
| 3D physics | Godot Physics, Jolt | Chaos | Jolt: rigid bodies, joints, character controller, vehicles | Launch |
| 3D renderer | Forward+, Mobile, Compatibility | Deferred and forward | Clustered forward+ with PBR, plus a low-end path for web and mobile | Launch |
| Shadows | Cascaded directional, omni, spot | Virtual shadow maps, cascades | Cascaded, omni and spot shadow maps | Launch |
| Post-processing | Tonemap, glow, SSAO, SSR, DoF | Full post stack | Tonemap, bloom, SSAO, SSR, DoF, auto exposure, color grading | Launch |
| Materials and shaders | Shading language, visual shaders | Material editor | HLSL shaders plus a JSON material graph agents write directly | Launch |
| Keyframe animation | AnimationPlayer on any property | Sequencer | Keyframe tracks on any reflected property | Launch |
| Skeletal animation | Skeleton3D, skins | Skeletal meshes | Skinned meshes, blending, root motion | Launch |
| Animation blending | AnimationTree, state machines, blend spaces | Animation Blueprints, Control Rig | Blend trees and state machines in text, two-bone and FABRIK IK | Launch |
| Particles | GPU and CPU particles | Niagara | CPU and GPU particles, particle graphs in text | Launch |
| Audio | Buses, effects, 3D audio | MetaSounds, spatial audio | miniaudio: buses, effects, 3D spatial audio | Launch |
| Input | Action maps, gamepads | Enhanced Input | Rebindable action maps over SDL3, injectable by agents | Launch |
| UI | Control nodes, containers, themes | UMG | Retained UI with flexbox layout and themes, defined in text | Launch |
| Text and fonts | Font rendering, BiDi | Slate fonts | FreeType, HarfBuzz, MSDF fonts, BiDi | Launch |
| Navigation | Navigation meshes, avoidance | Nav mesh, AI controllers | Recast and Detour, 2D and 3D, avoidance | Launch |
| Asset import | glTF, FBX, OBJ, images, audio | FBX, glTF, USD | glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG, MP3 | Launch |
| Profiling | Profiler, debugger, monitors | Unreal Insights | Tracy, plus perf.stats over the command API | Launch |
| Global illumination | SDFGI, VoxelGI, LightmapGI | Lumen | Baked lightmaps and probe-based GI | Launch |
| Sky and volumetrics | Procedural sky, volumetric fog | Sky atmosphere, volumetric clouds | Physical sky, volumetric fog | Launch |
| Decals | Decal node | Deferred decals | Clustered decals | Launch |
| Networking | High-level multiplayer, ENet, WebSocket, WebRTC | Replication | ENet transport, replication driven by reflection, WebSocket for web | Launch |
| Terrain | Plugins | Landscape | Heightmap terrain with splat materials and LOD | Launch |
| Level geometry | CSG, GridMap | Brushes, modeling tools | CSG and grid-based tile placement | Launch |
| LOD and culling | Auto mesh LOD, occlusion culling | Nanite, HLOD | Generated LODs with meshoptimizer, occlusion culling | Launch |
| XR | OpenXR | OpenXR | OpenXR | Launch |
| Localization | Translation tables, gettext | Localization dashboard | Translation tables in text | Launch |
| Video playback | Theora | Media Framework | AV1 through dav1d | Launch |
| Accessibility | Screen reader support | Partial | AccessKit | Launch |
| Analytics, ads and in-app purchases | Plugins (Google Play Billing, StoreKit) | Online Subsystem, in-app purchase APIs | One cross-platform API and command set, like Unity's Analytics, Ads and IAP; StoreKit, Play Billing and a developer-chosen analytics endpoint in the repo, ad networks as adapters outside it | Launch |
| One-command export | Export templates per platform | Packaging per platform | slash-cli export to macOS, Windows, Linux, iOS, Android and web | Launch |
| Large worlds | Double-precision builds | World Partition, large world coordinates | Double-precision transforms, world streaming | After launch |
| Cinematics | AnimationPlayer, cameras | Sequencer | Timeline tracks for cameras and events | After launch |
| Procedural generation | Noise textures | PCG framework | FastNoiseLite plus rule graphs agents write | After launch |
| Cloth and soft bodies | SoftBody3D | Chaos Cloth | Jolt soft bodies | After launch |
| Destruction | None built in | Chaos Destruction | Fracture and debris on Jolt | After launch |
| Virtualized geometry | None built in | Nanite | Meshlet rendering research | Research |
| Fully dynamic GI at UE quality | SDFGI (partial) | Lumen | Research after probe GI ships | Research |
| Digital humans | None built in | MetaHuman | Research; out of scope for releases | Research |

The Godot and UE5 columns come from their public feature lists, not a line-by-line audit; treat them as a starting checklist.

## No proprietary code

Every line in the repo is Apache 2.0 or comes from a dependency under a permissive open-source license, and CI fails any change that adds anything else.

- **Allowed licenses:** MIT, BSD, zlib, Apache 2.0, Boost, MIT-0 and public domain. No GPL or LGPL in the runtime, so games built on SlashEngine can ship under any license.
- **No copied engine code.** Nothing from UE5 or any other closed engine, including snippets from its source or docs. Godot is MIT, so its ideas and code may be borrowed with attribution in `THIRD_PARTY.md`.
- **System APIs are linked, not shipped.** Win32, Direct3D, Metal, Vulkan drivers and mobile OS SDKs are called through SDL3; none of their code lives in the repo.
- **Store and ad SDKs follow the same rule.** StoreKit is an OS framework and Google Play Billing is fetched at export time with the Android SDK, so the repo holds only SlashEngine's glue for them. Ad network SDKs are proprietary, so their adapters live outside the repo, like console backends (pending Dan's confirmation).
- **Consoles live outside the repo.** PlayStation, Xbox and Switch SDKs are closed and under NDA, so, as with Godot, console ports are private backends built by licensed porting partners and offered to studios separately. They plug in behind the same `RenderDevice` and platform interfaces.
- **Enforced in CI.** Each dependency is pinned with its license in a manifest, and a license check runs on every PR.

| Library | Used for | License |
| --- | --- | --- |
| SDL3, SDL_shadercross | Platform layer, GPU API, shader cross-compiling | zlib |
| EnTT | ECS and runtime type metadata | MIT |
| yyjson | JSON parsing | MIT |
| Jolt Physics | 3D physics, soft bodies | MIT |
| Box2D v3 | 2D physics | MIT |
| miniaudio | Audio | MIT-0 or public domain |
| Recast and Detour | Navigation | zlib |
| FreeType, HarfBuzz | Fonts and text shaping | FreeType License (BSD-style), MIT |
| cgltf, ufbx, stb_image | Asset import | MIT |
| meshoptimizer | LODs, mesh compression | MIT |
| ENet | Networking | MIT |
| OpenXR SDK | XR | Apache 2.0 |
| dav1d | Video | BSD |
| AccessKit | Accessibility | MIT or Apache 2.0 |
| Tracy | Profiling | BSD |
| doctest | Tests | MIT |

## Core architecture

The engine is a stack of five layers, and the command layer is the only way into the world. Agents, game code, the debug overlay and replays all issue the same commands, which is what makes every change undoable, recordable and visible to an agent.

```text
  AI agent (MCP)   Game code (C++)   Debug overlay   Replays
        |                 |                |             |
        v                 v                v             v
  +---------------------------------------------------------+
  | Command layer: registry, undo and redo, JSON-RPC server |
  +---------------------------------------------------------+
                              |
                              v
  +---------------------------------------------------------+
  | World: ECS (EnTT) | Scenes and prefabs (JSON) | Reflection |
  +---------------------------------------------------------+
        |                 |                |             |
        v                 v                v             v
    Renderer           Audio          Hot reload    AI providers
        |                 |                |         (optional)
        v                 v                v
  +---------------------------------------------------------+
  | SDL3 platform layer: window, input, audio, files,       |
  | GPU API or a null backend for headless runs             |
  +---------------------------------------------------------+
```

- **World.** EnTT (MIT, header-only, used in shipped games such as Minecraft) holds entities and plain-data components in sparse sets; systems run in a fixed order each tick on a job system. Components are reflected, so the ECS, serializer and command layer never need hand-written glue.
- **Assets.** Handles with reference counting and hot reload: change a texture, the game module or a scene on disk and the running game picks it up. An agent's edit-and-check loop depends on this.
- **Memory.** Frame and arena allocators in core, no global `new` in hot paths, and a per-system memory report the agent can query.
- **Modules.** Renderer, physics, audio, navigation, networking, XR and AI are separate CMake targets; a game links only what it uses. Gameplay itself is a C++ shared library the engine reloads on rebuild, keeping world state because components live in the ECS, not in the module. Release, web and iOS builds link it statically.

## What AI-first means

AI-first means four concrete things: the whole project is text, the engine describes itself, every action is a command, and an agent can observe the result. If an agent cannot do something through these four, it counts as a bug. On top of that, the engine should be cheaper for an agent to use than editing files by hand, measured in calls and tokens (section 6).

### 1. Text-serializable scenes

Scenes, prefabs, materials and project settings are canonical JSON files (`.scene.json`, `.prefab.json`), validated by a JSON Schema the engine generates from its own reflection data. JSON wins over YAML or a custom format because every model writes it reliably and schema validators exist in every language.

- **Canonical form.** Keys sorted, two-space indent, one entity per block, stable string IDs (`"player"`, `"enemy_03"`). A formatter rewrites any valid file to this form, so diffs stay small and merges stay sane.
- **No binary in the source tree.** Textures and audio are referenced by path; imported caches live in a gitignored `.slash/` folder and rebuild on demand.
- **Prefabs by reference.** An entity can `"extends"` a prefab and override fields, so an agent edits one file to change every instance.

### 2. Reflection and self-description

Components register their fields with one macro, for example `SLASH_COMPONENT(Transform, position, rotation, scale)`, plus optional doc strings and value ranges. That one registration drives serialization, the JSON Schema, the debug inspector and the command API. The `engine.describe` command returns every component, system, asset type and command with its docs, so an agent learns the engine from the engine itself.

### 3. A command API, exposed over JSON-RPC and MCP

Every change to a running world is a `Command`: the debug overlay, undo/redo, replays and agents all go through the same path. The engine core speaks JSON-RPC 2.0 over stdio or localhost HTTP. A small `slash-mcp` adapter maps each command to an MCP tool and each scene, schema and log to an MCP resource, so any MCP client (Claude, other hosted models, local agents) can drive it with no custom glue. A REST mirror exposes the same commands over plain HTTP for tools that don't speak MCP, with an OpenAPI 3.1 document; both are generated from the same registry, so they never drift.

The HTTP server runs inside the game, so an agent can inspect and change a world while it plays. It serves `POST /v1/commands/<name>` (the REST mirror), `GET /v1/commands`, `GET /v1/openapi.json`, `POST /rpc` (JSON-RPC) and `POST /mcp` (MCP over Streamable HTTP). Requests arrive on a background thread but wait in a queue until the game loop runs them between ticks, so commands never race the simulation.

| Command group | Examples | Purpose |
| --- | --- | --- |
| Describe | `engine.describe`, `schema.get` | Learn what exists and how to call it |
| Query | `world.query`, `entity.get`, `asset.list` | Read state with filters, e.g. all entities with `Health < 10` |
| Edit | `entity.create`, `component.set`, `scene.save`, `undo` | Change the world or the files on disk |
| Run | `play`, `pause`, `step(n)`, `input.inject`, `replay.run` | Drive the simulation deterministically |
| Observe | `frame.capture`, `log.tail`, `events.subscribe`, `perf.stats` | See and measure what happened |

### 4. Observability and safety

Agents get structured JSON logs, a subscribable event stream, frame captures as PNG and per-system timings. The server binds to localhost only, requires a random bearer token, rejects requests whose `Host` or `Origin` header names another site (which blocks DNS rebinding from web pages), and grants capabilities in levels (read, edit, run, filesystem, build), so a player-facing build ships with the server compiled out. Its address and token go in `.slash/server.json`, which is owner-only on macOS and Linux, gitignored, and removed on exit.

### 5. Connecting Claude Code, Codex and Cursor

All three agents connect directly, because each one can launch a local MCP server over stdio. Running `slash-cli init` in a game project writes all three configs, an `AGENTS.md` (read by Codex and Cursor), a `CLAUDE.md` that points to it, a Claude Code skill for editing scenes, and an empty `scenes/main.scene.json` that the server opens at startup. The server's MCP `instructions` tell the model to use the engine tools rather than hand-edit scene files, and how to use them cheaply.

Claude Code reads `.mcp.json` in the project, or adds the server by hand:

```bash
claude mcp add slashengine -- slash-mcp --project .
```

Codex reads `.codex/config.toml` in a trusted project, or `~/.codex/config.toml`:

```toml
[mcp_servers.slashengine]
command = "slash-mcp"
args = ["--project", ".", "--scene", "scenes/main.scene.json"]
```

Cursor reads `.cursor/mcp.json`:

```json
{ "mcpServers": { "slashengine": { "command": "slash-mcp", "args": ["--project", "${workspaceFolder}"] } } }
```

To work on a game while it runs, start it with `slash-cli run --serve` (or `slash-cli serve` for a world with no window) and connect with `slash-mcp --live`, which finds the server through `.slash/server.json` and forwards each message to it:

```bash
claude mcp add slashengine-live -- slash-mcp --live --project .
```

Clients that speak MCP over HTTP can connect to `/mcp` directly with the token from that file.

MCP tool names use underscores (`entity_create` for the `entity.create` command), because some clients reject dots. Any other MCP client works the same way, and agents without MCP can use `slash-cli call`, `slash-cli rpc` (JSON-RPC over stdio), or the REST mirror.

### 6. Cheap for agents to use

Agents can't be forced to prefer a tool, but they reliably use what the project tells them to use and what gets the job done in fewer, cheaper steps. So every command is designed to cost as few calls and tokens as possible:

| Feature | What it saves | Milestone |
| --- | --- | --- |
| `batch` runs many commands in one call, all or nothing, as one undo step | A round trip per edit | 3 |
| Queries return IDs unless asked for components; edits return only changed fields; `omit_defaults` drops default values | Tokens per answer | 3 |
| `engine.describe` lists components with default values and command names in one short answer; full schemas only on request | Reading source or docs | 3 |
| Errors carry the JSON Pointer of the bad value and a "did you mean" hint | A docs lookup per mistake | 2 and 3 |
| `slash-cli init` and the MCP instructions point agents at the tools | Agents falling back to hand edits | 3 |
| A test command steps the game headless and returns only the checks that failed | Screenshots and log reading | 5 |
| Frame summaries list what is on screen as data; PNG captures stay available | Image tokens | 5 |
| A benchmark of standard agent tasks counts calls and tokens on a schedule | Regressions in agent efficiency | 6 |

A custom language for agents is deliberately not on this list: models write C++ and JSON reliably because they have seen so much of both, and MCP tool calls are JSON already. When agents repeat the same sequence of calls, the fix is a new command or prefab.

## Model-agnostic provider interface

Model agnosticism works at two levels. Outside the engine, agents connect over MCP and JSON-RPC, which are open protocols any model can use. Inside the engine, features that call a model (NPC dialogue, procedural content, an in-game assistant) go through one `ai::Provider` interface, and no engine code ever names a vendor.

```cpp
namespace slash::ai {

struct Capabilities {
    bool tools, json_schema_output, vision, streaming;
    int  context_tokens;
};

struct Request {
    std::vector<Message>    messages;      // system, user, assistant, tool results
    std::vector<ToolSpec>   tools;         // JSON Schema, generated from the command registry
    std::optional<Json>     output_schema; // ask for structured JSON back
    Params                  params;        // max_tokens, temperature, stop, seed
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual Capabilities capabilities() const = 0;
    virtual Task<Response> complete(const Request&, StreamSink* sink = nullptr) = 0;
};

} // namespace slash::ai
```

- **Adapters, not features.** Each vendor is a small adapter that translates `Request` and `Response`. The first release ships three: OpenAI-compatible HTTP (which also covers Ollama, llama.cpp server, vLLM, LM Studio and OpenRouter), Anthropic Messages, and Google Gemini.
- **Capabilities, not names.** Features ask for what they need, e.g. tools plus JSON output. If the configured model lacks a capability, the engine falls back (prompted JSON instead of native schema output) or reports a clear error.
- **Configuration in a file.** A `providers.json` maps roles (`dialogue`, `assistant`, `codegen`) to a provider, model and endpoint. API keys come from environment variables, never from files in the repo.
- **Same tools everywhere.** Tool specs for in-engine models come from the same command registry that MCP exposes, so an in-game assistant and an external agent have identical powers.
- **Testable offline.** A `ReplayProvider` records real responses once and replays them in CI, so tests never need a network or a key.
- **Optional module.** `slash_ai` is a separate CMake target. A game that uses no model links none of it and carries no HTTP client.

## Repo layout and milestones

The repo splits into a core library, optional modules and the tools agents use, with sample games that double as integration tests.

```text
SlashEngine/
  CMakeLists.txt, CMakePresets.json
  AGENTS.md          # how an agent builds, tests and drives the engine
  engine/
    core/            # memory, Result, logging, math, jobs, reflection
    platform/        # SDL3 wrapper: window, input, audio, files
    render/          # RenderDevice, SDL GPU + null backends, sprite and 3D passes
    world/           # ECS, scenes, prefabs, serialization
    command/         # Command registry, undo, JSON-RPC and MCP
    server/          # localhost HTTP: REST mirror, OpenAPI, JSON-RPC and MCP
    runtime/         # project.json, the game loop and every game's main()
    hotreload/       # loads and swaps the C++ game module, keeps world state
    ai/              # optional: Provider interface and adapters
  tools/
    exporter/        # export presets, building, signing and the export.run command
    slash-mcp/       # MCP over stdio, or forwarded to a running game
    slash-cli/       # run, export, format, validate scenes headless
    slash-game/      # runs a project the way its exports do
  export/            # CMake project that builds one game for one platform
  samples/pong/      # smallest complete game, built by an agent end to end
  tests/
```

**Milestones** (each ends with a green CI build and working exports on all six platforms)

1. **Skeleton.** CMake presets, CI matrix, SDL3 window on macOS, clang-format and tidy, `AGENTS.md`.
2. **Core and world.** Reflection macro, EnTT integration, JSON scene load and save with schema validation, headless runner.
3. **Commands.** Command registry, undo, JSON-RPC server, `slash-mcp`, REST mirror, `slash-cli`. Exit: Claude Code, Codex and Cursor each open, edit and save a scene over MCP.
4. **Export pipeline.** `slash-cli export` and toolchain install for all six platforms, plus the CI workflow. Exit: an empty project exports and passes headless tests everywhere. It comes this early so every later feature stays exportable. Built in two parts: (a) project files, the game runtime and exports for macOS, Windows, Linux and web on their own hosts, tested in CI; (b) iOS and Android, cross toolchains from a Mac, export presets with signing, and the `export.run` command.
5. **2D feature set.** Systems, play and pause, event subscriptions, prefabs, asset handles with hot reload and the CI license check first; then the renderer, materials, sprites, keyframe animation, tilemaps, 2D lights, Box2D physics, particles, UI and text, audio, input injection, frame capture, replays and the debug overlay. Exit: an agent builds `samples/pong` and exports it to all six platforms.
6. **Hot reload, AI and profiling.** Hot-reloadable C++ game module, `ai::Provider` with three adapters and `ReplayProvider`, the job system, Tracy, `perf.stats` and the per-system memory report.
7. **3D feature set.** Forward+ PBR renderer, shadows, post-processing, GI, sky, decals, LODs, Jolt physics, skeletal animation and blending, navigation, terrain, CSG, glTF and FBX import. Exit: an agent builds a small 3D sample end to end.
8. **Systems and platform services.** Networking, XR, localization, video, accessibility, and analytics, ads and in-app purchases behind one API with a headless test backend.
9. **Launch.** Every Launch row of the parity table done, the Mac engine and tools released, exports for all six platforms, reference docs generated from `engine.describe`.

After launch: Windows and Linux hosts for the engine and tools, then the After launch rows.

**Open questions**

- [x] Minimum OS versions. Decided by Dan: only versions released in 2020 or later are supported. That means macOS 11, iOS 14, Android 11 (API level 30), Windows 10 version 2004, Linux with glibc 2.31 (Ubuntu 20.04 or newer), and on the web Chrome and Edge 85, Firefox 79 and Safari 15, the oldest browsers Emscripten supports (Safari 14 also lacks WebGL2). SDL3's GPU API needs less on every platform.
