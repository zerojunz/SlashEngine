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

- **Renderer shape.** A frame graph with two full pipelines. 2D: batched sprites, tilemaps, 2D lights and shadows, canvas shaders. 3D: clustered forward+ with PBR materials, cascaded shadows and post-processing, and global illumination. Shaders are written once in HLSL. SPIR-V (Vulkan) and MSL (Metal) are compiled from it ahead of time with glslang and SPIRV-Cross and committed, so building the engine needs no shader tools; Direct3D 12 compiles the HLSL at startup with `d3dcompiler_47.dll`, which comes with Windows. SDL_shadercross does the same job but has no release yet and needs DXC to read HLSL, so it can replace these tools later. Canvas materials are JSON node graphs compiled on load to a short program that one interpreter shader runs, so they hot reload and need no shader compiler on any platform, and the CPU runs the same program to check a material's colors without a screenshot.
- **Headless backend.** A null device that runs the full simulation with no window or GPU. This is how agents and CI run games, tests and replays on a server.
- **Offscreen capture.** Any backend can render a frame to an image and return it as PNG. This is how an agent sees the game it is building.
- **Web.** Web export ships at launch. If SDL3's GPU API still has no web backend by then, SlashEngine adds its own WebGPU backend behind the same interface, with WebGL2 as the fallback for browsers without WebGPU.
- **Refresh rates.** The simulation ticks at a fixed rate while rendering runs at the display's rate and interpolates between ticks, so a 60 Hz simulation looks smooth on a 240 Hz display. As in Godot, a project sets a V-Sync mode (on, off, adaptive or mailbox) and a frame cap, a game's options menu can change both while it runs, and the engine reports the display's refresh rates. Variable refresh rate (G-Sync, FreeSync, Apple's ProMotion) is the display's job; the engine keeps it working by choosing a present mode and fullscreen mode that allow it. Phones render at 120 Hz where supported, and web games follow the browser's frame rate.
- **Determinism.** Fixed-timestep simulation, seeded RNG per world, and input recorded as events, so a replay file reproduces a run exactly. Agents rely on this to verify fixes.

| Platform | Engine and tools run here | Export from a Mac uses |
| --- | --- | --- |
| macOS (Apple silicon and Intel) | At launch | Apple's command-line tools (needed for signing) |
| iOS | No | Xcode's iOS SDK and command-line tools (no Xcode project) |
| Windows x64 | After launch | llvm-mingw cross toolchain |
| Linux x64 and arm64 | After launch | Clang (from llvm-mingw) with a Debian 11 sysroot (glibc 2.31) |
| Android | No | Android NDK |
| Web | No | Emscripten |

**Exporting.** `slash-cli export <platform>` (or the `export.run` command an agent calls over MCP) builds the game module, packs assets and signs the result. Because gameplay is compiled C++, every export is a real build for that platform, not a template plus scripts.

- **Projects.** A game is a folder with a `project.json`, the equivalent of Godot's `project.godot`: name, identifier, version, main scene, window size and tick rate, checked against a schema the engine publishes.
- **One entry point.** Every game executable starts in the same `main` built on SDL's main callbacks, so the loop is driven by the host where it must be (the browser, iOS) and by SDL elsewhere. The same executable runs headless with `--headless --ticks N` and prints a JSON summary, which is how exports are tested.
- **How an export runs.** The project and every scene are validated, staged as the game's data, and built with CMake from the engine sources (`export/CMakeLists.txt`), then laid out for the platform: an app bundle on macOS (universal for release), an executable and `data` folder on Windows (C++ runtime linked statically) and Linux (built against a glibc 2.31 sysroot, using the player's libstdc++), and `index.html` with WebAssembly on the web. The full build log goes to a file; the caller gets a short JSON result, or the failing step with the end of the log.

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
| Refresh rates and frame pacing | V-Sync modes, max FPS | V-Sync, max FPS, frame rate smoothing | V-Sync modes, frame cap, display refresh rates, variable refresh rate, rendering interpolated between fixed ticks | Launch |
| UI | Control nodes, containers, themes | UMG | Retained UI with flexbox layout and themes, defined in text | Launch |
| Text and fonts | Font rendering, BiDi, IME | Slate fonts, IME | FreeType, HarfBuzz, MSDF fonts, BiDi, IME input, fallback fonts | Launch |
| Navigation | Navigation meshes, avoidance | Nav mesh, AI controllers | Recast and Detour, 2D and 3D, avoidance | Launch |
| Asset import | glTF, FBX, OBJ, `.blend` through Blender, images, audio | FBX, glTF, USD | glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG, MP3, and `.blend` through the artist's own Blender | Launch |
| Profiling | Profiler, debugger, monitors | Unreal Insights | Tracy, plus perf.stats over the command API | Launch |
| Asset generation and library | Asset Library (add-ons, no generation) | Fab, shared Derived Data Cache | Generated assets saved once by content hash and shared by every project and teammate; agents search before generating, and agent feedback ranks results; 3D models as parametric recipes, cleaned-up generated meshes or imported files, combined into complex assets with variants and reviewed automatically | Launch |
| Global illumination | SDFGI, VoxelGI, LightmapGI | Lumen | Baked lightmaps and probe-based GI | Launch |
| Sky and volumetrics | Procedural sky, volumetric fog | Sky atmosphere, volumetric clouds | Physical sky, volumetric fog | Launch |
| Decals | Decal node | Deferred decals | Clustered decals | Launch |
| Networking | High-level multiplayer, ENet, WebSocket, WebRTC | Replication | ENet transport, replication driven by reflection, WebSocket for web | Launch |
| Terrain | Plugins | Landscape | Heightmap terrain with splat materials and LOD | Launch |
| Level geometry | CSG, GridMap | Brushes, modeling tools | CSG and grid-based tile placement | Launch |
| LOD and culling | Auto mesh LOD, occlusion culling | Nanite, HLOD | Generated LODs with meshoptimizer, occlusion culling | Launch |
| XR | OpenXR | OpenXR | OpenXR | Launch |
| Localization | Translation tables, gettext | Localization dashboard | CSV and `.po` translation tables, any source language, a runtime language setting, and commands to extract, draft and check translations | Launch |
| Video playback | Theora | Media Framework | AV1 through dav1d | Launch |
| Accessibility | Screen reader support | Partial | AccessKit | Launch |
| Storyboard | None built in | None built in | Style, world, story, design rules and decisions as typed project files that agents query and checks enforce | Launch |
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

- **Allowed licenses:** MIT (with its variants, such as HarfBuzz's "Old MIT"), BSD, zlib, Apache 2.0, Boost, MIT-0, the FreeType License and public domain. No GPL or LGPL in the runtime, so games built on SlashEngine can ship under any license.
- **No copied engine code.** Nothing from UE5 or any other closed engine, including snippets from its source or docs. Godot is MIT, so its ideas and code may be borrowed with attribution in `THIRD_PARTY.md`.
- **System APIs are linked, not shipped.** Win32, Direct3D, Metal, Vulkan drivers and mobile OS SDKs are called through SDL3; none of their code lives in the repo.
- **Generators are called, not shipped.** The asset generator talks to image and audio models over HTTP, like `ai::Provider`, so no model or vendor SDK is in the repo. Each generated asset records its generator, model and license terms, because what a team may do with an asset depends on the service that made it.
- **Store and ad SDKs follow the same rule.** StoreKit is an OS framework and Google Play Billing is fetched at export time with the Android SDK, so the repo holds only SlashEngine's glue for them. Ad network SDKs are proprietary, so their adapters live outside the repo, like console backends (decided by Dan).
- **Consoles live outside the repo.** PlayStation, Xbox and Switch SDKs are closed and under NDA, so, as with Godot, console ports are private backends built by licensed porting partners and offered to studios separately. They plug in behind the same `RenderDevice` and platform interfaces.
- **Enforced in CI.** Each dependency is pinned with its license in a manifest, and a license check runs on every PR.

| Library | Used for | License |
| --- | --- | --- |
| SDL3 | Platform layer, GPU API | zlib |
| glslang, SPIRV-Cross | Compiling HLSL shaders to SPIR-V and MSL ahead of time; tools only, nothing of theirs ships | BSD-3-Clause and others, Apache 2.0 |
| EnTT | ECS and runtime type metadata | MIT |
| yyjson | JSON parsing | MIT |
| Jolt Physics | 3D physics, soft bodies | MIT |
| Box2D v3 | 2D physics | MIT |
| miniaudio | Audio | MIT-0 or public domain |
| Recast and Detour | Navigation | zlib |
| FreeType, HarfBuzz | Fonts and text shaping | FreeType License (BSD-style), MIT |
| msdfgen | Multichannel signed distance field fonts | MIT |
| Roboto | The default font | Apache 2.0 |
| cgltf, ufbx, stb_image | Asset import | MIT |
| meshoptimizer | LODs, mesh compression | MIT |
| Manifold | CSG and parametric models | Apache 2.0 |
| xatlas | UV unwrapping | MIT |
| CoACD | Convex collision hulls | MIT |
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
| Run | `play`, `pause`, `step(n)`, `run.until`, `input.inject`, `replay.run`, `replay.seek` | Drive the simulation deterministically |
| Observe | `frame.capture`, `log.tail`, `events.subscribe`, `perf.stats`, `debug.overlay` | See and measure what happened |

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
| `storyboard.brief` returns only the facts a task touches and what changed since the agent last looked | Re-reading design notes, and mistakes from guessing | 5 |
| `"dry_run": true` on any edit or `batch` returns what would change and any errors, changing nothing | Undoing failed experiments | 5 |
| `world.snapshot`, `world.diff` and `world.restore` show exactly which entities and fields changed, by ID | Re-querying the world to see what an edit did | 5 |
| Every feature ships with a working example that CI builds and runs headless | Code written against APIs that don't exist | 5 |
| `run.until` steps a game headless until a condition on component fields holds and returns the tick and matching entities | Polling and reading logs | 5 |
| `replay.seek` jumps a recorded run to any tick, where every query works | Reproducing a bug by hand | 5 |
| `build.check` compiles only changed gameplay files and returns errors as data with the compiler's suggested fixes | Reading build logs, and full rebuilds | 6 |
| `api.search` returns real C++ signatures, docs and the verified examples that use them | Guessed function names and failed builds | 6 |
| `playtest.run` runs many headless copies at once with scripted bots and different tuning values, and returns a summary | One run per call when tuning gameplay | 6 |
| `project.map` says where systems, components, scenes and source files are | Searching a large codebase | 6 |
| A benchmark of standard agent tasks counts calls and tokens on a schedule | Regressions in agent efficiency | 6 |
| The asset library returns an earlier asset instead of generating it again, and describes assets in text | Generation calls and image tokens | 6 |

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

## Asset library and generation

Generating an image or a sound costs a model call, time and often money, and agents otherwise make the same asset again whenever a user switches tools, a teammate asks for something similar or a new project starts. The asset library keeps every generated asset so it can be found and reused instead. It is plain storage, search and commands, so it works with any agent and any model.

- **One library per machine, not per project.** Assets live once in a content-addressed store (`~/.slash/library`), keyed by the hash of their bytes. Every project made with the engine can search it. Using an asset copies it into the project, so exports never depend on the library.
- **Searched before generating.** `asset.search` matches text, tags and type and returns IDs with short text descriptions, so an agent can pick an asset without loading the image. The `asset.generate` command checks the library first: an exact repeat of an earlier request returns the stored asset with no model call, and close matches come back as candidates.
- **Every asset remembers how it was made:** the request, generator, model, seed, license terms, tags and its parent if it was refined from another asset. `asset.refine` makes a new version linked to its parent, so iterations form a history that can be compared and rolled back.
- **Optimized once.** Each asset is imported and compressed for each platform when first used, and the result is cached, so later projects reuse it without converting it again.
- **Learning from feedback, not training.** Agents record what happened with an asset through `asset.feedback`: accepted or rejected and why, by hand or automatically from scene validation, the headless test command and frame summaries. Feedback ranks search results, picks between candidates, and is passed back into later prompts as notes on what worked. No model is trained, so this works the same with every provider.
- **Shared with the team.** The library can sync with a store the team hosts: a shared folder or a small library server that `slash-cli` runs. Nothing goes to a central service the project runs, and a teammate's request finds assets someone else already made.
- **Model agnostic and testable.** Generators are adapters behind one interface, like `ai::Provider`, for hosted and local image and audio models; 3D models join when glTF import lands. A test generator lets agents and CI use the library headless with no model or key.

### 3D assets

Generated 3D meshes are usually too dense, badly scaled, hard to edit and lit into their textures, so a raw model is rarely ready for a game. The library treats 3D generation as a pipeline that ends in a game-ready, reviewed asset:

- **Parametric models first.** Many props, buildings and hard-surface objects are better written than generated. An agent writes a `.model.json` recipe (primitives, CSG on Manifold, extrusions, lathes, bevels, arrays, all with named parameters) and the engine builds the mesh. Recipes are small, exact and reproducible, and refining one means changing a number rather than generating again. This is Godot's CSG idea, written as text.
- **Concept image, then model.** For organic shapes a generator makes a cheap 2D concept first. The agent reviews it, and only then is a 3D model made from the image. The image stays in the library as the model's parent.
- **Game-ready cleanup.** Every generated or imported mesh is set to real-world scale, given a sensible pivot and up axis, reduced to a triangle budget (meshoptimizer), unwrapped (xatlas), rebaked into PBR textures with baked-in lighting removed, given LODs and given collision hulls (CoACD).
- **Automatic review.** The engine renders turntable views headless, checks the geometry (budget, scale, holes, texel density) and asks a vision-capable model through `ai::Provider` whether the views match the request. The results are recorded with `asset.feedback`, and a failed check retries within a call budget the caller sets.
- **Described in text.** Each model comes with its size, pivot, named parts, materials, triangle count and attachment points, so agents place and combine models without loading images.
- **Parts, rigs and animation.** Named parts and attachment points let wheels turn, doors open and parts from different models combine. Characters are auto-rigged through a generator adapter and retargeted to one standard humanoid skeleton, so every animation in the library works on every character.
- **One style per game.** The [storyboard](#storyboard)'s style section (palette, reference images, triangle and texel budgets) goes with every request and is refined from the assets agents accept, so a game's models match. Materials come from tileable PBR textures shared through the material graph rather than a unique texture per model.
- **Imported assets are first-class.** Files from Blender and other tools, asset stores or a team's artists go into the library like generated ones: the same cleanup, text description, review and versions, plus a license the importer records. `.blend` files are converted by running the artist's own Blender in the background, as Godot does, so no Blender code is linked or shipped. Editing the source file makes a new version.
- **Blender as a generator.** Agents can build assets by running Blender headless with Python scripts. The script is saved with the result, so an asset can be rebuilt or tweaked later instead of made again from scratch.
- **Complex assets are built from parts.** An asset can be made of other library assets, such as a building of walls, doors and props, or a character with outfits and accessories. Parts are referenced by hash, so shared parts are stored once, and updating a part offers new versions of everything that uses it. Each complex asset can have named variants (clean or damaged, seasons, team colors) and quality tiers per export preset, all built from one source. Checks for scale, material slot names, rigs, animation clips and missing parts run before an asset is saved and again in CI, which is what makes large assets reliable to rebuild.
- **Scenes are assembled, not generated.** Agents place library assets with constraints (on the ground, not overlapping, along a path) instead of generating a whole scene as one mesh, so every piece stays editable and reusable.

## Storyboard

Agents make things up when they lack the facts or guess at them, and the risk grows when several agents share a project or the project is too big to read. The storyboard is the game's single source of truth for everything that is not code: style, world, story, design rules, names, decisions and open work. It is typed data that agents query and checks enforce, not free-form notes, so a fact is looked up rather than remembered.

```text
my-game/storyboard/
  style.json       # art direction: palette, references, tone, triangle and texel budgets
  world.json       # characters, places, factions, items and lore facts
  story.json       # acts, beats and quests, in order
  design.json      # mechanics, rules, tuning numbers and controls
  glossary.json    # canonical names, aliases and words to avoid
  decisions.json   # what was decided, by whom and why
  work.json        # open work: who is on it, what it touches, open questions
```

- **Stable IDs and typed fields.** Every entry has an ID such as `character/mira` and fields from a schema the engine generates (`slash-cli schema storyboard`), so most facts can be checked by a program, not only read by a model.
- **Briefs, not the whole board.** `storyboard.brief` takes the scenes, entities, assets or files a task touches and returns only the facts, decisions and open work linked to them, plus what changed since the agent last looked. A brief stays small however large the project grows, which is what keeps this workable in big codebases.
- **Links make facts checkable.** A `StoryRef` component ties an entity to a storyboard ID, and assets and source files can be linked the same way. An unknown ID is an error with a "did you mean" hint, so a misremembered name fails at once instead of spreading.
- **Checks, not trust.** `storyboard.check` verifies references, names against the glossary, colors against the palette and assets against budgets. It runs in `scene validate`, the headless test command and CI. With `ai::Provider`, a model can also read new dialogue, text and assets against the storyboard and report likely contradictions; these findings are advisory, since a model's judgment is not proof.
- **Safe for many agents.** Edits go through `storyboard.edit` with undo. Each edit names the version it read, so a stale edit is refused instead of overwriting a newer fact. A journal records who changed what and why, and `work.json` says which agent is on what, so agents see each other's work.
- **Grounding code too.** `engine.describe` already lists the real components and commands. `project.map` adds a generated map of the project's systems, scenes and source files, linked to the storyboard. Because it is generated, it cannot go stale the way hand-written notes do.
- **Used by default.** `slash-cli init` writes a starter storyboard, and `AGENTS.md`, the skill and the MCP instructions tell agents to call `storyboard.brief` before a task and `storyboard.check` after it. The asset library sends the style section with every generation request, and asset feedback refines it.

What it cannot do: no tool can stop a model from inventing things in prose it writes. The storyboard makes that much less likely by putting the right facts in front of the agent, and it catches every contradiction in typed, linked data. Contradictions in free text, such as plot logic or tone, are caught only as well as the reviewing model reads them.

## Languages

Games ship in many languages, and the people and agents making them work in many languages too. Following Godot, the engine treats no language as special.

- **Players pick a language.** A game starts in the player's OS language when it has a translation for it, and its options menu can switch language while it runs. Lookups fall back from `es-MX` to `es` to the game's source language. Each language can have its own fonts, and right-to-left languages lay out right to left.
- **Typing works in every language.** Text fields support IME composition for Japanese, Chinese and Korean, with the candidate window placed at the field, and on-screen keyboards on phones. Fallback fonts cover scripts the main font lacks.
- **Any source language.** Translation tables are CSV or gettext `.po` files, as in Godot, so translators can use the tools they already know. A game can be written in Japanese or Korean first and translated to English later.
- **Agents do the busywork.** `l10n.extract` collects every player-facing string from scenes and from `tr("...")` calls in C++. `l10n.translate` drafts translations through `ai::Provider`, using the storyboard's glossary and characters so names and tone stay consistent, and marks them for human review. `l10n.check` finds missing or stale keys, mismatched placeholders, text that overflows its UI box in the chosen font and glyphs a font lacks, and runs in the headless test command and CI. Pseudo-localization shows untranslated or clipped text before any real translation exists.
- **Developers in any language.** Comments, scene text and storyboard entries can be in any language, and agents answer in the language they are asked in. Entity IDs, command names and error codes stay ASCII so every tool and terminal handles them, and readable names in any language go in fields beside them.

## Repo layout and milestones

The repo splits into a core library, optional modules and the tools agents use, with sample games that double as integration tests.

```text
SlashEngine/
  CMakeLists.txt, CMakePresets.json
  AGENTS.md          # how an agent builds, tests and drives the engine
  engine/
    core/            # memory, Result, logging, math, jobs, reflection
    platform/        # SDL3 wrapper: window, input, audio, files
    asset/           # asset handles, loaders and hot reload of asset files
    render/          # RenderDevice, SDL GPU + null backends, images, 3D passes
    world/           # ECS, scenes, prefabs, serialization
    input/           # input events, rebindable action maps, the input state, SDL events
    canvas/          # 2D drawing like Godot's canvas: sprites, materials, tilemaps, lights, particles
    command/         # Command registry, undo, JSON-RPC and MCP
    server/          # localhost HTTP: REST mirror, OpenAPI, JSON-RPC and MCP
    runtime/         # project.json, the game loop and every game's main()
    hotreload/       # loads and swaps the C++ game module, keeps world state
    ai/              # optional: Provider interface and adapters
    library/         # optional: asset library, generators and feedback
    storyboard/      # storyboard files, briefs, checks and the project map
  tools/
    exporter/        # export presets, building, signing and the export.run command
    slash-mcp/       # MCP over stdio, or forwarded to a running game
    slash-cli/       # run, export, format, validate scenes headless
    slash-game/      # runs a project the way its exports do
  export/            # CMake project that builds one game for one platform
  samples/pong/      # smallest complete game, built by an agent end to end
  examples/          # one small working example per feature, built and run in CI
  tests/
```

**Milestones** (each ends with a green CI build and working exports on all six platforms)

1. **Skeleton.** CMake presets, CI matrix, SDL3 window on macOS, clang-format and tidy, `AGENTS.md`.
2. **Core and world.** Reflection macro, EnTT integration, JSON scene load and save with schema validation, headless runner.
3. **Commands.** Command registry, undo, JSON-RPC server, `slash-mcp`, REST mirror, `slash-cli`. Exit: Claude Code, Codex and Cursor each open, edit and save a scene over MCP.
4. **Export pipeline.** `slash-cli export` and toolchain install for all six platforms, plus the CI workflow. Exit: an empty project exports and passes headless tests everywhere. It comes this early so every later feature stays exportable. Built in two parts: (a) project files, the game runtime and exports for macOS, Windows, Linux and web on their own hosts, tested in CI; (b) iOS and Android, cross toolchains from a Mac, export presets with signing, and the `export.run` command.
5. **2D feature set.** Systems, play and pause, event subscriptions, prefabs, asset handles with hot reload, the storyboard, dry runs, world snapshots and diffs, verified examples and the CI license check first; then the renderer, materials, sprites, keyframe animation, tilemaps, 2D lights, Box2D physics, particles, UI and text with IME input, audio, input injection, frame capture, replays with time travel, `run.until` and the debug overlay. Exit: an agent builds `samples/pong` and exports it to all six platforms.
6. **Hot reload, AI and profiling.** Hot-reloadable C++ game module with `build.check`, `api.search`, `playtest.run` with tuning sweeps, `ai::Provider` with three adapters and `ReplayProvider`, the asset library and generator for images and audio, storyboard review by a model and the project map, the job system, Tracy, `perf.stats` and the per-system memory report.
7. **3D feature set.** Forward+ PBR renderer, shadows, post-processing, GI, sky, decals, LODs, Jolt physics, skeletal animation and blending, navigation, terrain, CSG, glTF and FBX import, and 3D in the asset library: parametric models, game-ready cleanup, automatic review, rigging and scene assembly. Exit: an agent builds a small 3D sample end to end.
8. **Systems and platform services.** Networking, XR, localization, video, accessibility, and analytics, ads and in-app purchases behind one API with a headless test backend.
9. **Launch.** Every Launch row of the parity table done, the Mac engine and tools released, exports for all six platforms, reference docs generated from `engine.describe`.

After launch: Windows and Linux hosts for the engine and tools, then the After launch rows.

**Open questions**

- [x] Minimum OS versions. Decided by Dan: only versions released in 2020 or later are supported. That means macOS 11, iOS 14, Android 11 (API level 30), Windows 10 version 2004, Linux with glibc 2.31 (Ubuntu 20.04 or newer), and on the web Chrome and Edge 85, Firefox 79 and Safari 15, the oldest browsers Emscripten supports (Safari 14 also lacks WebGL2). SDL3's GPU API needs less on every platform.
