# Importing Godot projects

[`project.import`](commands.md#projectimport) and [`slash-cli import`](cli.md#import) convert a
Godot 4 project into a SlashEngine project in one pass. Most of it maps one to one, because
SlashEngine's components already use Godot's names, fields and enums. Scripts don't convert,
since SlashEngine games are plain C++: each becomes a storyboard task to port it, with what the
port needs. [examples/import-godot](../../examples/import-godot/) imports a small platformer.

Only Godot 4 projects import (`config_version=5` in `project.godot`). Open a Godot 3 project in
Godot 4 and save it there first; Godot converts it.

## What an import writes

| File | From |
| --- | --- |
| `<scene>.scene.json` beside each `<scene>.tscn` | The scene's nodes, as entities and components |
| `<name>.frames.json` | Each `SpriteFrames` resource: a `.tres` file's beside it, a scene's own as `<scene>_<id>.frames.json` |
| `project.json` | `project.godot`'s settings and input map, merged into the project's own if it has one |
| `storyboard/work.json` | A task per script, merged into the project's own tasks |
| `import/godot.json` | The full report: every node's entity, what each script needs and everything that didn't convert |

Textures (`.png`, `.bmp`), sounds (`.wav`, `.ogg`, `.mp3`), fonts (`.ttf`, `.otf`) and data files
(`.json`, `.txt`, `.csv`) are used as they are. When the Godot folder is inside the project,
they stay where they are and the converted files go beside the originals (`res://art/a.png` in
`godot/` is `godot/art/a.png`). When it is anywhere else, they and the scripts are copied in at
their `res://` paths. Godot's hidden folders (`.godot/`), `addons/` and `build/` are skipped.
`uid://` references resolve through the files' `.uid` and `.import` files and the scenes'
headers, so a resource that moved still resolves.

Importing again rewrites the converted files and keeps each task's status and the project's
other settings, so a project can be imported, ported for a while, and imported again after the
Godot version changes.

## Scenes

Each node becomes an entity named after it in snake case: `PlayerSprite` is `player_sprite`. A
name that is taken is prefixed with its parent's ID, then numbered, and the nodes an instanced
scene brings are prefixed with the instance's ID (`Player/Camera2D` from `player.tscn` is
`player_camera2d`). The report maps every node path to its entity.

Node2D nodes nest through their parent's `Transform2D` `children` and controls through their
parent's `Control` `children`, in Godot's order, so they draw in Godot's order. A root that is a
plain `Node`, and plain nodes under it such as a `CanvasLayer`, get a `Transform2D` to hold
their children in place. Controls draw on the screen above the world, as in a `CanvasLayer`.

| Godot node | Components |
| --- | --- |
| `Node`, `CanvasLayer` | `Transform2D` under a plain root, else none |
| `Node2D`, `Marker2D` | `Transform2D` |
| `Sprite2D`, `Camera2D`, `CollisionShape2D`, `CollisionPolygon2D`, `PointLight2D`, `DirectionalLight2D`, `LightOccluder2D`, `CanvasModulate`, `CPUParticles2D`, `AudioStreamPlayer2D`, `AudioListener2D` | `Transform2D` and the component of the same name |
| `AnimatedSprite2D` | `Transform2D`, `AnimatedSprite2D` and `Sprite2D` |
| `StaticBody2D`, `AnimatableBody2D`, `RigidBody2D`, `CharacterBody2D`, `Area2D` | `Transform2D` and the body; its `CollisionShape2D` children are its shapes |
| `PinJoint2D`, `DampedSpringJoint2D`, `GrooveJoint2D` | `Transform2D` and the joint |
| `AudioStreamPlayer` | `AudioStreamPlayer` |
| `Control` | `Control` |
| `Label`, `ColorRect`, `Panel`, `TextureRect`, `NinePatchRect`, `ProgressBar`, `Button`, `CheckBox`, `CheckButton`, `TextureButton`, `LineEdit`, `TextEdit`, `ScrollContainer`, `MarginContainer`, `CenterContainer`, `PanelContainer`, `GridContainer` | `Control` and the component of the same name |
| `HBoxContainer`, `VBoxContainer`, `BoxContainer` | `Control` and `BoxContainer` (`vertical` for `VBoxContainer`) |
| `HFlowContainer`, `VFlowContainer`, `FlowContainer` | `Control` and `FlowContainer` |
| `HSlider`, `VSlider` | `Control` and `Slider` |
| `HScrollBar`, `VScrollBar` | `Control` and `ScrollBar` |

Each property goes to the field of the same name: `position`, `rotation`, `scale` and `skew` to
`Transform2D`, `offset_left` to `Control`, `texture` to `Sprite2D`. Values convert by the field's
type:

- Enums are stored as numbers in Godot and names here, so `horizontal_alignment = 1` is
  `"center"` and `size_flags_horizontal = 3` is `"expand_fill"`.
- `NodePath`s name entities: a joint's `node_a = NodePath("../Floor")` is `"floor"`.
- Textures, sounds and fonts are project paths, and a `SpriteFrames` resource is its
  `.frames.json` file.
- `theme_override_colors/font_color` and the other theme overrides go in `Control`'s maps;
  `theme_override_styles` take `StyleBoxFlat`, `StyleBoxTexture` and `StyleBoxEmpty`
  resources in a theme's form.
- Shape, occluder, curve, gradient, `LabelSettings` and `PhysicsMaterial` resources are written
  into the fields they set: a `CollisionShape2D`'s `shape`, `size`, `radius`, `height`, `a`, `b`
  or `points`; a `LightOccluder2D`'s `polygon`, `closed` and `cull_mode`; particles' `_curve`
  points and color ramps; a label's theme overrides; a body's `friction` and `bounce`.
- `region_enabled` and `region_rect` are `Sprite2D`'s `region`; `self_modulate` multiplies
  into its `modulate`; an `AnimatedSprite2D` plays only with `autoplay`, as in Godot.

An instanced scene's nodes are copied into the scene that instances it, with that scene's
overrides; its own `.scene.json` file is written too. A value that doesn't fit its field is left
out and reported, so one bad value can't stop a scene.

## Project settings

| `project.godot` | `project.json` |
| --- | --- |
| `application/config/name` | `name` |
| `application/run/main_scene` | `main_scene`, the scene's `.scene.json` |
| `application/run/max_fps` | `max_fps` |
| `display/window/size/viewport_width`, `viewport_height` | `window.width`, `window.height` |
| `display/window/vsync/vsync_mode` | `window.vsync_mode` |
| `physics/common/physics_ticks_per_second` | `tick_hz` |
| `physics/2d/default_gravity`, `default_gravity_vector`, `default_linear_damp`, `default_angular_damp` | `physics_2d` |
| `rendering/environment/defaults/default_clear_color` | `rendering.clear_color` |
| `rendering/textures/canvas_textures/default_texture_filter` | `rendering.texture_filter` |
| `[input]` | `input` |

Each input action keeps its deadzone and its keys (`physical_keycode` as `physical_key`,
`keycode` as `key`, by Godot's key names, with modifiers), mouse buttons, gamepad buttons and
gamepad axes. Touch, gesture, MIDI and shortcut events are reported.

## Scripts

Each `.gd` or `.cs` file becomes a task in `storyboard/work.json`, such as
`task/port-player`, linked to the script and the scenes that use it, with what the script
extends, its class name, exported variables, signals and functions. Autoloads get tasks too.
The original scripts stay in the project for the agent to read while it writes the C++.

`import/godot.json` holds what the port needs that the scenes don't: for each script, the
entities that use it and the values their nodes gave its exported variables; the signal
connections to its methods, by entity; and the project's autoloads and groups.

```json
{
  "scripts": {
    "godot/player.gd": {
      "task": "task/port-player",
      "used_by": [{"entity": "player", "scene": "godot/main.scene.json", "values": {"speed": 150.0}}]
    }
  },
  "scenes": {
    "main.tscn": {
      "entities": {".": "main", "Player": "player", "Player/Camera2D": "player_camera2d"},
      "connections": [{"from": "coin", "method": "_on_coin_body_entered", "signal": "body_entered", "to": "main"}],
      "unconverted": {"timer": {"type": "Timer", "values": {"wait_time": 30.0}}}
    }
  }
}
```

## Not converted yet

The report lists each of these once per kind, with how many and where it was first seen.

| What | Why, and when |
| --- | --- |
| `TileMapLayer`, `TileMap`, `TileSet` | The importer's next step |
| `AnimationPlayer`, `AnimationTree`, `Animation` resources | The importer's next step (`AnimationTree` with milestone 7's blending) |
| Themes, `gui/theme/custom` and `AudioBusLayout` | The importer's next step |
| Scenes instanced as prefabs rather than copies | The importer's next step |
| C++ skeletons for scripts | With gameplay code in game projects, milestone 6 |
| Timers and other gameplay nodes | Gameplay code: their values are kept in the report |
| `display/window/stretch/mode` | Stretching isn't supported yet |
| Shaders (`.gdshader`) and `ShaderMaterial` | A game's own shaders are on the roadmap; canvas materials are `.material.json` graphs |
| SVG, JPEG and WebP textures | Textures are PNG or BMP files |
| 3D nodes and resources | Milestone 7 |
