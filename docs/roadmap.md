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
| **Launch** | Needed for launch; the design does not yet name the milestone it lands in. |
| **After launch** | Planned, but not needed for the first release. |
| **Research** | Explored, with no commitment to ship. |

## Where things stand

Milestones 1 to 3 are done. The engine builds on macOS, Linux and Windows, runs an EnTT world on
a fixed-timestep loop headless or in an SDL3 window, loads and saves JSON scenes checked against a
schema it generates from its own components, and serves every engine action as a command over
MCP, JSON-RPC, a localhost REST API and the command line, including while a game runs. The export
pipeline (milestone 4) is next. Nothing draws to the screen yet beyond an empty window; rendering
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
- [ ] Prefabs with `"extends"` and overrides (moved out of this milestone; see
      [Launch work not yet in a milestone](#launch-work-not-yet-in-a-milestone))

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

### Milestone 4: Export pipeline (Next)

- [ ] `slash-cli export <platform>` and the `export.run` command
- [ ] `slash-cli toolchain install`: pinned open-source cross toolchains (llvm-mingw, Clang with a
      Linux sysroot, Android NDK, Emscripten), so one Mac builds every target
- [ ] `export_presets.json`, with signing secrets read from environment variables and a throwaway
      Android debug keystore
- [ ] A GitHub Actions workflow from `slash-cli init` that builds and tests every platform on
      native runners
- [ ] CI exports to macOS, Windows, Linux, iOS, Android and web

Exit: an empty project exports to all six platforms and passes headless tests on each.

### Milestone 5: 2D feature set (Planned)

- [ ] Renderer foundation: `RenderDevice` over SDL3's GPU API, a null backend for headless runs,
      HLSL shaders cross-compiled with SDL_shadercross
- [ ] Sprites, sprite sheets and flipbooks
- [ ] Tilemap layers with terrain autotiling
- [ ] 2D lights, occluders and normal-mapped sprites
- [ ] 2D physics on Box2D v3
- [ ] Particles
- [ ] UI with flexbox layout and themes; text with FreeType, HarfBuzz and MSDF fonts
- [ ] Audio on miniaudio
- [ ] Rebindable input action maps, and `input.inject` for agents
- [ ] `frame.capture` (PNG) and replays (`replay.run`)
- [ ] A test command that steps a game headless and returns only the checks that failed
- [ ] Frame summaries: what is on screen as data instead of an image

Exit: an agent builds `samples/pong` end to end and exports it to all six platforms.

### Milestone 6: Hot reload and AI (Planned)

- [ ] Gameplay as a C++ shared library that reloads on rebuild and keeps world state
- [ ] `ai::Provider` with OpenAI-compatible, Anthropic and Gemini adapters
- [ ] `ReplayProvider` so tests that use a model run offline
- [ ] A benchmark of standard agent tasks that counts calls and tokens

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

Exit: an agent builds a small 3D sample end to end.

### Milestone 8: Systems (Planned)

- [ ] Networking: ENet transport, replication driven by reflection, WebSocket for web
- [ ] XR through OpenXR
- [ ] Localization with translation tables in text
- [ ] Video playback with dav1d (AV1)
- [ ] Accessibility through AccessKit

### Milestone 9: Launch (Planned)

- [ ] Every Launch row of the [feature table](#features-by-area) done
- [ ] The engine and tools released for macOS
- [ ] Exports for all six platforms
- [ ] Reference docs generated from `engine.describe`

### Launch work not yet in a milestone

The design needs these for launch but does not say which milestone builds them. Each lands by
milestone 9 at the latest; when one is scheduled, move it into that milestone above.

- [ ] Prefabs: an entity `"extends"` a `.prefab.json` file and overrides fields (Launch)
- [ ] Asset handles with reference counting and hot reload, and an `asset.list` command (Launch)
- [ ] Run control commands: `play` and `pause` (Launch)
- [ ] `events.subscribe` for a stream of world events (Launch)
- [ ] Profiling: Tracy and a `perf.stats` command (Launch)
- [ ] Job system and per-system memory report (Launch)
- [ ] Systems registered with the world; `engine.describe` already returns an empty `systems`
      list for them (Launch)
- [ ] Debug overlay (Launch)
- [ ] Keyframe animation tracks on any reflected property (Launch)
- [ ] Materials: HLSL shaders plus a JSON material graph agents write directly (Launch)
- [ ] A license check on every pull request, against a manifest of pinned dependencies (Launch)
- [ ] Emscripten in the CI matrix (Launch)

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
| Editor | No GUI editor: MCP and REST command API, debug overlay, frame capture | 3, 5 | In part: the command API is done, frame capture is planned for 5, the overlay is Launch |
| Gameplay code | C++ with hot reload; plugins as C++ modules with a stable C ABI | 6 | Planned |
| Scenes and prefabs | JSON scenes, prefabs with overrides, on EnTT | 2 | In part: scenes are done; prefabs are Launch |
| 2D sprites and animation | Batched sprites, sprite sheets, flipbooks | 5 | Planned |
| Tilemaps | Tilemap layers with terrain autotiling, stored as text | 5 | Planned |
| 2D lighting | 2D lights, occluders, normal-mapped sprites | 5 | Planned |
| 2D physics | Box2D v3 | 5 | Planned |
| 3D physics | Jolt: rigid bodies, joints, character controller, vehicles | 7 | Planned |
| 3D renderer | Clustered forward+ with PBR, plus a low-end path for web and mobile | 7 | Planned |
| Shadows | Cascaded, omni and spot shadow maps | 7 | Planned |
| Post-processing | Tonemap, bloom, SSAO, SSR, DoF, auto exposure, color grading | 7 | Planned |
| Materials and shaders | HLSL shaders plus a JSON material graph | Not set | Launch |
| Keyframe animation | Keyframe tracks on any reflected property | Not set | Launch |
| Skeletal animation | Skinned meshes, blending, root motion | 7 | Planned |
| Animation blending | Blend trees and state machines in text, two-bone and FABRIK IK | 7 | Planned |
| Particles | CPU and GPU particles, particle graphs in text | 5 | Planned |
| Audio | miniaudio: buses, effects, 3D spatial audio | 5 | Planned |
| Input | Rebindable action maps over SDL3, injectable by agents | 5 | Planned |
| UI | Retained UI with flexbox layout and themes, defined in text | 5 | Planned |
| Text and fonts | FreeType, HarfBuzz, MSDF fonts, BiDi | 5 | Planned |
| Navigation | Recast and Detour, 2D and 3D, avoidance | 7 | Planned |
| Asset import | glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG, MP3 | 7 | Planned |
| Profiling | Tracy, plus `perf.stats` over the command API | Not set | Launch |
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
| One-command export | `slash-cli export` to macOS, Windows, Linux, iOS, Android and web | 4 | Next |
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

## Platforms

| Platform | Engine and tools run there | Games export there |
| --- | --- | --- |
| macOS (Apple silicon and Intel) | At launch | Milestone 4 |
| Windows x64 | After launch (CI already builds and tests the engine there) | Milestone 4 |
| Linux x64 and arm64 | After launch (CI already builds and tests the engine on x64) | Milestone 4 |
| iOS | No | Milestone 4 |
| Android | No | Milestone 4 |
| Web | No | Milestone 4 |

## Open questions

- Minimum macOS, iOS and Android versions, which SDL3's GPU API sets in practice.

## Keeping this file current

- A pull request that finishes an item ticks its box and, if it finishes a milestone, changes the
  milestone's status. The next milestone becomes **Next**, and the one being built is
  **In progress**.
- New work goes under the milestone that will build it, or under
  [Launch work not yet in a milestone](#launch-work-not-yet-in-a-milestone) if none is chosen.
- Moving an item between milestones, or between Launch, After launch and Research, is a design
  change: update [design.md](design.md) in the same pull request.
- Never add dates.
