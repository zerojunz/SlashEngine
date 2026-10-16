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
4. **An agent can observe the result.** Commands return structured JSON, logs are JSON lines,
   `frame.capture` draws what the window shows and `frame.summary` lists it as data.

There is no GUI editor. People use a text editor, their agent and the debug overlay that F3
shows in `slash-cli run`.
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
| `render` | The render device: SDL3's GPU API (Vulkan, Metal, Direct3D 12), the software device that draws on the CPU, or the null device for headless runs |
| `asset` | Asset handles, loaders and hot reload |
| `world` | The EnTT world, components, the component registry, scenes and prefabs |
| `input` | Input, as Godot's: key, mouse, touch and gamepad events from SDL, rebindable actions and the state gameplay code reads |
| `canvas` | 2D drawing, as Godot's canvas: sprites, `.frames.json` animations, `.material.json` materials, `.tileset.json` tile sets and tile map layers, and the renderer that draws them |
| `animation` | Keyframe animation, as Godot's AnimationPlayer: `.anim.json` files and the system that plays them |
| `physics2d` | 2D physics on Box2D, as Godot's: bodies, areas, shapes, joints and queries |
| `text` | Fonts and text layout: FreeType, HarfBuzz, distance field fonts and glyph atlases |
| `ui` | UI, as Godot's Control nodes: layout of controls and labels, and their draw calls |
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
each component type. The transforms place every entity; the
[built-in components](reference/scene-format.md#built-in-components) also draw sprites and tile
maps, play animations and link entities to the storyboard:

| Component | Fields | Units |
| --- | --- | --- |
| `Transform2D` | `position` [x, y], `rotation`, `scale` [x, y] | Pixels; y points down; rotation in radians, clockwise |
| `Transform3D` | `position` [x, y, z], `rotation` quaternion [x, y, z, w], `scale` [x, y, z] | Meters; y points up |

Units and axes follow Godot. Components are **reflected**: one `SLASH_COMPONENT(Type, fields...)`
line lets the engine save, load, validate and describe the struct. Registering a component with
the **component registry** adds its docs and value ranges, and from then on it appears in scene
files, the scene schema, `engine.describe` and every command, with no other glue. Field types can
be `bool`, integers, `float`, `double`, `std::string`, `Vec2`, `Vec2i`, `Vec3`, `Vec4`, `Quat`,
a reference to another entity, a `std::vector` of these, a `std::map` of them by name, or a
nested struct declared with `SLASH_REFLECT`.

A **system** is code that updates components every tick, added with `World::add_system` and an
optional doc string and priority. Systems run in order of priority, lowest first as with Godot's
`process_priority`, and in the order they were added when priorities are equal. `engine.describe`
lists them in that order. The built-in systems apply input, play animations, step physics and
run particles: `input` applies the input events queued since the last tick, first,
`animation_player` runs `AnimationPlayer` components, `physics2d` moves bodies and reports
contacts, `animated_sprite_2d` runs `AnimatedSprite2D` flipbooks, then `cpu_particles_2d`
emits and moves `CPUParticles2D` particles. More arrive with the feature milestones (audio).

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
file, a texture and, once audio arrives, sounds. Loading one returns a **handle**; every handle to
the same path shares one copy, which stays loaded while any handle exists. `.json`, `.txt`,
`.csv`, `.png`, `.bmp`, `.frames.json`, `.material.json`, `.anim.json`, `.tileset.json`, `.ttf`,
`.otf` and `.font.json` files load out of the box, and a game can add its own types.

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

### Replays

Because runs repeat, a run can be recorded and played back. `replay.record` (or
`slash-cli run --record`) writes a **replay**, a `.replay.json` file: the entities at the start,
each tick the host stepped, the input it queued and the commands that changed the world, in
order, with a hash of the world every 60 ticks and after each command. `replay.run` plays it
back in any session and says whether it matched, and if not, the first tick it went another way
and what differs at the end. With `"tick"` it stops partway, so every query works on the world
as it was then. `replay.seek` moves the playback from there: to a tick, by a number of ticks, or
to the first tick where an entity matches a condition. Forward goes on from where it is; back
plays the run again from its start. A player's bug report becomes a run an agent can step
through, and a recorded run becomes a test that fails when a change alters it.

Recording and playback both start the world afresh: what systems keep outside components (held
input, focus, particles, physics contacts, playing sounds) starts over, through the resets
modules add with `World::add_reset`. A gameplay system that keeps its own state does the same.

### Tests

A **test** is a scene and a list of command calls, each with what its reply must hold, kept in a
`.test.json` file. `test.run` (or `slash-cli test`) runs every test headless, each in a session
of its own from its scene file, and returns only the checks that failed: a command that failed,
a reply that didn't hold what was expected, or a warning or error logged while it ran. Each
failure points into the test file with a JSON Pointer and gives the tick. Since runs repeat, a
test that passes once passes everywhere, so agents check a change in one call instead of reading
logs or screenshots, and games run their tests in CI.

## Input

Input follows Godot. Keys, mouse buttons and motion, touches and gamepads arrive as **input
events**, which wait in a queue and apply at the start of the next tick, so a run with the same
events at the same ticks plays out the same way. Gameplay code reads **actions** rather than
keys: `project.json`'s `input` names what the game does, such as `jump` or `move_left`, and the
**bindings** that do it (keys by label or by where they sit on the keyboard, mouse buttons,
gamepad buttons and stick directions), on top of Godot's built-in `ui_*` actions for moving
through and pressing UI. An action is pressed while any binding holds it, and "just pressed" or
"just released" for the one tick it changed, so a tap shorter than a tick still counts. A stick
presses an action past its **deadzone**, with a strength from 0 to 1, and `get_vector` turns
four actions into a direction. Players rebind actions while the game runs, from an options
menu. A touch also clicks the mouse, so mouse code works on phones.

Games read the events from SDL. Agents send them with `input.inject`, by key, button, finger,
stick or action name, then `step` and read back what the game saw with `input.state`;
`input.actions` lists the bindings. The [input example](../examples/input-actions/) moves a
player with WASD, a stick and taps.

## Sound

Sound follows Godot's AudioServer. **Sounds** are `.wav`, `.ogg` and `.mp3` files; a
`.audio.json` file loops one, or picks one of several at random each time it plays, as footsteps
want. **Players** are components: an `AudioStreamPlayer` plays everywhere at once, as music
does, and an `AudioStreamPlayer2D` fades with distance from the `AudioListener2D` and pans left
or right by where it is. Each player mixes into a **bus**, and each bus runs its **effects**
(filters, EQs, delay, reverb, compression and the rest of Godot's) and sends to another bus,
down to Master, which plays on the speakers. The project's `.bus_layout.json` file lists the
buses, so music and sound effects get their own volumes, as in an options menu.

Sound is mixed by the fixed ticks, a tick's worth at a time, not by the sound card, so the same
run makes the same sound, and a headless run makes it too. Agents can't listen, so they measure:
`audio.state` shows each bus's level in dB and what each player plays, `audio.capture` reports a
bus's levels over the last seconds, slice by slice, or writes it to a `.wav` file for a person to
hear, and `audio.play`, `audio.stop` and `audio.set_bus` change what plays. The
[audio example](../examples/audio/) plays music and footsteps around a listener.

## Rendering

Everything draws through a **render device**, about a dozen calls: make and update textures,
begin a frame, draw triangles, end it, and read a texture's pixels back. On a GPU it is SDL3's GPU
API, which uses Metal, Direct3D 12 or Vulkan, whichever the machine has. Headless runs, and
machines where no backend starts, use the **null device**: it keeps textures and checks every
call exactly as the GPU device does, but draws nothing. So code tested headless fails the same
way on a GPU, and `slash-cli run` and `slash-game` report which device drew in their summary's
`renderer`.

The **software device** draws on the CPU, into textures only, with the same shading code as the
GPU device's shaders, so it gives the same pixels on every machine. `frame.capture` draws the
frame the window shows on it, returning pixel colors, a PNG to look at or a file: that is how an
agent checks what a player sees with no window or GPU. Tests draw on both devices and hold them
to the same pixels.

A **frame summary** (`frame.summary`) reads the same frame thing by thing instead of pixel by
pixel: each sprite, tile map layer, particle emitter and control that shows, where it is and how
much of it shows, what is covered, offscreen or transparent, and what can't draw, with a text
map of the window if asked. It answers "can the player see the coin?" in a few dozen tokens,
where an image costs hundreds and needs a model that sees.

Shaders are written once in HLSL. Vulkan's SPIR-V and Metal's MSL are compiled ahead of time
and committed, so building the engine needs no shader tools, and Direct3D 12 compiles the HLSL at
startup with the compiler that comes with Windows. The
[draw-offscreen example](../examples/draw-offscreen/) draws into a texture and reads it back,
which is how tests check what a game draws.

2D drawing follows Godot's canvas. A **sprite** (`Sprite2D`) draws a texture, or one frame of a
sprite sheet, at its entity's `Transform2D`: positions are pixels with y down, and sprites draw in
`z_index` order, then in the order their entities were made. An `AnimatedSprite2D` plays a
**flipbook** from a `.frames.json` file (Godot's SpriteFrames) on the sprite, frame by frame at
the animation's frames per second, and emits `animation_finished` when one that doesn't loop
ends. Textures are `.png` or `.bmp` files loaded as assets, so an edited image shows up at once.
The [sprites example](../examples/sprites/) plays and draws a scene's sprites.

A **material** colors a sprite's pixels, like a Godot `canvas_item` shader, but written as a node
graph in a `.material.json` file: ops such as `mul`, `mix` and `texture` over inputs (`uv`,
`color`, `time`, the sprite's texture) and **params** each sprite sets in `material_params`, plus
a **blend mode** (`mix`, `add`, `sub`, `mul`, `premul_alpha`). The graph compiles when the file
loads to a short program that one shader, compiled ahead of time like the rest, runs on the GPU,
so a game needs no shader compiler and a material reloads at once. The same program runs on the
CPU for `material.eval`, which returns the colors a material draws: an agent checks its work
without a screenshot. The [materials example](../examples/materials/) has three.

**Keyframe animation** follows Godot's AnimationPlayer. A `.anim.json` file holds animations by
name, each a length, a loop mode and **tracks**. A value track sets one component field, or one
part of it such as `Sprite2D/modulate/a`, from **keys** at times, eased between them (linear,
cubic, with Godot's easing curves); an event track emits an event as playback passes each key.
An entity's `AnimationPlayer` plays one animation on its own components, so the same file
animates any field of any component, a game's own included. `animation.eval` returns the values
an animation sets at any times, so an agent checks it without stepping the game. The
[keyframe animation example](../examples/keyframe-animation/) plays a walk cycle and a hit.

**Tile maps** follow Godot's TileSet and TileMapLayer. A `.tileset.json` file cuts atlas
textures into **tiles** and groups **terrains** (grass, water) into terrain sets; each terrain
tile's **peering** says which terrain its sides and corners must touch. A `TileMapLayer` keeps
its cells as text, rows of one-letter **legend** keys, so an agent reads a whole map in one call.
`tilemap.paint` paints a terrain and picks, for each cell and the cells around it, the tile whose
peering fits its neighbors, so shores and corners draw themselves (**autotiling**);
`tilemap.set` places single tiles and `tilemap.get` reads the rows back. A layer draws in runs of
one texture, so a layer from one atlas is one GPU draw. The
[tile maps example](../examples/tilemaps/) paints an island with a pond.

**2D lights** follow Godot's. Once a scene has a light or a `CanvasModulate`, sprites and tile
layers are lit: the `CanvasModulate` color is the **ambient** light, a `PointLight2D` shines from
its entity through a texture (a soft round one by default), and a `DirectionalLight2D` lights
everything from one direction, like the sun. Each light adds, subtracts or mixes its color into
what it reaches, and **light masks** and z ranges pick what that is. A sprite's **normal map**
gives each pixel its own facing, so light falls on bumps and bevels by their angle. A
`LightOccluder2D` polygon casts **shadows** from lights that have them on: the CPU casts each
shadowed light's occluders into one row of a **shadow map**, and the material shader reads it.
A material's `light_mode` keeps a sprite unshaded, for glowing signs, or shows it only where light
falls. `light.sample` returns how lit points are and which lights reach them, computed as the GPU
shades them, so an agent checks lighting without a screenshot. The
[2D lights example](../examples/lights-2d/) has a torch, the moon and a shadowed crate.

**UI** follows Godot's Control nodes. A `Control` places a rect in its parent's by **anchors**
(where each edge sits in the parent, from 0 to 1) and offsets in pixels, so a panel can stay
centered or a score stay in a corner on any window, and grows to fit its content. `Label` draws
text in the rect, wrapped, aligned and cut with an ellipsis as Godot's does, and `ColorRect`
fills it. **Containers** place their children instead of anchors: a box container stacks them
in a row or column, a grid puts them in columns, a flow container wraps them onto lines, and
others keep a margin round them, center them or draw a panel behind them. Each child's **size
flags** say whether it fills, expands into spare room or sits at the start, center or end. A
control's font, size, colors, outline, spacing and **style boxes** (the rounded, bordered or
nine-patch panels drawn behind controls) are **theme items**, which its `theme_override_*`
fields set one by one and a `.theme.json` **theme** sets for every control of a type at once:
on a control and everything in it, or on the whole game. Text is shaped with HarfBuzz, so every script's letters join and
place as they should, and drawn from glyph images FreeType makes, or from **distance fields**
that stay sharp at any size, for fonts a `.font.json` file sets up so. Controls draw above the
world. `ui.layout` returns every control's rect, the lines each label shows and mistakes such as
a misspelled theme override, `ui.theme` says where each of a control's theme items comes from,
and `text.measure` sizes text, so an agent checks a screen without a screenshot. The
[UI and text example](../examples/ui-and-text/) is a pause screen, and the
[UI containers example](../examples/ui-containers/) an inventory laid out by containers.

**Buttons** follow Godot's too. The `gui` system hands each tick's input events to controls as
Godot's viewport does: the pointer goes to the control drawn on top under it, and on to its
parents as each one's **mouse filter** says (stop, pass or ignore). A `Button` or
`TextureButton` is pressed by a click or tap that ends over it, by `ui_accept` while it has
**focus**, or by its **shortcut**; toggle buttons switch on and off, and a **button group** keeps
at most one of them on. One control has focus: a click gives it quietly, and the arrow keys,
Tab and a gamepad move it to the closest button that way and show it. Controls report what
happened as **UI events** (`pressed`, `toggled`, `mouse_entered`, `focus_entered` and the rest),
which gameplay code reads each tick and agents subscribe to. An agent clicks and types with
`input.inject`, presses a button by ID with `ui.press` and moves focus with `ui.focus`, and
`ui.layout` shows each button's **draw mode** (normal, hover, pressed, hover pressed or
disabled) and which control has focus and which is hovered. The
[UI buttons example](../examples/ui-buttons/) is a title menu worked by mouse, keys and touch.

**Check boxes** (`CheckBox`) and **check buttons** (`CheckButton`, a switch) are toggle buttons
that draw an **icon** beside their text instead of a box; check boxes in a button group draw as
radio buttons. A **slider** (`Slider`, Godot's `HSlider` or `VSlider` with `vertical`) holds a
value between a minimum and a maximum, always a multiple of its **step**: a click moves its
grabber there, dragging moves it, and the wheel, arrow keys and gamepads step it, each change
emitting `value_changed`. Icons are theme items like colors and style boxes; Godot's default
ones are drawn by the engine, named `builtin:checked`, `builtin:toggle_on` and so on, so a game
needs no image files for them. The [UI settings example](../examples/ui-settings/) is a
settings screen.

A **scroll container** (`ScrollContainer`) shows its children through its rect, **scrolled**,
with a **scroll bar** on each side they don't fit; the wheel, dragging the bars and a finger
scroll it, and with `follow_focus` it scrolls a control that gets focus into view. A
`ScrollBar` also works on its own, holding a value like a slider whose grabber is a page of the
range long. Controls **clip** their children to their rect when `clip_contents` is on, as a
scroll container always does: what is cut off neither draws nor takes clicks, and `ui.layout`
and `frame.summary` say what shows. The [UI scrolling example](../examples/ui-scrolling/) is a
quest log.

Text in every language follows Godot's text server. **BiDi** (the Unicode Bidirectional
Algorithm) orders each paragraph: Arabic and Hebrew go right to left, with English words and
numbers in them still left to right, and a label's `text_direction` can set the paragraph's
direction. A control's **layout direction** (`layout_direction` `rtl`) mirrors it and everything
in it, so an Arabic menu's anchors, rows and alignment start at the right. A character a font
lacks comes from its **fallback fonts**, listed in its `.font.json` file and tried in order
(those that say they are for the text's `language` and script first), then from fonts installed
on the machine, which `ui.layout` counts in `system_glyphs` since other machines may lack them.
The [text languages example](../examples/text-languages/) is a menu in Arabic laid out right to
left, with Hebrew, Japanese and Chinese text.

How often frames are drawn follows Godot. The world ticks at a fixed rate (`tick_hz`), while frames
come as fast as the **V-Sync mode** lets them (`window.vsync_mode`: wait for each refresh, present
at once, mailbox or adaptive) and no faster than the **frame cap** (`max_fps`, none by default). So
a 60 Hz world on a 120 Hz display runs a tick every other frame, and the frames in between draw each
sprite part of the way from where it was before the last tick to where it is
(`rendering.interpolation`), so motion stays smooth whatever the display's rate. A game's options
menu can change both while it runs, and so can an agent, with `display.set`; `display.list` lists
the displays and their refresh rates, and `perf.stats` shows whether frames arrive evenly. Headless
runs ignore both and run one tick per frame, as fast as they can. The [frame-pacing
example](../examples/frame-pacing/) uses all three commands.

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
(`.github/workflows/export.yml`), an empty `scenes/main.scene.json`, a starter storyboard, a
starter test (`tests/main.test.json`), and `.gitignore` entries for `.slash/` and `build/`. The
MCP server's own instructions repeat the key rules (call `engine_describe` first, use `batch`,
brief before a task and check after it, run the tests after a change, don't hand-edit scenes
while the server runs) to every client that connects.

## Model agnostic

Nothing in SlashEngine depends on one AI vendor. Agents connect through open protocols (MCP,
JSON-RPC, HTTP), and any model that can call tools can drive the engine. A later milestone adds an
`ai::Provider` interface for in-game features that call a model, with adapters for
OpenAI-compatible servers (including local ones), Anthropic and Gemini; see the
[design](design.md#model-agnostic-provider-interface).

## Glossary

| Term | Meaning |
| --- | --- |
| Action | A named thing a game does, such as `jump`, and the input bindings that press it, as Godot's input map |
| Ambient light | The color lit draws start from where no light reaches: the `CanvasModulate` color |
| Anchor | Where a control's edge sits in its parent's rect, from 0 (left or top) to 1 (right or bottom) |
| Animation | A `.anim.json` file's named set of tracks, which an `AnimationPlayer` plays on its entity's fields |
| Autotiling | Picking each tile so its edges connect to its neighbors' terrains, as `tilemap.paint` does |
| Asset | A file a game loads by project path into a shared, reference-counted handle |
| Audio effect | What a bus does to its sound before sending it on: a filter, EQ, delay, reverb, compressor and the rest of Godot's |
| BiDi | The Unicode Bidirectional Algorithm, which orders text that mixes right-to-left scripts such as Arabic with left-to-right ones |
| Binding | A key, mouse button, gamepad button or stick direction that presses an action |
| Button group | Toggle buttons with the same `button_group` name, of which at most one is on |
| Bus | A channel players mix into, with a volume and effects, that sends on to another, down to Master |
| Blend mode | How a sprite's color combines with what is under it: `mix`, `add`, `sub`, `mul` or `premul_alpha` |
| Canonical form | The one way a scene is written: sorted keys, two-space indent, defaults left out |
| Capability | A permission a session grants: `read`, `edit`, `run`, `filesystem` or `build` |
| Check box | A toggle button drawn with a box beside its text (`CheckBox`), or a switch (`CheckButton`); in a button group, a radio button |
| Clip | Cutting a control's children off at its rect, in drawing and for the pointer (`clip_contents`, and every `ScrollContainer`) |
| Command | A named action with a params schema, capabilities and an effect |
| Command registry | The list of commands every front end is generated from |
| Component | A reflected C++ struct of data attached to an entity |
| Component registry | The list of component types a world knows, with their docs |
| Control | A UI element: a rect placed in its parent's by anchors and offsets, or by a container, which `Label`, `Panel` and the other drawing components draw in |
| Container | A control that places its children itself, in a row, a grid, lines that wrap, a margin or the center |
| Draw mode | How a button draws for its state: `normal`, `hover`, `pressed`, `hover_pressed` or `disabled` |
| Deadzone | How far a stick must move, from 0 to 1, before it presses an action |
| Decibels (dB) | How loud, on a log scale: 0 dB is full scale, -6 dB about half, and each 20 dB down a tenth |
| Discovery file | `.slash/server.json`: a running server's URL and token |
| Distance field font | A font drawn from one field per glyph of distances to its outline (MSDF), sharp at any size, scale and outline |
| Effect | How a command changes the world: read, edit, replace, history or compound |
| Entity | A thing in the world, named by a stable ID |
| Example | A small program or sequence of calls in `examples/`, run by CI with its output checked |
| Export | A build of a project for one platform, ready for players |
| Flipbook | An animation that shows a sprite's frames one after another, from a `.frames.json` file |
| Focus | The one control that gets the keyboard's and gamepad's `ui_*` actions first, which the arrow keys and Tab move |
| Fallback font | A font a `.font.json` file lists for the characters it lacks, tried in order |
| Font | A `.ttf` or `.otf` file, or a `.font.json` file naming one with settings |
| Frame summary | What the window shows, listed thing by thing as data: rects, how much shows, what is hidden and why |
| Front end | A way to reach the commands: MCP, JSON-RPC, REST or the CLI |
| Hot reload | Reloading edited files, and updating prefab instances, while the game runs |
| Input event | A key, mouse, touch, gamepad or action event, queued for the next tick |
| Interpolation | Drawing sprites part of the way between two ticks, so motion is smooth at any frame rate |
| ID | An entity's stable name: 1 to 64 letters, digits, `_` or `-` |
| Journal | `storyboard/journal.jsonl`: who changed which storyboard entries, when and why |
| JSON Pointer | A path to a value inside JSON, such as `/entities/player/components` (RFC 6901) |
| Key | A track's value, or event, at one time in an animation |
| Layout direction | Which way a control lays out: `rtl` mirrors its anchors, offsets, containers and alignment |
| Legend | A tile map layer's keys, each standing for one tile of its tile set |
| Light mask | Bits saying which lights reach a sprite or tile layer: those whose cull mask shares one |
| Mouse filter | Whether a click or tap stops at a control, goes on to its parent too, or passes it by |
| Main scene | The scene a game loads at startup, named in `project.json` |
| Material | A `.material.json` node graph that colors a sprite's pixels, with params each sprite sets |
| Normal map | A texture whose colors give each pixel of a sprite its own facing, so light falls on it by angle |
| Occluder | A polygon that casts shadows from 2D lights (`LightOccluder2D`) |
| Peering | The terrain a tile's side or corner must touch for the tile to fit, as Godot's terrain peering bits |
| Prefab | One entity's components in a `.prefab.json` file, which entities extend and override |
| Project | A game's folder: `project.json` plus its scenes |
| Project folder | The folder all command paths are relative to |
| Reflection | Field metadata from `SLASH_COMPONENT` or `SLASH_REFLECT` that drives saving, schemas and commands |
| Replay | A run recorded into a `.replay.json` file, which `replay.run` plays back exactly |
| Resource | Read-only data an MCP client can fetch: the scene, its schema or the logs |
| Scene | A world saved as a `.scene.json` file |
| Scroll bar | A bar whose grabber shows which part of a range is in view and moves it (`ScrollBar`, and a scroll container's own) |
| Scroll container | A control that shows its children scrolled through its rect, with scroll bars when they don't fit (`ScrollContainer`) |
| Session | One world with its open scene path, undo history and recent logs |
| Shadow map | A texture with one row per shadowed light, holding how far away the nearest occluder is in each direction |
| Shortcut | A binding or action that presses a button from anywhere, such as Escape for Quit |
| Size flags | How a container's child fills its cell: fill it, expand into spare room, or sit at the start, center or end |
| Slider | A control holding a value between a minimum and a maximum on its step, set by dragging its grabber, clicks, the wheel, keys and gamepads (`Slider`) |
| Software device | The render device that draws on the CPU, the same on every machine, which `frame.capture` uses |
| Sprite | A texture, or one frame of a sprite sheet, drawn at an entity's `Transform2D` (`Sprite2D`) |
| Sprite sheet | One texture holding a grid of frames, `hframes` across and `vframes` down |
| Storyboard | A game's facts that are not code, as typed entries in `storyboard/*.json` |
| StoryRef | The component that names the storyboard entries an entity stands for |
| Style box | What a panel or bar draws behind its content, a flat box with optional rounded corners, border and shadow or a nine-patch texture, and the margin its content keeps |
| System | Code that updates components every tick, in order of priority |
| Terrain | A kind of ground in a tile set, such as grass or water, whose tiles autotiling connects |
| Test | A scene and command calls with what their replies must hold, in a `.test.json` file, which `test.run` runs headless |
| Theme | A `.theme.json` file of theme items by control type, used by a control and everything in it, or by the whole game (`gui.theme`) |
| Theme item | A named color, constant, font, font size, icon or style box a control draws with, set by its `theme_override_*` fields or a theme |
| Tick | One fixed simulation step, 1/60 s by default |
| Tile set | A `.tileset.json` file: atlas textures cut into tiles, and the terrains that connect them |
| Tile map layer | A grid of tiles from a tile set, kept as rows of legend keys (`TileMapLayer`) |
| Tool name | A command's MCP name, with `_` in place of `.` |
| UI event | Something a control did, as Godot's signals: `pressed`, `toggled`, `button_down`, `button_up`, `mouse_entered`, `mouse_exited`, `focus_entered` or `focus_exited` |
| Track | One field an animation sets, or one kind of event it emits, over time, as keys |
| World | Every entity in a running game, held in EnTT |
