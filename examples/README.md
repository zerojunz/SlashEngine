# Examples

Small working examples, one per feature. CI builds each one and runs it headless with its
output checked exactly, so the code and calls here are known to work with this version of the
engine. Copy from them.

| Example | Shows |
| --- | --- |
| [components-and-systems](components-and-systems/) | A game's own component and a system that moves it, in C++ |
| [gameplay-code](gameplay-code/) | A game's own C++ code in `src/`, built with `gameplay.build` and changed while the game runs: the world keeps its entities and fields, and compiler errors come back with file and line |
| [sdk-package](sdk-package/) | A closed vendor SDK in an `sdk.json` package: per-platform libraries, defines, Info.plist and Android manifest entries and keys from the environment; `sdk.check` lists its problems, `sdk.add` adds another, and exports build and link it |
| [custom-commands](custom-commands/) | A game's own command with a params schema and undo, served like the built-in ones |
| [edit-a-scene](edit-a-scene/) | Creating, changing, finding and saving entities, `batch` and `undo` |
| [prefabs](prefabs/) | Entities that extend a `.prefab.json` file and override fields |
| [instanced-scenes](instanced-scenes/) | Entities that instance a `.scene.json` file and bring its entities with them: a player scene with a gun scene inside it, its entities' IDs under the instance's, overrides and left-out entities, and an edit to the scene reaching every instance |
| [try-and-roll-back](try-and-roll-back/) | Dry runs, and `world.snapshot`, `world.diff` and `world.restore` |
| [events](events/) | `events.subscribe` and `events.read` to hear what changed |
| [run-until](run-until/) | `run.until`: run a game headless until something happens, in one call |
| [storyboard](storyboard/) | Briefing before a task, editing facts with versions, and checking after |
| [palette-and-budgets](palette-and-budgets/) | `storyboard.check` holding textures to a `palette`'s colors and a `budget`'s `texture_size`: an off-palette sprite and a title over budget found, fixed and checked again |
| [import-godot](import-godot/) | `project.import` (`slash-cli import`): a Godot 4 platformer's scenes, sprite frames, animations, tile set, theme, buses, input map and settings converted in one call, its scripts turned into storyboard tasks to port to C++, and the report of what didn't convert |
| [import-unity](import-unity/) | `project.import` (`slash-cli import`): a Unity 2D game's scene, prefabs and prefab variant, sprites, a tile map and its tile set, an Animator controller and its clips, cameras, 2D physics and a hinge, 2D lights and a shadow caster, sounds and an audio mixer, a HUD canvas, input and settings converted in one call, measured in pixels, its C# scripts turned into storyboard tasks to port to C++, and the report of what didn't convert |
| [import-unity-isometric](import-unity-isometric/) | `project.import` of Unity's isometric and hexagonal tile maps: each Grid becomes a tile set of Godot's shape and layout, each layer moves so its cells stay where Unity draws them, and tile anchors and sprite pivots become `texture_origin`, checked with `frame.capture` |
| [draw-offscreen](draw-offscreen/) | The renderer's device: drawing into a texture and reading the pixels back |
| [web-rendering](web-rendering/) | How web exports draw: the OpenGL ES 3.0 device (WebGL 2 in browsers, the `opengl` driver elsewhere) drawing a scene with a material into a texture, the same pixels as the software device |
| [frame-pacing](frame-pacing/) | V-Sync, the frame cap and frame pacing: `display.list`, `display.set` and `perf.stats` |
| [parent-transforms](parent-transforms/) | Entities in another's `Transform2D` `children`: placed in its space, drawn after it with its `z_index`, visibility and tint, bodies moved with it, `entity.get`'s `parent` and `global`, a `top_level` child left where it was, `z_as_relative` off, refused loops and `entity.destroy` taking children along |
| [camera-2d](camera-2d/) | `Camera2D`: a view that follows the player with drag margins and smoothing, stays inside the level's limits and zooms in, with the coin counter staying put and `frame.summary`'s `camera` |
| [canvas-layers](canvas-layers/) | `CanvasLayer` and `Parallax2D`: hills that scroll at a quarter of the camera's speed and repeat to fill the window, drifting clouds, a HUD layer that stays put, and `canvas.convert` turning a click into the world |
| [sprites](sprites/) | `Sprite2D`, sprite sheets and `AnimatedSprite2D` flipbooks from a `.frames.json` file, drawn offscreen |
| [materials](materials/) | `.material.json` node graphs that color sprites, their params and blend modes, and `material.eval` |
| [custom-shaders](custom-shaders/) | A game's own HLSL shaders with `fragment()` and `vertex()`, run by `material.eval` and `frame.capture` and reloaded on edit |
| [keyframe-animation](keyframe-animation/) | `AnimationPlayer` and `.anim.json` files: tracks for any field, on the player's entity or another it targets, easing, loops, event keys and `animation.eval` |
| [tilemaps](tilemaps/) | `TileMapLayer` and `.tileset.json` files: terrain autotiling with `tilemap.paint`, `tilemap.set` and `tilemap.get`, drawn offscreen |
| [isometric-and-hex-tiles](isometric-and-hex-tiles/) | Isometric and hexagonal tile sets with Godot's `tile_shape`, `tile_layout` and `tile_offset_axis`: an island painted on diamonds by their sides and corners, a board of hexagons, trees lifted by their `texture_origin`, `tilemap.convert` between cells and points, checked with `frame.capture` |
| [animated-tiles](animated-tiles/) | Animated tiles and a tile two cells wide, as Godot's: `animation_frames`, `animation_columns`, random start times per cell, `size_in_atlas` and `texture_origin`, checked with `frame.capture` as ticks pass and at a given `time` |
| [tile-data-and-occluders](tile-data-and-occluders/) | Custom data and occluders on tiles, as Godot's: `custom_data_layers` read with `tilemap.get`'s `cells`, walls and a pillar whose `occlusion` polygons shadow a torch, checked with `light.sample` and `frame.capture`, and `occlusion_enabled` |
| [tile-collision](tile-collision/) | Collision polygons on tiles, physics layers, one-way tiles, cells turned with `:h`, an alternative tile, and `physics.raycast` reporting the cell |
| [lights-2d](lights-2d/) | `PointLight2D`, `DirectionalLight2D`, `CanvasModulate`, occluder shadows, normal maps and `light.sample`, drawn offscreen |
| [lit-tiles-and-layers](lit-tiles-and-layers/) | Normal and specular maps on a tile set's atlas source, lights' `range_layer_min` and `range_layer_max` across canvas layers, a `CanvasModulate` per canvas and `light.sample` on a HUD sprite, checked with `frame.capture` |
| [physics-2d](physics-2d/) | Static, rigid and character bodies, areas, concave and one-way shapes, a joint, and `physics.raycast`, `physics.overlap` and `physics.contacts` |
| [gravity-and-conveyors](gravity-and-conveyors/) | `Area2D` gravity and damping overrides by priority, a planet's point gravity, a conveyor belt made with a `StaticBody2D`'s `constant_linear_velocity`, and `physics.gravity` |
| [particles](particles/) | `CPUParticles2D`: emission shapes, curves and color ramps, a one-shot burst, preprocess, local coordinates and `particles.eval` |
| [gpu-particles](gpu-particles/) | `GPUParticles2D` and `.process_material.json` files: a fire, a one-shot burst that is the same each time with `use_fixed_seed`, snow already falling with `preprocess`, `particles.eval` running the steps on the CPU, `frame.capture` and the device running them |
| [ui-and-text](ui-and-text/) | `Control`, `Label` and `ColorRect`: anchors, wrapping and trimmed text, fonts and theme overrides, `ui.layout` and `text.measure`, drawn offscreen |
| [ui-containers](ui-containers/) | Box, grid, flow, center, margin and panel containers, `.theme.json` themes and variations, style boxes, `ProgressBar`, `TextureRect`, `NinePatchRect` and `ui.theme`, drawn offscreen |
| [ui-buttons](ui-buttons/) | `Button` and `TextureButton`: clicks, taps, hover, focus moved by keys and gamepads, toggle groups, shortcuts, UI events, `ui.press` and `ui.focus`, drawn offscreen |
| [ui-scrolling](ui-scrolling/) | `ScrollContainer`, `ScrollBar` and `clip_contents`: a quest log scrolled by the wheel, its scroll bar and `follow_focus`, a scroll bar dragged, a map clipped to its panel and `ui.layout`'s scroll fields |
| [ui-menus](ui-menus/) | `OptionButton`, `MenuButton` and `PopupMenu`: a menu bar that switches on hover, a list picked by keys, a context menu, `ui.popup`, `ui.select` and the lists in `ui.layout` and `frame.summary` |
| [ui-drag-and-drop](ui-drag-and-drop/) | Tooltips and `DragAndDrop`: an inventory whose items drag between slots and into a trash bin with a preview, a tooltip shown after a rest, a drag cancelled by a right click, gameplay code that handles `drop_data`, `ui.drag`, `ui.tooltip` and the tooltip and drag in `ui.layout` and `frame.summary` |
| [ui-settings](ui-settings/) | `CheckBox`, `CheckButton` and `Slider`: check boxes, a radio group, a switch, a volume slider dragged, clicked and stepped by keys, values set from outside, the built-in theme icons and `ui.layout`'s values |
| [unpack-godot](unpack-godot/) | `project.import` of a game exported with Godot 4: its `.pck` file unpacked back into the Godot project it was made from (binary scenes as `.tscn` text, script tokens as source, `project.binary`, textures and sounds), then converted in the same call |
| [unpack-unity](unpack-unity/) | `project.import` of a game built with Unity: AssetRipper (run as a program of its own, so it needs the `build` capability) rebuilds its Unity project, Unity's package scripts get their GUIDs back, sprites saved as assets keep their atlas rects and pixels per unit, and the project converts in the same call |
| [text-input](text-input/) | `LineEdit`: clicks, typing, an input method's composition, editing actions, undo, the clipboard, secret text, `max_length`, on-screen keyboards, `ui.clipboard` and the text events, drawn offscreen |
| [text-editor](text-editor/) | `TextEdit`: typing lines, wrapping, double-click selection, undo and redo, tabs, scrolling with the view following the caret, the text events and `ui.layout`'s `text_edit`, drawn offscreen |
| [code-editor](code-editor/) | `TextEdit` with a `CodeHighlighter` and a `CodeEdit`: code colored, line numbers, breakpoints clicked in and moving with their lines, `search_text` marked, `ui.search` and dragging its scroll bar |
| [text-languages](text-languages/) | Right-to-left text and layout (`layout_direction`, `text_direction`), fallback fonts in `.font.json` files, fonts per language, installed fonts and `text::box_font` placeholders |
| [input-actions](input-actions/) | Rebindable input actions in `project.json`, Godot's `ui_*` actions, sticks with deadzones, taps, touch as a mouse, and `input.inject`, `input.state` and `input.actions` |
| [audio](audio/) | `AudioStreamPlayer` and `AudioStreamPlayer2D`: a `.bus_layout.json` file's buses and effects, a randomizer in a `.audio.json` file, panning and fading by distance, the `finished` event, and `audio.state`, `audio.play`, `audio.set_bus` and `audio.capture` |
| [audio-areas-and-analysis](audio-areas-and-analysis/) | An `Area2D` sending the sounds inside it to a reverb bus, the `pitch_shift`, `spectrum_analyzer`, `capture` and `record` effects, and `audio.spectrum` and `audio.record` |
| [interactive-music](interactive-music/) | Interactive music that switches clips on the beat or bar when a player's `switch_to_clip` parameter asks, synchronized layers and a playlist, all `.audio.json` files |
| [generated-audio](generated-audio/) | A generator a gameplay system pushes a tune into each tick, and a polyphonic player that plays hits started with `audio.play`'s `stream`, each at its own volume and pitch |
| [microphone](microphone/) | A microphone player on a muted bus with a spectrum analyzer and a record effect, a gameplay system that reads the input and lights a lantern for each half second of singing, and `audio.input` singing into it headless |
| [frame-capture](frame-capture/) | `frame.capture`: what the window shows, drawn on the CPU the same on every machine, as pixel colors, a PNG to look at or a file |
| [replays](replays/) | `replay.record`, `replay.stop`, `replay.run` and `replay.seek`: record a played run to a `.replay.json` file, play it back exactly in another session, seek the moment something happened and the tick before, and see where a changed game first differs |
| [debug-overlay](debug-overlay/) | The F3 panel over a running game (frame stats, Pause, Step, the bodies under the mouse) and collision shapes drawn as Godot's Visible Collision Shapes, in the panel and in `frame.capture` |
| [headless-tests](headless-tests/) | `.test.json` tests and `test.run` (`slash-cli test`): each test opens a scene and runs commands with what their replies must hold; a run returns only the checks that failed, such as a reply that doesn't match or a warning logged |
| [frame-summary](frame-summary/) | `frame.summary`: what the window shows as data instead of an image (what shows where and how much, what is covered, offscreen or transparent, what can't draw) and a text map of the frame |

## What an example holds

- `README.md`: what it shows and what to notice.
- `main.cpp`: a C++ program, built as `example-<folder>`. Its output is `expected.txt`.
- `calls.jsonl`: JSON-RPC requests for `slash-cli rpc --project .`, the same calls an agent makes
  as MCP tools (`entity.create` is the tool `entity_create`). `results.jsonl` holds the
  responses, line for line.
- `project.json`, scenes, prefabs and a storyboard when the example is a game project. It must
  pass `slash-cli project validate`.

## Running and adding examples

`ctest --preset dev -R example` runs every example in a copy of its folder under
`build/dev/examples/`, and fails if any output differs from what is checked in.

To add one, make a folder with a `README.md` and a `main.cpp` or `calls.jsonl`, link it in the
table above, then write its expected output and read it before committing:

```bash
cmake --preset dev && cmake --build --preset dev
SLASH_UPDATE_EXAMPLES=1 ctest --preset dev -R example_<folder>
```

Every new feature ships with an example here.
