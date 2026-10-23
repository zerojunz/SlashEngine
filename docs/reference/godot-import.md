# Importing Godot projects

[`project.import`](commands.md#projectimport) and [`slash-cli import`](cli.md#import) convert a
Godot 4 project into a SlashEngine project in one pass. Most of it maps one to one, because
SlashEngine's components already use Godot's names, fields and enums. Scripts don't convert,
since SlashEngine games are plain C++: each becomes a storyboard task to port it, with what the
port needs. [examples/import-godot](../../examples/import-godot/) imports a small platformer.

Only Godot 4 projects import (`config_version=5` in `project.godot`). Open a Godot 3 project in
Godot 4 and save it there first; Godot converts it. A game exported with Godot 4 imports too,
from its `.pck` file or executable: see [Exported games](#exported-games).

## What an import writes

| File | From |
| --- | --- |
| `<scene>.scene.json` beside each `<scene>.tscn` | The scene's nodes, as entities and components |
| `<name>.frames.json` | Each `SpriteFrames` resource: a `.tres` file's beside it, a scene's own as `<scene>_<id>.frames.json` |
| `<name>.tileset.json` | Each `TileSet` resource, named the same way |
| `<name>.theme.json` | Each `Theme` resource, named the same way |
| `<name>.bus_layout.json` | Each `AudioBusLayout` resource, such as `default_bus_layout.tres` |
| `<name>.process_material.json` | Each `ParticleProcessMaterial` resource, named as frames are (see [particles](#particles)) |
| `<name>.audio.json` | Each `AudioStreamRandomizer`, `AudioStreamPlaylist`, `AudioStreamSynchronized`, `AudioStreamInteractive`, `AudioStreamGenerator`, `AudioStreamPolyphonic` and `AudioStreamMicrophone` resource, named as frames are, and `<sound>.audio.json` beside a sound whose import settings loop it or give it beats (see [sounds and music](#sounds-and-music)) |
| `<name>.anim.json` | Each `AnimationPlayer`'s animation libraries: one library's named as above, several together as `<scene>_<player>.anim.json` |
| `project.json` | `project.godot`'s settings and input map, merged into the project's own if it has one |
| `storyboard/work.json` | A task per script, merged into the project's own tasks |
| `import/godot.json` | The full report: every node's entity, what each script needs and everything that didn't convert |

Textures (`.png`, `.jpg`, `.bmp`), sounds (`.wav`, `.ogg`, `.mp3`), fonts (`.ttf`, `.otf`), 3D
models (`.glb`, `.gltf`, `.obj`, `.fbx`, `.blend`, with a `.gltf` file's buffers and images and an
`.obj` file's `.mtl` library) and data files (`.json`, `.txt`, `.csv`) are used as they are. When the Godot folder is inside the project,
they stay where they are and the converted files go beside the originals (`res://art/a.png` in
`godot/` is `godot/art/a.png`). When it is anywhere else, they and the scripts are copied in at
their `res://` paths. Godot's hidden folders (`.godot/`), `addons/` and `build/` are skipped.
`uid://` references resolve through the files' `.uid` and `.import` files and the scenes'
headers, so a resource that moved still resolves.

Importing again rewrites the converted files and keeps each task's status and the project's
other settings, so a project can be imported, ported for a while, and imported again after the
Godot version changes.

## Exported games

A game exported with Godot 4 imports from its `.pck` file, or from its executable with the pack
inside (Godot's "Embed PCK"). The game is unpacked back into the Godot project it was made from,
then converted as a project folder is. With `unpack_to` (`--unpack-to`) the unpacked project is
written to that folder of the SlashEngine project and the import uses its assets there; without
it, only the assets come along, at their `res://` paths. The report's `unpacked` says what came
back and lists what didn't. [examples/unpack-godot](../../examples/unpack-godot/) imports a
small shipped game.

| In the pack | Comes back as |
| --- | --- |
| Binary scenes and resources (`.scn`, `.res`), with the `.remap` files naming the text files they were exported from | `.tscn` and `.tres` text, as Godot's text saver writes it. External resources keep their paths and UIDs, numbered from 1, since the binary form keeps no IDs; groups are sorted, as Godot sorts them; empty object properties are left out |
| GDScript binary tokens (`.gdc`, versions 100 and 101: Godot 4.3 on), compressed or not | `.gd` source with every line and indent where it was, so errors still point at the right lines. Comments are gone, since tokens don't keep them, and numbers are written as their values (`0x1F` as `31`) |
| `project.binary` | `project.godot` |
| Imported textures (`.ctex`): PNG, WebP, or raw and S3TC (BC1 to BC3) pixels | PNG files. One imported from another format comes back as PNG under the new extension (`icon.svg` as `icon.png`), and references to it change to match |
| Imported sounds: `AudioStreamWAV` (8-bit, 16-bit, IMA-ADPCM and QOA), Ogg Vorbis and MP3 | WAV files with their loop, Ogg Vorbis files rebuilt from their pages, and the MP3 files |
| Imported fonts | The font files |
| `.godot/uid_cache.bin` | Each file's UID, in its scene or resource header or a `.uid` file |
| `.import` files | Kept beside the assets, so scenes still find them by UID |
| Anything else | As it is |

Packs of format 2 (Godot 4.0 to 4.4), 3 (Godot 4.5 and 4.6) and 4 unpack. A pack whose file list
is encrypted with the game's own key (Godot's "Encrypt Index") is refused, and a file encrypted
on its own is listed in `unsupported`: nothing here breaks encryption or DRM, so only the game's
maker, who has the key, can export it unencrypted. Godot 3 games and sparse packs, whose files
sit in a folder beside them, are refused too. Basis Universal, BPTC, ETC2 and ASTC textures, 3D
scenes imported from glTF or Blender files, translations and patch packs' changes to another
pack are listed. Unpack only games you made or have the rights to.

`slash/importer/godot_pack.hpp` also writes all of this as Godot's export does:
`export_project` turns a project's files into a pack's, `write_pack` lays out the pack (format 2,
3 or 4, in an executable or not), and `write_binary_resource`, `write_project_binary` and
`tokenize_gdscript` write the single formats. Checked against Godot 4.5's own exports, its script
tokens match byte for byte; textures are stored as PNG, as Godot's export does with
`rendering/textures/lossless_compression/force_png`. Tests and examples make exported games with
it, so no binary file is committed.

## Scenes

Each node becomes an entity named after it in snake case: `PlayerSprite` is `player_sprite`. A
name that is taken is prefixed with its parent's ID, then numbered. The nodes an instanced scene
brings keep their IDs from that scene's own `.scene.json` file under the instance's
(`Player/Camera2D` from `player.tscn` is `player/camera2d`). The report maps every node path to
its entity.

Node2D nodes nest through their parent's `Transform2D` `children`, controls through their
parent's `Control` `children` and Node3D nodes through their parent's `Transform3D` `children`, in
Godot's order, so they draw in Godot's order. A plain `Node` between two Node3D nodes breaks the
chain, as it does in Godot: the nodes under it are placed in the world. A root that is a
plain `Node`, and plain nodes under it or under a `CanvasLayer`, get a `Transform2D` to hold
their children in place. A `CanvasLayer` lists in its `children` the nodes that draw at the top
of its canvas, so they draw in it as in Godot. Controls in no `CanvasLayer` draw on the screen
above the world.

| Godot node | Components |
| --- | --- |
| `Node` | `Transform2D` under a plain root or a `CanvasLayer`, else none |
| `CanvasLayer` | `CanvasLayer` (its `transform` is its `offset`, `rotation` and `scale`, which convert as they are) |
| `Parallax2D` | `Transform2D` and `Parallax2D` |
| `ParallaxBackground`, `ParallaxLayer` | As Godot's Convert to Parallax2D: see [parallax](#parallax) |
| `Node2D`, `Marker2D` | `Transform2D` |
| `Sprite2D`, `Camera2D`, `CollisionShape2D`, `CollisionPolygon2D`, `PointLight2D`, `DirectionalLight2D`, `LightOccluder2D`, `CanvasModulate`, `CPUParticles2D`, `AudioStreamPlayer2D`, `AudioListener2D` | `Transform2D` and the component of the same name |
| `AnimatedSprite2D` | `Transform2D`, `AnimatedSprite2D` and `Sprite2D` |
| `GPUParticles2D` | `Transform2D` and `GPUParticles2D` (see [particles](#particles)) |
| `StaticBody2D`, `AnimatableBody2D`, `RigidBody2D`, `CharacterBody2D`, `Area2D` | `Transform2D` and the body, with its constant velocities, an area's gravity, damping and audio bus overrides and a character's floor, wall and platform options; its `CollisionShape2D` children are its shapes |
| `PinJoint2D`, `DampedSpringJoint2D`, `GrooveJoint2D` | `Transform2D` and the joint |
| `AudioStreamPlayer` | `AudioStreamPlayer` |
| `AnimationPlayer` | `AnimationPlayer` (see [animations](#animations)) |
| `Control` | `Control` |
| `Label`, `ColorRect`, `Panel`, `TextureRect`, `NinePatchRect`, `ProgressBar`, `Button`, `CheckBox`, `CheckButton`, `TextureButton`, `LineEdit`, `TextEdit`, `ScrollContainer`, `MarginContainer`, `CenterContainer`, `PanelContainer`, `GridContainer` | `Control` and the component of the same name |
| `CodeEdit` | `Control`, `TextEdit` and `CodeEdit`: its text, gutters and guidelines |
| `HBoxContainer`, `VBoxContainer`, `BoxContainer` | `Control` and `BoxContainer` (`vertical` for `VBoxContainer`) |
| `HFlowContainer`, `VFlowContainer`, `FlowContainer` | `Control` and `FlowContainer` |
| `OptionButton`, `MenuButton` | `Control` and the component of the same name, with its `popup/item_<n>/` items |
| `PopupMenu` | `PopupMenu`, an entity of its own with no `Control`, with its `item_<n>/` items |
| `HSlider`, `VSlider` | `Control` and `Slider` |
| `HScrollBar`, `VScrollBar` | `Control` and `ScrollBar` |
| `TileMapLayer` | `Transform2D` and `TileMapLayer` |
| `TileMap` | `Transform2D` and `TileMapLayer`, and an entity per layer (see [tile maps](#tile-sets-and-tile-maps)) |
| `Node3D`, `Marker3D` | `Transform3D` |
| `MeshInstance3D` | `Transform3D`, `MeshInstance3D`, and its mesh's and material's components (see [3D](#3d)) |
| `Camera3D`, `DirectionalLight3D`, `OmniLight3D`, `SpotLight3D`, `AudioStreamPlayer3D`, `AudioListener3D` | `Transform3D` and the component of the same name |
| `StaticBody3D`, `AnimatableBody3D`, `RigidBody3D`, `CharacterBody3D`, `Area3D` | `Transform3D` and the body; its `CollisionShape3D` children are its shapes |
| `CollisionShape3D` | `Transform3D` and `CollisionShape3D`, with its shape's fields (see [3D](#3d)) |
| `WorldEnvironment` | `WorldEnvironment`, with its environment's fields, and its sky's material (see [3D](#3d)) |
| An instance of a model file (`.glb`, `.gltf`, `.fbx`, `.blend`) | `Transform3D` and `MeshInstance3D` drawing the model |

Each property goes to the field of the same name: `position`, `rotation`, `scale`, `skew` and
`top_level` to `Transform2D` (or `Control` on a control), `offset_left` to `Control`, `texture`
and `z_as_relative` to `Sprite2D`. Values convert by the field's type:

- Enums are stored as numbers in Godot and names here, so `horizontal_alignment = 1` is
  `"center"` and `size_flags_horizontal = 3` is `"expand_fill"`.
- `NodePath`s name entities: a joint's `node_a = NodePath("../Floor")` is `"floor"`.
- Textures, sounds and fonts are project paths, and a `SpriteFrames`, `TileSet` or `Theme`
  resource is the file it converts to, so a control's `theme` names a `.theme.json` file.
- `theme_override_colors/font_color` and the other theme overrides go in `Control`'s maps;
  `theme_override_styles` take `StyleBoxFlat`, `StyleBoxTexture` and `StyleBoxEmpty`
  resources in a theme's form.
- Shape, occluder, curve, gradient, `LabelSettings` and `PhysicsMaterial` resources are written
  into the fields they set: a `CollisionShape2D`'s `shape`, `size`, `radius`, `height`, `a`, `b`
  or `points`; a `LightOccluder2D`'s `polygon`, `closed` and `cull_mode`; particles' `_curve`
  points and color ramps; a label's theme overrides; a body's `friction` and `bounce`.
- `region_enabled` and `region_rect` are `Sprite2D`'s `region`; `self_modulate` multiplies
  into its `modulate`; an `AnimatedSprite2D` plays only with `autoplay`, as in Godot.
- A `CanvasTexture`, as a sprite's `texture` or an atlas source's, becomes its
  `diffuse_texture` as the texture, with its `normal_texture`, `specular_texture`,
  `specular_color` and `specular_shininess` beside it, so lights shade it as in Godot. Its
  `texture_filter` and `texture_repeat` are reported: the node's own are used. A light's
  `range_layer_min` and `range_layer_max` keep which canvas layers it lights.
- A menu's `item_count` and each item's `text`, `icon`, `checkable` (0, 1, 2 for `none`,
  `check`, `radio`), `checked`, `id` (left out when it is the item's index), `disabled` and
  `separator` become its `items`; other item properties, such as shortcuts and submenus, are
  reported. An `OptionButton`'s `text` and `icon` are its selected option's, and a
  `PopupMenu`'s `size` is reported: a popup menu is as big as its items.
- `tooltip_text` is `Control`'s, and `drag_and_drop_selection_enabled` a text field's. A
  script's `_get_drag_data`, `_can_drop_data` and `_drop_data` are script code like the rest;
  in C++ they become a [`DragAndDrop`](scene-format.md#tooltips-and-drag-and-drop) component or
  `ui::set_drag_forwarding`.

## Parallax

`ParallaxBackground` and `ParallaxLayer`, Godot 3's parallax nodes, convert as Godot 4's editor
converts them to `Parallax2D` (`ParallaxBackgroundEditorPlugin::convert_to_parallax2d`):

- Each `ParallaxLayer` becomes a `Parallax2D` with `scroll_offset` its background's
  `scroll_base_offset` times its `motion_scale`, plus its `motion_offset` and `position`;
  `scroll_scale` the background's `scroll_base_scale` times its `motion_scale`; `repeat_size`
  its `motion_mirroring`; the background's `scroll_limit_begin` and `scroll_limit_end` on each
  axis they limit; and `follow_viewport` off when the background has
  `scroll_ignore_camera_zoom`.
- A `ParallaxBackground` with `scroll_ignore_camera_zoom` becomes a `CanvasLayer` at its
  `layer`, -100 unless set, holding its layers. Without it, it becomes a `Node2D` drawn with
  the world, as Godot's converter makes it; its `CanvasLayer` properties are reported.
- The background's `scroll_offset`, which its camera sets as the game runs, is left out.

## Tile sets and tile maps

A `TileSet` becomes a [`.tileset.json` file](scene-format.md#tile-sets) and keeps Godot's tile
shape, layout, offset axis and size, atlas sources, terrains, physics layers, collision polygons,
occlusion layers, occluders, custom data and alternative tiles:

- Each `TileSetAtlasSource` is a source named after its texture (`tiles.png` is `tiles`), with
  its texture (and a `CanvasTexture`'s normal and specular maps), region size, margins and
  separation. Each tile is keyed by its column and row
  (`"2,0"` for Godot's `2:0`) and keeps its probability and `texture_origin` (or its old name,
  `texture_offset`).
- A tile's `size_in_atlas` and its animation (each frame's duration, `animation_columns`,
  `animation_separation`, `animation_speed` and `animation_mode`) carry over as the same keys,
  with frame durations as [`animation_frames`](scene-format.md#animated-and-large-tiles). A
  duration of 0 or less, which Godot refuses to load, stays at its default of a second.
- Each terrain set is `set_<n>`, with its mode and its terrains' names; a name used in two sets
  gets the set's number added, since names are unique in the file.
- A tile's terrain and its peering bits become `terrain` and `peering`, named for the way each
  side or corner faces in the tile's [shape](scene-format.md#tile-shapes): on a square
  `right_side` is `e` and `bottom_right_corner` is `se`, on a diamond `right_corner` is `e` and
  `bottom_right_side` is `se`, and on a hexagon in offset rows `bottom_right_corner`, between the
  `e` and `se` sides, is `ese`. A bit with no terrain is `null`, and a bit with the tile's own
  terrain is left out.
- Each physics layer keeps its collision layer and mask, and its physics material's friction and
  bounce. A tile's polygons on it become its
  [`collision`](scene-format.md#tile-collision), with `one_way` and `one_way_margin`.
- Each occlusion layer keeps its `light_mask` and `sdf_collision`. A tile's `OccluderPolygon2D`s on it
  (`occlusion_layer_0/polygon_0/polygon`, or `occlusion_layer_0/polygon` before Godot 4.4)
  become its [`occlusion`](scene-format.md#tile-occluders-and-custom-data), with `closed` and
  `cull_mode`. Polygons on a layer the tile set lacks go, as Godot drops them, and so do closed
  ones of fewer than three points, which cast no shadow.
- Each navigation layer keeps its `layers`. A tile's `NavigationPolygon` on it
  (`navigation_layer_0/polygon`) becomes its
  [`navigation`](scene-format.md#tile-navigation) outlines, as the tile set editor drew them, or
  its polygons when it has no outlines, which cover the same area. Polygons on a layer the tile
  set lacks go, as Godot drops them; outlines with no area are left out and reported. A
  `TileMapLayer`'s `navigation_enabled`, or an old `TileMap` layer's, carries over.
- Each custom data layer of type `bool`, `int`, `float`, `String`, `StringName`, `Vector2`,
  `Vector2i` or `Color` keeps its name and becomes a layer of that type (`StringName` is
  `string`). Other types, such as `Dictionary`, or `Any`, are left out and reported. A tile's
  `custom_data_N` becomes its `custom_data` by the layer's name, numbers converted as Godot
  converts them (`2.7` in an `int` layer is `2`); values equal to the type's default, which
  Godot saves for every tile, are left out. A value Godot couldn't convert either is reported.
- Each alternative tile (`2:0/1`) becomes one of the tile's
  [`alternatives`](scene-format.md#alternative-tiles-and-turned-cells), with its flips, transpose,
  modulate, texture origin and own polygons, occluders and custom data.

A `TileMapLayer` node's `tile_map_data` becomes its layer's `origin`, `legend` and `rows`, so
`tilemap.get`, `tilemap.paint` and `tilemap.set` edit the cells as Godot left them. A cell keeps
its alternative tile and its flip and transpose flags as turns: `"tiles:1,0:1"`, `"tiles:0,1:ht"`.
Its `enabled`, `collision_enabled` and `occlusion_enabled` carry over.

The old `TileMap` node (Godot 4.0 to 4.2) keeps its tile set on its own `TileMapLayer` and puts
each layer on an entity of its own, in its `Transform2D` children, drawn before its other
children as Godot draws its layers. A layer's `modulate`, `z_index` and `enabled` go on that
entity, so they don't reach the node's children. A `TileMap` with one layer that sets none of
them keeps its cells on the node, with no extra entity. Layers are named after the node and the
layer: `Ground` in `Level` is `level_ground`.

## Themes and bus layouts

A `Theme` becomes a [`.theme.json` file](scene-format.md#themes): its `default_font`,
`default_font_size`, each type's `base_type` and its colors, constants, font sizes, fonts,
icons and style boxes, which take the same form as `theme_override_styles`. The project's
`gui/theme/custom` becomes `gui.theme`, and a control's `theme` names the file.

An `AudioBusLayout` becomes a [`.bus_layout.json` file](scene-format.md#bus-layouts): each bus's
name, volume, solo, mute, bypass and send, and its effects in order, each with its settings and
whether it is enabled. Effects convert to the effect of the same name: `AudioEffectLowPassFilter`
is `low_pass_filter`, a filter's `db` is 6, 12, 18 or 24 instead of Godot's 0 to 3, a distortion's
`mode` and a record effect's `format` are names, an `fft_size` is the size instead of Godot's 0
to 4, and a chorus's `voice/1/...` settings are `voices`. A record effect's IMA ADPCM and QOA
formats record at 16 bits, which is reported. Godot uses
`res://default_bus_layout.tres` unless `audio/buses/default_bus_layout` names another, and the
one it would use becomes `audio.bus_layout`.

An instanced scene stays an instance, as in Godot: its `.scene.json` file names its root, and
the instance's entity extends that file ([instanced scenes](scene-format.md#instanced-scenes)),
holding only the properties the instancing scene overrides, on the instance and on the nodes it
brings. Editing the instanced scene changes every instance. A value that doesn't fit its field is
left out and reported, so one bad value can't stop a scene.

## Sounds and music

Sounds are used as they are, unless their import settings (the `[params]` of their `.import`
files) change how they play. Then each gets a [`.audio.json` file](scene-format.md#audio-streams)
beside it, `music/theme.ogg.audio.json` for `music/theme.ogg`, and whatever played the sound
plays that file instead:

- An Ogg Vorbis or MP3 file's `loop`, `loop_offset`, `bpm`, `beat_count` and `bar_beats` carry
  over, so interactive music keeps its beats.
- A WAV file's `edit/loop_mode`, `edit/loop_begin` and `edit/loop_end` become `loop_mode`,
  `loop_begin` and `loop_end`, a negative frame counted back from the end as Godot counts it.
  With "Detect From WAV", Godot's default, the loop comes from the file's own `smpl` chunk, as
  Godot reads it. `edit/trim` and `edit/normalize` are reported: the sound plays as it is in the
  file.

Godot's music, generator and polyphonic resources become `.audio.json` files of the same kinds,
a `.tres` file's beside it and a scene's own as `<scene>_<id>.audio.json`. Their streams convert
as any reference does, so a playlist inside interactive music becomes a file of its own that the
clip names.

| Godot | `.audio.json` |
| --- | --- |
| `AudioStreamRandomizer` | `streams` with their `weight`, `playback_mode` by name, `random_pitch` and `random_volume_offset_db` |
| `AudioStreamPlaylist` | `playlist`, `shuffle`, `loop` and `fade_time` |
| `AudioStreamSynchronized` | `synchronized`, each stream's `volume` as `volume_db` |
| `AudioStreamInteractive` | `clips` (`auto_advance` and `next_clip` by name), `initial_clip` by name and `transitions`: each `Vector2i(from, to)` key becomes `from` and `to` by name, left out for -1 (any clip), and `from_time`, `to_time` and `fade_mode` are named. `filler_clip` comes with `use_filler_clip` |
| `AudioStreamGenerator` | `generator` with `mix_rate_mode` by name, `mix_rate` and `buffer_length` |
| `AudioStreamPolyphonic` | `polyphonic` with `polyphony` |
| `AudioStreamMicrophone` | `microphone` |

A clip with no name, or one another clip has, is renamed `clip_<index>` and reported, since
`switch_to_clip` asks for clips by name. A stream that doesn't convert is left out and reported;
a resource with none left is reported where it was used. `report.converted.audio_streams` counts
the files written.

## Animations

An `AnimationPlayer` node becomes an entity with an [`AnimationPlayer`](scene-format.md#animations)
that plays one `.anim.json` file holding all its libraries' animations: the default library's by
their names, another library's as `<library>/<animation>`, as Godot names them. `autoplay` is the
animation it plays, `speed_scale` carries over, and `active = false` is `playing` `false`.

A Godot track names the node it animates by its path from the player's root node (`root_node`,
the player's parent by default). Each converted track keeps that path as its `target`, and the
player's `targets` maps each path to the entity of that node, so `Sprite2D:frame` becomes target
`Sprite2D`, path `Sprite2D/frame`, and `".": "hero"` and `"Sprite2D": "sprite2d"` in `targets`.
An instance of the scene has the same library, with its targets under the instance's ID.

| Godot track | Becomes |
| --- | --- |
| Value track | A value track on the field the property converts to, as a node's property does (`.:position` is `Transform2D/position`, `Sprite2D:texture` a texture path); a part such as `modulate:a` or `position:y` is the field's part. Interpolation, easing (`transitions`), `loop_wrap` and discrete or continuous update modes carry over |
| Bezier track | A value track through its keys' values, with cubic interpolation; its handles are reported |
| Method track | An event track per method it calls, named after the method, with `{"args": [...]}` as each key's data when it passes arguments. The node's script becomes C++ that handles the event |

Each animation keeps its `length` and loop mode (`loop_mode` 1 is `linear`, 2 is `pingpong`).
Keys past the end, which Godot never plays, are left out, and so are disabled tracks.

## Particles

A `CPUParticles2D` keeps its settings in its component. A `GPUParticles2D` keeps the node's, and
its `ParticleProcessMaterial` becomes a [`.process_material.json`](scene-format.md#process-materials)
file that its `process_material` names:

- Vectors lose their z: `direction = Vector3(0, -1, 0)` is `[0, -1]`, except
  `turbulence_noise_speed`, which moves 4D noise and keeps all three. `emission_shape` is named
  (`1` is `"sphere"`), and the rest keep their names and values.
- A `CurveTexture`'s curve becomes the `_curve`'s points, and a `GradientTexture1D`'s gradient the
  ramp's stops.
- A material with no `gravity` gets `[0, 98]`, as Godot gives a 3D one on a `GPUParticles2D`;
  with `particle_flag_disable_z` it keeps Godot's `[0, -9.8]`.
- The node's `CanvasItemMaterial` gives its particle animation: `particles_anim_h_frames`,
  `particles_anim_v_frames` and `particles_anim_loop` become `hframes`, `vframes` and
  `anim_loop`. Its `blend_mode` and `light_mode` are reported: write a `.material.json` for them.
- `seed` is kept only with `use_fixed_seed`, since Godot picks another at random otherwise.
- `collision_mode` is named (`1` is `"rigid"`, `2` `"hide_on_contact"`), `collision_friction`
  and `collision_bounce` are clamped to 0 to 1 as Godot's editor keeps them, and the node's
  `collision_base_size` is kept: particles [collide](scene-format.md#gpu-particle-collision)
  with `LightOccluder2D`s and tile occluders as Godot's do.
- `sub_emitter_mode` is named (`1` is `"constant"`, `2` `"at_end"`, `3` `"at_collision"`, `4`
  `"at_start"`) with its frequency, amounts and `sub_emitter_keep_velocity`, and the node's
  `sub_emitter` path becomes the entity it names: particles start particles in
  [sub-emitters](scene-format.md#sub-emitters) as Godot's do. A sub-emitter that can't take part
  (one naming itself, say) is reported and left out.
- The node's `trail_enabled`, `trail_lifetime`, `trail_sections` and
  `trail_section_subdivisions` are kept: particles draw [trails](scene-format.md#gpu-particle-trails)
  as Godot's do, on every renderer (Godot's Compatibility renderer has none).
- Points given as textures and rings that don't face the screen are reported: SlashEngine
  doesn't have them yet.
  `report.converted.process_materials` counts the files written.

## 3D

Godot's 3D nodes keep their names and fields here, so most of a 3D scene converts as a 2D one
does. [examples/import-godot-3d](../../examples/import-godot-3d/) imports a courtyard.

- A node's `transform = Transform3D(...)` becomes its `Transform3D`'s `position`, `rotation` (in
  radians, in Godot's YXZ order) and `scale`, split as Godot's `Basis::get_euler` and
  `get_scale` split it: a mirrored basis gets a negative scale. `rotation_edit_mode` and
  `rotation_order`, which only change how Godot's inspector shows it, are left out.
- A `MeshInstance3D`'s `BoxMesh`, `SphereMesh`, `CylinderMesh`, `CapsuleMesh`, `PlaneMesh`,
  `QuadMesh`, `PrismMesh` or `TorusMesh` becomes the [component](scene-format.md#3d-nodes) of the
  same name with the mesh's fields. A mesh that is a model file (an `.obj` file Godot imports as a
  mesh) becomes `MeshInstance3D.mesh`, the file's path.
- The material that wins in Godot becomes the entity's `StandardMaterial3D`: `material_override`,
  then `surface_material_override/0`, then the mesh's own `material`. `StandardMaterial3D` and
  `ORMMaterial3D` properties keep their names. `transparency` is named; alpha hash cuts at
  `alpha_scissor_threshold` and depth pre-pass blends as alpha, and both are reported. An
  `ORMMaterial3D`'s `orm_texture` is its `roughness_texture` read from green and its
  `metallic_texture` read from blue.
- An instance of a `.glb`, `.gltf`, `.fbx` or `.blend` file, Godot's imported scene, becomes a
  `Transform3D` and a `MeshInstance3D` whose `mesh` is the file, which draws its nodes, meshes and
  materials as Godot imports them (see [models](scene-format.md#models)). The instance keeps the
  transform the scene gave it. The model's own nodes can't be changed from the scene, so
  editable children and nodes added under them are reported. A model's `.import` file with a
  `nodes/root_scale` or a `nodes/root_type` other than `Node3D` is reported too: scale the
  entities that draw it, or add the component to them.
- A `WorldEnvironment`'s `Environment` sets its `WorldEnvironment`'s fields of the same names,
  its `Sky`'s `radiance_size` is `sky_radiance_size` (`3` is `256`), and the sky's
  `ProceduralSkyMaterial`, `PhysicalSkyMaterial` or `PanoramaSkyMaterial` becomes the component
  of the same name on its entity.
- A `CollisionShape3D`'s `BoxShape3D`, `SphereShape3D`, `CapsuleShape3D`, `CylinderShape3D`,
  `ConvexPolygonShape3D`, `ConcavePolygonShape3D` (its `data` as `faces`) or
  `WorldBoundaryShape3D` becomes its `shape` and the fields it sets. A body's
  `physics_material_override` gives its `friction` and `bounce`, as a 2D body's does.

## Project settings

| `project.godot` | `project.json` |
| --- | --- |
| `application/config/name` | `name` |
| `application/run/main_scene` | `main_scene`, the scene's `.scene.json`; with none, the first scene by path |
| `application/run/max_fps` | `max_fps` |
| `display/window/size/viewport_width`, `viewport_height` | `window.width`, `window.height` |
| `display/window/vsync/vsync_mode` | `window.vsync_mode` |
| `display/display_server/driver.linuxbsd` | `window.linux_display_driver` when it is `wayland` |
| `physics/common/physics_ticks_per_second` | `tick_hz` |
| `physics/2d/default_gravity`, `default_gravity_vector`, `default_linear_damp`, `default_angular_damp` | `physics_2d` |
| `physics/3d/default_gravity`, `default_gravity_vector`, `default_linear_damp`, `default_angular_damp` | `physics_3d` |
| `physics/jolt_physics_3d/simulation/areas_detect_static_bodies` | `physics_3d.areas_detect_static_bodies` |
| `rendering/environment/defaults/default_clear_color` | `rendering.clear_color` |
| `rendering/textures/canvas_textures/default_texture_filter` | `rendering.texture_filter` |
| `rendering/renderer/rendering_method` | `rendering.driver` `opengl` when it is `gl_compatibility`, or `opengl_es` when `rendering/gl_compatibility/driver` is `opengl3_es` |
| `rendering/rendering_device/fallback_to_opengl3` | `rendering.fallback_to_opengl` |
| `gui/theme/custom` | `gui.theme`, the theme's `.theme.json` file |
| `audio/buses/default_bus_layout` | `audio.bus_layout`, the layout's `.bus_layout.json` file |
| `audio/driver/mix_rate`, `audio/driver/output_latency` | `audio.mix_rate`, `audio.output_latency` |
| `audio/general/2d_panning_strength` | `audio.panning_strength_2d` |
| `audio/general/3d_panning_strength` | `audio.panning_strength_3d` |
| `audio/driver/enable_input` | `audio.enable_input` |
| `[input]` | `input` |

Each input action keeps its deadzone and its keys (`physical_keycode` as `physical_key`,
`keycode` as `key`, by Godot's key names, with modifiers), mouse buttons, gamepad buttons and
gamepad axes. Touch, gesture, MIDI and shortcut events are reported.

## Scripts

Each `.gd` or `.cs` file becomes a task in `storyboard/work.json`, such as
`task/port-player`, linked to the script and the scenes that use it, with what the script
extends, its class name, exported variables, signals and functions. Autoloads get tasks too.
The original scripts stay in the project for the agent to read while it writes the C++.

Each `.gd` and `.cs` file also becomes a C++ skeleton for the agent to fill in, in the folder of
`project.json`'s gameplay sources (`src`, added to the sources when the project has none):

| File | What it holds |
| --- | --- |
| `src/imported/<name>.cpp` | The script's constants and enums; its variables as a component named after its `class_name` (or file; a C# script's class); an `emit_<signal>` helper per signal, which emits a world event with the signal's arguments as its data; a stub per function, with the GDScript or C# in comments; the system calling them; and `imported::register_<name>` |
| `src/imported/scripts.cpp` | `imported::register_scripts`, which registers every skeleton's component and system |
| `src/game.cpp` | The game module calling it, written only when the project's code has none (`src/imported/module.cpp` when `src/game.cpp` is taken) |

Scenes give each entity whose node has the script that component, with the values the node gave
its exported variables: node paths become the IDs of the entities they name, and an instance's
component holds only what its scene's file doesn't. Until the code is built, the component is a
stand-in that keeps those values, so the scenes open, change and save as they are.

| GDScript | C++ |
| --- | --- |
| `int`, `float`, `bool`, `String`, `StringName` | `int`, `float`, `bool`, `std::string` |
| `Vector2`, `Vector2i`, `Vector3`, `Vector4`, `Color`, `Rect2`, `Quaternion` | `slash::Vec2`, `Vec2i`, `Vec3`, `Vec4` (`Color` and `Rect2` too), `Quat` |
| Node types (`Node2D`, a `class_name` of the game's) and `NodePath` | `entt::entity`, the entity the scene's path names |
| Resource types (`Texture2D`, `PackedScene`, `AudioStream`...) | `std::string`, the converted file's path |
| `Array[T]`, packed arrays, `Dictionary[String, T]` | `std::vector`, `std::map<std::string, T>` |
| `Array`, `Dictionary`, enums, anything else | `slash::json::Value`; enums `int` |

Untyped variables take their value's type. Values translate when they are literals, constants,
enum values, `Vector2(...)`, `Color(...)`, `Color("#rrggbb")`, named colors or `preload()`;
others stay a comment. `@export_range` sets the field's range and `##` comments its doc.

The system calls the stubs when Godot would call the functions: `_init`, `_enter_tree` and
`_ready` for each entity made since the last tick (`entity.created` in `World::events()`),
`_physics_process` and `_process` once a tick with the tick's seconds, `_input` and the other
input functions for each input event, and a method a scene connects a signal to when that
signal's event comes from the connected entity. `_exit_tree`, `_draw` and Godot's other calls
are stubs nothing calls yet. `@onready` variables, setters and getters, static variables and
inner classes are listed at the top of the skeleton instead.

A C# script reads the same way: its class's `[Export]` fields and properties are the exported
variables (scenes set them by their C# names), `[Export(PropertyHint.Range, "0,10")]` sets a
range, `///` comments are docs, `[Signal]` delegates are signals (`HitEventHandler` is `Hit`, as
Godot names it), `_Ready` and Godot's other overrides are called as their GDScript names are,
and other fields and properties are its state. C# types map as GDScript's do, with `long`,
`double`, `List<T>` and arrays too. Static fields, nested types, operators, indexers and `async`
methods are listed at the top.

Importing again leaves a skeleton that exists as it is, its component taking scene values for
the fields it still has, and rewrites `scripts.cpp`. When the project's own game module doesn't
call `imported::register_scripts`, the report says so.
`examples/import-godot-scripts` imports a script, fills in its skeleton and runs it.

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
      "entities": {".": "main", "Player": "player", "Player/Camera2D": "player/camera2d"},
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
| `AnimationTree`, and the other 3D nodes SlashEngine has components for: CSG shapes, `Path3D` and `PathFollow3D`, `GridMap`, joints, `VehicleBody3D`, `Skeleton3D` and its modifiers, `PhysicalBone3D` | Not converted yet; add SlashEngine's component of the same name by hand |
| 3D tracks (`position_3d`, `rotation_3d`, `scale_3d`, `blend_shape`) | Not converted yet; a model's own animations play from the model file |
| A model's own nodes changed from a scene (editable children), and its `.import` file's root scale and root type | The model draws as its file has it; place, scale and add components to the entity that draws it |
| `ArrayMesh` resources and meshes saved in Godot's binary format | Export the mesh as a glTF file |
| Surface overrides past surface 0, `material_overlay` and a material's `next_pass` | An entity draws its mesh with one material |
| `ShaderMaterial` on a 3D mesh or a sky | A game's own 3D shaders aren't supported yet |
| `HeightMapShape3D` | Use an [`HTerrain`](scene-format.md#heightmap-terrains), whose height field collides |
| `GPUParticles3D`, `CPUParticles3D`, `Decal`, `ReflectionProbe`, `VoxelGI`, `LightmapGI` and other 3D nodes with no SlashEngine component | Not supported yet; their entities keep their place in the tree |
| Audio and animation tracks | Play the sound or the other animation from gameplay code, on an event track's event |
| Bezier handles, the capture update mode, blend times and markers | A bezier track plays cubic through its keys, a capture track as continuous, and animations switch without blending |
| Tracks on a control's `position` or `size` | Animate its offsets instead |
| Animation libraries no `AnimationPlayer` plays | A track's fields depend on the node it animates |
| Custom data of other types, and tiles' constant velocities | Custom data holds the types listed above; move a layer rather than give its tiles a velocity |
| Navigation nodes: `NavigationRegion2D`, `NavigationLink2D`, `NavigationAgent2D`, `NavigationObstacle2D` and their 3D twins | Not converted yet; add the [navigation](scene-format.md#navigation) components of the same names. Tiles' navigation polygons convert |
| Tiles' other properties, such as `z_index` and `y_sort_origin`, a base tile's flips and `modulate`, and scene collection sources | Tiles keep their texture origin, collision, occluders and custom data, and alternatives their flips and color too; place scenes as entities |
| Y-sorting tile map layers | Layers draw in order, by `z_index` |
| `gui/theme/custom_font` | Set `default_font` in the project's theme |
| Audio effects with no SlashEngine equivalent, such as the deprecated `AudioEffectLimiter` | Left out of the bus, which keeps its other effects |
| Timers and other gameplay nodes | Gameplay code: their values are kept in the report |
| `display/window/stretch/mode` | Stretching isn't supported yet |
| Shaders (`.gdshader`) and `ShaderMaterial` | A game's own shaders are on the roadmap; canvas materials are `.material.json` graphs |
| SVG and WebP textures | Textures are PNG, JPEG or BMP files |
| COLLADA (`.dae`) and other model formats | Models are glTF, OBJ, FBX or `.blend` files; export it as glTF |
