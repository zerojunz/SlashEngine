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
a folder with a `project.json`, and `slash-cli export` builds it for macOS, Windows, Linux, the
web, Android or iOS, signs it and can test the result headless. The rest of milestone 4, now in
progress, builds every platform from one Mac. Games draw sprites and flipbook animations through
a GPU render device, smoothly between fixed ticks and with Godot's V-Sync modes and frame cap,
color them with material graphs, animate any component field with keyframes and draw tile maps
that autotile terrain; the rest of the 2D renderer follows in milestone 5.

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
- [x] Prefabs with `"extends"` and overrides (done in [milestone 5](#milestone-5-2d-feature-set-in-progress))

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

- [x] Android exports: APKs for arm64-v8a and x86_64 with no Gradle, tested on an Android 11
      emulator in CI
- [x] iOS exports: an `.app` for the Simulator, tested there in CI, and an `.ipa` for devices
      signed with the team's identity and provisioning profile
- [ ] App icons from one image for every platform, which the App Store and Google Play require
- [x] `slash-cli toolchain install`: pinned open-source cross toolchains (llvm-mingw and Debian
      11 sysroots with glibc 2.31), so one Mac builds Windows and Linux x86_64 and arm64 games,
      tested in CI by running them on Windows and Ubuntu 20.04
- [ ] `slash-cli toolchain install` for Emscripten and the Android SDK, NDK and JDK, so web and
      Android exports need nothing installed by hand either
- [ ] Wayland in Linux exports (they use X11, which Wayland desktops run through XWayland,
      because the glibc 2.31 sysroot's Wayland is too old for SDL's generated protocol code)
- [x] `export_presets.json`, with signing secrets read from environment variables: macOS
      codesigning (ad hoc by default) and notarization, Windows signtool
- [x] A throwaway Android debug keystore, and release keystores from environment variables
- [x] The `export.run` command, so agents export over MCP
- [x] A GitHub Actions workflow from `slash-cli init` that builds and tests every platform on
      native runners
- [x] Exports pinned to the 2020 minimums: macOS 11, iOS 14, Windows 10 (subsystem version 10.0
      in MSVC builds),
      Android 11, Chrome 85, Firefox 79 and Safari 15

Exit: an empty project exports to all six platforms and passes headless tests on each.

### Milestone 5: 2D feature set (In progress)

Foundations first, in the order Godot built them: the run loop, scenes and resources, then
rendering on top.

- [x] Systems registered with the world, run in a fixed order each tick; `engine.describe`
      already returns an empty `systems` list for them
- [x] Run control commands: `play` and `pause`
- [x] `events.subscribe` for a stream of world events
- [x] Prefabs: an entity `"extends"` a `.prefab.json` file and overrides fields
- [x] Asset handles with reference counting and hot reload, and an `asset.list` command
- [ ] Storyboard: the game's style, world, story, design rules, names, decisions and open work
      as typed files in the project, so every agent works from the same facts
      ([design](design.md#storyboard))
  - [x] `storyboard/*.json` files with a schema (`slash-cli schema storyboard`), every entry
        under a stable ID such as `character/mira`, edited through commands with undo
  - [x] `storyboard.brief`: only the facts, decisions and open work linked to the scenes,
        entities, assets or files a task touches, plus what changed since the agent last looked
  - [x] `storyboard.query` and `storyboard.edit`; an edit names the version it read, so a stale
        edit from another agent is refused instead of overwriting newer facts
  - [x] Links: a `StoryRef` component ties entities to storyboard IDs, and assets and source
        files can be linked too; unknown IDs are errors with "did you mean"
  - [x] `storyboard.check`: references, glossary names and `StoryRef` IDs, checked by
        `scene validate`, `project validate` and CI
  - [ ] Palette and budget checks once sprites, textures and meshes exist, and storyboard
        checks in the headless test command
  - [x] A journal of who changed what and why, and open work that says which agent is on it
        and what it touches, so agents can see what others are doing
  - [x] `slash-cli init` writes a starter storyboard, and the MCP instructions tell agents to
        call `storyboard.brief` before a task and `storyboard.check` after it
- [x] Dry runs: `"dry_run": true` on any edit or `batch` returns what would change and any
      errors, and changes nothing
- [x] Snapshots and semantic diffs: `world.snapshot`, `world.diff` (entities and fields added,
      removed or changed, by ID) and `world.restore`
- [x] Verified examples: every feature from here on ships with a small working example in
      `examples/` that CI builds and runs headless, so agents copy code that is known to work
- [x] A license check on every pull request, against a manifest of pinned dependencies, before
      this milestone adds Box2D, miniaudio, FreeType and HarfBuzz
- [x] Renderer foundation: `RenderDevice` over SDL3's GPU API (Metal, Direct3D 12, Vulkan) and a
      null backend for headless runs. Shaders are written once in HLSL: SPIR-V and MSL are
      compiled ahead of time with glslang and SPIRV-Cross and committed, and Direct3D 12 compiles
      the HLSL at startup with Windows' own compiler. Games clear their window to
      `rendering.clear_color`
- [ ] A web backend behind `RenderDevice`, since SDL3's GPU API has none: WebGPU, with WebGL2
      for browsers without it. Web games run with the null device until then
- [x] Refresh rates and frame pacing, following Godot's settings: the displays, their modes and
      refresh rates as data (`display.list`); V-Sync modes (disabled, enabled, adaptive,
      mailbox) and a frame cap (`max_fps`, 0 for none) in `project.json`, changeable while the
      game runs (`display.set`); variable refresh rate (G-Sync, FreeSync, ProMotion) left working
      by keeping V-Sync's present mode; 120 Hz on iPhones and Android phones that support it;
      frame pacing stats in `perf.stats`. The loop reports how far real time is into the next
      tick, for interpolated drawing
- [x] Canvas materials as a JSON graph agents write directly: `.material.json` node graphs over
      Godot's canvas inputs, with params each sprite sets (`material_params`) and Godot's blend
      modes, compiled on load to a program one HLSL shader runs, so materials hot reload and need
      no shader compiler. `material.eval` runs the same program on the CPU to check colors without
      a screenshot; exports check and ship the files. Milestone 7 adds the PBR inputs
- [ ] A game's own HLSL shaders for canvas materials, compiled at export with the engine's
      shader tools, for effects the graph can't express
- [x] Sprites, sprite sheets and flipbooks, as in Godot: `Sprite2D` (frames, regions, flips,
      tint, `z_index`) and `AnimatedSprite2D` playing `.frames.json` files (Godot's
      SpriteFrames), drawn in order and interpolated between fixed ticks so high refresh rates
      look smooth. Textures are PNG or BMP assets that hot reload, and exports ship and check
      them with the frames files
- [x] Keyframe animation tracks on any reflected property, like Godot's AnimationPlayer:
      `.anim.json` libraries of animations with value tracks on any component field or part of
      one, Godot's interpolation, easing, update and loop modes, and event tracks that emit world
      events. `AnimationPlayer` plays them, `animation.eval` returns the values at any times, and
      exports check and ship the files
- [x] Tilemap layers with terrain autotiling, as Godot's TileSet and TileMapLayer:
      `.tileset.json` files cut atlas textures into tiles with terrain peering bits (corners and
      sides, corners or sides), and `TileMapLayer` keeps its cells as text, rows of legend keys.
      `tilemap.paint` paints terrain and picks the tiles that connect, `tilemap.set` places
      tiles and `tilemap.get` reads them; layers draw in runs of one texture, and validation and
      exports check every layer against its tile set
- [ ] Isometric and hexagonal tile shapes, Godot's other TileSet layouts, with their terrain
      peering bits, and alternative tiles (flipped, transposed) for tile sets that reuse art
- [x] 2D lights, occluders and normal-mapped sprites, as Godot's: `PointLight2D` (textured,
      with a height for normal maps), `DirectionalLight2D` and `CanvasModulate`, with light masks,
      z ranges and add, sub and mix blending; `LightOccluder2D` shadows with cull modes and PCF
      filters; `Sprite2D` normal and specular maps; and materials' `light_mode`. The material
      shader lights every lit draw, the CPU casts shadows into a shadow map, and `light.sample`
      returns how lit points are without a screenshot
- [ ] Normal and specular maps on tile set atlases, and lights' canvas layer ranges once canvas
      layers exist
- [x] 2D physics on Box2D v3, as Godot's: `StaticBody2D`, `AnimatableBody2D`, `RigidBody2D`
      and `CharacterBody2D` (`move_and_slide` with floors, walls, slopes, snapping, moving
      platforms and one-way platforms), `Area2D` with enter and exit events, rectangle, circle,
      capsule, segment and convex shapes, concave `CollisionPolygon2D`, collision layers and
      masks, pin, spring and groove joints, and `physics.raycast`, `physics.overlap` and
      `physics.contacts`. Results are the same on every platform
- [ ] More of Godot's 2D physics: collision polygons on tile set tiles, `Area2D` gravity and
      damping overrides, `StaticBody2D` constant velocities, and `CharacterBody2D`'s
      `floor_block_on_wall`, `floor_constant_speed`, `wall_min_slide_angle` and `safe_margin`
- [x] CPU particles, as Godot's `CPUParticles2D`: emission shapes, directions and spread,
      gravity, linear, radial and tangential acceleration, orbits, damping, angles and spin,
      scale, color ramps and hue variation, each a random range with a curve over the
      particle's life; sprite sheet animation, one-shots with a `finished` event, preprocess,
      explosiveness, local coordinates and draw orders. Seeded by entity ID, so the same scene
      makes the same particles on every platform, and `particles.eval` checks them without a
      screenshot
- [ ] GPU particles, as Godot's `GPUParticles2D`: process materials as text graphs on the GPU,
      for many thousands of particles, with sub-emitters, trails and collision with the scene
- [x] Text with FreeType, HarfBuzz and MSDF fonts, and Godot's `Control`, `Label` and
      `ColorRect`: anchors, offsets and grow directions, rotation, scale and modulate, labels
      that wrap, align, trim with an ellipsis and draw outlines and shadows, theme overrides and
      `.font.json` font settings, drawn above the world. `ui.layout` and `text.measure` check a
      screen without a screenshot, and `scene validate` and exports catch controls that don't
      lay out
- [x] UI layout and themes: Godot's containers (box, grid, margin, center, flow and panel),
      `.theme.json` themes with type variations and a project theme, `StyleBoxFlat` (rounded
      corners, borders, shadows, skew and anti-aliasing) and `StyleBoxTexture`, and `Panel`,
      `TextureRect`, `NinePatchRect` and `ProgressBar`. `ui.theme` says where each of a control's
      theme items comes from
- [x] Input, as Godot's: keyboard, mouse, touch and gamepad events from SDL, rebindable action
      maps in `project.json` over Godot's `ui_*` actions, deadzones and strengths, touch that
      clicks the mouse, and `input.inject`, `input.state` and `input.actions` so agents press
      keys and actions and see what the game saw
- [x] Buttons, focus and hover, as Godot's: `Button` and `TextureButton` pressed by clicks,
      taps, `ui_accept` and shortcuts, toggle buttons and button groups, mouse filters, focus
      moved by the arrow keys, Tab and gamepads, and UI events (`pressed`, `toggled`,
      `mouse_entered`, `focus_entered` and the rest). `ui.press` and `ui.focus` drive them, and
      `ui.layout` reports each button's draw mode and which control has focus and is hovered
- [ ] The other controls people press and drag, as Godot's: `CheckBox`, `CheckButton`,
      `OptionButton` and `MenuButton` with popups, sliders, scroll bars and `ScrollContainer`,
      with tooltips and drag and drop
- [x] Text in every language, as Godot's text server: BiDi (SheenBidi) so right-to-left and
      left-to-right text mix in one line, right-to-left layout of controls
      (`layout_direction`), `text_direction` and `language` on labels and buttons, fallback
      fonts in `.font.json` files picked by language and script, and fonts installed on the
      machine for the rest, counted in `system_glyphs`
- [x] Text input in every language, as Godot's `LineEdit`: typing, the caret, selection by
      keys, clicks and drags, undo and redo, the clipboard, secret text and `max_length`; IME
      composition for Japanese, Chinese and Korean with the candidate window placed at the
      caret; on-screen keyboards on phones by `virtual_keyboard_type`; Godot's text editing
      actions with Mac and PC shortcuts; and `input.inject` `text` and `ime` events,
      `ui.clipboard` and the text field's caret and selection in `ui.layout`
- [ ] `TextEdit`: text fields of several lines, as Godot's, with wrapping, scrolling and line
      numbers
- [ ] Audio on miniaudio
- [ ] `frame.capture` (PNG) and replays (`replay.run`)
- [ ] Time travel: `replay.seek` jumps a recorded run to any tick, where every query command
      works, so a bug can be reproduced and inspected at the moment it happened
- [ ] Debug overlay, drawn with the UI and text above
- [ ] A test command that steps a game headless and returns only the checks that failed
- [x] Watch conditions: `run.until` steps a game headless until a condition on component
      fields holds (say, the player below the floor) or a tick limit passes, and returns the
      tick and the matching entities
- [ ] Frame summaries: what is on screen as data instead of an image

Exit: an agent builds `samples/pong` end to end and exports it to all six platforms.

### Milestone 6: Hot reload, AI and profiling (Planned)

- [ ] Gameplay as a C++ shared library that reloads on rebuild and keeps world state
- [ ] `build.check`: compiles only the changed gameplay files and returns errors as data (file,
      line, message and the compiler's suggested fix), with no build log to read
- [ ] Job system, so systems run in parallel before the 3D work in milestone 7 needs it
- [ ] Profiling: Tracy, per-system timings in `perf.stats` (which reports frame pacing since
      milestone 5) and a per-system memory report
- [ ] `ai::Provider` with OpenAI-compatible, Anthropic and Gemini adapters
- [ ] `ReplayProvider` so tests that use a model run offline
- [ ] A benchmark of standard agent tasks that counts calls and tokens
- [ ] Storyboard review: a model through `ai::Provider` reads new text and assets against the
      storyboard and reports likely contradictions as advisory findings
- [ ] `project.map`: a generated map of the project's systems, components, scenes and source
      files, linked to the storyboard, so agents in a large codebase find code without guessing
- [ ] `api.search`: finds engine and project C++ functions and types by name or purpose and
      returns their signatures, docs and the verified examples that use them
- [ ] Playtests and tuning sweeps: `playtest.run` runs many headless copies of a game at once,
      driven by simple scripted bots and each with different values (jump height, enemy speed),
      and returns a summary of the results per value
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
- [ ] Import of glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG and MP3, plus Blender `.blend`
      files the way Godot does it: the engine runs the artist's own Blender in the background to
      convert them to glTF, so no Blender code is linked or shipped
- [ ] 3D in the asset library and generator ([design](design.md#3d-assets))
  - [ ] Parametric models: agents write a `.model.json` recipe of shapes, CSG on Manifold,
        extrusions, lathes, bevels and arrays, and the engine builds the mesh; the library keeps
        the recipe, so refining means changing a number
  - [ ] Concept image first: generate and review a cheap image, then make the 3D model from it,
        with the image kept as the model's parent
  - [ ] Game-ready cleanup for every generated or imported mesh: real-world scale, pivot and up
        axis, a triangle budget with meshoptimizer, UVs with xatlas, PBR textures with baked-in
        lighting removed, LODs and collision hulls with CoACD
  - [ ] Automatic review: headless turntable renders, geometry checks (budget, scale, holes,
        texel density) and a vision-capable model comparing the views with the request, all
        recorded as feedback; failed checks retry within a call budget
  - [ ] Text descriptions of 3D assets: size, pivot, parts, materials, triangle count and
        attachment points, so agents place models without loading images
  - [ ] Named parts and attachment points, so wheels turn, doors open and parts from different
        models combine
  - [ ] Auto-rigging through a generator adapter, retargeted to one standard humanoid skeleton
        so every library animation works on every character
  - [ ] The storyboard's style section (palette, reference images, triangle and texel
        budgets), refined from accepted assets and sent with every request so a game's assets
        match
  - [ ] Tileable PBR materials from the 2D generator, shared through the material graph instead
        of a unique texture per model
  - [ ] Imported assets: files from Blender and other tools, asset stores or a team's own
        artists go into the library like generated ones, with the same cleanup, text
        description, review, versions and a license the importer records; editing the source
        file makes a new version
  - [ ] A Blender bridge: agents build assets by running Blender headless with Python scripts
        as a generator; the script is saved with the result, so the asset can be rebuilt or
        tweaked later
  - [ ] Complex assets: an asset made of other library assets (a building of walls, doors and
        props, a character with outfits) that references its parts by hash, so shared parts
        are stored once and updating a part offers new versions of everything that uses it
  - [ ] Variants: one asset with named options (clean or damaged, seasons, team colors) and
        quality tiers per export preset (phone, desktop), all built from the same source
  - [ ] Checks for complex assets: scale, material slot names, rigs, animation clips and every
        part present, run before an asset is saved and again in CI
  - [ ] Scene assembly: agents place library assets with constraints (on the ground, not
        overlapping, along a path) instead of generating a scene as one mesh

Exit: an agent builds a small 3D sample end to end.

### Milestone 8: Systems and platform services (Planned)

- [ ] Networking: ENet transport, replication driven by reflection, WebSocket for web
- [ ] XR through OpenXR
- [ ] Localization, following Godot ([design](design.md#languages))
  - [ ] Translation tables as CSV or gettext `.po` files, with plural forms, placeholders and a
        fallback chain (`es-MX`, then `es`, then the source language)
  - [ ] Any language as the source: a game can be written in Japanese and translated to English
  - [ ] A language setting: the player's OS language by default, changeable in a game's options
        menu while it runs, with per-language fonts and right-to-left layout
  - [ ] `l10n.extract`: every player-facing string in scenes and C++ (`tr("...")`) collected
        into the tables, so no text is missed
  - [ ] `l10n.translate`: draft translations through `ai::Provider`, using the storyboard's
        glossary and characters so names and tone stay consistent, marked for review
  - [ ] `l10n.check`: missing or stale keys, placeholders that don't match, text that overflows
        its UI box in a given font, and glyphs a font lacks; run by the headless test command
        and CI
  - [ ] Pseudo-localization: a fake language with longer, accented text that shows untranslated
        or clipped strings before any real translation exists
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
| Scenes and prefabs | JSON scenes, prefabs with overrides, on EnTT | 2, 5 | Done |
| 2D sprites and animation | Batched sprites, sprite sheets, flipbooks | 5 | Done |
| Tilemaps | Tilemap layers with terrain autotiling, stored as text | 5 | In part: square tiles are done; isometric and hexagonal shapes in 5 |
| 2D lighting | 2D lights, occluders, normal-mapped sprites | 5 | Done |
| 2D physics | Box2D v3: bodies, areas, joints, character movement and queries | 5 | In part: tile set collision and area overrides in 5 |
| 3D physics | Jolt: rigid bodies, joints, character controller, vehicles | 7 | Planned |
| 3D renderer | Clustered forward+ with PBR, plus a low-end path for web and mobile | 7 | Planned |
| Shadows | Cascaded, omni and spot shadow maps | 7 | Planned |
| Post-processing | Tonemap, bloom, SSAO, SSR, DoF, auto exposure, color grading | 7 | Planned |
| Materials and shaders | HLSL shaders plus a JSON material graph | 5, 7 | In part: canvas material graphs are done; a game's own HLSL shaders in 5, PBR inputs in 7 |
| Keyframe animation | Keyframe tracks on any reflected property | 5 | Done |
| Skeletal animation | Skinned meshes, blending, root motion | 7 | Planned |
| Animation blending | Blend trees and state machines in text, two-bone and FABRIK IK | 7 | Planned |
| Particles | CPU and GPU particles, particle graphs in text | 5 | In part: CPU particles are done; GPU particles in 5 |
| Audio | miniaudio: buses, effects, 3D spatial audio | 5 | Planned |
| Input | Rebindable action maps over SDL3, injectable by agents | 5 | Done |
| Refresh rates and frame pacing | V-Sync modes, frame cap, display refresh rates, variable refresh rate, rendering interpolated between fixed ticks | 5 | Done |
| UI | Retained UI with flexbox layout and themes, defined in text | 5 | In part: controls that draw, containers, themes, style boxes, buttons and focus are done; check boxes, option buttons, sliders, scrolling and text fields in 5 |
| Text and fonts | FreeType, HarfBuzz, MSDF fonts, BiDi, IME input, fallback fonts | 5 | In part: FreeType, HarfBuzz, MSDF fonts, BiDi and fallback fonts are done; IME input in 5 |
| Navigation | Recast and Detour, 2D and 3D, avoidance | 7 | Planned |
| Asset import | glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG, MP3, and `.blend` through the artist's own Blender | 7 | Planned |
| Profiling | Tracy, plus `perf.stats` over the command API | 5, 6 | In part: `perf.stats` reports frame pacing; Tracy and per-system timings are planned for 6 |
| Asset generation and library | Generated assets saved once and shared by every project and teammate, searched before generating, ranked by agent feedback; 3D models as parametric recipes, cleaned-up generated meshes or imported files, combined into complex assets with variants, reviewed automatically | 6, 7 | Planned: images and audio in 6, 3D in 7 |
| Global illumination | Baked lightmaps and probe-based GI | 7 | Planned |
| Sky and volumetrics | Physical sky, volumetric fog | 7 | Planned |
| Decals | Clustered decals | 7 | Planned |
| Networking | ENet transport, replication driven by reflection, WebSocket for web | 8 | Planned |
| Terrain | Heightmap terrain with splat materials and LOD | 7 | Planned |
| Level geometry | CSG and grid-based tile placement | 7 | Planned |
| LOD and culling | Generated LODs with meshoptimizer, occlusion culling | 7 | Planned |
| XR | OpenXR | 8 | Planned |
| Localization | CSV and `.po` translation tables, any source language, a runtime language setting, and commands to extract, draft and check translations | 8 | Planned |
| Video playback | AV1 through dav1d | 8 | Planned |
| Accessibility | AccessKit | 8 | Planned |
| Storyboard | Style, world, story, design rules and decisions as typed project files that agents query and checks enforce | 5, 6 | In progress: files, commands and checks are done; palette and budget checks wait on sprites and meshes; model review and the project map in 6 |
| Analytics, ads and in-app purchases | One API for analytics, ads and purchases; StoreKit, Play Billing and a developer-chosen analytics endpoint in the repo, ad networks as outside adapters | 8 | Planned |
| One-command export | `slash-cli export` to macOS, Windows, Linux, iOS, Android and web | 4 | In progress: every platform exports (4a, 4b); building them all from one Mac is next |
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
| `storyboard.brief`: only the facts a task needs, and what changed since the agent last looked | 5 | Done |
| Dry runs: see what an edit would change before making it | 5 | Done |
| `world.snapshot` and `world.diff`: try something, see exactly what changed, roll back | 5 | Done |
| Verified examples for every feature, built and run in CI | 5 | Done |
| `run.until`: run headless until a condition holds instead of polling | 5 | Done |
| `replay.seek`: query any earlier tick of a recorded run | 5 | Planned |
| `build.check`: C++ compile errors as data from an incremental build | 6 | Planned |
| `api.search`: real signatures and examples instead of guessed function names | 6 | Planned |
| `playtest.run`: many headless bot runs and tuning sweeps in one call | 6 | Planned |
| `project.map`: where things are in a large project, without reading the source | 6 | Planned |
| A scheduled benchmark of standard agent tasks counting calls and tokens | 6 | Planned |
| The asset library: search before generating, and read a text description instead of the file | 6 | Planned |

## Platforms

| Platform | Engine and tools run there | Games export there | Oldest supported version |
| --- | --- | --- | --- |
| macOS (Apple silicon and Intel) | At launch | Yes (4a) | macOS 11 Big Sur |
| Windows x64 | After launch (CI already builds and tests the engine there) | Yes (4a) | Windows 10 version 2004 (May 2020) |
| Linux x64 and arm64 | After launch (CI already builds and tests the engine on x64) | Yes (4a) | glibc 2.31 (Ubuntu 20.04, Debian 11, Fedora 32 or newer) |
| iOS | No | Yes (4b) | iOS 14 |
| Android | No | Yes (4b) | Android 11 (API level 30) |
| Web | No | Yes (4a) | Chrome and Edge 85, Firefox 79, Safari 15 |

A Mac builds every platform: macOS and iOS with Xcode's tools, and Windows and Linux with the
toolchains `slash-cli toolchain install` downloads. Linux machines build Linux and Windows the
same way, and Windows machines build Windows with MSVC. Web exports build on any host with
Emscripten, and Android exports on any host with the Android SDK, NDK and a JDK.

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
