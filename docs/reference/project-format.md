# Project format

A game is a folder with a `project.json` at its root, plus its scenes. The file works: it names
the game and says which scene starts it. `slash-cli init` writes one, `slash-cli schema project`
prints its JSON Schema, and `slash-cli project validate` checks it.

Status: current · Updated: 2026-11-22

```json
{
  "format": 1,
  "main_scene": "scenes/main.scene.json",
  "name": "Test Game",
  "tick_hz": 50,
  "version": "1.2.3",
  "window": {
    "height": 240,
    "width": 320
  }
}
```

| Key | Required | Default | Meaning |
| --- | --- | --- | --- |
| `format` | Yes | | Format version. Must be `1` |
| `name` | Yes | | The game's name, 1 to 100 characters, shown in its title bar. Exported files use it with only letters and digits kept (`Test Game` becomes `TestGame`) |
| `main_scene` | Yes | | The scene loaded at startup, relative to the project folder |
| `id` | No | `com.example.<name>` | Reverse-DNS identifier for bundles and app stores, e.g. `com.example.pong`. Android also needs each part to start with a letter and use no `-`, and iOS allows only letters, digits and `-` in each part |
| `version` | No | `0.1.0` | Version as major.minor.patch |
| `icon` | No | the engine's icon | A square image that every platform's icons are made from and the window shows, relative to the project folder: see [Icons](#icons) |
| `tick_hz` | No | `60` | Fixed simulation ticks per second, above 0 and at most 1000 |
| `window.width` | No | `1280` | Initial window width in pixels, 1 to 16384 |
| `window.height` | No | `720` | Initial window height in pixels, 1 to 16384. [Controls](scene-format.md#ui-and-text) lay out in the window as it is; `ui.layout` and `scene validate` use this size |
| `window.vsync` | No | `enabled` | How frames wait for the display. `enabled` waits for each refresh, so nothing tears. `disabled` presents at once: the lowest latency, but it may tear. `mailbox` shows the newest finished frame at each refresh: low latency with no tearing. `adaptive` waits unless a frame is late. A mode the GPU or window lacks falls back to `enabled`; SDL's GPU API has no adaptive mode yet, so `adaptive` always does |
| `window.linux_display_driver` | No | `x11` | The display server a Linux game tries first: `x11`, which Wayland desktops run through XWayland, or `wayland`. If it can't start, the game tries the other. The `SDL_VIDEO_DRIVER` environment variable, such as `SDL_VIDEO_DRIVER=wayland`, overrides it. On Wayland the compositor draws the window's title bar, which GNOME doesn't do; that is why `x11` comes first |
| `max_fps` | No | `0` | Frames per second at most, 0 to 1000, e.g. 30, 60, 120 or 240. `0` means no cap, so V-Sync alone paces frames. Headless runs ignore it |
| `rendering.clear_color` | No | `[0.3, 0.3, 0.3, 1]` | What the window shows behind everything, as `[r, g, b, a]` from 0 to 1. The default is a mid gray |
| `rendering.driver` | No | `auto` | The GPU backend. `auto` takes the first that works: Metal on Apple platforms, Direct3D 12 then Vulkan on Windows, Vulkan on Linux and Android, and on the web WebGPU where the browser has it, else WebGL 2. `vulkan`, `metal` or `d3d12` asks for one, and `webgpu` asks for the browser's WebGPU (web exports only). The OpenGL drivers are the compatibility renderer: `opengl` is OpenGL 3.3 on Windows, macOS and Linux, and OpenGL ES 3.0 where that doesn't start; `opengl_es` is OpenGL ES 3.0, which is all the web (WebGL 2), Android and iOS have, so there both drivers are it. `null` draws nothing. If none starts, the game logs a warning and runs with the null device |
| `rendering.fallback_to_opengl` | No | `true` | Draw with `opengl` where the driver's GPU backend (`auto`'s, or the one `vulkan`, `metal` or `d3d12` names) doesn't work: an old GPU or driver, a virtual machine, a remote desktop. The game asks before it makes the window, logs a warning when it falls back, and reports `opengl` (or `opengl_es`) as its `renderer` |
| `rendering.renderer` | No | `clustered` | How 3D meshes are lit and their colors finished (`forward_plus` and `gl_compatibility`, the old names, still load): see [Rendering methods](#rendering-methods) |
| `rendering.texture_filter` | No | `linear` | How sprites whose `texture_filter` is `inherit` are sampled when scaled: `linear` is smooth, `nearest` keeps pixel art sharp |
| `rendering.scaling` | No | no scaling | Draw the world at another size and scale it to the window, under controls drawn at full size: see [Render scale](#render-scale) |
| `rendering.shadows` | No | 4096-texel atlases, `soft_low` | The 3D shadow atlases' sizes and edge filters: see [Shadows](#shadows) |
| `rendering.physical_light_units` | No | `false` | Lights shine by their lux or lumens and temperature, skies and emission by their nits, and cameras' exposure is normalized by their sensitivity: see [physical light units](scene-format.md#physical-light-units) |
| `rendering.depth_of_field` | No | `hexagon`, `low`, no jitter | The bokeh shape and quality of 3D [depth of field](scene-format.md#cameraattributespractical): see [Depth of field](#depth-of-field) |
| `rendering.ssao` | No | `medium`, half size, 2 blur passes | The quality and cost of 3D [ambient occlusion](scene-format.md#ambient-occlusion): see [SSAO](#ssao) |
| `rendering.ssr` | No | half size | The resolution of 3D [screen-space reflections](scene-format.md#screen-space-reflections): see [Screen-space reflections](#screen-space-reflections) |
| `rendering.subsurface` | No | low | How meshes' [subsurface scattering](scene-format.md#subsurface-scattering) is blurred: see [Subsurface scattering](#subsurface-scattering) |
| `rendering.volumetric_fog` | No | 64 x 64 x 64, filtered | The froxels of 3D [volumetric fog](scene-format.md#volumetric-fog): see [Volumetric fog](#volumetric-fog) |
| `rendering.ray_tracing` | No | off | 3D shadows, ambient occlusion, reflections and global illumination traced by rays: see [Ray tracing](#ray-tracing) |
| `rendering.reflections.probe_size`, `probe_count` | No | 256 texels, 64 probes | The size of 3D [reflection probes'](scene-format.md#reflection-probes) cubes and how many a frame draws: see [Reflection probes](#reflection-probes) |
| `rendering.reflections.sky_samples` | No | `32` | How many GGX samples each level of the sky's [radiance](scene-format.md#sky-light) cube blurs with, from 1 to 32; levels take a quarter, a half, all and four times it. Fewer filter faster and look grainier |
| `rendering.lightmaps.bake_quality` | No | 32, 128, 512, 2048 rays; probes 64, 256, 512, 2048 | The rays a [BakedLighting3D](scene-format.md#baked-lightmaps) bake traces at each `quality`: `low_quality_ray_count` (to `ultra_`) per texel for bounces and `low_quality_probe_ray_count` (to `ultra_`) per light probe |
| `rendering.lightmaps.bake_performance` | No | 32, 64, 512 | How bakes split their work: `max_rays_per_pass` and `max_rays_per_probe_pass` (rays a pass traces, each pass seeded afresh, so they change the noise) and `region_size` (a GPU bake's dispatch lights at most this many texels squared) |
| `rendering.lightmaps.probe_capture.update_speed` | No | `15` | How fast a moving mesh's light from [light probes](scene-format.md#baked-lightmaps) follows it, a share a second |
| `rendering.voxel_lighting.quality` | No | `low` | How many cones meshes trace through [`VoxelLighting3D`s](scene-format.md#voxelgi): see [VoxelLighting3D](#voxelgi) |
| `rendering.field_lighting` | No | see [SDFGI](sdfgi.md) | SDFGI's `probe_ray_count` (8), `frames_to_converge` (30) and `frames_to_update_lights` (4) |
| `rendering.occlusion_culling` | No | off | Leave out 3D meshes that [occluders](scene-format.md#occlusion-culling) hide: see [Occlusion culling](#occlusion-culling) |
| `rendering.lod.threshold_pixels` | No | `1` | How many pixels a model's coarser [LODs](scene-format.md#mesh-lods) may be off by where they draw: see [Mesh LODs](#mesh-lods) |
| `rendering.virtual_geometry` | No | no pool | How [virtualized](virtualized-geometry.md) surfaces' cluster pages stream: `streaming_pool_kb` (0) and `max_page_loads_per_frame` (32): see [Virtualized geometry streaming](#virtualized-geometry-streaming) |
| `rendering.textures` | No | `lossless`, `linear_mipmaps` | How exports ship textures: as their files, or compressed for each platform's GPUs, and how [decals](scene-format.md#decals) sample theirs: see [Texture forms](#texture-forms) |
| `rendering.interpolation` | No | `true` | Draw sprites part of the way between fixed ticks, from where they were before the last tick to where they are, so motion looks smooth when the display refreshes faster than the game ticks. Drawing lags the simulation by up to one tick; set `false` to draw exactly where things are |
| `physics_2d.gravity` | No | `980` | Gravity's strength in pixels per second squared |
| `physics_2d.gravity_vector` | No | `[0, 1]` | Gravity's direction, as `[x, y]`; y points down |
| `physics_2d.linear_damp` | No | `0.1` | How fast rigid bodies slow down, per second. A body's own `linear_damp` adds to it unless its `linear_damp_mode` is `replace` |
| `physics_2d.angular_damp` | No | `1` | How fast rigid bodies stop turning, per second, combined the same way |
| `physics_2d.substeps` | No | `4` | Box2D's solver substeps each physics tick, 1 to 16: more keep fast, small or stacked bodies steadier, at more cost |
| `physics_2d.break_budget` | No | `0` | The most pieces breaks from impacts cut each tick (the first always cuts; the rest wait for later ticks); 0 for no limit |
| `physics_2d.contact_hertz`, `contact_damping_ratio` | No | `30`, `10` | How stiff contacts are, in cycles a second, and how little they bounce while pushing overlapping bodies apart: higher hertz pushes faster but can jitter |
| `physics_2d.max_contact_push_speed` | No | `192` | The fastest contacts push overlapping bodies apart, in pixels per second (Box2D's 3 meters) |
| `physics_2d.max_linear_speed` | No | `25600` | The fastest bodies may move, in pixels per second (Box2D's 400 meters) |
| `physics_2d.bounce_velocity_threshold` | No | `64` | The speed, in pixels per second, below which colliding bodies don't bounce |
| `physics_2d.hit_event_threshold` | No | `64` | The speed, in pixels per second, a hit needs to count for breaking and damage |
| `physics_2d.allow_sleep`, `sleep_velocity_threshold` | No | `true`, `3.2` | Whether resting bodies sleep, and how slowly, in pixels per second, they must move for half a second first |
| `physics_2d.enable_continuous`, `enable_warm_starting`, `enable_speculative` | No | `true`, `true`, `true` | Whether fast bodies are swept against static ones so they don't tunnel, whether the solver starts from the last step's impulses (steadier stacks), and whether contacts are made a little before bodies touch |
| `physics_3d.gravity` | No | `9.8` | Gravity's strength in meters per second squared |
| `physics_3d.gravity_vector` | No | `[0, -1, 0]` | Gravity's direction, as `[x, y, z]`; y points up |
| `physics_3d.linear_damp` | No | `0.1` | How fast 3D rigid bodies slow down, per second, combined with a body's own as in 2D |
| `physics_3d.angular_damp` | No | `0.1` | How fast 3D rigid bodies stop turning, per second, combined the same way |
| `physics_3d.zones_detect_static_bodies` | No | `false` | Whether `Zone3D` reports static bodies |
| `physics_3d.substeps` | No | `1` | Collision steps each physics tick, 1 to 16, as Chaos's substeps: more keep fast, small or stacked bodies steadier, at that many times the cost |
| `physics_3d.break_budget` | No | `0` | The most pieces breaks from impacts cut each tick, as Chaos's break budget (the first always cuts; the rest wait for later ticks); 0 for no limit |
| `physics_3d.velocity_steps`, `position_steps` | No | `10`, `2` | Solver iterations for velocities (2 to 100) and positions (1 to 100) each step |
| `physics_3d.baumgarte_stabilization_factor` | No | `0.2` | How much of the penetration the position steps take out each step, 0 to 1 |
| `physics_3d.speculative_contact_distance`, `penetration_slop` | No | `0.02`, `0.02` | How close bodies come, in meters, before contacts are made for them, and how far they may sink into each other before they are pushed apart |
| `physics_3d.continuous_cd_movement_threshold`, `continuous_cd_max_penetration` | No | `0.75`, `0.25` | How far a body with `continuous_cd` must move in a step, as a fraction of its inner radius, before it is cast, and how far a cast body may still sink into what it hits |
| `physics_3d.body_pair_contact_cache_enabled` | No | `true` | Whether contacts are reused while two bodies barely move against each other, until they move `body_pair_contact_cache_distance_threshold` meters (`0.001`) or turn `body_pair_contact_cache_angle_threshold` radians (`0.0349`, 2 degrees) apart |
| `physics_3d.bounce_velocity_threshold` | No | `1` | The speed, in meters per second, below which colliding bodies don't bounce |
| `physics_3d.allow_sleep` | No | `true` | Whether resting bodies sleep, after `sleep_time_threshold` seconds (`0.5`) with every point slower than `sleep_velocity_threshold` meters per second (`0.03`) |
| `physics_3d.use_enhanced_internal_edge_removal` | No | `true` | Whether bodies skip contacts with the inner edges of meshes and compound shapes, so they slide over seams |
| `physics_3d.soft_body_point_radius` | No | `0.01` | The radius, in meters, of soft bodies' and cloths' points when they collide |
| `physics_3d.recovery_iterations`, `recovery_amount` | No | `4`, `0.4` | The passes a character's move makes to push out of what it overlaps, and the share of each overlap a pass takes |
| `physics_3d.motion_use_enhanced_internal_edge_removal` | No | `true` | Whether a character's moves skip contacts with inner edges |
| `physics_3d.collision_margin_fraction`, `active_edge_threshold` | No | `0.08`, `0.8727` | The share of a convex shape's smallest extent its margin may take, and the angle in radians (50 degrees) between faces under which a mesh's edge counts as smooth |
| `physics_3d.world_boundary_shape_size` | No | `2000` | How far a world boundary shape reaches, in meters |
| `physics_3d.max_linear_velocity`, `max_angular_velocity` | No | `500`, `47.12` | The fastest bodies may move, in meters per second, and turn, in radians per second (2700 degrees) |
| `physics_3d.max_bodies`, `max_body_pairs`, `max_contact_constraints` | No | `10240`, `65536`, `20480` | How many bodies, pairs of touching bodies and contact constraints a space holds; a step past the last two warns and drops contacts |
| `physics_3d.temporary_memory_buffer_size` | No | `4` | Jolt's scratch memory for a step, in megabytes; more comes from the heap |
| `navigation_2d.cell_size` | No | `1` | The 2D map's cell size in pixels: edge ends are rounded to it, so make navigation polygons for it |
| `navigation_2d.merge_cell_scale` | No | `1` | Edge ends closer than `cell_size` times this (at most 0.1) are the same point |
| `navigation_2d.connect_edges` | No | `true` | Whether regions' edges that come within `edge_connection_margin` join |
| `navigation_2d.edge_connection_margin` | No | `1` | How close, in pixels, two regions' edges must come to join |
| `navigation_2d.link_connection_radius` | No | `4` | How far, in pixels, a link's ends reach for a region |
| `navigation_3d.cell_size`, `navigation_3d.cell_height` | No | `0.25` | The 3D map's cell size and height in meters |
| `navigation_3d.merge_cell_scale`, `connect_edges` | No | `1`, `true` | As in 2D |
| `navigation_3d.edge_connection_margin` | No | `0.25` | How close, in meters, two regions' edges must come to join |
| `navigation_3d.link_connection_radius` | No | `1` | How far, in meters, a link's ends reach for a region |
| `navigation_3d.up` | No | `[0, 1, 0]` | Which way is up for paths |
| `npc.day_seconds` | No | `0` | Seconds of game time in a day of the [npc clock](scene-format.md#daily-schedules-and-routes), which NPC brains' `hour` and `day` facts read; `0` stops the clock at `start_hour` |
| `npc.start_hour` | No | `6` | The hour the npc clock shows when the world starts, from 0 up to 24 |
| `ui.theme` | No | | A [`.theme.json`](scene-format.md#themes) file every control uses after its own and its parents' themes, before the default theme. `project validate` checks that it exists and `export` checks and ships it |
| `ui.system_font_fallback` | No | `true` | Let text take characters its fonts and their [fallbacks](scene-format.md#fonts) lack from fonts installed on the machine. Off, text lays out the same on every machine, as on the web, which has none; a `.font.json` file's `allow_system_fallback` turns them off for one font |
| `ui.tooltip_delay_seconds` | No | `0.5` | Seconds the pointer rests on a control before its [tooltip](scene-format.md#tooltips-and-drag-and-drop) shows |
| `ui.tooltip_offset` | No | `[10, 10]` | Where a tooltip shows from the pointer |
| `ui.accessibility_support` | No | `"auto"` | When controls serve a [screen reader](scene-format.md#accessibility): while one is in use (`auto`), `always` or `disabled`, which also leaves the window with no screen reader adapter. Serving one, controls with `focus_mode` `accessibility` take focus |
| `ui.drag_threshold` | No | `10` | Pixels the pointer moves with the left button held before a [drag](scene-format.md#tooltips-and-drag-and-drop) starts |
| `ui.text_edit_merge_seconds` | No | `3` | Seconds without typing that end one undo step in a [`TextArea`](scene-format.md#text-fields-of-several-lines); typing before then merges into the step |
| `ui.text_edit_undo_limit` | No | `1024` | Undo steps a `TextArea` keeps, at least 1; the oldest go first |
| `ui.type_search_ms` | No | `2000` | Milliseconds between key presses that still add to a popup menu's or tree's search by typing |
| `ui.shortcut_flash_seconds` | No | `0.2` | Seconds a [button](scene-format.md#buttons) shows pressed after its shortcut; 0 shows none |
| `audio.bus_layout` | No | | A [`.bus_layout.json`](scene-format.md#bus-layouts) file with the buses players mix into. Without one, everything plays through Master. `project validate` checks it and `export` checks and ships it |
| `audio.mix_rate` | No | `44100` | Frames of sound mixed per second, 8000 to 192000. Sounds at other rates are resampled |
| `audio.output_latency` | No | `15` | Milliseconds of sound queued ahead of the speakers, 1 to 500. More is steadier when frames run late; less answers sooner |
| `audio.panning_strength_2d` | No | `0.5` | How far [`SoundPlayer2D`](scene-format.md#audio) sounds pan left and right, 0 to 4. Each player's `panning_strength` multiplies it |
| `audio.speaker_mode` | No | `"auto"` | The speakers to mix for: `stereo`, `surround_31`, `surround_51`, `surround_71`, or `auto` for the output device's; stereo where nothing plays the sound, as headless. See [surround sound](scene-format.md#surround-sound) |
| `audio.panning_strength_3d` | No | `0.5` | How far [`SoundPlayer3D`](scene-format.md#3d-sound) sounds pan left and right, 0 to 4. Each player's `panning_strength` multiplies it |
| `audio.silence_threshold_db` | No | `-60` | A bus's speaker pair quieter than this, -80 to 0 dB, for `silence_seconds` stops mixing its effects until sound comes again. Lower lets reverb and delay tails ring longer |
| `audio.silence_seconds` | No | `2` | Seconds, 0 to 5, a bus's speaker pair stays under `silence_threshold_db` before it stops |
| `audio.input_enabled` | No | `false` | Let [microphone](scene-format.md#microphone) players record. Exports then ask for the microphone: `NSMicrophoneUsageDescription` in macOS and iOS apps, the `RECORD_AUDIO` permission on Android, and the `com.apple.security.device.audio-input` entitlement for a Mac app signed with a Developer ID and no `entitlements` file of its own (one that has its own needs that key) |
| `audio.input_usage_description` | No | "*name* listens to the microphone." | Why the game records, which macOS and iOS show when they ask the player for the microphone. App Store review expects it to say what the game does with it |
| `gameplay.sources` | No | | The game's own [C++ code](#gameplay-code): folders and files relative to the project, at least one |
| `plugins` | No | | [Plugin](#plugins) files, relative to the project, each ending in `.plugin.json` |
| `sdks` | No | | [SDK packages](sdk-format.md) exports link: folders relative to the project, each with an `sdk.json`. `slash-cli sdk add` adds one |
| `services` | No | | [Ad placements, products, analytics, consent, achievements and leaderboards](#services) |
| `localization` | No | | [Translation files, the fallback locale, pseudo-localization and layout direction](#localization) |
| `xr` | No | | [The headset's runtime: whether XR starts, the reference space and how the picture blends with the room](#xr) |
| `threading.max_threads` | No | `-1` | Worker threads the [job system](../concepts.md#world-entities-and-components) runs threaded systems, particles and physics on beside the main thread, 0 to 63. `-1` is one per CPU core; `0` runs everything on the main thread, as the web does. Every count gives the same results |
| `work.journal` | No | `true` | Whether agents' sessions write a [work journal](watching-agents.md) in `.slash/work/` |
| `work.serve` | No | `true` | Whether sessions serve themselves read-only on the loopback interface for [watchers](watching-agents.md#watch-servers) |
| `work.checkpoints` | No | `true` | Whether journaled sessions keep [checkpoints](watching-agents.md#checkpoints) of the files they write, the world and a frame |
| `work.max_mb` | No | `64` | The most the project's work journals and checkpoints hold together, in MiB, at least 1; the oldest sessions' go first |
| `input` | No | The built-in `ui_*` actions | [Input actions](#input-actions) by name, each with the events that press it |
| `pointer.mouse_from_touch` | No | `true` | A touch also clicks the left mouse button and dragging it moves the mouse, so mouse code works on phones |
| `pointer.touch_from_mouse` | No | `false` | A left click also touches the screen and dragging with it drags a finger, so touch code can be tried with a mouse |

Unknown keys are errors, with a "did you mean" suggestion for a misspelled one.

A running game can change `vsync` and `max_fps` with
[`display.set`](commands.md#displayset), and its own code with `RenderDevice::set_vsync` and
`Loop::set_max_fps`, e.g. from an options menu. For variable refresh rate (G-Sync, FreeSync,
ProMotion), keep V-Sync enabled and cap the frame rate a little below the
display's refresh rate. Exports ask phones for their fastest refresh rate: iOS apps set
`CADisableMinimumFrameDurationOnPhone`, so iPhones with ProMotion run at 120 Hz, and Android apps
pick the display's fastest mode at its current resolution. On the web the browser calls each
frame at the display's rate, and a frame cap sets how often it calls instead.

## Render scale

`rendering.scaling` draws the world at a lower (or higher) resolution than the window and scales it
to the window's size; the Godot importer brings the setting over. `bilinear` and `fsr` scale the
whole world, 2D and 3D; `fsr2` and `fsr3` scale only the 3D world, and draw 2D at full size.
Controls, and canvas layers above the world, draw at the window's own size on top, so the UI stays
sharp while the world costs fewer pixels; canvas layers below the world are scaled with it.

```json
"rendering": {"scaling": {"mode": "fsr", "scale": 0.5, "sharpness": 0.2}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `mode` | `bilinear` | `bilinear` stretches the world with filtering. `fsr` is AMD's FidelityFX Super Resolution 1: EASU scales up along the edges it finds, then RCAS sharpens. `fsr2` is FSR 2.2: the camera moves by less than a pixel each frame and the 3D world is rebuilt at the window's size from its frames, its depth and its motion vectors, then sharpened by RCAS. `fsr3` is FSR 3.1's upscaler: it rebuilds the 3D world from the same inputs with AMD's later passes. `metalfx_spatial` and `metalfx_temporal` are Apple's MetalFX: the spatial scaler over the world, and its temporal scaler rebuilding the 3D world from the same inputs as `fsr2`, on Metal with macOS 13 or iOS 16 and a GPU that has them. Elsewhere they fall back: `metalfx_spatial` to `fsr`, `metalfx_temporal` to `metalfx_spatial` where that runs, else to `fsr2`, and to `fsr2` at a `scale` outside what the GPU's temporal scaler takes. `frame.capture`'s `upscaler` says which ran |
| `scale` | `1` | The world's size over the window's, 0.25 to 2. At `1` with `bilinear` nothing is scaled. Above `1` the world draws larger and is averaged down, always with `bilinear`, as FSR only scales up |
| `sharpness` | `0.2` | RCAS's sharpening in stops, 0 to 2, for `fsr`, `fsr2` and `fsr3`: `0` is the sharpest, `2` the softest |
| `upscaler` | | An upscaler the game adds with `render::add_upscaler` (from its own C++ or an [SDK package's](sdk-format.md) glue code), used instead of `mode` on the GPUs it supports. Lowercase letters, digits and underscores. Where it isn't added or doesn't run, the game logs a warning once and uses `mode` |

The world draws at the window's size in pixels times `scale`, rounded down: a 2560x1440 window at
`0.5` draws a 1280x720 world. Both upscalers are drawn with the render device's own calls, so
they work on every backend and on the software device behind `frame.capture`, which returns the
`upscaler` that ran and the `render_size`. A running game changes the settings with
[`display.set`](commands.md#displayset)'s `scaling`, and
[examples/render-scale](../../examples/render-scale/) does.

`fsr2` follows AMD's FidelityFX FSR 2.2 (MIT) pass by pass: the 3D world draws at the render size
with a jittered camera, the clustered renderer's prepass writes motion vectors as
[TAA](#temporal-antialiasing)'s does, and FSR 2's passes (luminance, dilated depth and motion,
reconstructed depth from the frame before, depth clip, locks, accumulation and RCAS) rebuild it at
the window's size before the 3D post-processing. Where no opaque mesh drew, such as the sky, FSR 2
works the motion vector out from the depth and the camera of the frame before, so a sky stays sharp
while the camera turns. The sky is seen through the same jittered camera as the meshes, with TAA
too. It runs in the clustered renderer only, at a `scale` below `1`; in the compatibility renderer,
with the OpenGL drivers, or with no 3D in the world, the world scales with `bilinear` instead. TAA
is off while it runs, since FSR 2 does its job. It has no reactive mask, so transparent meshes,
which write no depth, can trail for a frame or two.

`fsr3` follows the upscaler of AMD's FidelityFX Super Resolution 3.1 (FidelityFX SDK 1.1.4, MIT)
pass by pass, with the settings FSR 2 has, and runs where `fsr2` runs, from the same
jittered frame, motion vectors and depth. Its passes prepare the inputs (dilated depth and motion,
the farthest depth and the luma), reconstruct the depth of the frame before, build a pyramid of
how much the luma changed since the frame before at half the render size, work out the reactive
masks (disocclusion, shading change and the frames each render pixel has gathered), find thin
features to lock, measure the luma's instability over the last four frames, accumulate, and end
with FSR 2's RCAS. Where it has gathered many frames it clamps the frames before to three times
the spread of the colors around each pixel rather than to their range, which keeps detail
steadier; a pixel whose shading changes everywhere around it takes the new color at once, while
the edge of something that changed color fades over a few frames.
[examples/fsr3-3d](../../examples/fsr3-3d/) rebuilds, turns and recolors a card.

Upscalers a game adds are `render::Upscaler` classes (`slash/render/upscaler.hpp`): `input`
gives the render target the world draws into, `upscale` runs the upscaler's passes and returns
the texture and material the window's frame draws over the screen, and `release` frees what it
made on the device. `render::UpscalerInfo::supports` says which devices one runs on, such as a
vendor's on its own GPUs.

One that is `temporal` (DLSS, XeSS, FSR 4 from a vendor's SDK) rebuilds the 3D world from its depth,
motion vectors and jittered camera, as `fsr2` does: `make_temporal` makes a
`render::TemporalUpscaler`, and where `scaling.upscaler` names it and FSR 2 would run (the clustered
renderer, a `scale` under 1, the GPU device on Vulkan, Direct3D 12 or Metal, or the software device,
and `supports` says yes) the device runs it in FSR 2's place. Its `upscale` gets a
`render::TemporalFrame`: the device's native objects (the `VkDevice` and `VkQueue`, the
`ID3D12Device`, the `MTLDevice`), the command buffer it records into, the color, depth and motion
textures at the render size and the output at the window's size as native images, the jitter in
render pixels, whether the history starts over, the sharpness and the camera. The textures are left
in the state shaders read them in. On the software device the textures are float pixels instead, so
a game's tests see what its upscaler gets. Elsewhere `mode` scales the world, and `frame.capture`'s
`upscaler` says which ran. A vendor's upscaler on Vulkan may need device extensions SDL doesn't turn
on (DLSS asks for NVIDIA's), so check its requirements in `supports`.
[examples/temporal-upscaler](../../examples/temporal-upscaler/) adds one and reads its frames.

## Rendering methods

`rendering.renderer` picks how 3D [lights](scene-format.md#lights) reach meshes and how their
colors are [finished](scene-format.md#worldenvironment):

| Method | Lights | How | Colors |
| --- | --- | --- | --- |
| `clustered` | 8 directional and 512 omni and spot | Clusters: each frame, the CPU sorts the omni and spot lights nearest first and marks the 32-pixel tiles of the screen, and the 32 slices of depth out to the camera's far plane, that each light's range reaches. A pixel adds the directional lights, then only the lights of its tile and slice, so a plaza of hundreds of lanterns costs each pixel the few near it | Linear, in a texture of 16-bit floats: brighter than white glows through up to seven levels, then a pass tonemaps the frame |
| `compatibility` | The first 64 of any kind, the directional ones first | the compatibility renderer: every pixel adds each light | Tonemapped as meshes draw, or through an 8-bit texture with glow or adjustments |

The OpenGL drivers (`opengl`, `opengl_es`, and WebGL 2 on the web) always draw as
`compatibility`. Every other device, and `frame.capture`, draws as the project says; both
methods light a pixel the same where a scene has 64 lights or fewer, and finish its color within
a step or two of each other where nothing glows and nothing is transparent.

## Mesh LODs

`rendering.lod.threshold_pixels` (default `1`) sets the LOD change threshold. Each frame, a model's
surfaces draw the coarsest of their [LODs](scene-format.md#mesh-lods) that is off by no more than
this many pixels of the view's width, so a higher threshold draws coarser LODs nearer and saves more
triangles. `0` draws every mesh in full.

```json
"rendering": {"lod": {"threshold_pixels": 2}}
```

## Virtualized geometry streaming

`rendering.virtual_geometry` sets how [virtualized](virtualized-geometry.md#streaming) surfaces
stream their cluster pages, as Unreal's Nanite streaming pool. With `streaming_pool_kb` above `0`,
each surface keeps that many kilobytes of its clusters' triangles loaded: pages load as the camera's
cuts need them, at most `max_page_loads_per_frame` (default `32`) a frame, and the least recently
used are dropped for room. `0` (the default) keeps every page loaded.

```json
"rendering": {"virtual_geometry": {"streaming_pool_kb": 256, "max_page_loads_per_frame": 2}}
```

## Occlusion culling

`rendering.occlusion_culling` holds the occlusion culling settings. With
`use_occlusion_culling` on, each frame leaves out the 3D meshes that
[`Occluder3D`s](scene-format.md#occlusion-culling) hide from the camera, by rays cast
into the occluders through a small depth buffer.

```json
"rendering": {"occlusion_culling": {"enabled": true}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | `false` | Whether occluders hide meshes |
| `rays_per_thread` | `512` | The buffer's texels over 8: 1 to 65536. More texels hide meshes more exactly and take longer |
| `jitter_projection` | `true` | Move the rays through nine places across a texel, one a frame, and keep a mesh that was seen drawn for nine frames, so thin gaps between occluders don't make meshes flicker |

## Shadows

`rendering.shadows` sizes the two atlases 3D lights draw their
[shadow maps](scene-format.md#lights) into and picks how their edges are filtered.

```json
"rendering": {"shadows": {
  "directional_size": 4096, "directional_softness": "soft_low",
  "positional_atlas_size": 2048, "positional_softness": "hard"
}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `directional_size` | `4096` | The atlas directional lights' cascades share, in texels across: a power of two from 256 to 16384 |
| `directional_softness` | `soft_low` | `hard` takes one sample a pixel; `soft_very_low` and `soft_low` 5, filtered between texels; `soft_medium`, `soft_high` and `soft_ultra` 13, softer still |
| `positional_atlas_size` | `4096` | The atlas omni and spot lights share, as `directional_size` |
| `positional_softness` | `soft_low` | As `directional_softness` |

A device draws an atlas at most as large as its largest texture (8192 texels for WebGPU), and a
frame with no shadowed light draws none. Larger atlases make sharper shadows and cost GPU memory: a
4096-texel atlas takes 64 MB. Every tile in the atlas is the same size.

## Depth of field

`rendering.depth_of_field` says how a camera's
[depth of field](scene-format.md#cameraattributespractical) blurs.

```json
"rendering": {"depth_of_field": {"bokeh_shape": "circle", "bokeh_quality": "medium"}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `bokeh_shape` | `hexagon` | The shape out-of-focus lights spread into: `box` blurs across then down, `hexagon` along three directions, and `circle` gathers a spiral of samples, the softest and the slowest |
| `bokeh_quality` | `low` | `very_low`, `low`, `medium` or `high`: more samples each pass (6, 12, 12 and 24). The box and hexagon blur at half size at `very_low` and `low`, and the circle always does |
| `jitter` | `false` | Turns the samples a little each frame by game time, trading banding for noise |

## SSAO

`rendering.ssao` says how a `Environment3D`'s
[ambient occlusion](scene-format.md#ambient-occlusion) is worked out.

```json
"rendering": {"ssao": {"quality": "high", "half_size": false}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `quality` | `medium` | `very_low`, `low`, `medium`, `high` or `ultra`: more samples each pixel. From `high` on, samples further out read depth at lower resolutions, and `ultra` takes up to 32 where an importance map says occlusion changes |
| `half_size` | `true` | Works at a quarter of the frame's resolution in each direction, rather than half, and blurs the result back up: faster, and softer |
| `adaptive_target` | `0.5` | `ultra`: how many of the extra samples to take, from 0 to 1 |
| `blur_passes` | `2` | Edge-aware blurs of the occlusion, from 0 to 6; `very_low` always blurs once |
| `fadeout_from`, `fadeout_to` | `50`, `300` | Occlusion fades out from this distance and is gone at that one, in meters |

## Screen-space reflections

`rendering.ssr` says how a `Environment3D`'s
[screen-space reflections](scene-format.md#screen-space-reflections) are worked out.

```json
"rendering": {"ssr": {"half_size": false}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `half_size` | `true` | Traces at half the frame's resolution in each direction, then resolves the reflections to its size along its depths and normals: faster, and softer at edges |

## Subsurface scattering

`rendering.subsurface` says how materials'
[subsurface scattering](scene-format.md#subsurface-scattering) is blurred.

```json
"rendering": {"subsurface": {"quality": "high", "scale": 0.08}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `quality` | `"low"` | `"disabled"` turns it off; `"low"`, `"medium"` and `"high"` blur with 11, 17 and 25 samples across |
| `scale` | `0.05` | How wide the blur is at full strength, as a share of the screen, 0 to 1 |
| `depth_scale` | `0.01` | How much the blur narrows with distance, from 0 (not at all) to 1 |

## Volumetric fog

`rendering.volumetric_fog` says how finely a `Environment3D`'s
[volumetric fog](scene-format.md#volumetric-fog) is worked out.

```json
"rendering": {"volumetric_fog": {"volume_size": 96, "volume_depth": 128}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `volume_size` | `64` | Froxels across and down for a square frame, from 16 to 512; a wider frame takes more across and fewer down |
| `volume_depth` | `64` | Froxels from the camera to the fog's length, from 16 to 512 |
| `use_filter` | `true` | Blurs the fog across and down before it is gathered, which smooths it |

## Ray tracing

`rendering.ray_tracing` turns on effects traced by rays in `clustered`. Devices that can't trace
rays draw as if they were off.

```json
"rendering": {"ray_tracing": {"shadows": true, "shadow_samples": 16}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `shadows` | `false` | Traces the first 8 shadowed lights' [shadows](scene-format.md#ray-traced-shadows) from each pixel instead of sampling their shadow maps |
| `shadow_samples` | `1` | Rays a pixel sends toward each light, from 1 to 64: more smooth the penumbras of lights with a size |
| `ambient_occlusion` | `false` | Traces the environment's [SSAO](scene-format.md#ambient-occlusion) by rays over each pixel's half of the sphere instead of gathering it from the screen |
| `ambient_occlusion_samples` | `4` | Rays a pixel sends for its ambient occlusion, from 1 to 64: more is less noisy |
| `reflections` | `false` | Traces the environment's [SSR](scene-format.md#screen-space-reflections) by rays reflected off each pixel instead of finding reflections on the screen; the software device, and Vulkan, Metal or Direct3D 12 where the GPU traces rays |
| `reflection_samples` | `1` | Rays a pixel reflects, from 1 to 64: more make rough reflections less grainy |
| `global_illumination` | `false` | Traces each opaque pixel's [ambient light](scene-format.md#ray-traced-global-illumination) by rays bouncing off what is near it; on the software device and on Vulkan, Metal or Direct3D 12 where the GPU traces rays |
| `global_illumination_samples` | `4` | Rays a pixel sends for its ambient light, from 1 to 64: more is less noisy |
| `many_lights` | `false` | Lights each opaque pixel's omni and spot lights by [ReSTIR](restir.md): a few lights a pixel, resampled from its cluster's, last frame's and its neighbors', each with one shadow ray. Research: only the software device draws it; others light every light as before |
| `many_light_candidates` | `8` | Lights a pixel draws from its cluster each frame, from 1 to 32 |
| `many_light_samples` | `1` | Reservoirs a pixel keeps, each a light and a shadow ray, from 1 to 4: more is less noisy |

## Reflection probes

`rendering.reflections` sizes the atlas of cubes that
[reflection probes](scene-format.md#reflection-probes) are drawn into. Every probe's cube is the
same size; a larger one shows sharper reflections and costs more to draw and keep.

```json
"rendering": {"reflections": {"probe_size": 512, "probe_count": 16}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `probe_size` | `256` | Texels across each face of a probe's cube: 32, 64, 128, 256, 512, 1024 or 2048 |
| `probe_count` | `64` | How many probes a frame draws, from 1 to 4096, at most 42 here; the largest are drawn first and the rest reflect nothing |

## VoxelLighting3D

`rendering.voxel_lighting.quality` says how meshes trace their ambient light through
[`VoxelLighting3D`s](scene-format.md#voxelgi): `low` traces four cones of 45 degrees, `high` six
narrower ones, which show finer detail and cost more.

```json
"rendering": {"voxel_lighting": {"quality": "high"}}
```

## Temporal antialiasing

`rendering.anti_aliasing.taa` turns on temporal antialiasing for the 3D world in `clustered`.
Each frame moves the camera by less than a pixel, along the next of 16 points of a Halton
sequence. Meshes write how far each pixel moved since the frame before, from their transforms,
skins and the camera's. A pass then blends the frame with the frames before it, moved by those
motion vectors, and clips the old colors to those around each pixel so that moving things leave
little trail. Edges smooth out over about 16 frames; details thinner than a pixel, which most
frames miss, fade instead.

```json
"rendering": {"anti_aliasing": {"taa": true}}
```

TAA is off by default. It leaves the 2D canvas, controls and `compatibility` alone (the
compatibility renderer has no TAA), and reflection probes capture without it. A frame with no
meshes starts the history again. `frame.capture` keeps the history between captures, and its
`frames` draws several at once.

## Frame generation

`rendering.frame_generation` shows a frame made between each two the game draws, as AMD's FSR 3
frame generation does, so a game drawn at 40 frames a second shows 80. The
world, with the canvas layers below it, draws into a target of its own; the frame halfway
between it and the one before is made from the 3D world's motion vectors and depth (in
`clustered`) and the optical flow of the two pictures (everywhere, 2D too); then the game's
window shows the made frame, then the drawn one, the world's controls, the layers above and the
debug overlay drawn over both, so the UI never smears.

```json
"rendering": {"frame_generation": true}
```

It is off by default. With V-Sync on, each frame waits for the display, which spaces the two out;
with it off (or `mailbox`), the game waits half a frame between them (the frame cap's period, or
the time frames have been taking). Frames are made by the GPU devices (Vulkan, Metal, Direct3D
12), WebGPU and the software device; on OpenGL, headless and on the web (where the browser shows
one frame a callback) frames draw once. A first frame, or one after a scene change, shows twice.
The made frame adds half a frame of latency, as AMD's does, so it suits games already above 30
frames a second. [`display.set`](commands.md#displayset)'s `frame_generation` turns it on and off
while the game runs.

## Texture forms

`rendering.textures` says how exports ship textures.
An export makes each texture's forms for its platform once, keeps them in the
[asset library's](commands.md#asset-library) cache (`forms/` in `SLASH_LIBRARY_DIR` or
`~/.slash/library`, keyed by the file's SHA-256 and the format) and ships them in place of the
file, beside where it was: `sprites/hero.png` in BC7 is `sprites/hero.png.bc7.ktx2`. Scenes keep
naming `sprites/hero.png`; the game reads the form its GPU samples.

```json
"rendering": {"textures": {"compress_mode": "vram_compressed", "paths": {"ui/": "lossless"}}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `compress_mode` | `lossless` | `lossless` ships the file as it is, decoded when it loads for 2D. `vram_compressed` ships blocks GPUs sample as they are, a quarter of the GPU memory of RGBA8: BC7 on macOS, Windows and Linux, ASTC 4x4 on Android and iOS, and both on the web, whose browsers have one or the other. `vram_uncompressed` ships RGBA8 texels, as big on disk as in memory but ready for the GPU |
| `paths` | | Modes for folders (ending in `/`) and files, such as `{"ui/": "lossless", "ui/atlas.png": "vram_compressed"}`; the longest one that holds a texture wins |
| `decals.filter` | `linear_mipmaps` | How [decals](scene-format.md#decals) sample their textures: `nearest`, `linear`, `nearest_mipmaps`, `linear_mipmaps`, `nearest_mipmaps_anisotropic` or `linear_mipmaps_anisotropic`. The mipmap filters pick a level by how far the decal is; the anisotropic ones sample as their mipmap twins |

BC7 and ASTC are blocks of 4x4 texels, so a texture whose sides aren't multiples of 4 ships as it
is, and the export logs a warning. Compressing keeps the colors close rather than exact, so pixel
art and UI often stay `lossless`; [`texture.forms`](commands.md#textureforms) makes the forms
ahead of an export and reports each one's size and PSNR, so an agent can choose. A device that
doesn't sample a form's format decodes it when the texture loads, so every form draws everywhere.
Mipmaps aren't made yet.

## Icons

`icon` names a square image. Give it 1024x1024 pixels: the App Store asks for that size, and
smaller images are scaled up. Exports make each platform's icons from it, resized with a Lanczos
filter:

| Platform | What the export ships |
| --- | --- |
| macOS | `AppIcon.icns` in the bundle's `Resources`: PNG images from 1024 to 32 pixels, and 32 and 16 pixel ones run-length encoded with masks. An opaque icon is drawn into Apple's rounded square, 824 of 1024 pixels with corners rounded 185, as macOS 11 icons are; one with transparent pixels has its own shape and stays as it is |
| Windows | `icon.ico`, PNG images from 16 to 256 pixels, compiled into the executable as its first icon resource, which Explorer and the window show |
| Linux | Nothing beside the executable: the window shows the icon |
| Web | `favicon.png` (192 pixels) and `apple-touch-icon.png` (180, opaque), linked from `index.html` |
| Android | An adaptive icon (`res/mipmap-anydpi-v26/icon.xml`): the icon fills the middle 72 of the foreground layer's 108 dp, which every launcher's mask shows, over the background color. `android.icon_foreground` and `android.icon_background` in a preset change the layers |
| iOS | An asset catalog with one 1024-pixel image, which Xcode's `actool` compiles into the app with every size older iOS versions read. iOS icons are opaque, so transparent pixels become `ios.icon_background` |

Without `icon`, games get the engine's: a white slash on a dark blue square, drawn in code. A
preset's `icon` replaces the project's for that preset. Exports and `slash-cli project validate`
report an icon that is missing, isn't an image or isn't square, with the JSON Pointer of the
setting that names it. Windowed games, and `slash-cli run`, show the icon in their window's
title bar and task bar, except on a Mac, which shows the bundle's.

## Gameplay code

A game's own components and systems are plain C++; there is no scripting language.
`gameplay.sources` names where the code is: every `.cpp` file in a folder and its subfolders is
compiled, and so is each `.cpp` file listed. The folders, and the folders of listed headers, are
searched for includes.

```json
"gameplay": {"sources": ["src"]}
```

One source file defines the game's registration function with `SLASH_GAME_MODULE`
(`slash/world/game_module.hpp`):

```cpp
#include "slash/world/components.hpp"
#include "slash/world/game_module.hpp"

struct Wallet {
    int coins = 0;
    int income = 1;
};
SLASH_COMPONENT(Wallet, coins, income)

void earn(slash::world::World& world, double /*tick_seconds*/) {
    world.ecs().view<Wallet>().each([](Wallet& wallet) { wallet.coins += wallet.income; });
}

SLASH_GAME_MODULE(game) {
    game.add_component<Wallet>().set_doc("Coins an entity holds.");
    game.add_system("earn", earn, {.doc = "Pays each wallet its income."});
}
```

Exports compile the sources into the game for every platform. The tools build them into
`.slash/gameplay/game.so` (`game.dll` on Windows) with [`slash-cli build`](cli.md#build) or
[`gameplay.build`](commands.md#gameplaybuild) and load that, on macOS, Linux and Windows; when it is
rebuilt they swap it in, and the world keeps its entities and each field the new code still has.
Libraries built against other engine headers, or with another compiler family or C++ standard
library ABI, are refused; other versions of the engine's compiler are fine. Until the
code is built, each component a `SLASH_COMPONENT` line in the sources declares is a stand-in
that keeps whatever values scenes give it, so their scenes open, change and save as written;
the first build swaps the code's types in, keeping the values of the fields they have.

Code is built as the engine is, with no exceptions or RTTI. Keep the game's state in registered
components, so scenes save it, `undo` restores it and rebuilding keeps it: values of components
the code never registered, and anything kept in globals, are lost when the code is rebuilt, and
the world's context (`World::ecs().ctx()`) must hold no values of the game's own types. A
component or system named like one of the engine's is refused.

On Windows the tools build the code with MSVC (run them from a Developer PowerShell) into a DLL
that calls the functions the program exports. It can't use the engine's global variables directly
(a few non-`inline` ones, such as a class's `static` members defined in the engine's sources),
only through functions; the link names any it tries to. The tools refuse a DLL that calls a
function the program lacks, naming it.

## Plugins

A plugin is a library built apart from the engine that adds components and systems to a game. It
is built against `engine/plugin/include/slash/plugin/slash_plugin.h` alone: a C interface that
only grows, so a plugin built once loads into later engines, written in any language that calls C
and linked to nothing of the engine's. `plugins` lists `.plugin.json` files (`slash-cli schema
plugin`):

```json
{
  "abi": 1,
  "entry": "orbit_plugin_entry",
  "libraries": {"linux": "bin/orbit.so", "macos": "bin/orbit.so", "windows": "bin/orbit.dll"}
}
```

| Field | Required | Default | Meaning |
| --- | --- | --- | --- |
| `entry` | Yes | | The C name of the entry function, unique among the plugins a game links |
| `abi` | No | `1` | The `SLASH_PLUGIN_ABI_VERSION` it was built against; an engine loads plugins of its version or older |
| `libraries` | One of these two | | The library for each platform, keyed `<os>.<arch>` or `<os>` (`android`, `ios`, `linux`, `macos`, `web`, `windows`; `arm64`, `wasm32`, `x86_64`), relative to the file. The `<os>.<arch>` key wins. A shared library, or a static one (`.a`) for iOS and the web |
| `sources` | One of these two | | C, C++ and Objective-C files and folders, relative to the file, that exports compile into the game in place of its libraries, for every platform (Objective-C only for macOS and iOS). The tools still load the library for the machine they run on |

slash-cli, slash-mcp and games load the plugins before scenes open, and the plugin's name is its
file's name before the first dot. The entry function is handed `get_proc`, which gives each
engine function by name, and returns 1 once it registered what it adds or 0 to refuse:

```c
#include "slash/plugin/slash_plugin.h"

typedef struct Orbit { float center[2]; float radius; float speed; float angle; } Orbit;
static SlashComponent orbit;
static SlashWorldEach world_each;

static void move(void* userdata, SlashEntity entity, void** components) {
    Orbit* it = (Orbit*)components[0];
    it->angle += (float)(it->speed * *(double*)userdata);
}

static void run(void* userdata, SlashWorld* world, double seconds) {
    world_each(world, &orbit, 1, move, &seconds);
}

SLASH_PLUGIN_EXPORT int orbit_plugin_entry(SlashGetProc get_proc, SlashPlugin* plugin,
                                           SlashPluginInit* init) {
    static const SlashField fields[] = {
        {"center", SLASH_FIELD_VEC2, offsetof(Orbit, center), 0, "What it circles.", 0, 0, 0, 0},
        {"radius", SLASH_FIELD_FLOAT, offsetof(Orbit, radius), 0, "Pixels.", 1, 0, 0, 0},
        {"speed", SLASH_FIELD_FLOAT, offsetof(Orbit, speed), 0, "Radians a second.", 0, 0, 0, 0},
        {"angle", SLASH_FIELD_FLOAT, offsetof(Orbit, angle), 0, "Radians.", 0, 0, 0, 0},
    };
    static const SlashComponentInfo info = {"Orbit", "Moves round a circle.", sizeof(Orbit),
                                            SLASH_ALIGNOF(Orbit), NULL, fields, 4};
    SlashSystemInfo system = {"orbit", "Moves Orbit entities.", 100, run, NULL};
    world_each = (SlashWorldEach)get_proc("slash_world_each");
    orbit = ((SlashRegisterComponent)get_proc("slash_register_component"))(plugin, &info);
    return orbit != 0 &&
           ((SlashRegisterSystem)get_proc("slash_register_system"))(plugin, &system);
}
```

- **Components** are C structs of at most 4096 bytes, aligned to at most 16, whose fields the
  plugin lists with their offsets: `SLASH_FIELD_BOOL` (`uint8_t`), `INT32`, `INT64`, `FLOAT`,
  `DOUBLE`, `VEC2` to `VEC4` (floats), `STRING` (a `char` array of the field's `size`, UTF-8 ending
  in 0) and `ENTITY` (a `SlashEntity`, saved as the entity's ID). Each has a doc and, for
  numbers, a range. The engine stores the structs, and scenes, prefabs, `undo`, `world.query`,
  `schema.get` and the scene schema treat them as the engine's own components, saving only the
  fields that differ from the struct of defaults the plugin gives.
- **Systems** are C functions the world calls each tick, by `priority` as engine systems are.
  `slash_world_each` hands them the entities with a set of the plugin's components;
  `slash_component_get_json` and `slash_component_set_json` read and write any component, the
  engine's too; `slash_world_emit` and `slash_world_events` send and read world events;
  `slash_input_pressed`, `slash_input_just_pressed` and `slash_input_strength` read input
  actions; `slash_entity_create`, `find`, `destroy` and `id` work with entities by their IDs.
- A plugin that registers anything wrong (fields that overlap or leave the struct, a name that
  isn't PascalCase or snake_case, a name another component or system has) adds nothing, and the
  error says which. A component's name is the same in every game, so prefix it with the plugin's.
- `engine.describe` lists each plugin with its components and systems. Plugins stay loaded until
  the program exits, and the engine calls the `deinitialize` the entry function sets as it lets
  go of them.

The tools load the library a file names for the machine they run on (macOS, Windows and Linux),
and so do exports' checks of the project's scenes; a plugin with no library for that machine,
or with `sources` and its library not built there, leaves its components unchecked. [`slash-cli export`](cli.md#export) ships each plugin file in
the game's data, and with it:

- A plugin with `sources`: compiled into the game, on every platform. The export writes a
  `slash_game_plugins` function naming the entry functions, and the game enters them without
  loading anything.
- Otherwise its library for the platform and the build's CPU: in the data folder at the path the
  file names on Windows and Linux, in the app bundle's `Contents/Frameworks` (signed with the
  game) on macOS, and in the APK's `lib/<abi>/` on Android, which needs it named
  `lib<name>.so`. The game looks for a library by its file name there. iOS and the web load no
  libraries, so there the file names a static library (`.a`), which is linked in and entered.

A plugin that can't ship for the platform (no library and no sources for it, a shared library
named for iOS or the web) fails the export with the file and the reason. An export's test
reports the `plugins` the game loaded. [examples/plugins](../../examples/plugins/) is a plugin
in C, with its source for exports.

## Input actions

`input` is the input map: actions by name, each with a `deadzone` and the `events` that press
it. Gameplay code asks about actions (`input::is_action_pressed(world, "jump")`), never keys, so
players can rebind them and agents can press them by name with
[`input.inject`](commands.md#inputinject). From [examples/input-actions](../../examples/input-actions/):

```json
"input": {
  "jump": {
    "events": [
      {"physical_key": "Space", "type": "key"},
      {"button": "a", "type": "joypad_button"}
    ]
  },
  "move_left": {
    "deadzone": 0.25,
    "events": [
      {"physical_key": "A", "type": "key"},
      {"key": "Left", "type": "key"},
      {"axis": "left_x", "axis_value": -1, "type": "joypad_motion"}
    ]
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `deadzone` | `0.2` | How far a stick or trigger must move, from 0 to 1, to press the action. Past it, the action's strength runs from 0 to 1 |
| `events` | `[]` | Up to 32 bindings, any of which presses the action |

Each binding has a `type` and the fields that say which key, button or axis:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `key` | `key` or `physical_key`, and `shift`, `ctrl`, `alt`, `meta` | A key by name (`"A"`, `"Space"`, `"Enter"`, `"Left"`, `"F5"`, `"Kp 1"`). `key` is what the layout labels it; `physical_key` is where it sits on a US QWERTY keyboard, so WASD stays in place on any layout. The modifiers named must be held; more are fine |
| `mouse_button` | `button`, and the modifiers | `left`, `right`, `middle`, `wheel_up`, `wheel_down`, `wheel_left`, `wheel_right`, `xbutton1` or `xbutton2` |
| `joypad_button` | `button` | `a`, `b`, `x`, `y` (the bottom, right, left and top face buttons), `back`, `guide`, `start`, `left_stick`, `right_stick`, `left_shoulder`, `right_shoulder`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, `misc1`, `paddle1` to `paddle4` or `touchpad` |
| `joypad_motion` | `axis`, `axis_value` | A stick or trigger moved one way: `left_x`, `left_y`, `right_x`, `right_y`, `trigger_left` or `trigger_right`, and `-1` or `1`. Sticks go from -1 to 1 with y down, so up is `left_y` `-1` |

Every binding also takes `device`: only events from that keyboard, mouse or gamepad, from 0.
Left out, it is `-1` and matches every device.

The built-in actions are always there unless an entry with the same name replaces one: each
has a deadzone of 0.5 and default bindings.

| Action | Bindings |
| --- | --- |
| `ui_accept` | Enter, Kp Enter, Space |
| `ui_select` | Gamepad Y, Space |
| `ui_cancel` | Escape |
| `ui_focus_next` | Tab |
| `ui_focus_prev` | Shift+Tab |
| `ui_left`, `ui_right`, `ui_up`, `ui_down` | The arrow keys, the D-pad and the left stick |
| `ui_page_up`, `ui_page_down`, `ui_home`, `ui_end` | PageUp, PageDown, Home, End |

[Text fields](scene-format.md#text-fields) and [`TextArea`s](scene-format.md#text-fields-of-several-lines)
edit with built-in text actions, bound to Ctrl
on Windows, Linux, Android and the web, and to Command (and Option for words) on macOS and iOS.
`input.state` and `input.actions` list them only when asked by
name.

| Action | Bindings on a PC | On a Mac |
| --- | --- | --- |
| `ui_cut`, `ui_copy`, `ui_paste` | Ctrl+X, Ctrl+C, Ctrl+V; Shift+Delete, Ctrl+Insert, Shift+Insert | Command instead of Ctrl |
| `ui_undo`, `ui_redo` | Ctrl+Z; Ctrl+Shift+Z, Ctrl+Y | Command instead of Ctrl |
| `ui_text_submit` | Enter, Kp Enter | The same |
| `ui_text_backspace`, `ui_text_delete` | Backspace (and Shift+Backspace), Delete | The same |
| `ui_text_backspace_word`, `ui_text_delete_word` | Ctrl+Backspace, Ctrl+Delete | Option+Backspace, Option+Delete |
| `ui_text_backspace_all_to_left`, `ui_text_delete_all_to_right` | None | Command+Backspace, Command+Delete |
| `ui_text_caret_left`, `_right`, `_up`, `_down` | The arrow keys | The same |
| `ui_text_caret_word_left`, `ui_text_caret_word_right` | Ctrl+Left, Ctrl+Right | Option+Left, Option+Right |
| `ui_text_caret_line_start`, `ui_text_caret_line_end` | Home, End | Command+Left, Command+Right, Ctrl+A, Ctrl+E, Home, End |
| `ui_text_caret_page_up`, `ui_text_caret_page_down` | PageUp, PageDown | The same |
| `ui_text_caret_document_start`, `ui_text_caret_document_end` | Ctrl+Home, Ctrl+End | Command+Up, Command+Down, Command+Home, Command+End |
| `ui_text_select_all` | Ctrl+A | Command+A |
| `ui_text_clear_carets_and_selection` | Escape | The same |
| `ui_swap_input_direction` | Ctrl+` | Command+` |
| `ui_text_newline` (TextArea) | Enter, Kp Enter, with or without Shift | The same |
| `ui_text_newline_blank`, `ui_text_newline_above` (TextArea) | Ctrl+Enter, Ctrl+Shift+Enter (and Kp Enter) | Command instead of Ctrl |
| `ui_text_indent`, `ui_text_dedent` (TextArea) | Tab, Shift+Tab | The same |
| `ui_text_scroll_up`, `ui_text_scroll_down` (TextArea) | Ctrl+Up, Ctrl+Down | Command+Option+Up, Command+Option+Down |
| `ui_text_select_word_under_caret` (TextArea) | Alt+G | Ctrl+Command+G |
| `ui_text_toggle_insert_mode` (TextArea) | Insert | The same |
| `ui_focus_mode` (TextArea) | Ctrl+M | The same |
| `ui_menu` | Menu | The same |

`project validate` checks every binding, naming a misspelled key, button or axis with the closest
one. Games read keys, the mouse, touches and gamepads from SDL; gamepads connect and disconnect as
they are plugged in, numbered from 0. A window losing focus releases everything held.

## Localization

`localization` holds the translation files games read and how their text and layout follow the
locale (see [`l10n.state`](commands.md#l10nstate) and
[Translated text](scene-format.md#translated-text)):

```json
"localization": {
  "translations": ["translations/menu.csv", "translations/pl.po"],
  "fallback": "en",
  "test": "",
  "pseudo": {"enabled": false, "expansion_ratio": 0.3},
  "layout_direction": "application_locale"
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `translations` | `[]` | Translation files by project path: `.csv`, `.po` or `.mo` |
| `fallback` | `en` | The locale whose translations answer where the game's locale has none; a message neither has shows as written, so a game can be written in any language |
| `test` | `""` | A locale to run in instead of the player's, to check a translation |
| `pseudo.enabled` | `false` | Every translated text as a fake translation that shows what isn't translated, what gets cut off and what can't go right to left |
| `pseudo.replace_with_accents` | `true` | Letters with accents: `Start` is `Ŝŧáŕŧ` |
| `pseudo.double_vowels` | `false` | Vowels doubled: `Start` is `Staart` |
| `pseudo.fake_bidi` | `false` | The text marked right to left |
| `pseudo.override` | `false` | Every character `*`, so text that isn't looked up stands out |
| `pseudo.expansion_ratio` | `0` | Underscores around the text adding this share of its length, 0 to 1 |
| `pseudo.prefix`, `suffix` | `[`, `]` | Around every text, so cut-off ends show |
| `pseudo.skip_placeholders` | `true` | `%s`, `%d` and the other placeholders kept as they are |
| `layout_direction` | `application_locale` | Which way root controls that inherit their direction lay out: `application_locale` (the game's locale), `system_locale` (the player's), `ltr` or `rtl` |
| `force_right_to_left` | `false` | Every control right to left but those set to `ltr` |

A game starts in the player's locale (the system's first preferred language) unless `test` names
one, and `l10n::set_locale` changes it while it runs. A locale takes the translations of the
locale that matches it best and has the message (`fr` answers `fr_CA`, `es` before `es_AR` for
`es_MX`). Sessions start in `test` or else `fallback`, never the machine's language, so their
replies repeat. Exports ship the files, and `project validate` and exports check each one loads.

The files read as follows:

- CSV: a header of `keys` then a locale per column (columns
  starting with `_` are left out), then a row per message. A `?context` column tells apart
  messages written the same; a `?plural` column gives a message's plural, with its other forms in
  the rows after it whose key is empty; a `?pluralrule` row gives a column a gettext plural rule.
  `"` quotes a field, `""` is a quote inside one, and `\n`, `\t`, `\"` and `\\` in translations
  stand for what they mean. A column with no valid locale is an error.
- gettext `.po` files: `msgctxt`, `msgid`, `msgid_plural`, `msgstr` and
  `msgstr[n]`, with the header's `Language` and `Plural-Forms`; fuzzy entries are left out, and
  plural messages need `Plural-Forms`. `.mo` files are the same compiled by `msgfmt`.

Plural forms follow the file's rule, else the locale's own, else English's. Placeholders such as
`%d` stay in the translated text for the game's code to fill.

## XR

`xr` holds the OpenXR settings:

```json
"xr": {
  "openxr": {"enabled": true, "reference_space": "local_floor"}
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `openxr.enabled` | `false` | Start OpenXR when a windowed game starts, on the runtime the Khronos loader finds (the active one, or `XR_RUNTIME_JSON`'s). Sessions start the [test headset](scene-format.md#xr) instead, which agents move with `xr.set` |
| `openxr.form_factor` | `"head_mounted"` | Or `"handheld"`: a phone or tablet held up to the room |
| `openxr.view_configuration` | `"stereo"` | Or `"mono"`: one view instead of one for each eye |
| `openxr.reference_space` | `"stage"` | Where the play space's origin is: where the head started (`local`, the `sitting` play area), the room's center on the floor (`stage`) or the floor under where the head started (`local_floor`, `roomscale`). `center_on_hmd` does nothing on a stage, which is the room's own |
| `openxr.environment_blend_mode` | `"opaque"` | How the picture blends with the room: not at all (virtual reality), `"additive"` or by its `"alpha"` (augmented reality) |
| `shaders.enabled` | `false` | Draw for headsets, each eye's view in one pass |

## Services

`services` names the ad placements, products, achievements and leaderboards a game's code shows,
sells, unlocks and submits to by name, through [the services API](services.md):

```json
"services": {
  "ads": {
    "placements": {
      "bonus": {
        "format": "rewarded",
        "ids": {"android": "android-bonus-unit", "ios": "ios-bonus-unit"},
        "reward": {"amount": 25, "type": "coins"}
      },
      "footer": {"format": "banner"}
    }
  },
  "analytics": {"enabled": true},
  "consent": {"analytics": "unknown", "personalized_ads": "unknown"},
  "games": {
    "achievements": {
      "first_win": {"ids": {"android": "CgkIexample", "ios": "first_win"}},
      "ten_wins": {"steps": 10}
    },
    "leaderboards": {
      "best_time": {"ids": {"ios": "best_time"}, "sort_order": "low_to_high"}
    }
  },
  "purchases": {
    "products": {
      "coins_100": {"type": "consumable"},
      "remove_ads": {"type": "non_consumable"}
    }
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `ads.placements` | | Ad placements by name (letters, digits, `_` and `-`) |
| `ads.placements.*.format` | required | `banner`, `interstitial` or `rewarded` |
| `ads.placements.*.ids` | | The ad network's ad unit ID on each platform: `android`, `ios`, `macos`, `windows`, `linux` or `web`. The ads backend loads the one for the platform the game runs on |
| `ads.placements.*.reward` | | What a watched rewarded ad gives when the network doesn't say: `{"type", "amount"}`. Only rewarded placements take one |
| `ads.placements.*.position` | `bottom_center` | Where a banner shows: `bottom_center`, `bottom_left`, `bottom_right`, `top_center`, `top_left`, `top_right` or `center`, as Unity's `BannerPosition`. Only banners take one; `set_banner_position` moves it while the game runs |
| `ads.placements.*.auto_load` | `true` | Load at start and again after each time the ad closes |
| `purchases.products` | | Products by the store's product ID, the same on every store: a letter or digit, then letters, digits, `_` and `.` |
| `purchases.products.*.type` | required | `consumable` (bought again once finished), `non_consumable` (owned for good) or `subscription` |
| `analytics.enabled` | `true` | Send analytics events to the backends while the player consents. Off, events are only kept |
| `analytics.endpoint` | | Send events, batched as JSON, to a server you run: `url`, `format` (`json` or `posthog`), `api_key`, `headers`, `batch_size`, `flush_seconds`, `session_minutes`, `max_queued` ([services](services.md#sending-to-your-own-server)) |
| `analytics.funnels` | | Funnels by name, each the event names a player goes through in order; each step reached is a `funnel_step` event ([services](services.md#funnels)) |
| `consent.analytics` | `unknown` | The player's analytics consent until the game sets it: events are sent only when `granted` |
| `consent.personalized_ads` | `unknown` | Ads are personalized only when `granted` |
| `consent.tracking_usage` | | Why the game asks to track the player, shown in Apple's App Tracking Transparency prompt (`NSUserTrackingUsageDescription` in iOS and macOS exports). `request_tracking` needs it there |
| `games.achievements` | | Achievements by name (letters, digits, `_` and `-`) |
| `games.achievements.*.ids` | | Its ID in Google Play Games (`android`) and Game Center (`ios`, `macos`). The games backend gets the one for the platform the game runs on |
| `games.achievements.*.steps` | | Steps that unlock an incremental achievement, 2 to 10000. Leave it out for one unlocked at once |
| `games.leaderboards` | | Leaderboards by name (letters, digits, `_` and `-`) |
| `games.leaderboards.*.ids` | | Its ID on each platform, as for achievements |
| `games.leaderboards.*.sort_order` | `high_to_low` | Which scores rank first: `high_to_low` or `low_to_high` (times), as set up in Play Console and App Store Connect |
| `games.sign_in_at_start` | `true` | Sign the player in without asking when the game starts, as the platforms do |

`project validate` names a misspelled platform with the closest one. Ad unit IDs, product IDs,
achievement and leaderboard IDs and placement names are not secrets; API keys an SDK needs are [environment
variables](sdk-format.md).

## Export presets

`export_presets.json`, next to `project.json`, holds named ways to export the game. It is
optional: without it, `slash-cli export <platform>` uses that platform's defaults. `slash-cli
schema presets` prints its JSON Schema and `slash-cli project validate` checks it.

```json
{
  "format": 1,
  "presets": {
    "mac-store": {
      "macos": {
        "codesign_identity": "Developer ID Application: Example (ABCDE12345)",
        "notarize": true
      },
      "output": "dist/mac",
      "platform": "macos"
    },
    "windows": {
      "platform": "windows",
      "windows": {"sign": true}
    }
  }
}
```

Preset names are 1 to 64 letters, digits, `-` or `_`. A preset named after a platform replaces
that platform's defaults, so `slash-cli export windows` above signs.

| Key | Default | Meaning |
| --- | --- | --- |
| `platform` | required | `macos`, `windows`, `linux`, `web`, `android` or `ios` |
| `output` | `build/export/<preset>` | Output folder, inside the project |
| `debug` | `false` | Debug build instead of Release |
| `icon` | `project.json`'s `icon` | A square image inside the project that this preset's [icons](#icons) are made from |
| `model_quality` | none | The quality tier, such as `"phone"`, that model recipes ship as: each recipe whose `quality` has it ships [as the tier makes it](scene-format.md#variants-and-quality-tiers), and the rest as written |
| `macos.codesign_identity` | `"-"` | Keychain identity to sign with; `"-"` signs ad hoc |
| `macos.entitlements` | none | Entitlements `.plist` to sign with, inside the project |
| `macos.notarize` | `false` | Notarize with Apple and staple the ticket; needs a Developer ID identity |
| `windows.sign` | `false` | Sign the executable with signtool |
| `windows.timestamp_url` | `http://timestamp.digicert.com` | RFC 3161 timestamp server, or `""` for none |
| `linux.arch` | `x86_64` | CPU architecture: `x86_64` or `arm64` |
| `android.abis` | `["arm64-v8a", "x86_64"]` | CPU architectures to build: `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86` |
| `android.package` | the project's `id` | Application ID; parts start with a letter and use letters, digits and `_` |
| `android.version_code` | from `version` | versionCode; by default major × 1000000 + minor × 1000 + patch, so `1.2.3` is `1002003` |
| `android.icon_foreground` | the icon, filling the middle 72 dp | The adaptive icon's foreground: a square image inside the project, full bleed at 432x432 pixels (108 dp), of which launchers show the middle 288 through their mask |
| `android.icon_background` | `"#ffffff"` | The adaptive icon's background: a color as `"#rrggbb"`, or a square image inside the project |
| `ios.team_id` | none | Apple Developer team ID, 10 capital letters and digits (developer.apple.com shows it under Membership) |
| `ios.destination` | `device` with a `team_id`, else `simulator` | `device` builds an `.ipa` for iPhones and iPads; `simulator` builds an `.app` for the iOS Simulator |
| `ios.bundle_id` | the project's `id` | Bundle ID; parts use letters, digits and `-` |
| `ios.codesign_identity` | the team's `Apple Development` identity (debug) or `Apple Distribution` (release) | Keychain identity for device builds, by name or SHA-1; `"-"` signs ad hoc with no profile, so it needs no team but the `.ipa` installs nowhere until re-signed |
| `ios.build_number` | from `version` | CFBundleVersion, which must grow with each upload of the same version to App Store Connect |
| `ios.icon_background` | `"#ffffff"` | The color behind the icon's transparent pixels, as `"#rrggbb"`, since iOS icons are opaque |
| `sdks` | every package with a section for the platform | The [SDK packages](sdk-format.md#in-projectjson-and-presets) to link, by name, such as an ad network only in store builds; `[]` links none |

A `macos`, `windows`, `linux`, `android` or `ios` section is allowed only in a preset for that
platform.
Secrets never go in the file, so it can be committed: a key that looks like one (`password`, `secret`, `token`,
`private`) is an error that points to the environment variables instead. Notarizing reads an App
Store Connect API key from `SLASH_APPLE_API_KEY_PATH`, `SLASH_APPLE_API_KEY_ID` and
`SLASH_APPLE_API_ISSUER`; Windows signing reads a `.pfx` certificate from
`SLASH_WINDOWS_CERTIFICATE_PATH` and its password from `SLASH_WINDOWS_CERTIFICATE_PASSWORD`.
Release Android exports are signed with the keystore in `SLASH_ANDROID_KEYSTORE_PATH`, its
password in `SLASH_ANDROID_KEYSTORE_PASSWORD`, the key alias in `SLASH_ANDROID_KEY_ALIAS` and the
key's password in `SLASH_ANDROID_KEY_PASSWORD` if it differs; without them, and for debug builds,
the debug keystore signs, which Google Play rejects. iOS device builds embed the provisioning
profile in `SLASH_IOS_PROVISIONING_PROFILE`, or else the installed profile (Xcode keeps them in
`~/Library/MobileDevice/Provisioning Profiles`) that covers the team and bundle ID and lasts
longest, preferring an exact bundle ID to a wildcard: a development profile for debug builds and
a distribution one (App Store or Ad Hoc) for release.

## Layout

```text
my-game/
  project.json
  export_presets.json   # optional
  sdks/                 # optional: SDK packages, each a folder with an sdk.json
    hello/sdk.json
  scenes/
    main.scene.json
  prefabs/              # optional: .prefab.json files that entities extend
    enemy.prefab.json
  sprites/              # optional: textures, .frames.json animations and .material.json
    hero.png            #   materials, in any folder
    hero.frames.json
    hit_flash.material.json
  animations/           # optional: .anim.json keyframe animations, in any folder
    hero.anim.json
  tiles/                # optional: .tileset.json tile sets and their atlas textures, in any
    ground.tileset.json #   folder
    ground.png
  fonts/                # optional: .ttf and .otf fonts, and .font.json files with settings,
    title.ttf           #   in any folder
    title.font.json
  src/                  # optional: the game's C++ code, as gameplay.sources names it
    game.cpp
  build/           # exports land in build/export/<preset>/ (gitignored)
  .slash/          # caches, export logs, a running server's token, the built gameplay
                   # library (gitignored)
```

Only `project.json`, `.scene.json`, `.prefab.json`, `.frames.json`, `.material.json`,
`.particles.json`, `.anim.json`, `.tileset.json`, `.font.json`, `.ttf`, `.otf`, `.png`
and `.bmp` files ship in an export, and each is checked first: a texture SDL can't read or a
font FreeType can't draw is an error, like a scene, material, process material, animation, tile
set or font settings file that fails validation, a tile map layer whose legend or rows don't
match its tile set, controls that don't lay out as written, or a sub-emitter that can't take
part in a chain. Components of the game's own code
are checked by the names and fields its sources declare. See
[cli.md](cli.md#export) for `slash-cli export` and [scene-format.md](scene-format.md) for scenes.
