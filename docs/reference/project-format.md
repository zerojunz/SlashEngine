# Project format

A game is a folder with a `project.json` at its root, plus its scenes. The file works like
Godot's `project.godot`: it names the game and says which scene starts it. `slash-cli init`
writes one, `slash-cli schema project` prints its JSON Schema, and `slash-cli project validate`
checks it.

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
| `window.vsync_mode` | No | `enabled` | How frames wait for the display, as in Godot. `enabled` waits for each refresh, so nothing tears. `disabled` presents at once: the lowest latency, but it may tear. `mailbox` shows the newest finished frame at each refresh: low latency with no tearing. `adaptive` waits unless a frame is late. A mode the GPU or window lacks falls back to `enabled`; SDL's GPU API has no adaptive mode yet, so `adaptive` always does |
| `window.linux_display_driver` | No | `x11` | The display server a Linux game tries first, as Godot's `display_server/driver.linuxbsd`: `x11`, which Wayland desktops run through XWayland, or `wayland`. If it can't start, the game tries the other. The `SDL_VIDEO_DRIVER` environment variable, such as `SDL_VIDEO_DRIVER=wayland`, overrides it. On Wayland the compositor draws the window's title bar, which GNOME doesn't do; that is why `x11` comes first |
| `max_fps` | No | `0` | Frames per second at most, 0 to 1000, e.g. 30, 60, 120 or 240. `0` means no cap, so V-Sync alone paces frames. Headless runs ignore it |
| `rendering.clear_color` | No | `[0.3, 0.3, 0.3, 1]` | What the window shows behind everything, as `[r, g, b, a]` from 0 to 1. The default is Godot's gray |
| `rendering.driver` | No | `auto` | The GPU backend. `auto` takes the first that works: Metal on Apple platforms, Direct3D 12 then Vulkan on Windows, Vulkan on Linux and Android, and on the web WebGPU where the browser has it, else WebGL 2. `vulkan`, `metal` or `d3d12` asks for one, and `webgpu` asks for the browser's WebGPU (web exports only). The OpenGL drivers are Godot's compatibility renderer: `opengl` is OpenGL 3.3 on Windows, macOS and Linux, and OpenGL ES 3.0 where that doesn't start; `opengl_es` is OpenGL ES 3.0, as Godot's `opengl3_es`, which is all the web (WebGL 2), Android and iOS have, so there both drivers are it. `null` draws nothing. If none starts, the game logs a warning and runs with the null device |
| `rendering.fallback_to_opengl` | No | `true` | Draw with `opengl` where the driver's GPU backend (`auto`'s, or the one `vulkan`, `metal` or `d3d12` names) doesn't work, as Godot's `rendering_device/fallback_to_opengl3`: an old GPU or driver, a virtual machine, a remote desktop. The game asks before it makes the window, logs a warning when it falls back, and reports `opengl` (or `opengl_es`) as its `renderer` |
| `rendering.rendering_method` | No | `forward_plus` | How 3D meshes are lit and their colors finished, as Godot's `rendering/renderer/rendering_method`: see [Rendering methods](#rendering-methods) |
| `rendering.texture_filter` | No | `linear` | How sprites whose `texture_filter` is `inherit` are sampled when scaled: `linear` is smooth, `nearest` keeps pixel art sharp |
| `rendering.scaling` | No | no scaling | Draw the world at another size and scale it to the window, under controls drawn at full size: see [Render scale](#render-scale) |
| `rendering.lights_and_shadows` | No | 4096-texel atlases, `soft_low` | The 3D shadow atlases' sizes and edge filters: see [Shadows](#shadows) |
| `rendering.camera.depth_of_field` | No | `hexagon`, `low`, no jitter | The bokeh shape and quality of 3D [depth of field](scene-format.md#cameraattributespractical): see [Depth of field](#depth-of-field) |
| `rendering.environment.ssao` | No | `medium`, half size, 2 blur passes | The quality and cost of 3D [ambient occlusion](scene-format.md#ambient-occlusion): see [SSAO](#ssao) |
| `rendering.environment.screen_space_reflection` | No | half size | The resolution of 3D [screen-space reflections](scene-format.md#screen-space-reflections): see [Screen-space reflections](#screen-space-reflections) |
| `rendering.reflections.reflection_atlas` | No | 256 texels, 64 probes | The size of 3D [reflection probes'](scene-format.md#reflection-probes) cubes and how many a frame draws: see [Reflection probes](#reflection-probes) |
| `rendering.global_illumination.voxel_gi.quality` | No | `low` | How many cones meshes trace through [VoxelGIs](scene-format.md#voxelgi): see [VoxelGI](#voxelgi) |
| `rendering.textures` | No | `lossless` | How exports ship textures: as their files, or compressed for each platform's GPUs: see [Texture forms](#texture-forms) |
| `rendering.interpolation` | No | `true` | Draw sprites part of the way between fixed ticks, from where they were before the last tick to where they are, so motion looks smooth when the display refreshes faster than the game ticks. Drawing lags the simulation by up to one tick; set `false` to draw exactly where things are |
| `physics_2d.gravity` | No | `980` | Gravity's strength in pixels per second squared, as Godot's `physics/2d/default_gravity` |
| `physics_2d.gravity_vector` | No | `[0, 1]` | Gravity's direction, as `[x, y]`; y points down |
| `physics_2d.linear_damp` | No | `0.1` | How fast rigid bodies slow down, per second. A body's own `linear_damp` adds to it unless its `linear_damp_mode` is `replace` |
| `physics_2d.angular_damp` | No | `1` | How fast rigid bodies stop turning, per second, combined the same way |
| `physics_3d.gravity` | No | `9.8` | Gravity's strength in meters per second squared, as Godot's `physics/3d/default_gravity` |
| `physics_3d.gravity_vector` | No | `[0, -1, 0]` | Gravity's direction, as `[x, y, z]`; y points up |
| `physics_3d.linear_damp` | No | `0.1` | How fast 3D rigid bodies slow down, per second, combined with a body's own as in 2D |
| `physics_3d.angular_damp` | No | `0.1` | How fast 3D rigid bodies stop turning, per second, combined the same way |
| `physics_3d.areas_detect_static_bodies` | No | `false` | Whether `Area3D` reports static bodies, as Godot's `physics/jolt_physics_3d/simulation/areas_detect_static_bodies` |
| `navigation_2d.cell_size` | No | `1` | The 2D map's cell size in pixels, as Godot's `navigation/2d/default_cell_size`: edge ends are rounded to it, so make navigation polygons for it |
| `navigation_2d.merge_rasterizer_cell_scale` | No | `1` | Edge ends closer than `cell_size` times this (at most 0.1) are the same point, as Godot's |
| `navigation_2d.use_edge_connections` | No | `true` | Whether regions' edges that come within `edge_connection_margin` join |
| `navigation_2d.edge_connection_margin` | No | `1` | How close, in pixels, two regions' edges must come to join |
| `navigation_2d.link_connection_radius` | No | `4` | How far, in pixels, a link's ends reach for a region |
| `navigation_3d.cell_size`, `navigation_3d.cell_height` | No | `0.25` | The 3D map's cell size and height in meters, as Godot's `navigation/3d/default_cell_size` and `default_cell_height` |
| `navigation_3d.merge_rasterizer_cell_scale`, `use_edge_connections` | No | `1`, `true` | As in 2D |
| `navigation_3d.edge_connection_margin` | No | `0.25` | How close, in meters, two regions' edges must come to join |
| `navigation_3d.link_connection_radius` | No | `1` | How far, in meters, a link's ends reach for a region |
| `navigation_3d.up` | No | `[0, 1, 0]` | Which way is up for paths, as Godot's `navigation/3d/default_up` |
| `gui.theme` | No | | A [`.theme.json`](scene-format.md#themes) file every control uses after its own and its parents' themes, before Godot's default theme, as Godot's `gui/theme/custom`. `project validate` checks that it exists and `export` checks and ships it |
| `gui.allow_system_fallback` | No | `true` | Let text take characters its fonts and their [fallbacks](scene-format.md#fonts) lack from fonts installed on the machine. Off, text lays out the same on every machine, as on the web, which has none; a `.font.json` file's `allow_system_fallback` turns them off for one font |
| `gui.tooltip_delay_sec` | No | `0.5` | Seconds the pointer rests on a control before its [tooltip](scene-format.md#tooltips-and-drag-and-drop) shows, as Godot's `gui/timers/tooltip_delay_sec` |
| `gui.tooltip_position_offset` | No | `[10, 10]` | Where a tooltip shows from the pointer, as Godot's `display/mouse_cursor/tooltip_position_offset` |
| `gui.drag_threshold` | No | `10` | Pixels the pointer moves with the left button held before a [drag](scene-format.md#tooltips-and-drag-and-drop) starts, as Godot's `Viewport.gui_drag_threshold` |
| `audio.bus_layout` | No | | A [`.bus_layout.json`](scene-format.md#bus-layouts) file with the buses players mix into, as Godot's `audio/buses/default_bus_layout`. Without one, everything plays through Master. `project validate` checks it and `export` checks and ships it |
| `audio.mix_rate` | No | `44100` | Frames of sound mixed per second, 8000 to 192000, as Godot's `audio/driver/mix_rate`. Sounds at other rates are resampled |
| `audio.output_latency` | No | `15` | Milliseconds of sound queued ahead of the speakers, 1 to 500, as Godot's `audio/driver/output_latency`. More is steadier when frames run late; less answers sooner |
| `audio.panning_strength_2d` | No | `0.5` | How far [`AudioStreamPlayer2D`](scene-format.md#audio) sounds pan left and right, 0 to 4, as Godot's `audio/general/2d_panning_strength`. Each player's `panning_strength` multiplies it |
| `audio.speaker_mode` | No | `"auto"` | The speakers to mix for: `stereo`, `surround_31`, `surround_51`, `surround_71`, or `auto` for the output device's, as Godot asks its audio driver; stereo where nothing plays the sound, as headless. See [surround sound](scene-format.md#surround-sound) |
| `audio.panning_strength_3d` | No | `0.5` | How far [`AudioStreamPlayer3D`](scene-format.md#3d-sound) sounds pan left and right, 0 to 4, as Godot's `audio/general/3d_panning_strength`. Each player's `panning_strength` multiplies it |
| `audio.enable_input` | No | `false` | Let [microphone](scene-format.md#microphone) players record, as Godot's `audio/driver/enable_input`. Exports then ask for the microphone: `NSMicrophoneUsageDescription` in macOS and iOS apps, the `RECORD_AUDIO` permission on Android, and the `com.apple.security.device.audio-input` entitlement for a Mac app signed with a Developer ID and no `entitlements` file of its own (one that has its own needs that key) |
| `audio.input_usage_description` | No | "*name* listens to the microphone." | Why the game records, which macOS and iOS show when they ask the player for the microphone. App Store review expects it to say what the game does with it |
| `gameplay.sources` | No | | The game's own [C++ code](#gameplay-code): folders and files relative to the project, at least one |
| `sdks` | No | | [SDK packages](sdk-format.md) exports link: folders relative to the project, each with an `sdk.json`. `slash-cli sdk add` adds one |
| `services` | No | | [Ad placements, products, analytics, consent, achievements and leaderboards](#services) |
| `threading.max_threads` | No | `-1` | Worker threads the [job system](../concepts.md#world-entities-and-components) runs threaded systems, particles and physics on beside the main thread, 0 to 63, as Godot's `threading/worker_pool/max_threads`. `-1` is one per CPU core; `0` runs everything on the main thread, as the web does. Every count gives the same results |
| `input` | No | Godot's `ui_*` actions | [Input actions](#input-actions) by name, each with the events that press it |
| `input_devices.pointing.emulate_mouse_from_touch` | No | `true` | A touch also clicks the left mouse button and dragging it moves the mouse, so mouse code works on phones, as Godot's setting of the same name |
| `input_devices.pointing.emulate_touch_from_mouse` | No | `false` | A left click also touches the screen and dragging with it drags a finger, so touch code can be tried with a mouse |

Unknown keys are errors, with a "did you mean" suggestion for a misspelled one.

A running game can change `vsync_mode` and `max_fps` with
[`display.set`](commands.md#displayset), and its own code with `RenderDevice::set_vsync` and
`Loop::set_max_fps`, e.g. from an options menu. For variable refresh rate (G-Sync, FreeSync,
ProMotion), keep V-Sync enabled and, as Godot suggests, cap the frame rate a little below the
display's refresh rate. Exports ask phones for their fastest refresh rate: iOS apps set
`CADisableMinimumFrameDurationOnPhone`, so iPhones with ProMotion run at 120 Hz, and Android apps
pick the display's fastest mode at its current resolution. On the web the browser calls each
frame at the display's rate, and a frame cap sets how often it calls instead.

## Render scale

`rendering.scaling` draws the world at a lower (or higher) resolution than the window and scales
it to the window's size, as Godot's `rendering/scaling_3d` settings. Godot scales only its 3D
world, and draws 2D at full size; until the 3D renderer exists, this scales the 2D world, so
the Godot importer leaves a project's `scaling_3d` settings out. Controls, and canvas layers
above the world, draw at the window's own size on top, so the UI stays sharp while the world
costs fewer pixels; canvas layers below the world are scaled with it.

```json
"rendering": {"scaling": {"mode": "fsr", "scale": 0.5, "fsr_sharpness": 0.2}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `mode` | `bilinear` | `bilinear` stretches the world with filtering. `fsr` is AMD's FidelityFX Super Resolution 1: EASU scales up along the edges it finds, then RCAS sharpens. Godot's `fsr2`, `metalfx_spatial` and `metalfx_temporal` need the 3D renderer's depth and motion vectors, so `project validate` refuses them for now |
| `scale` | `1` | The world's size over the window's, 0.25 to 2, as Godot's `scaling_3d/scale`. At `1` with `bilinear` nothing is scaled. Above `1` the world draws larger and is averaged down, always with `bilinear`, as FSR only scales up |
| `fsr_sharpness` | `0.2` | RCAS's sharpening in stops, 0 to 2: `0` is the sharpest, `2` the softest |
| `upscaler` | | An upscaler the game adds with `render::add_upscaler` (from its own C++ or an [SDK package's](sdk-format.md) glue code), used instead of `mode` on the GPUs it supports. Lowercase letters, digits and underscores. Where it isn't added or doesn't run, the game logs a warning once and uses `mode` |

The world draws at the window's size in pixels times `scale`, rounded down: a 2560x1440 window at
`0.5` draws a 1280x720 world. Both upscalers are drawn with the render device's own calls, so
they work on every backend and on the software device behind `frame.capture`, which returns the
`upscaler` that ran and the `render_size`. A running game changes the settings with
[`display.set`](commands.md#displayset)'s `scaling`, and
[examples/render-scale](../../examples/render-scale/) does.

Upscalers a game adds are `render::Upscaler` classes (`slash/render/upscaler.hpp`): `input`
gives the render target the world draws into, `upscale` runs the upscaler's passes and returns
the texture and material the window's frame draws over the screen, and `release` frees what it
made on the device. `render::UpscalerInfo::supports` says which devices one runs on, such as a
vendor's on its own GPUs. One that is `temporal` (DLSS, XeSS, FSR 2 and later) needs depth,
motion vectors and a jittered camera, so `add_upscaler` refuses it until the 3D renderer makes
them.

## Rendering methods

`rendering.rendering_method` picks how 3D [lights](scene-format.md#lights) reach meshes and how
their colors are [finished](scene-format.md#worldenvironment), as Godot's renderers do:

| Method | Lights | How | Colors |
| --- | --- | --- | --- |
| `forward_plus` | 8 directional and 512 omni and spot | Godot's Forward+ clusters: each frame, the CPU sorts the omni and spot lights nearest first and marks the 32-pixel tiles of the screen, and the 32 slices of depth out to the camera's far plane, that each light's range reaches. A pixel adds the directional lights, then only the lights of its tile and slice, so a plaza of hundreds of lanterns costs each pixel the few near it | Linear, in a texture of 16-bit floats: brighter than white glows through up to seven levels, then a pass tonemaps the frame |
| `gl_compatibility` | The first 64 of any kind, the directional ones first | Godot's Compatibility renderer: every pixel adds each light | Tonemapped as meshes draw, or through an 8-bit texture with glow or adjustments |

The OpenGL drivers (`opengl`, `opengl_es`, and WebGL 2 on the web) always draw as
`gl_compatibility`, as Godot's do. Every other device, and `frame.capture`, draws as the project
says; both methods light a pixel the same where a scene has 64 lights or fewer, and finish its
color within a step or two of each other where nothing glows and nothing is transparent.

## Shadows

`rendering.lights_and_shadows` sizes the two atlases 3D lights draw their
[shadow maps](scene-format.md#lights) into and picks how their edges are filtered, as Godot's
`rendering/lights_and_shadows` settings.

```json
"rendering": {"lights_and_shadows": {
  "directional_shadow": {"size": 4096, "soft_shadow_filter_quality": "soft_low"},
  "positional_shadow": {"atlas_size": 2048, "soft_shadow_filter_quality": "hard"}
}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `directional_shadow.size` | `4096` | The atlas directional lights' cascades share, in texels across: a power of two from 256 to 16384 |
| `directional_shadow.soft_shadow_filter_quality` | `soft_low` | `hard` takes one sample a pixel; `soft_very_low` and `soft_low` 5, filtered between texels; `soft_medium`, `soft_high` and `soft_ultra` 13, softer still |
| `positional_shadow.atlas_size` | `4096` | The atlas omni and spot lights share, as `size` |
| `positional_shadow.soft_shadow_filter_quality` | `soft_low` | As the directional one's |

A device draws an atlas at most as large as its largest texture (8192 texels for WebGPU), and a
frame with no shadowed light draws none. Larger atlases make sharper shadows and cost GPU
memory: a 4096-texel atlas takes 64 MB. Godot's Compatibility renderer splits the positional
atlas into quadrants of different tile sizes; here every tile is the same size.

## Depth of field

`rendering.camera.depth_of_field` says how a camera's
[depth of field](scene-format.md#cameraattributespractical) blurs, as Godot's
`rendering/camera/depth_of_field` settings.

```json
"rendering": {"camera": {"depth_of_field": {
  "depth_of_field_bokeh_shape": "circle", "depth_of_field_bokeh_quality": "medium"
}}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `depth_of_field_bokeh_shape` | `hexagon` | The shape out-of-focus lights spread into: `box` blurs across then down, `hexagon` along three directions, and `circle` gathers a spiral of samples, the softest and the slowest |
| `depth_of_field_bokeh_quality` | `low` | `very_low`, `low`, `medium` or `high`: more samples each pass (6, 12, 12 and 24). The box and hexagon blur at half size at `very_low` and `low`, and the circle always does |
| `depth_of_field_use_jitter` | `false` | Turns the samples a little each frame by game time, trading banding for noise |

## SSAO

`rendering.environment.ssao` says how a `WorldEnvironment`'s
[ambient occlusion](scene-format.md#ambient-occlusion) is worked out, as Godot's
`rendering/environment/ssao` settings.

```json
"rendering": {"environment": {"ssao": {"quality": "high", "half_size": false}}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `quality` | `medium` | `very_low`, `low`, `medium`, `high` or `ultra`: more samples each pixel. From `high` on, samples further out read depth at lower resolutions, and `ultra` takes up to 32 where an importance map says occlusion changes |
| `half_size` | `true` | Works at a quarter of the frame's resolution in each direction, rather than half, and blurs the result back up: faster, and softer |
| `adaptive_target` | `0.5` | `ultra`: how many of the extra samples to take, from 0 to 1 |
| `blur_passes` | `2` | Edge-aware blurs of the occlusion, from 0 to 6; `very_low` always blurs once |
| `fadeout_from`, `fadeout_to` | `50`, `300` | Occlusion fades out from this distance and is gone at that one, in meters |

## Screen-space reflections

`rendering.environment.screen_space_reflection` says how a `WorldEnvironment`'s
[screen-space reflections](scene-format.md#screen-space-reflections) are worked out, as Godot's
`rendering/environment/screen_space_reflection` settings.

```json
"rendering": {"environment": {"screen_space_reflection": {"half_size": false}}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `half_size` | `true` | Traces at half the frame's resolution in each direction, then resolves the reflections to its size along its depths and normals: faster, and softer at edges |

## Reflection probes

`rendering.reflections.reflection_atlas` sizes the atlas of cubes that
[reflection probes](scene-format.md#reflection-probes) are drawn into, as Godot's
`rendering/reflections/reflection_atlas` settings. Every probe's cube is the same size; a larger
one shows sharper reflections and costs more to draw and keep.

```json
"rendering": {"reflections": {"reflection_atlas": {"reflection_size": 512, "reflection_count": 16}}}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `reflection_size` | `256` | Texels across each face of a probe's cube: 32, 64, 128, 256, 512, 1024 or 2048 |
| `reflection_count` | `64` | How many probes a frame draws, from 1 to 4096, at most 42 here; the largest are drawn first and the rest reflect nothing |

## VoxelGI

`rendering.global_illumination.voxel_gi.quality` says how meshes trace their ambient light
through [VoxelGIs](scene-format.md#voxelgi), as Godot's
`rendering/global_illumination/voxel_gi/quality`: `low` traces four cones of 45 degrees, `high`
six narrower ones, which show finer detail and cost more.

```json
"rendering": {"global_illumination": {"voxel_gi": {"quality": "high"}}}
```

## Temporal antialiasing

`rendering.anti_aliasing.quality.use_taa` turns on temporal antialiasing for the 3D world in
`forward_plus`, as Godot's `rendering/anti_aliasing/quality/use_taa`. Each frame moves the
camera by less than a pixel, along the next of 16 points of a Halton sequence. Meshes write how
far each pixel moved since the frame before, from their transforms, skins and the camera's. A
pass then blends the frame with the frames before it, moved by those motion vectors, and clips
the old colors to those around each pixel so that moving things leave little trail. Edges smooth
out over about 16 frames; details thinner than a pixel, which most frames miss, fade instead.

```json
"rendering": {"anti_aliasing": {"quality": {"use_taa": true}}}
```

TAA is off by default, as in Godot. It leaves the 2D canvas, controls and `gl_compatibility`
alone (Godot's Compatibility renderer has no TAA), and reflection probes capture without it. A
frame with no meshes starts the history again. `frame.capture` keeps the history between
captures, and its `frames` draws several at once.

## Texture forms

`rendering.textures` says how exports ship textures, as Godot's texture import `compress/mode`.
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
| `compress_mode` | `lossless` | `lossless` ships the file as it is, decoded when it loads, as Godot's default for 2D. `vram_compressed` ships blocks GPUs sample as they are, a quarter of the GPU memory of RGBA8: BC7 on macOS, Windows and Linux, ASTC 4x4 on Android and iOS, and both on the web, whose browsers have one or the other. `vram_uncompressed` ships RGBA8 texels, as big on disk as in memory but ready for the GPU |
| `paths` | | Modes for folders (ending in `/`) and files, such as `{"ui/": "lossless", "ui/atlas.png": "vram_compressed"}`; the longest one that holds a texture wins |

BC7 and ASTC are blocks of 4x4 texels, so a texture whose sides aren't multiples of 4 ships as it
is, and the export logs a warning. Compressing keeps the colors close rather than exact, so pixel
art and UI often stay `lossless`; [`texture.forms`](commands.md#textureforms) makes the forms
ahead of an export and reports each one's size and PSNR, so an agent can choose. A device that
doesn't sample a form's format decodes it when the texture loads, so every form draws everywhere.
Mipmaps aren't made yet.

## Icons

`icon` names a square image, as Godot's `application/config/icon`. Give it 1024x1024 pixels:
the App Store asks for that size, and smaller images are scaled up. Exports make each
platform's icons from it, resized with a Lanczos filter:

| Platform | What the export ships |
| --- | --- |
| macOS | `AppIcon.icns` in the bundle's `Resources`, as Godot writes one: PNG images from 1024 to 32 pixels, and 32 and 16 pixel ones run-length encoded with masks. An opaque icon is drawn into Apple's rounded square, 824 of 1024 pixels with corners rounded 185, as macOS 11 icons are; one with transparent pixels has its own shape and stays as it is |
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

A game's own components and systems are plain C++, as Godot's scripts are; there is no scripting
language. `gameplay.sources` names where the code is: every `.cpp` file in a folder and its
subfolders is compiled, and so is each `.cpp` file listed. The folders, and the folders of
listed headers, are searched for includes.

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
`.slash/gameplay/game.so` with [`slash-cli build`](cli.md#build) or
[`gameplay.build`](commands.md#gameplaybuild) and load that, on macOS and Linux; when it is
rebuilt they swap it in, and the world keeps its entities and each field the new code still has.
Libraries built against other engine headers or with another compiler are refused. Until the
code is built, each component a `SLASH_COMPONENT` line in the sources declares is a stand-in
that keeps whatever values scenes give it, so their scenes open, change and save as written;
the first build swaps the code's types in, keeping the values of the fields they have.

Code is built as the engine is, with no exceptions or RTTI. Keep the game's state in registered
components, so scenes save it, `undo` restores it and rebuilding keeps it: values of components
the code never registered, and anything kept in globals, are lost when the code is rebuilt, and
the world's context (`World::ecs().ctx()`) must hold no values of the game's own types. A
component or system named like one of the engine's is refused.

## Input actions

`input` is Godot's input map: actions by name, each with a `deadzone` and the `events` that press
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
| `key` | `key` or `physical_key`, and `shift`, `ctrl`, `alt`, `meta` | A key by Godot's name (`"A"`, `"Space"`, `"Enter"`, `"Left"`, `"F5"`, `"Kp 1"`). `key` is what the layout labels it; `physical_key` is where it sits on a US QWERTY keyboard, so WASD stays in place on any layout. The modifiers named must be held; more are fine |
| `mouse_button` | `button`, and the modifiers | `left`, `right`, `middle`, `wheel_up`, `wheel_down`, `wheel_left`, `wheel_right`, `xbutton1` or `xbutton2` |
| `joypad_button` | `button` | `a`, `b`, `x`, `y` (the bottom, right, left and top face buttons), `back`, `guide`, `start`, `left_stick`, `right_stick`, `left_shoulder`, `right_shoulder`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, `misc1`, `paddle1` to `paddle4` or `touchpad` |
| `joypad_motion` | `axis`, `axis_value` | A stick or trigger moved one way: `left_x`, `left_y`, `right_x`, `right_y`, `trigger_left` or `trigger_right`, and `-1` or `1`. Sticks go from -1 to 1 with y down, so up is `left_y` `-1` |

Every binding also takes `device`: only events from that keyboard, mouse or gamepad, from 0.
Left out, it is `-1` and matches every device.

Godot's built-in actions are always there unless an entry with the same name replaces one: each
has a deadzone of 0.5 and Godot's bindings.

| Action | Bindings |
| --- | --- |
| `ui_accept` | Enter, Kp Enter, Space |
| `ui_select` | Gamepad Y, Space |
| `ui_cancel` | Escape |
| `ui_focus_next` | Tab |
| `ui_focus_prev` | Shift+Tab |
| `ui_left`, `ui_right`, `ui_up`, `ui_down` | The arrow keys, the D-pad and the left stick |
| `ui_page_up`, `ui_page_down`, `ui_home`, `ui_end` | PageUp, PageDown, Home, End |

[Text fields](scene-format.md#text-fields) and [`TextEdit`s](scene-format.md#text-fields-of-several-lines)
edit with Godot's built-in text actions, bound to Ctrl
on Windows, Linux, Android and the web, and to Command (and Option for words) on macOS and iOS,
as Godot's `.macos` bindings. `input.state` and `input.actions` list them only when asked by
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
| `ui_text_newline` (TextEdit) | Enter, Kp Enter, with or without Shift | The same |
| `ui_text_newline_blank`, `ui_text_newline_above` (TextEdit) | Ctrl+Enter, Ctrl+Shift+Enter (and Kp Enter) | Command instead of Ctrl |
| `ui_text_indent`, `ui_text_dedent` (TextEdit) | Tab, Shift+Tab | The same |
| `ui_text_scroll_up`, `ui_text_scroll_down` (TextEdit) | Ctrl+Up, Ctrl+Down | Command+Option+Up, Command+Option+Down |
| `ui_text_select_word_under_caret` (TextEdit) | Alt+G | Ctrl+Command+G |
| `ui_text_toggle_insert_mode` (TextEdit) | Insert | The same |
| `ui_focus_mode` (TextEdit) | Ctrl+M | The same |
| `ui_menu` | Menu | The same |

`project validate` checks every binding, naming a misspelled key, button or axis with the closest
one. Games read keys, the mouse, touches and gamepads from SDL; gamepads connect and disconnect as
they are plugged in, numbered from 0. A window losing focus releases everything held.

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
| `ads.placements.*.auto_load` | `true` | Load at start and again after each time the ad closes |
| `purchases.products` | | Products by the store's product ID, the same on every store: a letter or digit, then letters, digits, `_` and `.` |
| `purchases.products.*.type` | required | `consumable` (bought again once finished), `non_consumable` (owned for good) or `subscription` |
| `analytics.enabled` | `true` | Send analytics events to the backends while the player consents. Off, events are only kept |
| `consent.analytics` | `unknown` | The player's analytics consent until the game sets it: events are sent only when `granted` |
| `consent.personalized_ads` | `unknown` | Ads are personalized only when `granted` |
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

`export_presets.json`, next to `project.json`, holds named ways to export the game, like Godot's
`export_presets.cfg`. It is optional: without it, `slash-cli export <platform>` uses that
platform's defaults. `slash-cli schema presets` prints its JSON Schema and
`slash-cli project validate` checks it.

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
`.process_material.json`, `.anim.json`, `.tileset.json`, `.font.json`, `.ttf`, `.otf`, `.png`
and `.bmp` files ship in an export, and each is checked first: a texture SDL can't read or a
font FreeType can't draw is an error, like a scene, material, process material, animation, tile
set or font settings file that fails validation, a tile map layer whose legend or rows don't
match its tile set, controls that don't lay out as written, or a sub-emitter that can't take
part in a chain. Components of the game's own code
are checked by the names and fields its sources declare. See
[cli.md](cli.md#export) for `slash-cli export` and [scene-format.md](scene-format.md) for scenes.
