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

Milestones 1 to 4 and 6 are done, and milestone 5 is nearly done. The engine builds on macOS, Linux and Windows, runs an EnTT world on
a fixed-timestep loop headless or in an SDL3 window, loads and saves JSON scenes checked against a
schema it generates from its own components, and serves every engine action as a command over
MCP, JSON-RPC, a localhost REST API and the command line, including while a game runs. A game is
a folder with a `project.json`, and `slash-cli export` builds it for macOS, Windows, Linux, the
web, Android or iOS, from one Mac with toolchains it downloads, signs it and can test the result
headless. Games draw sprites and flipbook animations through
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

### Milestone 4: Export pipeline (Done)

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

**4b: mobile, signing and one-Mac builds (Done)**

- [x] Android exports: APKs for arm64-v8a and x86_64 with no Gradle, tested on an Android 11
      emulator in CI
- [x] iOS exports: an `.app` for the Simulator, tested there in CI, and an `.ipa` for devices
      signed with the team's identity and provisioning profile
- [x] App icons from one image for every platform, which the App Store and Google Play require:
      `project.json`'s `icon` becomes an `.icns`, an `.ico` resource, an Android adaptive icon,
      an iOS asset catalog and a favicon, and the window's icon
- [x] `slash-cli toolchain install`: pinned open-source cross toolchains (llvm-mingw and Debian
      11 sysroots with glibc 2.31), so one Mac builds Windows and Linux x86_64 and arm64 games,
      tested in CI by running them on Windows and Ubuntu 20.04
- [x] `slash-cli toolchain install` for Emscripten, with Node.js and Python, so web exports need
      nothing installed by hand either
- [x] `slash-cli toolchain install` for the Android SDK, NDK and JDK, so Android exports need
      nothing installed by hand either (once the person accepts Google's Android SDK license)
- [x] Wayland in Linux exports, with Wayland 1.18's scanner built into `slash-cli` for the
      glibc 2.31 sysroot's Wayland; games try X11 first and fall back to Wayland, as Godot's do,
      unless `window.linux_display_driver` asks for Wayland first
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
  - [x] Instanced scenes, as Godot's PackedScene: an entity `"extends"` a `.scene.json` file
        with a `root` and the scene's other entities come with it, with IDs under the
        instance's (`hero/gun/muzzle`), overrides and `null` per entity, nested instances, and
        edits to the scene reaching every instance
- [x] Parent transforms, as Godot's Node2D children: entities in a `Transform2D`'s `children`
      move, turn, scale (and skew) with it and draw after it with its `z_index`, visibility
      and tint; physics, lights, particles and sound use global transforms; loops and second
      parents are refused; `entity.destroy` takes children along
  - [x] A body's shapes in child entities, as Godot's `CollisionShape2D` nodes
  - [x] `top_level` and `z_as_relative` off, on Node2D entities and controls
- [x] Asset handles with reference counting and hot reload, and an `asset.list` command
- [x] Storyboard: the game's style, world, story, design rules, names, decisions and open work
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
  - [x] Storyboard checks in the headless test command
  - [x] Palette and budget checks on textures, and a budget's triangle count on models since
        milestone 7
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
- [x] A web backend behind `RenderDevice`, since SDL3's GPU API has none: an OpenGL ES 3.0
      device (the `opengl` driver, Godot's compatibility tier), which is WebGL 2 in browsers and
      also runs on Android and Linux. The GLSL ES is compiled from the same HLSL and committed,
      and tests hold it to the software device's pixels on Mesa
- [x] Desktop OpenGL 3.3 for the `opengl` driver on Windows, macOS and Linux (`opengl_es` keeps
      OpenGL ES 3.0, as Godot's `opengl3_es`), and falling back to it where no Vulkan, Metal or
      Direct3D 12 starts, as Godot's `fallback_to_opengl3`
- [x] WebGPU on the web where browsers have it, with WebGL 2 kept for the rest (`auto` picks
      WebGPU when the browser has it); its WGSL is compiled from the same HLSL with Tint and
      committed, and tests hold it to the software device's pixels on Dawn over Mesa
- [x] Game shaders and FSR on WebGPU, with Tint in the shader tools
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
- [x] A game's own HLSL shaders for canvas materials, for effects the graph can't express:
      `fragment()` and `vertex()` with Godot's built-ins, which slash-cli and slash-mcp compile
      with glslang and SPIRV-Cross for every backend and the CPU (`material.eval`, frame
      captures), hot reloaded, and compiled into the material files at export
- [x] Sprites, sprite sheets and flipbooks, as in Godot: `Sprite2D` (frames, regions, flips,
      tint, `z_index`) and `AnimatedSprite2D` playing `.frames.json` files (Godot's
      SpriteFrames), drawn in order and interpolated between fixed ticks so high refresh rates
      look smooth. Textures are PNG or BMP assets that hot reload, and exports ship and check
      them with the frames files
- [x] A 2D camera, as Godot's `Camera2D`: the first enabled one in drawing order is current,
      with anchor modes, zoom, offset, rotation, limits (smoothed or not), drag margins and
      offsets, and position and rotation smoothing, ported from Godot's
      `get_camera_transform`. The world draws through it, interpolated between ticks; controls
      stay on the screen, 2D sound is heard from the middle of the view, and `frame.summary`
      reports the world rect it shows
- [x] Canvas layers and parallax, as Godot's `CanvasLayer` (with `follow_viewport`) and
      `Parallax2D`, and a call that turns a point on the screen into the world through the
      camera: layers draw what their `children` hold in their own canvas, in `layer` order
      around the world, unlit, with their controls laid out and clicked where they show; the
      `parallax_2d` system scrolls each `Parallax2D` as Godot's `_update_scroll`, with limits,
      autoscroll and repeats drawn to fill the window; `canvas.convert` and
      `canvas::to_world` turn points through the camera or a layer; `frame.summary` and
      `ui.layout` name each thing's layer; and the Godot importer converts `CanvasLayer`,
      `Parallax2D`, `ParallaxBackground` and `ParallaxLayer`
- [x] Keyframe animation tracks on any reflected property, like Godot's AnimationPlayer:
      `.anim.json` libraries of animations with value tracks on any component field or part of
      one, Godot's interpolation, easing, update and loop modes, and event tracks that emit world
      events. `AnimationPlayer` plays them on its own entity or, through track targets, on the
      entities it names, `animation.eval` returns the values at any times, and exports check and
      ship the files
- [x] Tilemap layers with terrain autotiling, as Godot's TileSet and TileMapLayer:
      `.tileset.json` files cut atlas textures into tiles with terrain peering bits (corners and
      sides, corners or sides), and `TileMapLayer` keeps its cells as text, rows of legend keys.
      `tilemap.paint` paints terrain and picks the tiles that connect, `tilemap.set` places
      tiles and `tilemap.get` reads them; layers draw in runs of one texture, and validation and
      exports check every layer against its tile set
- [x] Alternative tiles and turned cells, as Godot's: a tile's numbered alternatives with their
      own flips, transpose, color and collision, and cells that flip or transpose their tile
      (`slope:h`), drawing and colliding turned
- [x] Isometric and hexagonal tile shapes, Godot's other TileSet layouts, with their terrain
      peering bits: `tile_shape` (square, isometric, half-offset square, hexagon), the six
      `tile_layout`s and both offset axes, placed, neighbored and drawn down the screen as
      Godot's, 16 compass peering directions, terrain painting by each shape's sides and corners,
      `tilemap.convert` between cells and points, and Godot's shapes imported, with tiles'
      `texture_origin` lifting tall tiles on their cells
- [x] Animated tiles and tiles larger than one cell, as Godot's TileSetAtlasSource: frames
      laid out after the tile with columns and separation, per-frame durations, speed, random
      start times per cell, `size_in_atlas`, overlap checks, and Godot's animated and large tiles
      and Unity's `AnimatedTile`s imported
- [x] The rest of Godot's TileSet: custom data layers of seven types read with `tilemap.get`'s
      `cells` and `TileSet::custom_data`, and occlusion layers whose tiles' occluders cast
      shadows, turn with their cells and switch off with `occlusion_enabled`, both imported from
      Godot; tile navigation polygons came with navigation in milestone 7
- [x] 2D lights, occluders and normal-mapped sprites, as Godot's: `PointLight2D` (textured,
      with a height for normal maps), `DirectionalLight2D` and `CanvasModulate`, with light masks,
      z ranges and add, sub and mix blending; `LightOccluder2D` shadows with cull modes and PCF
      filters; `Sprite2D` normal and specular maps; and materials' `light_mode`. The material
      shader lights every lit draw, the CPU casts shadows into a shadow map, and `light.sample`
      returns how lit points are without a screenshot
- [x] Normal and specular maps on tile set atlases, as a Godot `CanvasTexture`'s, and lights'
      `range_layer_min`/`range_layer_max` over canvas layers, each canvas with its own
      `CanvasModulate`
- [x] 2D physics on Box2D v3, as Godot's: `StaticBody2D`, `AnimatableBody2D`, `RigidBody2D`
      and `CharacterBody2D` (`move_and_slide` with floors, walls, slopes, snapping, moving
      platforms and one-way platforms), `Area2D` with enter and exit events, rectangle, circle,
      capsule, segment and convex shapes, concave `CollisionPolygon2D`, collision layers and
      masks, pin, spring and groove joints, and `physics.raycast`, `physics.overlap` and
      `physics.contacts`. Results are the same on every platform
- [x] Tile collision, as Godot's TileSet physics layers: collision polygons on tiles (one-way
      too), physics layers with a collision layer, mask, friction and bounce, each
      `TileMapLayer` a static body rebuilt when it changes, and `physics.raycast` reporting the
      cell it hit
- [x] More of Godot's 2D physics: `Area2D` gravity and damping overrides, `StaticBody2D` constant velocities, and `CharacterBody2D`'s
      `floor_block_on_wall`, `floor_constant_speed`, `wall_min_slide_angle` and `safe_margin`
- [x] CPU particles, as Godot's `CPUParticles2D`: emission shapes, directions and spread,
      gravity, linear, radial and tangential acceleration, orbits, damping, angles and spin,
      scale, color ramps and hue variation, each a random range with a curve over the
      particle's life; sprite sheet animation, one-shots with a `finished` event, preprocess,
      explosiveness, local coordinates and draw orders. Seeded by entity ID, so the same scene
      makes the same particles on every platform, and `particles.eval` checks them without a
      screenshot
- [x] GPU particles, as Godot's `GPUParticles2D`: `.process_material.json` files with Godot's
      `ParticleProcessMaterial` properties, stepped as Godot's node and particles storage step
      them (emission cycles, `fixed_fps` with interpolation, preprocess, one-shots with a
      `finished` event) and run by a compute shader on Metal, Direct3D 12 and Vulkan, for many
      thousands of particles; the same steps make the same particles on every device and on
      the CPU, where `particles.eval` lists them. Godot's `GPUParticles2D` nodes and
      `ParticleProcessMaterial` resources import
- [x] Particle graphs in text (Godot's particle shaders, as `start` and `process` node graphs in
      process materials, run on the GPU and the CPU alike), turbulence, collision with
      `LightOccluder2D`s and tile occluders, sub-emitters (chains of up to 8 emitters stepping
      together) and trails (Godot's ribbons through copies that follow each particle's path).
      Attractors come with the 3D particles in milestone 7, as Godot has them only in 3D
      (`CPUParticles3D` is done)
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
- [x] Check boxes and sliders, as Godot's: `CheckBox` (radio buttons in a group) and
      `CheckButton`, `HSlider` and `VSlider` dragged, clicked, scrolled and stepped by keys and
      gamepads with `value_changed`, `drag_started` and `drag_ended`, and theme icons with
      Godot's default ones drawn by the engine (`builtin:` names)
- [x] Scrolling, as Godot's: `HScrollBar`, `VScrollBar` and `ScrollContainer` dragged, clicked
      and scrolled by the wheel, touch and focus, with `clip_contents`; `ui.layout` and
      `frame.summary` report scrolls and what a clip cuts off
- [x] Drop-down lists and menus, as Godot's: `OptionButton`, `MenuButton` and `PopupMenu`
      picked by clicks, held clicks, taps, the arrow keys and typing, menu bars that switch on
      hover, check and radio items and separators; `ui.popup` and `ui.select` drive them, and
      `ui.layout` and `frame.summary` report the lists open and their items
- [x] Tooltips and drag and drop, as Godot's: `tooltip_text` shown after a pause (and menu
      items' tooltips), controls dragged and dropped onto others with a preview through the
      `DragAndDrop` component or C++ drag forwarding, and text fields' selections dragged
      between them; `ui.drag` and `ui.tooltip` drive them, and `ui.layout` and
      `frame.summary` report them
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
- [x] `TextEdit`: text fields of several lines, as Godot's: typing and new lines, word
      wrapping, scrolling by wheel, finger and keys with the view following the caret and scroll
      bars drawn, Godot's undo history, the caret by character, word, row and page, selection by
      keys, clicks, double and triple clicks and drags, copying and pasting whole lines, tabs,
      typing over, IME composition, and the caret, selection and lines in view in `ui.layout`
- [x] Syntax highlighting, search and scroll bars that can be dragged in a `TextEdit`, as
      Godot's `CodeHighlighter`, `TextEdit::search` and `ScrollBar`: `search_text` and
      `highlight_all_occurrences` marked, `ui.search`, and grabbers in `ui.layout`
- [x] Line numbers and other gutters, as Godot's `CodeEdit`: breakpoints, bookmarks and
      executing lines that move with their lines, clicks in gutters, line length guidelines
      and a `TextEdit`'s own gutters of text or icons, with their rects in `ui.layout`
- [x] Audio, as Godot's AudioServer, on miniaudio's decoders: `.wav`, `.ogg` and `.mp3`
      sounds, `.audio.json` loops and randomizers, `AudioStreamPlayer` and
      `AudioStreamPlayer2D` with polyphony, pitch, pausing, distance and panning from an
      `AudioListener2D`, `.bus_layout.json` buses with volume, mute, solo, sends and 20 of
      Godot's effects (filters, EQs, delay, reverb, chorus, phaser, distortion, compressor,
      limiter, panner and stereo enhance), mixed by the fixed ticks so runs sound the same.
      `audio.state`, `audio.play`, `audio.stop`, `audio.set_bus` and `audio.capture` (levels in
      dB, or a `.wav` file) let agents hear a game without speakers
- [x] `Area2D` audio bus overrides, as Godot's: 2D players inside an area whose mask they
      match play on its bus. Godot's pitch shift, spectrum analyzer, capture and record
      effects (24 of its effects now), read and driven with `audio.spectrum` and
      `audio.record`; Godot's and Unity's pitch shifters import. (Reverb overrides are
      `Area3D`'s in Godot, and come with 3D audio.)
- [x] Interactive music, as Godot's: `.audio.json` playlists (in turn, with fades and
      shuffle), synchronized layers and interactive clips with Godot's transitions (on the
      beat or bar, fades, filler clips, auto advance and hold and return), steered by a
      player's `parameters` (`switch_to_clip`, `stream_<index>_volume_db`), with the clip
      playing in `audio.state`
- [x] More of Godot's audio: generated and polyphonic streams
- [x] More of Godot's audio: microphone input
- [x] `frame.capture`: the frame the window shows, drawn on the CPU by a software render
      device that shades as the GPU device does, so the pixels are the same on every machine;
      returns pixel colors, a PNG (an image block over MCP) or a file, for a region and at a
      scale
- [x] Replays: `replay.record` (or `slash-cli run --record`) writes a `.replay.json` file of a
      run (the start, the ticks stepped, input and commands, with hashes along the way);
      `replay.run` plays it back exactly in any session, stops at a tick, and reports the first
      tick a changed game goes another way and what differs at the end. 2D physics steps the
      same in every session that holds the same world
- [x] Time travel: `replay.seek` jumps a recorded run to any tick, by a number of ticks or to
      the first tick a condition holds, where every query command works, so a bug can be
      reproduced and inspected at the moment it happened. Forward goes on from where it is;
      back plays from the start
- [x] Debug overlay, drawn with the UI and text above: F3 in `slash-cli run` shows frame stats,
      the tick, what was drawn and the bodies under the mouse, with Pause, Step and Shapes
      buttons that run the same commands an agent would; collision shapes drawn as Godot's
      Visible Collision Shapes (`--debug-collisions`, `frame.capture`'s `collisions`), and
      `debug.overlay` for agents
- [x] A test command that steps a game headless and returns only the checks that failed:
      `test.run` (and `slash-cli test`) runs `.test.json` tests, each a scene and command calls
      with what their replies must hold, in sessions of their own that can't write files, and
      returns each failed check (a failed command, a reply that doesn't match, a warning or error
      logged) with a JSON Pointer into its file; a whole-project run adds `storyboard.check`, and
      games made by `slash-cli init` start with a test their CI runs
- [x] Watch conditions: `run.until` steps a game headless until a condition on component
      fields holds (say, the player below the floor) or a tick limit passes, and returns the
      tick and the matching entities
- [x] Frame summaries: `frame.summary` says what the window shows as data instead of an image:
      each sprite, tile map layer, particle emitter and control that shows, in drawing order,
      with its rect, its text and how much of it shows; what is covered, offscreen or clear;
      what can't draw; and, with `map`, a text picture of the frame
- [ ] Importing games made with other engines: `project.import` (and `slash-cli import`)
      converts another engine's project into this one in one call and reports what didn't
      convert and why. Godot first, then Unity; Unreal follows the 3D work in milestone 7
  - [x] Godot 4 projects: scenes (nodes to entities and components), `SpriteFrames`, textures, sounds and fonts, the input map and
        project settings. Each GDScript or C# script becomes a storyboard task to port it to
        C++, and a report keeps what the port needs: the values each node gave its exported
        variables, signal connections, groups, autoloads and the entity of every node path
  - [x] Godot scenes that instance other scenes stay instances, holding only their overrides
  - [x] Godot's `TileSet` resources, `TileMapLayer` nodes and the old `TileMap`'s layers,
        themes, `LabelSettings` resources and audio bus layouts, with the project's theme and
        buses
  - [x] Godot's tile physics layers, collision polygons, alternative tiles and flipped or
        transposed cells
  - [x] Godot's `AnimationPlayer` nodes and their animation libraries, with value, bezier and
        method tracks, through track targets
  - [x] Godot's music and sound settings: `AudioStreamRandomizer`, `AudioStreamPlaylist`,
        `AudioStreamSynchronized` and `AudioStreamInteractive` resources, and the loops and
        beats sounds' `.import` settings give them (a WAV file's own `smpl` loop included)
  - [x] Unity projects: 2D scenes, prefabs (nested prefabs and variants staying instances),
        sprites, cameras, 2D physics, sounds, both input systems and project settings, measured
        in pixels. Each C# script becomes a storyboard task, with the values each object gave
        it in the report
  - [x] Unity's rectangular tile maps: a tile set from the sprites cells show, with polygons
        from sprites' physics shapes, alternatives for cell colors and turns from cell matrices
  - [x] Unity's Animator controllers and animation clips: an animation library per
        controller, an animation per state, curves keyed for SlashEngine's tracks, and the state
        machine as a storyboard task
  - [x] Unity's audio mixers and 2D joints: one bus layout from every mixer's groups, with
        their effects and Duck Volume sends as sidechains, and hinge, spring and slider joints as
        Godot's pin, spring and groove joints
  - [x] Unity's uGUI and TextMesh Pro: canvases and their scalers, rects as anchors and
        offsets, images, text, buttons, toggles, sliders, scroll bars, input fields, scroll
        views, masks and layout groups as Godot's controls and containers, with UI events'
        calls in their scripts' tasks
  - [x] Unity's 2D lights: the global light as a `CanvasModulate`, spot, freeform and sprite
        lights as `PointLight2D`s through textures of their shapes and falloff, sorting layers as
        z ranges, shadow casters as occluders and sprites' normal maps
  - [x] Unity's isometric and hexagonal tile maps: each Grid's cell layout and swizzle as a tile
        set's shape, layout and offset axis, layers moved so their cells stay where Unity draws
        them, and tile anchors and sprite pivots as `texture_origin`
  - [x] Godot games exported as a `.pck` file or an executable unpacked back into their
        projects and imported: binary scenes and resources as text, script tokens as source,
        `project.binary`, textures, sounds and fonts, for games the user made or has the rights
        to. Encrypted packs are refused, never decrypted
  - [x] Unity builds unpacked back into a project, the same way: Windows, macOS, Linux, web,
        Android and iOS builds, Mono or IL2CPP, rebuilt by AssetRipper (GPL, so run as a program
        of its own and installed on first use), with Unity's package scripts given their GUIDs
        back and sprites saved as assets kept on their atlases. Encrypted bundles are refused,
        never decrypted

Exit: an agent builds `samples/pong` end to end and exports it to all six platforms.

### Milestone 6: Hot reload, AI and profiling (Done)

- [x] Gameplay as a C++ shared library that reloads on rebuild and keeps world state: a game's
      code in `project.json`'s `gameplay.sources`, registered with `SLASH_GAME_MODULE`, linked
      into exports for every platform and loaded by the tools on macOS and Linux.
      `gameplay.build` and `slash-cli build` compile it and return compiler errors with file and
      line; the new code swaps into the running world, keeping each entity and the fields the
      code still has and reporting what it dropped. Libraries built against other engine headers
      or with another compiler are refused, and scenes are checked by the components the sources
      declare when the code isn't built
- [x] SDK packages, so a game adds a closed vendor SDK (an ad network, DLSS, Steamworks, a store
      or analytics service) without touching the engine, as Godot's Android and iOS plugins and
      Unity's External Dependency Manager do. Nothing from the SDKs enters this repository
  - [x] An `sdk.json` file per SDK (`$cli schema sdk`): for each platform the static or dynamic
        libraries, Apple frameworks and xcframeworks, Java `.jar` files, the files shipped
        beside the game, and the Info.plist, entitlement and Android manifest entries it needs.
        Binaries come from the vendor's URL pinned by SHA-256, or from a path the developer
        gives; API keys are named environment variables, as signing secrets are
  - [x] Exports fetch, check and link each preset's SDKs on all six platforms, Apple frameworks
        without CocoaPods, and list the SDKs each build includes, shipping their license files
        with the game so they stay visible
  - [x] Android `.aar` files and Maven packages, their dependencies resolved without Gradle
        (`sdk lock` pins them in `sdk.lock.json`) and their manifests, resources and native
        libraries merged into the APK
  - [x] Engine interfaces that SDK glue code implements for ads, analytics and purchases
        (`engine/services`: `AdsBackend`, `AnalyticsBackend` and `PurchasesBackend`), with a
        test backend sessions run on, so headless runs and CI need no vendor binary
  - [x] Glue code in SDK packages that registers backends when the game starts (`sources`,
        `register`), and bridges for backends written in Java or Objective-C (`java`,
        `backends`), with analytics flushed when the game goes to the background
  - [x] `render::Upscaler`, the interface glue code adds a vendor's upscaler through, with the
        render scale for the 2D world, bilinear and FSR 1 built in (`rendering.scaling`)
  - [x] Achievements, leaderboards and sign-in, behind an interface of their own
        (`GamesBackend`, with Java and Objective-C bridges and sdk.json's `backends.games`),
        on the test backend in sessions
  - [x] `sdk.add` (from a folder, a git repository or a URL) and `sdk.check` (missing files,
        keys and platforms), with an example SDK package built in CI from a stand-in library
- [x] Imported scripts as C++ skeletons: `project.import` writes a component for each script's
      exported variables, events for its signals and a system with its functions for the agent
      to fill in, beside the storyboard task (GDScript, Godot's C# and Unity's C#)
- [x] `build.check`: compiles only the changed gameplay files and returns errors as data (file,
      line, message and the compiler's suggested fix), with no build log to read
- [x] Job system, so systems run in parallel before the 3D work in milestone 7 needs it
- [x] Profiling
  - [x] Tracy (the `tracy` build preset) and per-system timings in `perf.stats`, which reports
        frame pacing since milestone 5, in every session
  - [x] A per-system memory report: `perf.stats`'s `memory` gives the process's memory and what
        the world, components, assets, physics, audio, particles and glyph atlases hold, and a
        running game's GPU textures
- [x] `ai::Provider` with OpenAI-compatible, Anthropic and Gemini adapters
  - [x] The interface and the three adapters (tools, structured output, images, streaming and
        the providers' reasoning kept between turns), roles in `providers.json`, engine commands
        as a model's tools, and `ai.providers` and `ai.complete` with the `network` capability
  - [x] TLS for hosted providers' `https://` APIs, checked against the system's certificates on
        every platform, and proxies as curl reads them
  - [x] Games calling models: `ai::ask` from gameplay code with replies as world events, the
        same replies at the same ticks in headless runs, and exports that ship `providers.json`
  - [x] Web exports calling models through the browser's `fetch`, streaming, with retries on
        the browser's timers
- [x] `ReplayProvider` so tests that use a model run offline
- [x] A benchmark of standard agent tasks that counts calls and tokens
  - [x] Scripted tasks played through MCP with checks, an estimate of tokens written and read,
        and a baseline `ctest` holds every pull request to (`slash-cli bench`)
  - [x] Live runs on a schedule: a model through `ai::Provider` does each task from its prompt
        (`slash-cli bench --live`, weekly in `.github/workflows/bench.yml` once a model is set)
- [x] Storyboard review: a model through `ai::Provider` reads new text and assets against the
      storyboard and reports likely contradictions as advisory findings (`storyboard.review`)
- [x] `project.map`: a generated map of the project's systems, components, scenes and source
      files, linked to the storyboard, so agents in a large codebase find code without guessing
- [x] `api.search`: finds engine and project C++ functions and types by name or purpose and
      returns their signatures, docs and the verified examples that use them
- [x] Playtests and tuning sweeps: `playtest.run` runs many headless copies of a game at once,
      driven by simple scripted bots and each with different values (jump height, enemy speed),
      and returns a summary of the results per value
- [x] Asset library and generator, so agents reuse generated assets instead of making them again
      ([design](design.md#asset-library-and-generation))
  - [x] A library shared by every project on the machine: each asset stored once by content
        hash, with the request that made it, its generator, model, seed, license, tags and a short
        text description agents can read instead of the file
  - [x] `asset.search` by text, tags and type; agents search before they generate
  - [x] `asset.generate` through generator adapters for hosted and local image and audio
        models, behind the same interface style as `ai::Provider`; an exact repeat of an earlier
        request returns the stored asset with no model call
  - [x] Iteration: `asset.refine` makes a new version linked to its parent, so every asset keeps
        its history and can be rolled back
  - [x] Ready-to-use forms: imported and compressed for each platform once, then cached
  - [x] Feedback: agents record results with `asset.feedback`, by hand or automatically from
        the storyboard's palette and budget checks; feedback ranks search results, picks between
        candidates and is passed back into later prompts, with the storyboard's style
  - [x] Automatic feedback from the headless test command and frame summaries: `asset.check`
        (run by `test.run`) looks at every scene that draws a library file and records what it
        finds, with `frame.summary`'s `contrast` for what is hard to see
  - [x] Team sharing: the library syncs with a store the team hosts, such as a shared folder or
        a library server that `slash-cli` runs, so a teammate's request finds assets someone
        else already made
  - [x] A test generator, so agents and CI can use the library headless with no model or key
- [x] Soundtracks: agents plan, write, arrange and check a game's music, played by the
      interactive music already built in milestone 5 ([design](design.md#soundtracks)). The
      parts that need no model (scores, analysis, arranging and checks) can come first
  - [x] Music in the storyboard: themes for characters, places and factions (key, tempo,
        instruments, mood), a loudness target, and a cue sheet saying which cue plays in each
        scene, story beat and game state; `soundtrack.plan` drafts the cue sheet from the
        storyboard and the project's scenes
  - [x] Scores as text: `.score.json` tracks of notes in beats, with tempo, meter, key and named
        sections, plus MIDI import and export, so an agent can change one bar or one instrument
        exactly instead of making the whole piece again
  - [x] `soundtrack.render`: scores played through a SoundFont synth (TinySoundFont) into one
        `.ogg` stem per track, with `bpm`, `beat_count` and `bar_beats` filled in
  - [x] Generated and imported music: music models through the asset library's generator
        adapters, and composers' tracks and stems imported as library assets, each with its
        generator or author and license recorded
  - [x] `soundtrack.arrange`: turns a cue sheet into `.audio.json` files the runtime already
        plays: cues become interactive clips, intensity levels become synchronized layers,
        and transitions land on the bar, with filler clips as stingers
  - [x] `audio.analyze`: tempo and beat grid, key, loudness (LUFS) and true peak, clipping,
        silence, frequency balance and how cleanly a loop joins, so agents judge music without
        listening
  - [x] `soundtrack.check`: missing cues, beat counts that don't match a file's length, loops
        that click, transitions off the bar, keys that clash across a transition and
        loudness off target; run by the headless test command and CI
  - [x] A music timeline: `run.until` and `playtest.run` report which clip played when and
        every transition, so agents can check the music follows the game

### Milestone 7: 3D feature set (In progress)

- [x] Clustered forward+ renderer with PBR materials, plus a low-end path for web and mobile
  - [x] Meshes, a depth buffer and `StandardMaterial3D`'s PBR inputs lit by up to 64
        directional, omni and spot lights in every render device, as Godot's Compatibility
        renderer (the low-end path), with the software device's CPU twin
  - [x] Forward+'s clustered lights, as Godot's: 8 directional and 512 omni and spot lights
        sorted into screen tiles and depth slices on the CPU, read from storage buffers on
        Vulkan, Metal, Direct3D 12 and WebGPU and by the software device's twin;
        `rendering.rendering_method` picks `forward_plus` or `gl_compatibility`, which the
        OpenGL drivers always draw
  - [x] 3D nodes in scenes, as Godot's: `Transform3D` trees, `MeshInstance3D` with Godot's
        eight primitive meshes, `StandardMaterial3D`, `Camera3D`, `DirectionalLight3D`,
        `OmniLight3D`, `SpotLight3D` and `WorldEnvironment`, drawn under the canvas by every
        game and by `frame.capture`
- [x] Cascaded, omni and spot shadows
- [x] Post-processing: tonemap, bloom, SSAO, SSR, depth of field, auto exposure, color grading
  - [x] Godot's Compatibility renderer's tonemappers (linear, Reinhard, filmic, ACES, AgX),
        exposure, glow, brightness, contrast, saturation and 1D or 3D color correction LUTs in
        every render device, set by `WorldEnvironment`
  - [x] Forward+'s HDR world, as Godot 4.6's: sky and meshes drawn linear in half floats, glow
        through up to seven levels with Godot's weights, strength, normalization and five blend
        modes, Godot 4.6's tonemappers and whites (AgX's own white and contrast), on Vulkan,
        Metal, Direct3D 12, WebGPU and the software device
  - [x] Auto exposure and the exposure multiplier, as Godot 4.6's `CameraAttributesPractical`
        on a camera or `WorldEnvironment`: Forward+ frames measured in blocks of 8 down to one
        brightness, adapting over game time, on every Forward+ device
  - [x] Depth of field, as Godot 4.6's: `CameraAttributesPractical`'s far and near blur by
        depth, with the box, hexagon and circle bokeh shapes at every quality, on every Forward+
        device
  - [x] SSAO, as Godot 4.6's Forward+: a normal prepass and Intel's ASSAO passes at every
        quality (deinterleaved depth, adaptive samples at ultra, edge-aware blur), set by
        `WorldEnvironment` and `rendering.environment.ssao`, on every Forward+ device
  - [x] SSR, as Godot 4.6's Forward+: a Hi-Z trace reading the frame before, blurred levels
        by roughness and a resolve at half size, set by `WorldEnvironment` and
        `rendering.environment.screen_space_reflection`, on every Forward+ device
- [x] Baked lightmaps and probe-based global illumination
  - [x] Reflection probes, as Godot 4.6's Forward+ `ReflectionProbe`: cubes captured into one
        atlas, box projection, blending where probes overlap and their ambient modes, on every
        Forward+ device, imported from Godot and Unity scenes
  - [x] Baked lightmaps, as Godot 4.6's `LightmapGI`: meshes unwrapped with xatlas, direct
        light, soft shadows, bounces and the environment traced on the CPU as Godot's
        `LightmapperRD` traces them, denoised, and drawn on every device; `lightmap.bake`,
        imported from Godot scenes
  - [x] Light probes for meshes that move, as Godot 4.6's `LightmapGI`: placed on an octree
        over the static meshes and at `LightmapProbe`s, traced with the lightmap, joined into
        Delaunay tetrahedra with a BSP tree, and drawn on every device as moving meshes'
        ambient light
  - [x] VoxelGI, as Godot 4.6's: static meshes voxelized into an octree by `voxel_gi.bake`,
        lights injected into its cells on the CPU as they change (shadows, two bounces,
        propagation up the octree), and cones traced for ambient light and reflections on
        every Forward+ device
  - [x] Moving meshes lighting VoxelGIs, as Godot's dynamic objects: plotted into the cells'
        light from six sides each frame they move, lit by the same lights
  - [x] VoxelGIs imported from Godot scenes: their settings, camera attributes and baked cells
        (`.tscn`, `.tres` or `.res`) as `.voxelgi.json` files, and the cones setting
- [ ] Render scale and upscalers, as Godot's `scaling_3d` setting: bilinear, FSR 1 and FSR 2/3
      (AMD's FidelityFX code, MIT) on every backend and MetalFX on Apple, set in `project.json`
      and changeable while a game runs; a `render::Upscaler` interface lets a game add DLSS, XeSS
      or FSR 4 with its vendor's SDK, outside the repo, since those ship only as closed binaries
  - [x] Bilinear and FSR 1 on every backend and the software device, the `render::Upscaler`
        interface and `display.set`'s `scaling`, scaling the 2D world under the UI
  - [x] Temporal antialiasing, as Godot's `use_taa`: a jittered camera, motion vectors from
        the Forward+ prepass (transforms, skins and the camera) and Godot's TAA resolve, on
        every Forward+ device
  - [x] FSR 2.2, as Godot's `fsr2`: the 3D world drawn at the render size with a jittered
        camera and rebuilt at the window's size by AMD's passes, on every Forward+ device, with
        Godot's `scaling_3d` settings imported
  - [x] The sky's motion vectors worked out from the depth in FSR 2, and the sky jittered with
        TAA and FSR 2, as Godot's
  - [x] FSR 3.1's upscaler, the scaling mode `fsr3`: AMD's passes over the depth, motion vectors
        and jittered camera that FSR 2 uses, on every Forward+ device
  - [x] MetalFX on Apple's GPUs, as Godot's `metalfx_spatial` and `metalfx_temporal`: Apple's
        spatial scaler over the world, and its temporal one over the depth, motion vectors and
        jittered camera that FSR 2 uses, falling back as Godot's where Metal has neither
  - [x] Temporal upscalers through `render::Upscaler`, on the depth, motion vectors and jittered
        camera that FSR 2 uses: a game's `render::TemporalUpscaler` runs in FSR 2's place with
        the native API's device, command buffer and textures
  - [ ] FSR 3 frame generation, once the render device controls presenting frames
- [ ] Hardware ray tracing: acceleration structures and ray queries in `render::RenderDevice`,
      native on Vulkan, Direct3D 12 and Metal beside SDL3's GPU API (which has none), with a CPU
      twin on the software device; ray-traced shadows, reflections, ambient occlusion and GI,
      falling back to the raster and probe paths where the hardware has none (the web, older
      phones)
  - [x] Ray-traced shadows on the software device: `rendering.ray_tracing`, rays from Forward+'s
        prepass toward the first 8 shadowed lights, soft by the lights' sizes, shadow maps where
        a device can't trace rays (`RenderDevice::ray_tracing`)
  - [x] Acceleration structures and ray queries on Vulkan: the GPU device asks for
        `VK_KHR_acceleration_structure` and `VK_KHR_ray_query` where the GPU has them and
        records the structures and a ray query compute shader into SDL's command buffer
  - [ ] Direct3D 12 and Metal
  - [x] Ray-traced reflections on the software device: `rendering.ray_tracing.reflections`
        traces the environment's SSR by rays reflected about GGX's microfacet normals, lit as
        the path tracer's bounces, with SSR where a device can't trace them
  - [x] Ray-traced reflections on Vulkan: a ray query compute shader shades what rays meet from
        the draws' materials, the copies of their vertices the structures are built from, and
        their textures, lit by the frame's lights through traced shadows and the sky's radiance
  - [x] Ray-traced ambient occlusion: `rendering.ray_tracing.ambient_occlusion` traces the
        environment's SSAO by rays over each pixel's half of the sphere, on the software device
        and Vulkan, with SSAO where a device can't trace rays
  - [x] Ray-traced global illumination on the software device:
        `rendering.ray_tracing.global_illumination` traces each pixel's ambient light by
        cosine-weighted rays lit as the path tracer's bounces, smoothed across its surface
  - [x] Ray-traced global illumination on Vulkan: the reflections' ray query shader traces each
        pixel's light rays, then smooths them across its surface, into the screen texture the
        meshes take their ambient light from
- [x] Path tracing: a GPU path tracer that bakes the lightmaps above, as Godot's LightmapGI
      does, and renders progressive reference stills that `frame.capture` can return, so agents
      compare the real-time frame with ground truth
  - [x] Progressive reference stills on the CPU: `frame.capture`'s `path_trace` traces paths
        through a bounding volume hierarchy of the frame's triangles, shaded as the live frame
        shades them, and adds samples up over captures
  - [x] The same tracer on the GPU, through the ray queries above: Vulkan traces each frame's
        paths with the software device's code and random numbers, and `display.set`'s
        `path_trace` path traces a running game's window
  - [x] Lightmaps baked on the GPU: `lightmap.bake`'s `device` `"gpu"` traces the direct
        light, bounces and light probes with Vulkan's ray queries, as Godot's `LightmapperRD`
        does, ported from the CPU lightmapper with its random numbers
- [x] Procedural, physical and panorama skies and fog, and the sky's light on meshes (ambient
      light and reflections), as Godot's Compatibility renderer
- [x] Volumetric fog
  - [x] Godot 4.6's Forward+ volumetric fog: froxels lit by every light through its shadows, the
        ambient light and the sky, reprojected from frame to frame, blurred and gathered, over
        the meshes and the sky, on every Forward+ device and its CPU twin, imported from Godot
  - [x] `FogVolume` and `FogMaterial`: box, ellipsoid, cone, cylinder and world volumes added in
        Godot's fixed point, with density, height falloff, edge fade, albedo and emission,
        imported from Godot
  - [x] VoxelGI's light in the fog (`gi_inject`), as Godot's density pass takes it
- [x] Clustered decals, as Godot 4.6's Forward+ `Decal`: albedo, normal, ORM and emission
      textures in one atlas, fades, cull masks and the project's decal filter, imported from Godot
- [x] 3D particles, as Godot 4.6's
  - [x] `CPUParticles3D`: Godot's emission shapes (ball, box, points, ring with its cone), cone
        spread with flatness, forces, damping, curves, color ramps and the align, rotate and
        disable-z flags, drawn as the entity's mesh at each particle with `StandardMaterial3D`'s
        billboard modes and particle frames, sorted by view depth; the same particles on every
        platform and thread count, `particles.eval` in 3D, and Godot's `CPUParticles3D` nodes
        imported
  - [x] `GPUParticles3D` with process materials, attractors and collision shapes, and Godot's
        imported
    - [x] `GPUParticles3D` stepped as Godot's (emission cycles, `fixed_fps`, preprocess,
          one-shots), process materials read with Godot's 3D fields (ring emission, flatness,
          directional velocity, scale over velocity, per-axis curves), up to four draw passes
          with draw orders, transform alignment and visibility boxes, and `particles.eval`
    - [x] The steps, the view depth sort and the draw passes' meshes in compute shaders on
          Metal, Direct3D 12, Vulkan and WebGPU, with the CPU twin on the software and OpenGL
          devices
    - [x] Attractors (sphere, box and vector field), paired with emitters as Godot's scene
          cull pairs them
    - [x] Collision shapes (sphere, box, SDF, height field)
      - [x] Spheres and boxes, with rigid and hide-on-contact collision and
            `collision_base_size`
      - [x] Signed distance fields baked by `particles.bake_sdf` and height fields drawn from
            above, following the camera
    - [x] Godot's `GPUParticles3D` nodes and 3D process materials imported, with their
          attractors and colliders
- [x] Generated LODs with meshoptimizer and occlusion culling
  - [x] Generated LODs, as Godot 4.6's importer makes them with meshoptimizer and its renderers
        pick them by distance, `lod_bias` and the project's threshold, imported from Godot
  - [x] Occlusion culling, as Godot's `OccluderInstance3D` and raycast culler: quad, box,
        sphere, polygon and baked shapes, `occluder.bake` and the project's settings, imported
        from Godot
- [x] 3D physics on Jolt: rigid bodies, joints, character controller, vehicles
  - [x] Static, animatable, rigid and character bodies, areas with gravity and damping
        overrides, every Godot shape, `move_and_slide` and the queries, as Godot 4.4's Jolt
        module
  - [x] Pin, hinge, slider, cone twist and 6DOF joints, and `VehicleBody3D` with
        `VehicleWheel3D`, as Godot 4.4's
  - [x] Ragdolls: `PhysicalBone3D` bodies and joints that follow a model's bones or move them,
        as Godot 4.6's `PhysicalBoneSimulator3D`
- [x] Skinned meshes, animation blending, state machines, root motion, two-bone and FABRIK IK
  - [x] glTF skins skinning meshes, model animations played by `AnimationPlayer` (alone or
        `from` a `.anim.json` animation) and `Skeleton3D` bone poses, as Godot's importer,
        skeleton and `AnimationMixer`; `skeleton.get`
  - [x] FBX skins and animation stacks, as Godot's FBX importer
  - [x] Blend trees, one-shots, transitions and blend spaces with root motion (`AnimationTree`
        and `.animtree.json`), as Godot's `AnimationTree` and `AnimationMixer`;
        `animation.tree` and `animation.set_parameters`
  - [x] State machines with travel, conditions and grouped and nested machines, as Godot's
        `AnimationNodeStateMachine`
  - [x] Advance expressions, as Godot's `Expression`, over a tree's variables and triggers
  - [x] Two-bone, FABRIK, CCD and Jacobian IK with joint limits, as Godot 4.6's `TwoBoneIK3D`,
        `FABRIK3D`, `CCDIK3D` and `JacobianIK3D`
  - [x] Bone attachments, as Godot's `BoneAttachment3D`: entities that follow a bone after the
        animations and IK, carrying what hangs under them
- [x] Navigation in 2D and 3D, as Godot 4.6's `NavigationServer2D` and `NavigationServer3D`
  - [x] Navigation meshes and polygons (`.navmesh.json`, `.navpoly.json`), regions, links,
        layers, edge connections and agents' paths, as Godot's own A* over polygons;
        `navigation.path`, `navigation.closest`, `navigation.random`, `navigation.map` and
        `navigation.agent`
  - [x] Baking navigation meshes from a scene's geometry on Recast in 3D, and polygons on
        Clipper2 and polypartition in 2D, as Godot does (`navigation.bake`)
  - [x] Avoidance on RVO2 for agents and obstacles, and obstacles cut out of bakes, as Godot's
  - [x] Navigation layers on tile sets: each cell a region of its tile's navigation polygons,
        and tiles' outlines and collision polygons in 2D bakes, as Godot's `TileMapLayer`;
        imported from Godot
- [x] 3D audio, as Godot's `AudioStreamPlayer3D` and `AudioListener3D`
  - [x] Attenuation models, panning, the high shelf that muffles distant sounds, emission
        angles, Doppler and `Area3D` bus and reverb overrides, as Godot 4.6's, in stereo
  - [x] Up to 7.1 speakers, as Godot's speaker modes: a buffer and effects per speaker pair,
        Spcap panning for 3D sounds, mix targets, and `audio.state` and `audio.capture` per
        speaker
- [x] Level geometry and terrain
  - [x] CSG shapes on Manifold, as Godot 4.6's: boxes, spheres, cylinders and cones, tori,
        polygons pushed or spun, meshes and combiners; drawn, colliding and baked into
        navigation meshes, with `csg.get`
  - [x] `Path2D`, `Path3D`, `PathFollow2D`, `PathFollow3D` and `CSGPolygon3D`'s path mode, as
        Godot's: curves baked with up vectors and tilt, followers placed each tick, and
        `path.get`
  - [x] `GridMap` and `MeshLibrary` for grid-based tile placement, as Godot 4.6's: items of a
        `.meshlib.json` library in cells written as rows of legend keys, turned Godot's 24
        ways, drawn and colliding by octant, navigation regions per cell, with `gridmap.set`,
        `gridmap.get` and the cell a ray hits
  - [x] Heightmap terrain with splat materials and LOD, as the HTerrain plugin for Godot:
        `HTerrain` heights from 16-bit `.height.png` files, drawn in chunks by its quad tree
        with seams stitched, four textures by splat map, a color map with holes, colliding as a
        height field and walked by navigation bakes, with `terrain.generate`, `terrain.brush`
        and `terrain.get`
- [x] glTF 2.0 models (`.gltf` and `.glb`: meshes, materials and textures) on `MeshInstance3D`,
      and JPEG textures, as Godot's importer reads them
- [x] OBJ models (`.obj` with their `.mtl` libraries) on `MeshInstance3D`, as Godot's OBJ importer
      reads them
- [x] FBX models through ufbx, as Godot 4.3's importer reads them
- [x] Blender `.blend` files the way Godot does it: the engine runs the artist's own Blender in
      the background to convert them to glTF, so no Blender code is linked or shipped
- [x] 3D scenes in `project.import` from Godot and Unity projects, and Unreal Engine projects
      imported through the user's own Unreal Editor: its project files are a closed binary
      format, so the editor exports the levels and assets and the importer converts what it
      writes. GameMaker and RPG Maker projects come after
  - [x] Godot's 3D nodes: transforms, primitive meshes and materials, instanced and mesh model
        files, cameras, lights, `WorldEnvironment` and skies, 3D bodies and collision shapes, 3D
        sound and the 3D project settings
  - [x] Godot's CSG shapes, paths, `GridMap` and mesh libraries (text and binary), joints,
        vehicles and navigation nodes, meshes and polygons
  - [x] Godot's `AnimationTree`: blend trees, blend spaces, transitions, state machines and
        their parameters and advance expressions, on a model's own animations or a player's
        libraries
  - [x] Godot's 3D animation tracks, as node tracks on a model's bones and `Transform3D` tracks
        on a scene's 3D nodes
  - [x] Godot's skeletons: a model's bone poses, IK modifiers, ragdolls and bone attachments
  - [x] Godot's HTerrain data: heights, splat and color maps, texture sets and shader settings
  - [x] Unity's 3D scenes: transforms mirrored into Godot's axes, built-in meshes, model
        instances, Standard and URP materials, cameras, lights, 3D bodies and colliders,
        character controllers, 3D sound, LOD groups, skyboxes, ambient light and fog
  - [x] Unity's meshes picked out of models, as glTF files of their own, and mesh colliders of
        models
  - [x] Unity's Animator state machines as `AnimationTree` state machines: sub-state
        machines, Any State, entry and exit, muted and solo transitions, exit times and
        durations, with conditions as advance expressions and parameters as the tree's
        variables and triggers
  - [x] Unity's blend trees as blend spaces: 1D and 2D, nested, their positions set from the
        controller's parameters
  - [x] Unity's terrains as `HTerrain`s: heights, alpha maps, layers, holes and the collider
  - [x] Unity's 3D animation: Animators on 3D objects and on models' instances, clips moving
        `Transform3D` and the models' nodes, and model clips played from the converted models
  - [x] Unity's humanoid animation, retargeted from one model's Avatar to another's, and root
        motion taken out of the pose by the `AnimationTree`
  - [x] Unity's skinned meshes: nodes posed in a scene as the model's `Skeleton3D`, objects on
        nodes as `BoneAttachment3D`s, and models unpacked into objects folded back into one
  - [x] Unity's particle systems in 3D scenes as `CPUParticles3D`: shapes, emission and bursts,
        start values, gravity and force, color, size and rotation over each life, texture
        sheets, billboards and mesh particles, with Unity's default particle material
  - [x] Unreal Engine levels and assets through the user's own Unreal Editor: a Python script
        the editor runs headless exports each level's actors and components, Enhanced Input,
        static meshes as glTF and sounds as WAV; the importer converts meshes with their
        collision, lights in Unreal's units, the sky and fog, cameras and spring arms, bodies,
        shapes, volumes, sounds, both input systems and the settings, and makes a task per
        Blueprint and C++ class
  - [x] Unreal's materials and their overrides (a copy of the mesh drawn with them) and skeletal
        meshes playing their animation sequences
  - [x] Unreal's C++ classes as skeletons, as Unity's scripts are: properties as a component the
        level's actors fill in, delegates as events and functions as stubs the system calls
  - [x] Unreal's landscapes as HTerrains, their heights exactly as Unreal keeps them
  - [x] Unreal's Animation Blueprints' sequences as an `AnimationTree` state machine for their
        task to join
  - [x] Unreal's Niagara particles as `CPUParticles3D` and `GPUParticles3D`, read from the text
        Unreal writes of a system
- [x] 3D in the asset library and generator ([design](design.md#3d-assets))
  - [x] Parametric models: agents write a `.model.json` recipe of shapes, CSG on Manifold,
        extrusions, lathes, bevels and arrays, and the engine builds the mesh; the library keeps
        the recipe, so refining means changing a number
  - [x] Concept image first: generate and review a cheap image, then make the 3D model from it,
        with the image kept as the model's parent: `asset.generate`'s `image` (Stability AI's
        image to 3D, or the test generator's reliefs), and `review` makes it again with what was
        wrong until it is accepted
  - [x] Game-ready cleanup for every generated or imported mesh: real-world scale, pivot and up
        axis, a triangle budget with meshoptimizer, UVs with xatlas, PBR textures with baked-in
        lighting removed, LODs and collision hulls: `model.cleanup` turns, delights, sizes,
        pivots, simplifies to the storyboard's budget, makes LODs, unwraps, makes collision
        (trimesh, convex hull or V-HACD convex decomposition, as Godot) and writes a `.glb` file,
        and models collide by Godot's import hints
  - [x] Automatic review: headless turntable renders, geometry checks (budget, scale, holes,
        texel density) and a vision-capable model comparing the views with the request, all
        recorded as feedback (`asset.review`), and a rejected model made again within a call
        budget (`asset.generate`'s `review`)
  - [x] Text descriptions of 3D assets: size, pivot, parts, materials, triangle count and
        attachment points, so agents place models without loading images
  - [x] Named parts and attachment points, so wheels turn, doors open and parts from different
        models combine
  - [x] Auto-rigging through a generator adapter, retargeted to one standard humanoid skeleton
        so every library animation works on every character
    - [x] Retargeting rigged humanoids to Godot's `SkeletonProfileHumanoid`: `model.retarget`
          guesses the bone map as Godot's `BoneMapper`, renames the bones and turns their rests
          as Godot's importer (Overwrite Axis, optionally fixing an A-pose's silhouette), and
          animations play across models in proportion to their heights
    - [x] Rigging an unrigged mesh through a generator adapter: `asset.refine`'s `rig` with
          Meshy's rigging (`meshy-rig`) or the test generator's placeholder rig, retargeted
          before the library keeps it
  - [x] The storyboard's style section (palette, reference images, triangle and texel
        budgets), refined from accepted assets and sent with every request so a game's assets
        match
  - [x] Tileable PBR materials from the 2D generator, shared through the material graph instead
        of a unique texture per model: `asset.generate`'s `material` type makes the image tile,
        works out normal and roughness maps and writes a `.standard_material.json` file that
        recipes name or `extends`
  - [x] Imported assets: files from Blender and other tools, asset stores or a team's own
        artists go into the library like generated ones, with the same cleanup, text
        description, review, versions and a license the importer records; editing the source
        file makes a new version
  - [x] A Blender bridge: agents build assets by running Blender headless with Python scripts
        as a generator; the script is saved with the result, so the asset can be rebuilt or
        tweaked later
  - [x] Complex assets: an asset made of other library assets (a building of walls, doors and
        props, a character with outfits) that references its parts by hash, so shared parts
        are stored once and updating a part offers new versions of everything that uses it
  - [x] Variants: one asset with named options (clean or damaged, seasons, team colors) and
        quality tiers per export preset (phone, desktop), all built from the same source
  - [x] Checks for complex assets: scale, material slot names, rigs, animation clips and every
        part present, run before an asset is saved and again in CI
  - [x] Scene assembly: agents place library assets with constraints (on the ground, not
        overlapping, along a path) instead of generating a scene as one mesh

Exit: an agent builds a small 3D sample end to end.

### Milestone 8: Systems and platform services (Planned)

- [ ] Networking: ENet transport, replication driven by reflection, WebSocket for web, following
      Godot's high-level multiplayer ([reference](reference/networking.md))
  - [x] Peers, as Godot's `MultiplayerPeer`: the ENet transport (`ENetMultiplayerPeer`) and an
        offline peer
  - [x] A world's multiplayer, as Godot's `SceneMultiplayer`: peers announced through the
        server, relayed packets, authentication and raw packets
  - [x] Remote procedure calls on entities, with Godot's modes (authority or any peer, call
        local, transfer mode and channel)
  - [x] Replication driven by reflection: spawners and synchronizers of component fields, as
        Godot's `MultiplayerSpawner` and `MultiplayerSynchronizer`
  - [x] WebSocket for the web, as Godot's `WebSocketMultiplayerPeer`: servers and clients
        natively, clients on the web through the browser's WebSocket
  - [ ] `wss://` natively, as Godot's `StreamPeerTLS`
  - [x] Commands that host, join and inspect a game's network, and examples that play a game on
        several sessions at once
- [ ] XR through OpenXR
- [x] Localization, following Godot ([design](design.md#languages))
  - [x] Translation tables as CSV or gettext `.po` files, with plural forms, placeholders and a
        fallback chain (`es-MX`, then `es`, then the source language)
  - [x] Any language as the source: a game can be written in Japanese and translated to English
  - [x] A language setting: the player's OS language by default, changeable in a game's options
        menu while it runs, with per-language fonts and right-to-left layout
  - [x] `l10n.extract`: every player-facing string in scenes and C++ (`tr("...")`) collected
        into the tables, so no text is missed
  - [x] `l10n.translate`: draft translations through `ai::Provider`, using the storyboard's
        glossary and characters so names and tone stay consistent, marked for review
  - [x] `l10n.check`: missing or stale keys, placeholders that don't match, text that overflows
        its UI box in a given font, and glyphs a font lacks; run by the headless test command
        and CI
  - [x] Pseudo-localization: a fake language with longer, accented text that shows untranslated
        or clipped strings before any real translation exists
- [x] Video playback with dav1d (AV1): `VideoStreamPlayer` in controls, `video:<id>` textures
      on sprites and materials, and the engine's own palette AV1 encoder for tests
  - [ ] Sound in videos (Opus in WebM or Matroska)
  - [ ] A full AV1 encoder for the tools, so agents turn captured frames into cutscenes
- [ ] Accessibility through AccessKit
  - [x] Godot 4.5's accessibility tree from controls (roles, names, values, states, actions,
        related controls, open menus), `focus_mode` `accessibility`, `gui.accessibility_support`,
        `ui.accessibility` and `ui.accessibility_action`
  - [ ] The platform backend that hands the tree to VoiceOver, Narrator, Orca, TalkBack and the
        web's screen readers
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
  - [x] A test backend for all three, so agents and CI can run purchases, ads and events
        headless with no store account, plus commands to inspect and drive it
        (`services.state`, `services.test`)

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
| Editor | No GUI editor: MCP and REST command API, debug overlay, frame capture | 3, 5 | Done |
| Gameplay code | C++ with hot reload; plugins as C++ modules with a stable C ABI | 6 | In part: game code in C++ that hot reloads in the tools and links into exports is done; plugin modules with a C ABI in 6 |
| Scenes and prefabs | JSON scenes, prefabs and instanced scenes with overrides, on EnTT | 2, 5 | Done |
| 2D sprites and animation | Batched sprites, sprite sheets, flipbooks | 5 | Done |
| Tilemaps | Tilemap layers with terrain autotiling, stored as text | 5 | In part: square, isometric and hexagonal tiles, alternatives, turned cells and collision are done; animated and multi-cell tiles in 5 |
| 2D lighting | 2D lights, occluders, normal-mapped sprites | 5 | Done |
| 2D physics | Box2D v3: bodies, areas, joints, character movement and queries | 5 | In part: tile collision is done; area overrides in 5 |
| 3D physics | Jolt: rigid bodies, joints, character controller, vehicles | 7 | In part: bodies, areas, shapes, character movement and queries are done; joints and vehicles in 7 |
| 3D renderer | Clustered forward+ with PBR, plus a low-end path for web and mobile | 7 | Done |
| Shadows | Cascaded, omni and spot shadow maps | 7 | Done |
| Post-processing | Tonemap, bloom, SSAO, SSR, DoF, auto exposure, color grading | 7 | Done |
| Materials and shaders | HLSL shaders plus a JSON material graph | 5, 7 | In part: canvas material graphs, a game's own HLSL canvas shaders and `StandardMaterial3D`'s inputs on meshes are done; 3D shaders and material files in 7 |
| Keyframe animation | Keyframe tracks on any reflected property | 5 | Done |
| Skeletal animation | Skinned meshes, blending, root motion | 7 | Done |
| Animation blending | Blend trees and state machines in text, two-bone and FABRIK IK | 7 | Done |
| Particles | CPU and GPU particles, particle graphs in text | 5, 7 | Done: CPU and GPU particles in 2D and 3D with process materials, particle graphs, turbulence, collision, sub-emitters, trails, attractors and colliders (sphere, box, SDF and height field), imported from Godot, Unity's particle systems and Unreal's Niagara |
| Soundtracks | Music planned in the storyboard, written as scores or generated, arranged into interactive music and checked by analysis, all through commands | 6 | Done |
| Audio | miniaudio: buses, effects, 3D spatial audio | 5, 7 | Done |
| Input | Rebindable action maps over SDL3, injectable by agents | 5 | Done |
| Refresh rates and frame pacing | V-Sync modes, frame cap, display refresh rates, variable refresh rate, rendering interpolated between fixed ticks | 5 | Done |
| UI | Retained UI with flexbox layout and themes, defined in text | 5 | Done |
| Text and fonts | FreeType, HarfBuzz, MSDF fonts, BiDi, IME input, fallback fonts | 5 | Done |
| Navigation | Godot's navigation server, Recast baking, 2D and 3D, avoidance | 7 | Done |
| Asset import | glTF 2.0, FBX, OBJ, PNG, JPG, KTX2, WAV, OGG, MP3, and `.blend` through the artist's own Blender | 7 | Done |
| Profiling | Tracy, plus `perf.stats` over the command API | 5, 6 | Done: Tracy, and `perf.stats` reports frame pacing, tick and system times and memory by part of the engine |
| Asset generation and library | Generated assets saved once and shared by every project and teammate, searched before generating, ranked by agent feedback; 3D models as parametric recipes, cleaned-up generated meshes or imported files, combined into complex assets with variants, reviewed automatically | 6, 7 | In progress: the library, search, refining, hosted and test generators, feedback, team sharing, ready-to-use texture forms, parametric model recipes, models' text descriptions and attachment points, imported .glb models and versions from edited files, scene assembly (`entity.place`) and complex assets (recipes built of other model files, kept in the library by their parts' IDs), variants and quality tiers, model checks, automatic review and models made from concept images done; the rest of 3D in 7 |
| Global illumination | Baked lightmaps and probe-based GI | 7 | Done: reflection probes, baked lightmaps and their light probes, and VoxelGI with its dynamic objects, all imported from Godot |
| Ray tracing | Acceleration structures and ray queries in the render device on Vulkan, Direct3D 12 and Metal: ray-traced shadows, reflections, AO and GI, with raster and probe fallbacks | 7 | In progress: ray-traced shadows, ambient occlusion, reflections and GI on the software device and Vulkan |
| Path tracing | A GPU path tracer for baked lightmaps and progressive reference stills | 7 | Done: progressive reference stills and lightmap bakes on the CPU and on Vulkan's ray queries |
| Upscaling and frame generation | Render scale with FSR 1, FSR 2/3 and MetalFX built in, FSR 3 frame generation, DLSS, XeSS and FSR 4 as adapters outside the repo | 7 | Planned |
| Sky and volumetrics | Physical sky, volumetric fog | 7 | Done: procedural, physical and panorama skies, their light on meshes, exponential, depth and height fog, and volumetric fog with fog volumes and VoxelGI's light |
| Decals | Clustered decals | 7 | Done, imported from Godot |
| Networking | ENet transport, replication driven by reflection, WebSocket for web | 8 | Planned |
| Terrain | Heightmap terrain with splat materials and LOD | 7 | Planned |
| Level geometry | CSG and grid-based tile placement | 7 | In progress: CSG shapes, paths and `GridMap` done; heightmap terrain planned |
| LOD and culling | Generated LODs with meshoptimizer, occlusion culling | 7 | Done, imported from Godot |
| XR | OpenXR | 8 | Planned |
| Localization | CSV and `.po` translation tables, any source language, a runtime language setting, and commands to extract, draft and check translations | 8 | In progress: translation tables, the language setting, auto-translated controls, pseudo-localization, `l10n.extract` and `l10n.check` are done |
| Video playback | AV1 through dav1d | 8 | Done (sound to come) |
| Accessibility | AccessKit | 8 | Tree done, backend to come |
| Storyboard | Style, world, story, design rules and decisions as typed project files that agents query and checks enforce | 5, 6 | Done: files, commands and checks, palette, texture and triangle budget checks included, model review, the project map, and the style refined from accepted assets |
| Analytics, ads and in-app purchases | One API for analytics, ads and purchases; StoreKit, Play Billing and a developer-chosen analytics endpoint in the repo, ad networks as outside adapters | 8 | In progress: the API, its backend interfaces, consent and the test backend are done |
| Closed vendor SDKs | SDK packages: an `sdk.json` per SDK that exports fetch, check and link on all six platforms, with JNI and Objective-C bridges and engine interfaces for the glue | 6 | In progress: packages, exports, `sdk.add`, `sdk.check`, Android libraries and Maven, services and glue code |
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
| A test command that returns only the checks that failed | 5 | Done |
| Frame summaries as data instead of screenshots | 5 | Done |
| `storyboard.brief`: only the facts a task needs, and what changed since the agent last looked | 5 | Done |
| Dry runs: see what an edit would change before making it | 5 | Done |
| `world.snapshot` and `world.diff`: try something, see exactly what changed, roll back | 5 | Done |
| Verified examples for every feature, built and run in CI | 5 | Done |
| `run.until`: run headless until a condition holds instead of polling | 5 | Done |
| `replay.seek`: query any earlier tick of a recorded run | 5 | Done |
| `build.check`: C++ compile errors as data from an incremental build | 6 | Done |
| `api.search`: real signatures and examples instead of guessed function names | 6 | Done |
| `playtest.run`: many headless bot runs and tuning sweeps in one call | 6 | Done |
| `audio.analyze`: tempo, key, loudness and loop seams as data instead of listening | 6 | Done |
| `project.map`: where things are in a large project, without reading the source | 6 | Done |
| A scheduled benchmark of standard agent tasks counting calls and tokens | 6 | Done |
| The asset library: search before generating, and read a text description instead of the file | 6 | Done |

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
same way, and Windows machines build Windows with MSVC. Web exports build on any host, with the
Emscripten that `slash-cli toolchain install web` downloads or an activated emsdk, and Android
exports on any host with the Android SDK, NDK and JDK that
`slash-cli toolchain install android --accept-android-sdk-license` installs, or an Android Studio
install.

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
