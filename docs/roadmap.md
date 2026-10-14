# Roadmap

This is the plan for SlashEngine from today to launch and beyond. It has no dates by design: the
order is the plan, and each item carries a status instead. It is drawn from the approved
[design](design.md), whose milestone list and feature parity table it tracks.

## Status words

| Status | Meaning |
| --- | --- |
| **Done** | Merged to `main`, tested in CI. |
| **In progress** | Someone is building it now. |
| **Next** | The next milestone to start. |
| **Planned** | Scheduled for a named milestone before launch. |
| **After launch** | Planned, but not needed for the first release. |
| **Research** | Explored, with no commitment to ship. |

## Where things stand

Milestones 1 to 3 and the first half of milestone 4 are done. The engine builds on macOS, Linux and Windows, runs an EnTT world on
a fixed-timestep loop headless or in an SDL3 window, loads and saves JSON scenes checked against a
schema it generates from its own components, and serves every engine action as a command over
MCP, JSON-RPC, a localhost REST API and the command line, including while a game runs. A game is
a folder with a `project.json`, and `slash-cli export` builds it for macOS, Windows, Linux or the
web and can test the result headless. The second half of milestone 4, now in progress, adds iOS,
Android, signing and building every platform from one Mac. Nothing draws to the screen yet beyond an empty window; rendering
arrives with the 2D feature set in milestone 5.

## The path to launch

Each milestone ends with a green CI build. From milestone 4 on, each also ends with working
exports to all six target platforms, which is why the export pipeline comes before any feature
work.

### Milestone 1: Skeleton (Done)

- [x] CMake presets (`dev`, `ci`) and dependencies pinned through CPM.cmake
- [x] CI on macOS, Linux and Windows, with clang-format and clang-tidy checks
- [x] SDL3 window, with a headless run mode
- [x] `slash-cli`, structured JSON logging, `Result<T>` and `SLASH_ASSERT`
- [x] `AGENTS.md`

### Milestone 2: Core and world (Done)

- [x] Reflection with `SLASH_COMPONENT` and `SLASH_REFLECT`
- [x] EnTT world with stable string IDs and a component registry
- [x] Built-in `Transform2D` and `Transform3D` components
- [x] JSON scenes: load, save, canonical format, JSON Schema generated from components
- [x] `slash-cli scene validate`, `scene format` and `schema scene`
- [ ] Prefabs with `"extends"` and overrides (moved to [milestone 5](#milestone-5-2d-feature-set-planned))

### Milestone 3: Commands (Done)

- [x] Command registry with a JSON Schema per command and capability levels
- [x] Sessions with undo, redo and history; a failed command changes nothing
- [x] 18 built-in commands, including `batch`, `world.query` and `step`
- [x] JSON-RPC 2.0 over stdio (`slash-cli rpc`) and one-shot calls (`slash-cli call`)
- [x] `slash-mcp`: MCP over stdio, or forwarded to a running game with `--live`
- [x] Localhost HTTP server: REST mirror, OpenAPI 3.1, JSON-RPC and MCP over HTTP
- [x] Live games: `slash-cli run --serve` runs commands between ticks
- [x] `slash-cli init` writes MCP configs for Claude Code, Codex and Cursor
- [ ] Exit check: Claude Code, Codex and Cursor each open, edit and save a scene over MCP. The
      tools are built and tested; a recorded run with each agent is still to do.

### Milestone 4: Export pipeline (In progress)

Built in two halves.

**4a: projects and desktop and web exports (Done,
[PR #6](https://github.com/zerojunz/SlashEngine/pull/6))**

- [x] `project.json`, like Godot's `project.godot`: name, version, main scene, window size and
      tick rate, with `slash-cli schema project` and `slash-cli project validate`
- [x] A game runtime that loads a project's main scene and runs it, headless or in a window
- [x] `slash-cli export <platform>` for macOS (universal app bundle), Windows, Linux and web
- [x] `--test`, which runs the exported game headless and reports whether it worked
- [x] A CI job that exports an empty game for macOS, Windows, Linux and web and runs each one,
      with Emscripten pinned

**4b: mobile, signing and one-Mac builds (In progress)**

- [ ] iOS and Android exports
- [ ] `slash-cli toolchain install`: pinned open-source cross toolchains (llvm-mingw, Clang with a
      Linux sysroot, Android NDK), so one Mac builds every target
- [x] `export_presets.json`, with signing secrets read from environment variables: macOS
      codesigning (ad hoc by default) and notarization, Windows signtool
- [ ] A throwaway Android debug keystore, with the Android export
- [x] The `export.run` command, so agents export over MCP
- [x] A GitHub Actions workflow from `slash-cli init` that builds and tests every platform on
      native runners
- [x] Exports pinned to the 2020 minimums: macOS 11, Windows 10 (subsystem version 10.0), Chrome
      85, Firefox 79 and Safari 15

Exit: an empty project exports to all six platforms and passes headless tests on each.

### Milestone 5: 2D feature set (Planned)

Foundations first, in the order Godot built them: the run loop, scenes and resources, then
rendering on top.

- [ ] Systems registered with the world, run in a fixed order each tick; `engine.describe`
      already returns an empty `systems` list for them
- [ ] Run control commands: `play` and `pause`
- [ ] `events.subscribe` for a stream of world events
- [ ] Prefabs: an entity `"extends"` a `.prefab.json` file and overrides fields
- [ ] Asset handles with reference counting and hot reload, and an `asset.list` command
- [ ] A license check on every pull request, against a manifest of pinned dependencies, before
      this milestone adds Box2D, miniaudio, FreeType and HarfBuzz
- [ ] Renderer foundation: `RenderDevice` over SDL3's GPU API, a null backend for headless runs,
      HLSL shaders cross-compiled with SDL_shadercross
- [ ] Materials: HLSL shaders plus a JSON material graph agents write directly, starting with
      2D (canvas) materials; milestone 7 adds the PBR inputs
- [ ] Sprites, sprite sheets and flipbooks
- [ ] Keyframe animation tracks on any reflected property, like Godot's AnimationPlayer
- [ ] Tilemap layers with terrain autotiling
- [ ] 2D lights, occluders and normal-mapped sprites
- [ ] 2D physics on Box2D v3
- [ ] Particles
- [ ] UI with flexbox layout and themes; text with FreeType, HarfBuzz and MSDF fonts
- [ ] Audio on miniaudio
- [ ] Rebindable input action maps, and `input.inject` for agents
- [ ] `frame.capture` (PNG) and replays (`replay.run`)
- [ ] Debug overlay, drawn with the UI and text above
- [ ] A test command that steps a game headless and returns only the checks that failed
- [ ] Frame summaries: what is on screen as data instead of an image

Exit: an agent builds `samples/pong` end to end and exports it to all six platforms.

### Milestone 6: Hot reload, AI and profiling (Planned)

- [ ] Gameplay as a C++ shared library that reloads on rebuild and keeps world state
- [ ] Job system, so systems run in parallel before the 3D work in milestone 7 needs it
- [ ] Profiling: Tracy, a `perf.stats` command and a per-system memory report
- [ ] `ai::Provider` with OpenAI-compatible, Anthropic and Gemini adapters
- [ ] `ReplayProvider` so tests that use a model run offline
- [ ] A benchmark of standard agent tasks that counts calls and tokens
- [ ] Asset library and generator, so agents reuse generated assets instead of making them again
      ([design](design.md#asset-library-and-generation))
  - [ ] A library shared by every project on the machine: each asset stored once by content
        hash, with the request that made it, its generator, model, seed, license, tags and a short
        text description agents can read instead of the file
  - [ ] `asset.search` by text, tags and type; agents search before they generate
  - [ ] `asset.generate` through generator adapters for hosted and local image and audio
        models, behind the same interface style as `ai::Provider`; an exact repeat of an earlier
        request returns the stored asset with no model call
  - [ ] Iteration: `asset.refine` makes a new version linked to its parent, so every asset keeps
        its history and can be rolled back
  - [ ] Ready-to-use forms: imported and compressed for each platform once, then cached
  - [ ] Feedback: agents record results with `asset.feedback`, by hand or automatically from
        checks such as validation, the headless test command and frame summaries; feedback
        ranks search results, picks between candidates and is passed back into later prompts
  - [ ] Team sharing: the library syncs with a store the team hosts, such as a shared folder or
        a library server that `slash-cli` runs, so a teammate's request finds assets someone
        else already made
  - [ ] A test generator, so agents and CI can use the library headless with no model or key

### Milestone 7: 3D feature set (Planned)

- [ ] Clustered forward+ renderer with PBR materials, plus a low-end path for web and mobile
- [ ] Cascaded, omni and spot shadows
- [ ] Post-processing: tonemap, bloom, SSAO, SSR, depth of field, auto exposure, color grading
- [ ] Baked lightmaps and probe-based global illumination
- [ ] Physical sky and volumetric fog; clustered decals
- [ ] Generated LODs with meshoptimizer and occlusion culling
- [ ] 3D physics on Jolt: rigid bodies, joints, character controller, vehicles
- [ ] Skinned meshes, animation blending, state machines, root motion, two-bone and FABRIK IK
- [ ] Navigation on Recast and Detour, in 2D and 3D, with avoidance
- [ ] Heightmap terrain; CSG and grid-based tile placement
- [ ] Import of glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG and MP3
- [ ] 3D models and materials in the asset library and generator

Exit: an agent builds a small 3D sample end to end.

### Milestone 8: Systems and platform services (Planned)

- [ ] Networking: ENet transport, replication driven by reflection, WebSocket for web
- [ ] XR through OpenXR
- [ ] Localization with translation tables in text
- [ ] Video playback with dav1d (AV1)
- [ ] Accessibility through AccessKit
- [ ] Platform services, like Unity's Analytics, Ads and In-App Purchasing: one cross-platform C++
      API and command set, with the store or network chosen per export preset
  - [ ] Analytics: custom events, sessions and funnels, batched and sent as JSON to an endpoint
        the developer chooses, such as a self-hosted open-source server
  - [ ] In-app purchases: products, purchase, restore and receipts, on StoreKit for iOS and
        macOS and Google Play Billing for Android
  - [ ] Ads: banner, interstitial and rewarded placements behind one interface; each ad network
        is an adapter (see the note below)
  - [ ] Consent and privacy: one consent state that gates analytics and ads, with Apple's App
        Tracking Transparency prompt on iOS
  - [ ] A test backend for all three, so agents and CI can run purchases, ads and events
        headless with no store account, plus commands to inspect and drive it

Ad network SDKs (AdMob, Unity Ads, AppLovin and others) are proprietary, so, like console ports,
their adapters live outside this repository. StoreKit is an OS framework, and Google Play Billing
is fetched at export time like the rest of the Android SDK, so neither is committed here. Dan
chose this split.

### Milestone 9: Launch (Planned)

- [ ] Every Launch row of the [feature table](#features-by-area) done
- [ ] The engine and tools released for macOS
- [ ] Exports for all six platforms
- [ ] Reference docs generated from `engine.describe`

## After launch

Once the macOS release ships:

1. **Windows and Linux hosts.** The engine and tools run on Windows and Linux, not only export to
   them. CI already builds and tests on both, so this is mostly packaging and host-only tools.
2. **After-launch features**, in no fixed order:
   - Large worlds: double-precision transforms and world streaming
   - Cinematics: timeline tracks for cameras and events
   - Procedural generation: FastNoiseLite plus rule graphs agents write
   - Cloth and soft bodies on Jolt
   - Destruction: fracture and debris on Jolt

## Research

These are UE5-class systems with no committed plan to ship. Work on them happens only after the
launch rows and the after-launch features above.

- Virtualized geometry in the style of Nanite (meshlet rendering)
- Fully dynamic global illumination at Lumen quality, after probe GI ships
- Digital humans in the style of MetaHuman; out of scope for releases

Console ports are not on this roadmap and never will be in this repository. As with Godot,
PlayStation, Xbox and Switch support comes from licensed porting partners as private backends.

## Features by area

The design's parity table, with each row's milestone and status. The Godot 4 and UE5 equivalents
are in [design.md](design.md#feature-parity-with-godot-and-ue5).

| Area | SlashEngine approach | Milestone | Status |
| --- | --- | --- | --- |
| Editor | No GUI editor: MCP and REST command API, debug overlay, frame capture | 3, 5 | In part: the command API is done; frame capture and the overlay are planned for 5 |
| Gameplay code | C++ with hot reload; plugins as C++ modules with a stable C ABI | 6 | Planned |
| Scenes and prefabs | JSON scenes, prefabs with overrides, on EnTT | 2, 5 | In part: scenes are done; prefabs are planned for 5 |
| 2D sprites and animation | Batched sprites, sprite sheets, flipbooks | 5 | Planned |
| Tilemaps | Tilemap layers with terrain autotiling, stored as text | 5 | Planned |
| 2D lighting | 2D lights, occluders, normal-mapped sprites | 5 | Planned |
| 2D physics | Box2D v3 | 5 | Planned |
| 3D physics | Jolt: rigid bodies, joints, character controller, vehicles | 7 | Planned |
| 3D renderer | Clustered forward+ with PBR, plus a low-end path for web and mobile | 7 | Planned |
| Shadows | Cascaded, omni and spot shadow maps | 7 | Planned |
| Post-processing | Tonemap, bloom, SSAO, SSR, DoF, auto exposure, color grading | 7 | Planned |
| Materials and shaders | HLSL shaders plus a JSON material graph | 5, 7 | Planned |
| Keyframe animation | Keyframe tracks on any reflected property | 5 | Planned |
| Skeletal animation | Skinned meshes, blending, root motion | 7 | Planned |
| Animation blending | Blend trees and state machines in text, two-bone and FABRIK IK | 7 | Planned |
| Particles | CPU and GPU particles, particle graphs in text | 5 | Planned |
| Audio | miniaudio: buses, effects, 3D spatial audio | 5 | Planned |
| Input | Rebindable action maps over SDL3, injectable by agents | 5 | Planned |
| UI | Retained UI with flexbox layout and themes, defined in text | 5 | Planned |
| Text and fonts | FreeType, HarfBuzz, MSDF fonts, BiDi | 5 | Planned |
| Navigation | Recast and Detour, 2D and 3D, avoidance | 7 | Planned |
| Asset import | glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG, MP3 | 7 | Planned |
| Profiling | Tracy, plus `perf.stats` over the command API | 6 | Planned |
| Asset generation and library | Generated assets saved once and shared by every project and teammate, searched before generating, ranked by agent feedback | 6, 7 | Planned: images and audio in 6, 3D in 7 |
| Global illumination | Baked lightmaps and probe-based GI | 7 | Planned |
| Sky and volumetrics | Physical sky, volumetric fog | 7 | Planned |
| Decals | Clustered decals | 7 | Planned |
| Networking | ENet transport, replication driven by reflection, WebSocket for web | 8 | Planned |
| Terrain | Heightmap terrain with splat materials and LOD | 7 | Planned |
| Level geometry | CSG and grid-based tile placement | 7 | Planned |
| LOD and culling | Generated LODs with meshoptimizer, occlusion culling | 7 | Planned |
| XR | OpenXR | 8 | Planned |
| Localization | Translation tables in text | 8 | Planned |
| Video playback | AV1 through dav1d | 8 | Planned |
| Accessibility | AccessKit | 8 | Planned |
| Analytics, ads and in-app purchases | One API for analytics, ads and purchases; StoreKit, Play Billing and a developer-chosen analytics endpoint in the repo, ad networks as outside adapters | 8 | Planned |
| One-command export | `slash-cli export` to macOS, Windows, Linux, iOS, Android and web | 4 | In progress: desktop and web done (4a), iOS and Android in progress (4b) |
| Large worlds | Double-precision transforms, world streaming | After launch | After launch |
| Cinematics | Timeline tracks for cameras and events | After launch | After launch |
| Procedural generation | FastNoiseLite plus rule graphs agents write | After launch | After launch |
| Cloth and soft bodies | Jolt soft bodies | After launch | After launch |
| Destruction | Fracture and debris on Jolt | After launch | After launch |
| Virtualized geometry | Meshlet rendering research | Research | Research |
| Fully dynamic GI at UE quality | Research after probe GI ships | Research | Research |
| Digital humans | Research; out of scope for releases | Research | Research |

## Agent efficiency

Features that make the engine cheaper for an agent to use than editing files by hand, measured in
calls and tokens ([design section 6](design.md#6-cheap-for-agents-to-use)).

| Feature | Milestone | Status |
| --- | --- | --- |
| `batch`: many commands in one call, all or nothing, one undo step | 3 | Done |
| Queries return IDs unless asked for components; edits return only changed fields; `omit_defaults` | 3 | Done |
| `engine.describe` lists components with defaults and command names in one short answer | 3 | Done |
| Errors carry the JSON Pointer of the bad value and a "did you mean" hint | 2, 3 | Done |
| `slash-cli init` and the MCP server's instructions point agents at the tools | 3 | Done |
| A test command that returns only the checks that failed | 5 | Planned |
| Frame summaries as data instead of screenshots | 5 | Planned |
| A scheduled benchmark of standard agent tasks counting calls and tokens | 6 | Planned |
| The asset library: search before generating, and read a text description instead of the file | 6 | Planned |

## Platforms

| Platform | Engine and tools run there | Games export there | Oldest supported version |
| --- | --- | --- | --- |
| macOS (Apple silicon and Intel) | At launch | Yes (4a) | macOS 11 Big Sur |
| Windows x64 | After launch (CI already builds and tests the engine there) | Yes (4a) | Windows 10 version 2004 (May 2020) |
| Linux x64 and arm64 | After launch (CI already builds and tests the engine on x64) | Yes (4a) | glibc 2.31 (Ubuntu 20.04, Debian 11, Fedora 32 or newer) |
| iOS | No | Milestone 4b | iOS 14 |
| Android | No | Milestone 4b | Android 11 (API level 30) |
| Web | No | Yes (4a) | Chrome and Edge 85, Firefox 79, Safari 15 |

Until milestone 4b, each desktop export is built on a machine running that platform; web exports
build on any host with Emscripten.

SlashEngine supports operating system versions released in 2020 or later, and nothing older. Each
minimum above is the first release of its OS from 2020. SDL3's GPU API needs less than this on
every platform (Metal, Vulkan and Direct3D 12 all predate 2020), so the rule is the limit, not the
graphics API. On the web the minimums are the oldest browsers Emscripten supports, which also
fit the rule except Safari: Safari 15 is from 2021, and Safari 14 has no WebGL2, which the renderer
needs as its fallback. When an exporter sets a deployment target, it uses these versions.

## Keeping this file current

- A pull request that finishes an item ticks its box and, if it finishes a milestone, changes the
  milestone's status. The next milestone becomes **Next**, and the one being built is
  **In progress**.
- New work goes under the milestone that will build it. Launch work with no obvious home goes in
  the earliest milestone that builds what it depends on, following Godot's order.
- Moving an item between milestones, or between Launch, After launch and Research, is a design
  change: update [design.md](design.md) in the same pull request.
- Never add dates.
