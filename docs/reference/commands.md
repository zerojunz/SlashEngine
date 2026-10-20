# Command reference

Every command the engine registers, with its params, the capabilities it needs and a real example.
The same commands are available everywhere:

| Front end | How to call `entity.create` |
| --- | --- |
| MCP | Tool `entity_create` with the params as arguments |
| JSON-RPC | `{"jsonrpc":"2.0","id":1,"method":"entity.create","params":{...}}` |
| REST | `POST /v1/commands/entity.create` with the params as the body |
| CLI | `slash-cli call entity.create '{...}'` |

The engine is the source of truth for this page: `engine.describe` with `"full": true`, or
`schema.get` with `"kind": "command"`, returns each command's live params schema. Examples below
run against `tests/data/basic.scene.json`, which holds `camera` (a `Transform3D`), `player` (a
`Transform2D`) and `spawn_point` (no components).

Rules that apply to every command (see [concepts.md](../concepts.md#sessions)):

- Params are a JSON object, checked against the command's schema before it runs. Unknown params
  are errors. Left out, params take the defaults listed here.
- A failed command changes nothing.
- Commands that change the world (marked "Takes `dry_run`" below) accept `"dry_run": true`; see
  [Dry runs](#dry-runs).
- Paths are relative to the project folder and cannot leave it. Names Windows keeps for devices
  (`con`, `prn`, `aux`, `nul`, `com1` to `com9`, `lpt1` to `lpt9`, with any extension) are
  refused on every platform, since opening one on Windows reaches the device.
- Component fields are written as in scene files: vectors as arrays (`[x, y]`), quaternions as
  `[x, y, z, w]`.

## Dry runs

Add `"dry_run": true` to the params of any command that changes the world (`scene.new`,
`scene.open`, `entity.create`, `entity.destroy`, `component.set`, `component.remove`, `step`,
`run.until`, `world.restore`, `storyboard.edit`, `replay.run`, `replay.seek` and `batch`) to see what it would do. The command runs, then
the world, the scene's path, the next generated ID and the tick counter are put back, and no undo
step is recorded. An error is returned as usual. The result holds the command's own result and the
changes, in the same form as [`world.diff`](#worlddiff):

```json
{"id": "player", "dry_run": true, "components": {"Transform2D": {"rotation": 1.0}}}
```

```json
{"changes":{"changed":{"player":{"Transform2D":{"rotation":1.0}}}},"dry_run":true,"result":{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}}
```

A batch takes `dry_run` itself; the commands inside it cannot. In a dry-run batch, `scene.save`
and `prefab.save` check everything and return their usual result but write no file, and
`storyboard.edit` returns the versions it would write. Commands that change nothing, such
as `entity.get`, reject it as an unknown param.

## Contents

| Group | Commands |
| --- | --- |
| Describe | [`engine.describe`](#enginedescribe), [`schema.get`](#schemaget) |
| Scene | [`scene.get`](#sceneget), [`scene.new`](#scenenew), [`scene.open`](#sceneopen), [`scene.save`](#scenesave) |
| Entities | [`entity.create`](#entitycreate), [`entity.destroy`](#entitydestroy), [`entity.get`](#entityget) |
| Prefabs | [`prefab.save`](#prefabsave) |
| Assets | [`asset.list`](#assetlist) |
| Materials | [`material.eval`](#materialeval) |
| Animations | [`animation.eval`](#animationeval) |
| Tile maps | [`tilemap.paint`](#tilemappaint), [`tilemap.set`](#tilemapset), [`tilemap.get`](#tilemapget), [`tilemap.convert`](#tilemapconvert) |
| Physics | [`physics.raycast`](#physicsraycast), [`physics.overlap`](#physicsoverlap), [`physics.contacts`](#physicscontacts), [`physics.gravity`](#physicsgravity) |
| Particles | [`particles.eval`](#particleseval) |
| Lights | [`light.sample`](#lightsample) |
| UI and text | [`ui.layout`](#uilayout), [`ui.theme`](#uitheme), [`ui.press`](#uipress), [`ui.popup`](#uipopup), [`ui.select`](#uiselect), [`ui.drag`](#uidrag), [`ui.tooltip`](#uitooltip), [`ui.focus`](#uifocus), [`ui.search`](#uisearch), [`ui.clipboard`](#uiclipboard), [`text.measure`](#textmeasure) |
| Input | [`input.inject`](#inputinject), [`input.state`](#inputstate), [`input.actions`](#inputactions) |
| Audio | [`audio.state`](#audiostate), [`audio.capture`](#audiocapture), [`audio.play`](#audioplay), [`audio.stop`](#audiostop), [`audio.set_bus`](#audioset_bus), [`audio.spectrum`](#audiospectrum), [`audio.record`](#audiorecord), [`audio.input`](#audioinput) |
| Services | [`services.state`](#servicesstate), [`services.test`](#servicestest) |
| Storyboard | [`storyboard.query`](#storyboardquery), [`storyboard.brief`](#storyboardbrief), [`storyboard.check`](#storyboardcheck), [`storyboard.edit`](#storyboardedit) |
| Components | [`component.set`](#componentset), [`component.remove`](#componentremove) |
| Query | [`world.query`](#worldquery) |
| Snapshots | [`world.snapshot`](#worldsnapshot), [`world.diff`](#worlddiff), [`world.restore`](#worldrestore) |
| Run | [`step`](#step), [`run.until`](#rununtil), [`play`](#play), [`pause`](#pause) |
| Replays | [`replay.record`](#replayrecord), [`replay.stop`](#replaystop), [`replay.run`](#replayrun), [`replay.seek`](#replayseek) |
| Tests | [`test.run`](#testrun) |
| History | [`undo`](#undo), [`redo`](#redo), [`history.get`](#historyget) |
| Several at once | [`batch`](#batch) |
| Observe | [`events.subscribe`](#eventssubscribe), [`events.read`](#eventsread), [`events.unsubscribe`](#eventsunsubscribe), [`log.tail`](#logtail), [`frame.capture`](#framecapture), [`frame.summary`](#framesummary), [`canvas.convert`](#canvasconvert) |
| Display | [`display.list`](#displaylist), [`display.set`](#displayset), [`perf.stats`](#perfstats), [`debug.overlay`](#debugoverlay) |
| Import | [`project.import`](#projectimport) |
| Export | [`export.run`](#exportrun), [`sdk.check`](#sdkcheck), [`sdk.add`](#sdkadd), [`sdk.lock`](#sdklock) |
| Gameplay code | [`gameplay.build`](#gameplaybuild) |


## Describe

### engine.describe

Lists the components with their default field values, the command names, the granted
capabilities and the open scene. Call this first.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `full` | boolean | `false` | Also return each command's description, capabilities and params schema, and each component's field schema with docs and ranges |

Needs: `read`.

```json
{}
```

```json
{"capabilities":["read","edit","run","filesystem","build"],"commands":["animation.eval","asset.list","batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","events.read","events.subscribe","events.unsubscribe","export.run","history.get","light.sample","log.tail","material.eval","particles.eval","physics.contacts","physics.gravity","physics.overlap","physics.raycast","prefab.save","redo","run.until","scene.get","scene.new","scene.open","scene.save","schema.get","step","storyboard.brief","storyboard.check","storyboard.edit","storyboard.query","text.measure","tilemap.get","tilemap.paint","tilemap.set","ui.layout","ui.theme","undo","world.diff","world.query","world.restore","world.snapshot"],"components":[{"defaults":{"children":[],"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0],"skew":0.0,"top_level":false},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down. The entities in children are placed in its space: they move, turn and scale with it, and draw after it.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"},{"defaults":{"ids":[]},"doc":"The storyboard entries this entity stands for.","name":"StoryRef"},{"defaults":{"centered":true,"flip_h":false,"flip_v":false,"frame":0,"hframes":1,"light_mask":1,"material":"","material_params":{},"modulate":[1.0,1.0,1.0,1.0],"normal_texture":"","offset":[0.0,0.0],"region":[0.0,0.0,0.0,0.0],"specular_color":[1.0,1.0,1.0,1.0],"specular_shininess":1.0,"specular_texture":"","texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_as_relative":true,"z_index":0},"doc":"A texture drawn at the entity's Transform2D. Sprites draw in order of z_index, then in the order their entities were made.","name":"Sprite2D"},{"defaults":{"animation":"default","frame":0,"frame_progress":0.0,"playing":true,"speed_scale":1.0,"sprite_frames":""},"doc":"Plays an animation from a .frames.json file on the entity's Sprite2D, which it adds if missing. Emits animation_finished when an animation that doesn't loop ends.","name":"AnimatedSprite2D"},{"defaults":{"animation":"","backwards":false,"library":"","playing":true,"speed_scale":1.0,"targets":{},"time":0.0},"doc":"Plays a keyframe animation from a .anim.json file on the entity's components, and on the entities its targets name. Emits animation_finished when an animation that doesn't loop ends, and the events of its event tracks.","name":"AnimationPlayer"},{"defaults":{"collision_enabled":true,"legend":{},"light_mask":1,"modulate":[1.0,1.0,1.0,1.0],"occlusion_enabled":true,"origin":[0,0],"rows":[],"texture_filter":"inherit","tile_set":"","visible":true,"z_as_relative":true,"z_index":0},"doc":"A grid of tiles from a .tileset.json file, drawn at the entity's Transform2D if it has one. Cells are text: rows of legend keys, \".\" for empty. Edit them with tilemap.paint and tilemap.set.","name":"TileMapLayer"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"offset":[0.0,0.0],"range_item_cull_mask":1,"range_layer_max":0,"range_layer_min":0,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1,"texture":"","texture_scale":1.0},"doc":"A light shining from the entity's Transform2D through a texture. It lights sprites and tile map layers whose light_mask shares a bit with range_item_cull_mask, in canvases its layer range holds; with shadow_enabled, LightOccluder2D polygons and tiles' occluders cast shadows from it.","name":"PointLight2D"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"max_distance":10000.0,"range_item_cull_mask":1,"range_layer_max":0,"range_layer_min":0,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1},"doc":"Light from one direction over everything, such as the sun. It shines down the screen, turned by the entity's Transform2D rotation. Masks, ranges and shadows work as PointLight2D's.","name":"DirectionalLight2D"},{"defaults":{"closed":true,"cull_mode":"disabled","occluder_light_mask":1,"polygon":[]},"doc":"A polygon that casts shadows from lights with shadow_enabled, placed by the entity's Transform2D.","name":"LightOccluder2D"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Tints everything lit in its canvas: the light the world, or the CanvasLayer it is in, has where no light reaches. One per canvas.","name":"CanvasModulate"},{"defaults":{"a":[0.0,0.0],"b":[0.0,10.0],"disabled":false,"height":30.0,"one_way_collision":false,"one_way_collision_margin":1.0,"points":[],"position":[0.0,0.0],"radius":10.0,"rotation":0.0,"shape":"rectangle","size":[20.0,20.0]},"doc":"A shape of the entity's physics body or area, moved by position and rotation and scaled by its Transform2D. On an entity with no body or area, a shape of its parent's.","name":"CollisionShape2D"},{"defaults":{"build_mode":"solids","disabled":false,"one_way_collision":false,"one_way_collision_margin":1.0,"polygon":[]},"doc":"A polygon shape of the entity's physics body or area (its parent's, on an entity with neither): any simple polygon, filled or only its edges.","name":"CollisionPolygon2D"},{"defaults":{"bounce":0.0,"collision_layer":1,"collision_mask":1,"constant_angular_velocity":0.0,"constant_linear_velocity":[0.0,0.0],"friction":1.0},"doc":"A physics body that doesn't move on its own, such as a wall. Changing its Transform2D moves it at once.","name":"StaticBody2D"},{"defaults":{"bounce":0.0,"collision_layer":1,"collision_mask":1,"constant_angular_velocity":0.0,"constant_linear_velocity":[0.0,0.0],"friction":1.0,"sync_to_physics":true},"doc":"A physics body that goes where its Transform2D says, such as a moving platform, pushing bodies in its way.","name":"AnimatableBody2D"},{"defaults":{"angular_damp":0.0,"angular_damp_mode":"combine","angular_velocity":0.0,"bounce":0.0,"can_sleep":true,"collision_layer":1,"collision_mask":1,"constant_force":[0.0,0.0],"constant_torque":0.0,"contact_monitor":false,"continuous_cd":"disabled","freeze":false,"freeze_mode":"static","friction":1.0,"gravity_scale":1.0,"inertia":0.0,"linear_damp":0.0,"linear_damp_mode":"combine","linear_velocity":[0.0,0.0],"lock_rotation":false,"mass":1.0,"sleeping":false},"doc":"A physics body moved by gravity, collisions, forces and joints. The physics step writes its Transform2D, velocities and sleeping.","name":"RigidBody2D"},{"defaults":{"collision_layer":1,"collision_mask":1,"floor_block_on_wall":true,"floor_constant_speed":false,"floor_max_angle":0.785398,"floor_normal":[0.0,0.0],"floor_snap_length":1.0,"floor_stop_on_slope":true,"max_slides":4,"motion_mode":"grounded","on_ceiling":false,"on_floor":false,"on_wall":false,"platform_floor_layers":4294967295,"platform_on_leave":"add_velocity","platform_velocity":[0.0,0.0],"platform_wall_layers":0,"safe_margin":0.08,"slide_on_ceiling":true,"up_direction":[0.0,-1.0],"velocity":[0.0,0.0],"wall_min_slide_angle":0.261799,"wall_normal":[0.0,0.0]},"doc":"A physics body moved by setting its velocity: each tick it slides along what it hits, as Godot's move_and_slide, and reports floors, walls and ceilings.","name":"CharacterBody2D"},{"defaults":{"angular_damp":1.0,"angular_damp_space_override":"disabled","collision_layer":1,"collision_mask":1,"gravity":980.0,"gravity_direction":[0.0,1.0],"gravity_point":false,"gravity_point_center":[0.0,1.0],"gravity_point_unit_distance":0.0,"gravity_space_override":"disabled","linear_damp":0.1,"linear_damp_space_override":"disabled","monitorable":true,"monitoring":true,"priority":0},"doc":"A region that emits body_entered, body_exited, area_entered and area_exited events, and can change the gravity and damping of rigid bodies in it. It goes where its Transform2D says.","name":"Area2D"},{"defaults":{"angular_limit_enabled":false,"angular_limit_lower":0.0,"angular_limit_upper":0.0,"disable_collision":true,"motor_enabled":false,"motor_target_velocity":0.0,"node_a":null,"node_b":null},"doc":"Pins two bodies together at the entity's Transform2D, letting them turn.","name":"PinJoint2D"},{"defaults":{"damping":1.0,"disable_collision":true,"length":50.0,"node_a":null,"node_b":null,"rest_length":0.0,"stiffness":20.0},"doc":"A spring from node_a at the entity's Transform2D to node_b, length pixels along the entity's y axis.","name":"DampedSpringJoint2D"},{"defaults":{"disable_collision":true,"initial_offset":25.0,"length":50.0,"node_a":null,"node_b":null},"doc":"Keeps node_b on a groove of node_a's, from the entity's Transform2D length pixels along its y axis; node_b slides along it and turns freely.","name":"GrooveJoint2D"},{"defaults":{"amount":8,"angle_curve":[],"angle_max":0.0,"angle_min":0.0,"angular_velocity_curve":[],"angular_velocity_max":0.0,"angular_velocity_min":0.0,"anim_loop":false,"anim_offset_curve":[],"anim_offset_max":0.0,"anim_offset_min":0.0,"anim_speed_curve":[],"anim_speed_max":0.0,"anim_speed_min":0.0,"color":[1.0,1.0,1.0,1.0],"color_initial_ramp":[],"color_ramp":[],"damping_curve":[],"damping_max":0.0,"damping_min":0.0,"direction":[1.0,0.0],"draw_order":"index","emission_colors":[],"emission_normals":[],"emission_points":[],"emission_rect_extents":[1.0,1.0],"emission_ring_inner_radius":0.0,"emission_ring_radius":1.0,"emission_shape":"point","emission_sphere_radius":1.0,"emitting":true,"explosiveness":0.0,"gravity":[0.0,980.0],"hframes":1,"hue_variation_curve":[],"hue_variation_max":0.0,"hue_variation_min":0.0,"initial_velocity_max":0.0,"initial_velocity_min":0.0,"lifetime":1.0,"lifetime_randomness":0.0,"light_mask":1,"linear_accel_curve":[],"linear_accel_max":0.0,"linear_accel_min":0.0,"local_coords":false,"material":"","material_params":{},"one_shot":false,"orbit_velocity_curve":[],"orbit_velocity_max":0.0,"orbit_velocity_min":0.0,"particle_flag_align_y":false,"preprocess":0.0,"radial_accel_curve":[],"radial_accel_max":0.0,"radial_accel_min":0.0,"randomness":0.0,"scale_amount_curve":[],"scale_amount_max":1.0,"scale_amount_min":1.0,"seed":0,"speed_scale":1.0,"spread":45.0,"tangential_accel_curve":[],"tangential_accel_max":0.0,"tangential_accel_min":0.0,"texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_as_relative":true,"z_index":0},"doc":"Emits particles from the entity's Transform2D, moves and draws them, as Godot's CPUParticles2D. Each takes a random value between each _min and _max when emitted; a _curve multiplies it as it ages. Emits finished when a one_shot's last particle dies.","name":"CPUParticles2D"},{"defaults":{"anchor_bottom":0.0,"anchor_left":0.0,"anchor_right":0.0,"anchor_top":0.0,"children":[],"clip_contents":false,"custom_minimum_size":[0.0,0.0],"focus_mode":"","focus_neighbor_bottom":null,"focus_neighbor_left":null,"focus_neighbor_right":null,"focus_neighbor_top":null,"focus_next":null,"focus_previous":null,"grow_horizontal":"end","grow_vertical":"end","layout_direction":"inherited","modulate":[1.0,1.0,1.0,1.0],"mouse_filter":"","offset_bottom":0.0,"offset_left":0.0,"offset_right":0.0,"offset_top":0.0,"pivot_offset":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0],"self_modulate":[1.0,1.0,1.0,1.0],"size_flags_horizontal":"fill","size_flags_stretch_ratio":1.0,"size_flags_vertical":"fill","texture_filter":"inherit","theme":"","theme_override_colors":{},"theme_override_constants":{},"theme_override_font_sizes":{},"theme_override_fonts":{},"theme_override_icons":{},"theme_override_styles":{},"theme_type_variation":"","tooltip_text":"","visible":true,"z_as_relative":true,"z_index":0},"doc":"A UI element's rect, as Godot's Control: placed in its parent's rect by anchors and offsets, or in the viewport when no Control lists it in children. Label, ColorRect and the other UI components draw in it; they lay out with these defaults when the entity has no Control. A container component places its children by size flags instead of their anchors and offsets. Controls draw above the world, or in the CanvasLayer that holds them, parents before children.","name":"Control"},{"defaults":{"autowrap_mode":"off","clip_text":false,"ellipsis_char":"…","horizontal_alignment":"left","language":"","lines_skipped":0,"max_lines_visible":-1,"text":"","text_direction":"auto","text_overrun_behavior":"no_trimming","uppercase":false,"vertical_alignment":"top","visible_characters":-1,"visible_characters_behavior":"chars_before_shaping"},"doc":"Text in the control's rect, as Godot's Label. Theme items: font, font_size, font_color, font_outline_color, outline_size, font_shadow_color, shadow_offset_x, shadow_offset_y, shadow_outline_size, line_spacing and paragraph_spacing.","name":"Label"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Fills the control's rect with one color, as Godot's ColorRect.","name":"ColorRect"},{"defaults":{},"doc":"Draws the theme's panel style box in the control's rect, as Godot's Panel.","name":"Panel"},{"defaults":{"expand_mode":"keep_size","flip_h":false,"flip_v":false,"stretch_mode":"scale","texture":""},"doc":"Draws a texture in the control's rect, as Godot's TextureRect.","name":"TextureRect"},{"defaults":{"axis_stretch_horizontal":"stretch","axis_stretch_vertical":"stretch","draw_center":true,"patch_margin_bottom":0,"patch_margin_left":0,"patch_margin_right":0,"patch_margin_top":0,"region_rect":[0.0,0.0,0.0,0.0],"texture":""},"doc":"Draws a texture cut into nine parts in the control's rect, as Godot's NinePatchRect: the corners keep their size, the sides stretch along and the middle both ways. Its minimum size is its margins.","name":"NinePatchRect"},{"defaults":{"allow_greater":false,"allow_lesser":false,"fill_mode":"begin_to_end","max_value":100.0,"min_value":0.0,"show_percentage":true,"step":0.01,"value":0.0},"doc":"A bar filled to show value between min_value and max_value, with the percentage on it, as Godot's ProgressBar. Theme items: background, fill, font, font_size, font_color, font_outline_color and outline_size.","name":"ProgressBar"},{"defaults":{"alignment":"begin","vertical":false},"doc":"Lays out its children in a row, or a column when vertical, as Godot's HBoxContainer and VBoxContainer: each at its minimum size along it, plus a share of the space left for those that expand. Theme constant: separation.","name":"BoxContainer"},{"defaults":{"columns":1},"doc":"Lays out its children in a grid, as Godot's GridContainer: left to right, a new row every columns children. Theme constants: h_separation and v_separation.","name":"GridContainer"},{"defaults":{},"doc":"Fits its children in its rect less margins, as Godot's MarginContainer. Theme constants: margin_left, margin_top, margin_right and margin_bottom.","name":"MarginContainer"},{"defaults":{"use_top_left":false},"doc":"Keeps its children at their minimum sizes in its center, as Godot's CenterContainer.","name":"CenterContainer"},{"defaults":{"alignment":"begin","last_wrap_alignment":"inherit","reverse_fill":false,"vertical":false},"doc":"Lays out its children in rows that wrap to its width, or columns that wrap to its height when vertical, as Godot's HFlowContainer and VFlowContainer. Theme constants: h_separation and v_separation.","name":"FlowContainer"},{"defaults":{},"doc":"Draws the theme's panel style box and fits its children inside the style's margins, as Godot's PanelContainer.","name":"PanelContainer"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":["input","gui","animation_player","physics2d","animated_sprite_2d","camera_2d","cpu_particles_2d","audio"]}
```

`scene` describes the open scene: how many entities it has, its path (empty for an unsaved one)
and the current tick. In a running game, which has [`play`](#play) and [`pause`](#pause), it also
says whether the game is `playing`.

`gameplay` describes the game's own C++ code (see [`gameplay.build`](#gameplaybuild)) when some
is loaded or failed to load: the `library` it came from, whether it is `loaded`, the
`components` and `systems` it added, and the `error` of the last load that failed.

```json
{"components":["Wallet"],"library":".slash/gameplay/game.so","loaded":true,"systems":["earn"]}
```

`systems` lists the systems in the order they run. With `"full": true`, each is an object:

```json
{"name":"movement","doc":"Moves things by their velocity.","priority":0}
```

### schema.get

Returns a JSON Schema: for scene, prefab, [`.frames.json`](scene-format.md#sprite-frames),
[`.material.json`](scene-format.md#materials), [`.anim.json`](scene-format.md#animations),
[`.process_material.json`](scene-format.md#process-materials),
[`.tileset.json`](scene-format.md#tile-sets), [`.font.json`](scene-format.md#fonts),
[`.theme.json`](scene-format.md#themes), [`.audio.json`](scene-format.md#audio-streams),
[`.bus_layout.json`](scene-format.md#bus-layouts), [`.replay.json`](scene-format.md#replays),
[`.test.json`](scene-format.md#tests) or storyboard files, one component or one command.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | `"scene"`, `"prefab"`, `"frames"`, `"material"`, `"animation"`, `"process_material"`, `"tileset"`, `"font"`, `"theme"`, `"audio"`, `"bus_layout"`, `"replay"`, `"test"`, `"storyboard"`, `"component"` or `"command"` | required | Which schema |
| `name` | string | | Component or command name, for `component` and `command` |

Needs: `read`.

```json
{"kind": "command", "name": "step"}
```

```json
{"additionalProperties":false,"properties":{"tick_seconds":{"default":0.016666666666666666,"description":"Seconds per tick.","exclusiveMinimum":0,"maximum":1,"type":"number"},"ticks":{"default":1,"description":"Ticks to run.","maximum":100000,"minimum":1,"type":"integer"}},"type":"object"}
```

## Scene

### scene.get

Returns the whole open scene in scene-file form, with default fields left out. Prefer
`world.query` or `entity.get` for parts of it.

No params. Needs: `read`.

```json
{"path":"basic.scene.json","scene":{"entities":{"camera":{"components":{"Transform3D":{"position":[0.0,2.0,-5.0]}}},"player":{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}}},"spawn_point":{}},"format":1}}
```

### scene.new

Replaces the open scene with an empty one and clears undo history.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | Where `scene.save` will write it, e.g. `"scenes/level2.scene.json"`. Nothing is written yet |

Needs: `edit`. Takes `dry_run`.

```json
{"path": "scenes/two.scene.json"}
```

```json
{"path":"scenes/two.scene.json"}
```

### scene.open

Loads a `.scene.json` file in place of the open scene and clears undo history. An invalid file
changes nothing and every error is listed. Prefabs its entities extend are read from the project
folder.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | required | Scene file, e.g. `"scenes/main.scene.json"` |

Needs: `edit`, `filesystem`. Takes `dry_run`.

```json
{"path": "basic.scene.json"}
```

```json
{"entities":3,"path":"basic.scene.json"}
```

### scene.save

Writes the open scene to a `.scene.json` file in canonical form, creating folders as needed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | the scene's current file | File to write |
| `root` | string | the scene's root | The entity an instance of this scene becomes when another scene's entity extends it ([instanced scenes](scene-format.md#instanced-scenes)); `""` for none. Kept for later saves |

Needs: `read`, `filesystem`.

```json
{"path": "out/level.scene.json"}
```

```json
{"entities":3,"path":"out/level.scene.json"}
```

```json
{"path": "scenes/player.scene.json", "root": "player"}
```

```json
{"entities":4,"path":"scenes/player.scene.json","root":"player"}
```

A `root` no entity has fails with `-32001`, and one an instanced scene brings with `-32002`.

A path outside the project folder fails with `-32003`:

```json
{"code":-32003,"data":{"path":"/path"},"message":"/path: path \"../x.scene.json\" leaves the project folder"}
```

## Entities

### entity.create

Creates an entity, optionally from a prefab and with components. Returns its ID.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | generated | Stable ID: 1 to 64 letters, digits, `_` or `-` |
| `extends` | string | | A `.prefab.json` file to start from, e.g. `"prefabs/enemy.prefab.json"`, or a `.scene.json` file with a `root` |
| `components` | object | | Components keyed by type name. Fields left out keep their defaults, or the prefab's |

Needs: `edit`. Takes `dry_run`.

```json
{"id": "enemy_01", "components": {"Transform2D": {"position": [200, 40]}}}
```

```json
{"id":"enemy_01"}
```

Left out, the ID is generated (`entity_1`, `entity_2` and so on). An ID already in use is an
error. With `extends`, the entity gets the prefab's components and stays linked to it, so saving
the scene writes only the fields that differ from the prefab ([prefabs](scene-format.md#prefabs)).
Each command reads prefab files afresh, so an edited prefab shows up in the next instance.

With a scene, the entity becomes the scene's root and the scene's other entities come too, as
`<id>/<their id>` ([instanced scenes](scene-format.md#instanced-scenes)); `entity.get` and
`component.set` take those IDs. A scene with no `root` fails with `-32002`:

```json
{"code":-32002,"data":{"path":"/extends"},"message":"/extends: scene \"scenes/level.scene.json\" has no \"root\": name the entity an instance of it becomes (scene.save takes \"root\")"}
```

### entity.destroy

Destroys an entity and its components, and the entities in its `Transform2D`, `Control` or
`CanvasLayer` `children`, theirs too, as Godot frees a node's children, and those its instanced
scene brought.
They are listed as `descendants`, and leave the `children` lists of the parents that stay.
Destroying one entity an instanced scene brought saves as `null` for it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |

Needs: `edit`. Takes `dry_run`.

```json
{"id": "camera"}
```

```json
{"destroyed":"camera"}
```

```json
{"descendants":["mast","cannon","flag"],"destroyed":"boat"}
```

### entity.get

Returns an entity's components, and the prefab it extends, if any, as `extends`. For an entity
in another's `Transform2D` `children`, it adds that `parent` and the `global` transform its
parents put it at ([parent transforms](scene-format.md#parent-transforms)), rounded to four
decimals.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `components` | array of strings | all | Only these components |
| `omit_defaults` | boolean | `false` | Leave out fields equal to their defaults |

Needs: `read`.

```json
{"id": "player", "omit_defaults": true}
```

```json
{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}},"id":"player"}
```

```json
{"components":{"Transform2D":{"position":[6.0,-9.0]}},"global":{"position":[122,93],"rotation":0,"scale":[1,1]},"id":"flag","parent":"mast"}
```

An unknown ID fails with `-32001` (not found):

```json
{"code":-32001,"data":{"path":"/id"},"message":"/id: no entity has the id \"nobody\""}
```

## Prefabs

### prefab.save

Writes an entity to a `.prefab.json` file that entities can extend, creating folders as needed.
An entity that extends a prefab is saved as `extends` plus what differs from it. The entity
itself is not linked to the new file; `entity.create` with `extends` makes instances. A prefab
holds one entity, so an instance of a scene, or an entity one brought, fails with `-32002`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `path` | string | required | File relative to the project folder, e.g. `"prefabs/enemy.prefab.json"` |

Needs: `read`, `filesystem`.

```json
{"id": "player", "path": "prefabs/player.prefab.json"}
```

```json
{"components":["Transform2D"],"path":"prefabs/player.prefab.json"}
```

Saving an entity to the prefab it extends fails, since the prefab would extend itself.

## Assets

### asset.list

Lists loaded assets and the prefab files entities extend. `version` is 1 when a file is first
loaded and one more each time it reloads; `refs` counts the handles to an asset, or the entities
that extend a prefab. Files reload on their own when they change (see
[hot reload](../concepts.md#assets)), so this command is for checking what is loaded and whether
an edit was picked up.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `type` | string | all | Only assets of this type: `prefab`, `json`, `text` or one a game added |

Needs: `read`.

```json
{}
```

```json
{"assets":[{"path":"data/tuning.json","refs":1,"type":"json","version":2},{"path":"prefabs/enemy.prefab.json","refs":3,"type":"prefab","version":1}]}
```

## Materials

### material.eval

Returns the colors a [material](scene-format.md#materials) draws at texture coordinates, before
blending, from 0 to 1 and computed on the CPU as the GPU computes them: a check that needs no
screenshot. Given an entity, it uses its `Sprite2D`'s material, texture, `material_params` and
`modulate`; the other params try other values. A sprite with no material gives its texture times
its modulate.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | | An entity with a `Sprite2D`. Give `id` or `path` |
| `path` | string | | A `.material.json` file |
| `uv` | array of `[u, v]` | required | The texture coordinates, 1 to 64 of them |
| `texture` | string | the entity's, or none | The sprite's texture path; none samples white |
| `params` | object | | Param values by name, over the entity's `material_params` |
| `color` | `[r, g, b, a]` | the entity's `modulate`, or white | What the `color` input reads |
| `frag_coord` | `[x, y]` | `[0, 0]` | What `fragcoord` reads, in pixels |
| `time` | number | `0` | What `time` reads, in seconds |

Needs: `read`.

```json
{"id": "hero", "uv": [[0.5, 0.5]], "params": {"flash": 0.5}}
```

```json
{"colors":[[1.0,0.625,0.5,1.0]]}
```

Colors are rounded to three decimals. A param the material doesn't have fails with the closest
name, and a value of the wrong type or outside the param's range says what it expected; the
error's `path` is `/params/<name>` for a param given here and `/id` for one of the entity's.

A material with a [shader](scene-format.md#shaders) runs its `fragment()` on the CPU, with
derivatives of 0 and no `vertex()`; a discarded pixel is `[0, 0, 0, 0]`. A shader that doesn't
compile fails with its file and line. `color`, `frag_coord` and `time` are what `COLOR` (times
the texture), `FRAGCOORD` and `TIME` read.

## Animations

### animation.eval

Returns the values an [animation](scene-format.md#animations)'s value tracks set at the times
given, as `AnimationPlayer` sets them, without playing it: a check that needs no stepping. Given
an entity, it uses its `AnimationPlayer`'s library and, unless `animation` names another, its
animation.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | | An entity with an `AnimationPlayer`. Give `id` or `path` |
| `path` | string | | A `.anim.json` file |
| `animation` | string | the entity's | The animation's name; required with `path` |
| `times` | array of numbers | required | Seconds into the animation, 1 to 100 of them, each from 0 to its length |
| `tracks` | array of strings | all | The value tracks to return, by path, or `"target:path"` for a track with a target |

Needs: `read`.

```json
{"id": "hero", "times": [0, 0.125, 0.25, 0.375]}
```

```json
{"length":0.5,"loop":"linear","tracks":{"Sprite2D/frame":[0,1,2,3],"Sprite2D/offset/y":[0,-2,-4,-2]}}
```

`tracks` holds one value per time for each value track, keyed by path, or for a track with a
target by its target, `:` and its path, as Godot's `Node:property` (`sword:Transform2D/rotation`).
Numbers are rounded to four decimals. A discrete track gives the value of the last key at or before each time. An
unknown animation or track path fails with the closest name, and a time past the animation's
length fails with its `path`, `/times/<n>`. Event tracks are left out; read them in the file.

## Tile maps

These commands edit an entity's [`TileMapLayer`](scene-format.md#tile-map-layers) through its
[tile set](scene-format.md#tile-sets), and write its cells back in canonical form. Each takes
the cells as `cells`, a list of `[column, row]` (up to 4096), or `rect`,
`[column, row, width, height]` (up to 65536 cells); give one. Columns and rows are from -32768 to
32767. A layer whose tile set doesn't load, or whose legend or rows are wrong, fails with `path`
`/id` and the message names the bad value, as `"map"'s TileMapLayer/rows/2: ...`.

### tilemap.paint

Paints a terrain on cells and picks tiles for them and the terrain cells around them so their
edges connect, as Godot's terrain painting does. `null` erases the cells and fixes up their
neighbors. Returns how many cells changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `TileMapLayer` |
| `cells` | array of `[column, row]` | | The cells to paint. Give `cells` or `rect` |
| `rect` | `[column, row, width, height]` | | Every cell in it |
| `terrain` | string or `null` | required | A terrain of the tile set, or `null` to erase |

Needs: `edit`.

```json
{"id": "ground", "rect": [2, 1, 8, 6], "terrain": "grass"}
```

```json
{"changed":48,"id":"ground"}
```

Each cell gets the tile of its terrain whose `peering` fits its neighbors best, first where its
own terrain stops and then which terrain lies beyond; empty cells count as no terrain, so
painting in empty space draws edges. Equal fits are picked by the cell's position, weighted by
`probability`, so the same paint gives the same tiles. An unknown terrain fails with the
closest name.

### tilemap.set

Sets cells to one tile, or erases them, without changing the tiles around them. Returns how
many cells changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `TileMapLayer` |
| `cells` | array of `[column, row]` | | The cells to set. Give `cells` or `rect` |
| `rect` | `[column, row, width, height]` | | Every cell in it |
| `tile` | string or `null` | required | A tile by name or as `"source:column,row"`, then `:<alternative>` and `:<turns>` if any (`"slope:h"`, `"rock:1:hv"`), or `null` to erase |

Needs: `edit`.

```json
{"id": "ground", "cells": [[8, 2]], "tile": "rock"}
```

```json
{"changed":1,"id":"ground"}
```

An unknown tile or source fails with the closest name, and an unknown alternative with the ones
the tile has. Turns are `h` (flip left to right), `v` (top to bottom) and `t` (swap the axes),
each at most once and in that order, as Godot's cell transform flags; see
[turned cells](scene-format.md#alternative-tiles-and-turned-cells). The tile's collision polygons
turn with it.

### tilemap.get

Returns a layer's cells as rows of legend keys from an origin, with `.` for an empty cell: the
whole layer as stored, or a window of it in the layer's own keys. Given `cells`, it returns each
one's tile and [custom data](scene-format.md#tile-occluders-and-custom-data) instead.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `TileMapLayer` |
| `rect` | `[column, row, width, height]` | the whole layer | Only these cells |
| `cells` | array of `[column, row]` | | 1 to 1024 cells whose tiles and custom data to return. Not with `rect` |

Needs: `read`.

```json
{"id": "ground", "rect": [3, 2, 3, 3]}
```

```json
{"legend":{"G":"grass_n","a":"grass_s","b":"grass_w","c":"grass_e","j":"grass_in_se","k":"grass_in_sw","l":"grass_in_ne","m":"grass_in_nw","w":"water"},"origin":[3,2],"rows":["jak","cwb","lGm"]}
```

With `rect`, `origin` is the rect's top left, there is a row for each of its rows, and each row
ends at its last tile. `legend` holds only the keys the rows use.

With `cells`, in [examples/tile-data-and-occluders](../../examples/tile-data-and-occluders/):

```json
{"id": "ground", "cells": [[2, 3], [5, 3]]}
```

```json
{"cells":[{"cell":[2,3],"data":{"damage":10,"kind":"lava","speed":0.5},"tile":"lava"},{"cell":[5,3],"data":{"damage":0,"kind":"stone","speed":1.0},"tile":"floor"}]}
```

Each cell has its `tile` as a legend would name it, or `null` when empty, and `data`, its value
in every custom data layer of the tile set, the layer type's default where the tile sets none.
A tile set with no custom data layers gives no `data`.

### tilemap.convert

Turns cells into the points at their centers, or points into the cells they are in, as Godot's
`map_to_local` and `local_to_map` through the layer's `Transform2D` (and its parents'). Points
are in the canvas the layer draws in: the world's, unless the layer is in a `CanvasLayer`. Cells
need not hold a tile. For squares the answer is plain arithmetic; for diamonds and hexagons this
is how an agent finds where a cell is drawn or which cell a click landed in
([tile shapes](scene-format.md#tile-shapes)).

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `TileMapLayer` |
| `cells` | array of `[column, row]` | | Up to 1024 cells: returns `points`, their centers |
| `points` | array of `[x, y]` | | Up to 1024 points: returns `cells`, the cells they are in |

Needs: `read`. Give `cells`, `points` or both.

```json
{"id": "board", "points": [[136, 149], [136, 156]]}
```

```json
{"cells":[[3,0],[3,1]]}
```

The two points lie either side of the slanted edge between two hexagons of
[examples/isometric-and-hex-tiles](../../examples/isometric-and-hex-tiles/).

## Physics

Questions about the scene's [2D physics](scene-format.md#2d-physics) bodies and areas, as
Godot's `PhysicsDirectSpaceState2D` queries. They see the world as the last step left it, plus
any bodies added, moved or removed since. The examples are the scene of
[examples/physics-2d](../../examples/physics-2d/) after six seconds, with the hero standing at
(420.3, 189) on the hill.

`physics.raycast` and `physics.overlap` take the same filter, as Godot's query parameters:

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `collision_mask` | integer | `4294967295` | Look only in these physics layers, as bits; the default is all of them |
| `exclude` | array of strings | `[]` | Entity IDs to leave out |
| `collide_with_bodies` | boolean | `true` | Find bodies |
| `collide_with_areas` | boolean | `false` | Find areas |

### physics.raycast

Returns the first body (or area, with `collide_with_areas`) that a ray from `from` to `to`
hits: its ID, the point, the surface normal there and the fraction of the way the ray got. `hit`
is `null` if it hits nothing. A ray that starts inside a shape doesn't hit that shape. A ray that
hits a [tile map layer's](scene-format.md#tile-collision) tile also gives the `cell` it hit, as
`[column, row]`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `from` | `[x, y]` | required | Where the ray starts |
| `to` | `[x, y]` | required | Where it ends |

Needs: `read`. Straight down from the hero:

```json
{"from": [420, 189], "to": [420, 300], "exclude": ["hero"]}
```

```json
{"hit":{"fraction":0.1441,"id":"hill","normal":[0,-1],"point":[420,205]}}
```

Numbers are rounded to four decimals. A ray down onto a tile map layer, from
[examples/tile-collision](../../examples/tile-collision/):

```json
{"hit":{"cell":[4,6],"fraction":0.75,"id":"level","normal":[0,-1],"point":[72,96]}}
```

### physics.overlap

Returns the IDs of the bodies (or areas) whose shapes contain a point, or overlap a shape placed
somewhere, sorted.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `point` | `[x, y]` | | Find shapes containing this point. Give `point` or `shape` |
| `shape` | object | | `CollisionShape2D` fields, e.g. `{"shape": "circle", "radius": 8}`; fields left out take the component's defaults, so `{}` is a 20 by 20 rectangle |
| `position` | `[x, y]` | `[0, 0]` | Where the shape goes |
| `rotation` | number | `0` | The shape's rotation, in radians clockwise |
| `max_results` | integer | `32` | At most this many IDs, 1 to 256 |

Needs: `read`. A point in the crate, then a 40-pixel box over the ledge, areas included (the coin
that was there has been taken):

```json
{"point": [150, 240]}
```

```json
{"ids":["crate"]}
```

```json
{"shape": {"size": [40, 40]}, "position": [190, 175], "collide_with_areas": true}
```

```json
{"ids":["ledge"]}
```

A misspelled shape name fails at `/shape/shape` with a "did you mean".

### physics.contacts

Returns what touches an entity after the last step. For a body, `bodies` lists the bodies
touching it (for a `CharacterBody2D`, what its last move hit and the floor it stands on) and
`areas` the areas it is in; for an area, what is in it. As Godot's `get_colliding_bodies`,
`get_overlapping_bodies` and `get_overlapping_areas`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | A body or area |

Needs: `read`.

```json
{"id": "hero"}
```

```json
{"areas":[],"bodies":["hill"]}
```

An entity with no body or area returns empty lists.

### physics.gravity

Returns the gravity and damping each body gets after the last step, from the
[areas](scene-format.md#gravity-and-damping-zones) it is in and the project's settings, as
Godot's `get_gravity`. A rigid body's `gravity_scale` and its own damping are applied. `areas`
lists the areas that set them, highest priority first, and is left out when none do. A
`CharacterBody2D` gets the gravity its gameplay code should add to its velocity.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ids` | string array | required | 1 to 256 bodies |

Needs: `read`. The rock below the planet in
[examples/gravity-and-conveyors](../../examples/gravity-and-conveyors/), falling up:

```json
{"ids": ["rock_below"]}
```

```json
{"bodies":{"rock_below":{"angular_damp":1,"areas":["planet_pull"],"gravity":[0,-440.894],"linear_damp":0.1}}}
```

An unknown ID, or an entity with no body or with an `Area2D`, is an error at its index in
`/ids`.

## Particles

### particles.eval

Returns a [`CPUParticles2D`](scene-format.md#particles)'s or
[`GPUParticles2D`](scene-format.md#gpu-particles)'s live particles: how many, the box they fill
in the world and, with `list`, each one. Without `time`, the particles the world has after its
last tick; a `GPUParticles2D`'s are made by running the steps its emitter kept on the CPU, as
every device runs them. With `time`, it runs the emitter's settings from the start for that many
seconds, from where the entity is, apart from the world, which it leaves unchanged: what that
many ticks would give, since the same seed, entity ID and ticks give the same particles. A
`GPUParticles2D` whose process material doesn't load fails with the file's errors.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `CPUParticles2D` or `GPUParticles2D` |
| `time` | number | | Seconds to run the settings from the start, 0 to 3600; preprocess runs first |
| `tick_seconds` | number | `1/60` | The tick length for `time`, above 0 and at most 1 |
| `list` | boolean | `false` | Also return each particle |
| `max_results` | integer | `32` | List at most this many, 1 to 256 |

Needs: `read`. The sparks of [examples/particles](../../examples/particles/) half a second after
they went off:

```json
{"id": "sparks", "list": true, "max_results": 2}
```

```json
{"alive":24,"bounds":[125.0635,58.4695,113.0968,114.781],"particles":[{"age":0.5,"color":[1,0.325,0.0833,0.1667],"position":[238.1602,134.3399],"rotation":-0.694,"scale":2.262},{"age":0.5,"color":[1,0.325,0.0833,0.1667],"position":[125.0635,136.8566],"rotation":0.6476,"scale":2.0231}]}
```

`bounds` is `[x, y, width, height]` round the particles' positions, or `null` when none are
alive. Each particle has its position in the world (particles in `local_coords` are placed by
the entity's `Transform2D`), its rotation in radians clockwise, its scale, its color as
`[r, g, b, a]` and its age in seconds, in draw order. Numbers are rounded to four decimals.

## Lights

### light.sample

Returns how lit points are: the color a white pixel of a lit sprite gets there from the scene's
[2D lights](scene-format.md#2d-lights), computed on the CPU as the GPU shades it, and each light
that reaches it. A check that needs no screenshot. Points are in the world's canvas. Given an
entity, it samples at its `Transform2D` position in the [canvas](scene-format.md#canvas-layers-and-parallax)
it draws in, with its `Sprite2D`'s (or `TileMapLayer`'s) `light_mask` and `z_index`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `points` | array of `[x, y]` | | The points, 1 to 256 of them. Give `points` or `id` |
| `id` | string | | An entity with a `Transform2D` |
| `light_mask` | integer | the entity's, or `1` | The draw's light mask, 0 to 1048575 |
| `z_index` | integer | the entity's, or `0` | The draw's z index, for the lights' z ranges |

Needs: `read`. In the scene of [examples/lights-2d](../../examples/lights-2d/), a point by the
torch and one behind the crate from it:

```json
{"points": [[96, 60], [200, 90]]}
```

```json
{"ambient":[0.12,0.12,0.2],"lit":true,"points":[{"color":[1.244,1.02,0.9],"lights":[{"color":[0.125,0.15,0.25],"id":"moon"},{"color":[0.999,0.75,0.45],"id":"torch","shadow":0}]},{"color":[0.245,0.27,0.45],"lights":[{"color":[0.125,0.15,0.25],"id":"moon"},{"color":[0,0,0],"id":"torch","shadow":1}]}]}
```

- `ambient` is the color of the sampled canvas's `CanvasModulate`, and `lit` says whether
  anything lights that canvas; when nothing does, draws there are unshaded and every point is
  white. For an entity in a `CanvasLayer`, `layer` is the layer's ID, and only lights whose
  layer range holds its layer are listed.
- Each point's `color` is `[r, g, b]` after the ambient color and every light, before clamping,
  so a value above 1 means the light is saturated there.
- `lights` lists the lights that reach the point, in draw order, each with its color there times
  its alpha (what it adds with `blend_mode` `add`). A light with `shadow_enabled` and occluders
  in the scene, `LightOccluder2D`s or tiles' occluders, also has `shadow`, from 0 (lit) to 1
  (hidden by occluders). A light whose texture is clear at the point
  is left out.
- Normal maps aren't sampled: the pixel faces the viewer. Values are rounded to three decimals.
- A light that can't shine, such as one whose texture doesn't load, is left out and listed in
  `warnings` with its entity's ID and why.

## UI and text

### ui.layout

Returns where [controls](scene-format.md#ui-and-text) are on screen, in drawing order, with the
lines each label, button, progress bar and text field shows, how each
[button](scene-format.md#buttons) draws, each [slider](scene-format.md#sliders)'s value, where
each [text field](scene-format.md#text-fields)'s caret and selection are, how far each
[scroll container](scene-format.md#scrolling) is scrolled and what its clip cuts off, which
[popup menus](scene-format.md#menus) are open and where their items are, which control has
focus and which is hovered, and the mistakes that keep controls from laying out or working as
written. A check that needs no screenshot.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ids` | array of strings | all | Only these controls (and the popups of these menus), and only their problems |
| `viewport` | `[width, height]` | the project's `window` size | The viewport to lay out in, to try another window size |

Needs: `read`. In the scene of [examples/ui-and-text](../../examples/ui-and-text/), the panel and
the sign on a smaller window:

```json
{"ids": ["panel", "sign"], "viewport": [480, 270]}
```

```json
{"controls":[{"id":"panel","rect":[90,45,300,180]},{"id":"sign","lines":["出口 Exit"],"missing_glyphs":2,"rect":[424,8,44,22]}],"viewport":[480,270]}
```

- `rect` is `[x, y, width, height]` in the viewport: the box round the control as it draws,
  turned and scaled, and placed by its [canvas layer](scene-format.md#canvas-layers-and-parallax)
  if it is in one, which `layer` names. A control hidden by its own, a parent's or its layer's
  `visible` has `"visible": false`.
- A label has `lines`, the lines it shows, without trailing spaces, those cut ending with the
  ellipsis. A button with text has its lines too, and a progress bar showing its percentage has
  it as its one line, e.g. `["72%"]`. `overflow` is `true` when a line is wider than the rect or
  lines don't fit its height. `rtl` is `true` when its lines go right to left (a list of one
  `true` or `false` per line when only some do). `missing_glyphs` counts characters no font has,
  `system_glyphs` characters drawn from fonts installed on the machine (other machines may lack
  them), and `font_error` says why its font didn't load, in which case the built-in font drew
  it.
- A `Button`, `CheckBox`, `CheckButton`, `OptionButton`, `MenuButton` or `TextureButton` has
  `draw_mode`: `"normal"`,
  `"hover"`, `"pressed"`, `"hover_pressed"` or `"disabled"`, as Godot's. A toggle button that is
  on (a check box that is checked) draws `"pressed"`, and `"hover_pressed"` while hovered; a
  button its shortcut just pressed draws `"hover_pressed"` for 0.2 seconds. The control with focus has `"focused": true`, and the one under the mouse
  `"hovered": true`.
- `popups` lists the popup menus open, the last on top: `id`, the `PopupMenu`, `OptionButton` or
  `MenuButton` it belongs to, `rect` in the viewport, `items` (each with its `text`, `rect` in
  the viewport and `"selectable": false` for a separator or a disabled item), `focused_item`
  while an item is focused, and `scroll` and `scroll_max` when it scrolls. From
  [examples/ui-menus](../../examples/ui-menus/):

  ```json
  {"popups":[{"id":"game","items":[{"rect":[4,36,89,22],"text":"New Game"},{"rect":[4,62,89,22],"selectable":false,"text":"Load"},{"rect":[4,88,89,0],"selectable":false,"text":""},{"rect":[4,92,89,22],"text":"Quit"}],"rect":[0,30,97,90]}]}
  ```

- `tooltip` is the [tooltip](scene-format.md#tooltips-and-drag-and-drop) shown: `id`, the
  control it belongs to (or the popup menu whose item's it is), `text` and `rect` in the viewport.
  It is left out when `ids` doesn't name its control.
- `drag` is the drag going on: `from`, the control dragged, `data`, what it carries, `position`,
  the pointer, `over`, the control that would take the drop there (left out for none), and
  `preview`, the control following the pointer, or `preview_text`, a text field's dragged text.
  From [examples/ui-drag-and-drop](../../examples/ui-drag-and-drop/):

  ```json
  {"drag":{"data":{"id":"sword","kind":"item"},"from":"sword","over":"slot_3","position":[152,64],"preview":"sword_ghost"}}
  ```

- A `Slider` has `value`, on its step and in range, and `ratio`, how far along it is from 0 to
  1, e.g. `{"id": "volume", "ratio": 0.8, "rect": [66, 147, 226, 16], "value": 80}`. A
  `ScrollBar` has them too.
- A `ScrollContainer` has `scroll`, `[horizontal, vertical]` pixels scrolled, `scroll_max`, how
  far each can go, and `scroll_bars`, the bars showing (`"horizontal"`, `"vertical"`), from
  [examples/ui-scrolling](../../examples/ui-scrolling/):
  `{"id": "log", "rect": [8, 8, 160, 164], "scroll": [0, 41], "scroll_bars": ["vertical"], "scroll_max": [0, 172]}`.
- A control a [clipping](scene-format.md#scrolling) parent cuts off has `shown_rect`, the part
  that shows, or `"clipped": true` when none of it does; `rect` is still where it is.
- A `LineEdit`'s `lines` are what it shows: its text with an input method's composition at the
  caret, a secret field's dots, or its placeholder. It has `line_edit`, from
  [examples/text-input](../../examples/text-input/):

  ```json
  {"focused":true,"id":"name","line_edit":{"caret_column":7,"editing":true,"selection":[0,7]},"lines":["Mira 未来"],"rect":[40,42,240,30]}
  ```

  `caret_column` counts characters before the caret; `editing` is listed while it takes typing,
  `selection` (`[from, to)` in characters) while text is selected, `ime` while an input method
  is composing, `placeholder` while the placeholder shows, and `visible` (`[from, to)` of what
  it shows) when text is scrolled out of the field. A font that doesn't load is `font_error`.
- A `TextEdit`'s `lines` are its rows in view, with an input method's composition at the caret
  or its placeholder. It has `text_edit`, from
  [examples/text-editor](../../examples/text-editor/):

  ```json
  {"focused":true,"id":"letter","lines":["Meet me at the old lighthouse before","the tide turns.","","\tBring the lantern.","Kai"],"rect":[16,38,288,124],"text_edit":{"caret":[4,3],"rows":7,"v_scroll_bar":{"grabber":[296,79.7143,8,74.2857]},"visible_lines":[1,4]}}
  ```

  `caret` is `[line, column]`, from 0, the column counting characters; `selection`
  (`[[line, column], [line, column]]`, in text order) is listed while text is selected, `ime`
  while an input method is composing, `placeholder` while the placeholder shows,
  `visible_lines` (the first and last lines with a row in view) when some are scrolled out,
  `rows` when wrapping makes more rows than lines, `v_scroll_bar` and `h_scroll_bar` while
  they show (`grabber`, the grabber's rect on screen, for a drag with
  [`input.inject`](#inputinject)), `search_results` (`[line, column]` of each match of its
  `search_text` in view), `gutters` and `line_height` while it draws
  [gutters](scene-format.md#gutters-and-line-numbers), and `font_error` for a font that doesn't
  load. Each gutter has its `name` (a `CodeEdit`'s are `main_gutter` and `line_numbers`) and
  `rect` on screen, from the first row's top: a line in view is
  `(line - visible_lines[0]) * line_height` below it when scrolled to a whole row, as in
  [examples/code-editor](../../examples/code-editor/):

  ```json
  "gutters":[{"name":"main_gutter","rect":[12,12,26,152]},{"name":"line_numbers","rect":[38,12,36,152]}],"line_height":26
  ```

  The gutters' text is left out of `lines`.
- `problems` lists, for each control, a `children` entry that can't be laid out (no such
  entity, not a control, the control itself, a second parent or a loop), a second container on
  one entity, a theme override name its type doesn't have, with the closest one, a style box
  override with a mistake, a theme that doesn't load, a `theme_type_variation` no theme
  defines, a font or texture that doesn't load, a button `shortcut` entry that isn't a binding
  or names no action, a `button_group` on a button without `toggle_mode`, a `Label` on a button
  entity (a button draws its own text), two buttons on one entity, a `focus_*` field that
  names no control, a `LineEdit` with a `Label` or button on its entity, a
  `secret_character` longer than one character, a `TextEdit` with a `Label`, button or
  `LineEdit` on its entity, a menu item's `checkable` that isn't `none`, `check` or `radio`, a
  `PopupMenu` on a control entity, an `OptionButton`'s `selected` past its options, a
  `tooltip_text` on a control the pointer passes by, a `drag_preview` that isn't a control with
  no parent and a `DragAndDrop` on an entity with no `Control`. Each has
  the
  control's `id`, the JSON Pointer `path` of the value in the entity and a `message`:

```json
{"id":"hint","message":"Label has no colors item \"font_colour\"; did you mean \"font_color\"?","path":"/components/Control/theme_override_colors/font_colour"}
```

An ID that names no entity, or an entity with no `Control` or other UI component, is an error at
its place in `ids`.

### ui.theme

Returns the theme items a control uses, each with its value and where it comes from: its
`theme_override_*` fields, a [theme](scene-format.md#themes) file, or Godot's default theme.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The control |
| `items` | array of strings | all | Only these: kinds (`"colors"`, `"constants"`, `"fonts"`, `"font_sizes"`, `"styles"`) or items such as `"constants/separation"` |

Needs: `read`. In the scene of [examples/ui-containers](../../examples/ui-containers/), a slot's
panel, from the theme's `Slot` variation:

```json
{"id": "slot_0", "items": ["styles"]}
```

```json
{"items":[{"from":"ui/game.theme.json","item":"styles/panel","value":{"bg_color":[0.2,0.22,0.3,1.0],"content_margin":8.0,"corner_radius":6}}]}
```

- `item` is `"kind/name"`, sorted by kind then name. Every item the control's types have is
  listed: an entity with a `Label` and a `Panel` has both types' items.
- `from` is `"override"`, the project path of the theme file that gave it, or `"default"`.
- Style boxes are written as in a theme file, with only the fields that differ from a flat
  box's defaults.
- An item name the control doesn't use is an error at its place in `items`, with the closest
  one; so is an `id` that names no entity or no control.

### ui.press

Presses a [`Button`, `CheckBox`, `CheckButton`, `OptionButton`, `MenuButton` or
`TextureButton`](scene-format.md#buttons) at the next tick, as a click released over it would: a
toggle button toggles (and turns the others in its `button_group` off), an `OptionButton` or
`MenuButton` opens its list (or closes it, if open) with the option shown or its first item
focused, as a key would, then it emits `pressed`. Nothing else about the pointer or focus changes.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The button |

Needs: `run`. Changes no entities until the next `step`, so there is nothing to undo; a failed
`batch` takes the press back.

```json
{"id": "play"}
```

```json
{"tick":4}
```

`tick` is the tick it is pressed at, which the next `step` runs. An ID that names no entity is an
error with the closest one, and so are an entity with neither button component and a disabled
button.

To press a button the way a player does, with hover, focus, `button_down` and `button_up`, send
the click or tap with [`input.inject`](#inputinject) instead.

### ui.popup

Opens a [popup menu](scene-format.md#menus): a `PopupMenu` at its `position` (setting
`visible`), or an `OptionButton`'s or `MenuButton`'s list below it, as their `show_popup` does,
with the option shown (or the first item) focused. Any other popup closes. `"open": false`
closes it instead.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The `PopupMenu`, `OptionButton` or `MenuButton` |
| `open` | boolean | `true` | `false` closes it |

Needs: `run`. A `PopupMenu` or `MenuButton` emits `about_to_popup` as it opens and
`popup_hide` as it closes; there is nothing to undo. From [examples/ui-menus](../../examples/ui-menus/):

```json
{"id": "context"}
```

```json
{"open":["context"]}
```

`open` lists the popups open now. Unlike a press, opening an option button's or menu button's
list this way doesn't press the button. An ID that names no entity is an error with the closest
one, and so are an entity with none of the three components and a button that is hidden.

### ui.select

Picks an item of a [popup menu](scene-format.md#menus) at the next tick, as a click on it would,
whether its list is open or not: an `OptionButton` selects the option, setting `selected` and
emitting `item_selected` (unless it is selected already and `allow_reselect` is off); a
`PopupMenu` or `MenuButton` emits `id_pressed` and `index_pressed`. An open list closes as a
click would close it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The `PopupMenu`, `OptionButton` or `MenuButton` |
| `index` | integer | | The item's index |
| `item_id` | integer | | The item's `id` (its index, if it has none) |
| `text` | string | | The first item with this text |

Needs: `run`. Give one of `index`, `item_id` and `text`. From
[examples/ui-menus](../../examples/ui-menus/):

```json
{"id": "context", "text": "Rename"}
```

```json
{"index":3,"tick":5}
```

`index` is the item picked and `tick` the tick it is picked at, which the next `step` runs. An
item that doesn't exist (with the closest text), a separator, a disabled item and a disabled
button are errors at their param.

### ui.drag

Drags one control onto another at the next tick, as the mouse would from the middle of `from` to
the middle of `to`: `from` (or the first parent that gives something) gives its
[`DragAndDrop`](scene-format.md#tooltips-and-drag-and-drop) `drag_data` or a text field's
selection, and `to` (or the first parent that takes it) takes it if one of its `can_drop`
patterns matches, or as a text field takes text.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `from` | string | required | The control dragged |
| `to` | string | required | The control it is dropped on |

Needs: `run`. The tick emits `drag_begin` from the control dragged, `drop_data` from the one
that takes it (data `{"data", "from", "position"}`) and `drag_end` (data `{"successful"}`);
read them with [`events.read`](#eventsread). What a drop changes is up to the game's code. From
[examples/ui-drag-and-drop](../../examples/ui-drag-and-drop/):

```json
{"from": "potion", "to": "sword"}
```

```json
{"tick":32}
```

`tick` is the tick the drag happens at, which the next `step` runs. Nothing happens when `from`
gives nothing. An ID that names no entity (with the closest one) or an entity that isn't a
control is an error at its param. To drag the way a player does, with the pointer moving over
other controls, send the mouse events with [`input.inject`](#inputinject).

### ui.tooltip

Shows a control's [tooltip](scene-format.md#tooltips-and-drag-and-drop) now, below the middle
of the control, as Godot's `Viewport::show_tooltip`: its `tooltip_text`, or the first parent's up
to one that stops the mouse. `"show": false` hides the tooltip shown instead.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | | The control; needed unless `show` is `false` |
| `show` | boolean | `true` | `false` hides the tooltip |

Needs: `run`. From [examples/ui-drag-and-drop](../../examples/ui-drag-and-drop/):

```json
{"id": "trash"}
```

```json
{"tooltip":{"id":"trash","text":"Drop an item here to throw it away"}}
```

`tooltip` is `null` when the control has none. [`ui.layout`](#uilayout) gives the tooltip's
rect. Mouse motion and presses hide it as they would hide one the pointer showed.

### ui.focus

Gives a control focus, as Godot's `grab_focus`, and shows it: `ui_accept` (Enter or Space)
presses a focused button, a focused `LineEdit` starts editing, and `ui_up`, `ui_down`,
`ui_left`, `ui_right`, Tab and Shift+Tab move focus on from it. `null` takes focus away.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string or null | required | The control, or `null` for none |

Needs: `run`. Emits `focus_exited` and `focus_entered` at the next `step`.

```json
{"id": "play"}
```

```json
{"focused":"play"}
```

`focused` is the control with focus now, or `null`. A control that is hidden, or whose
`focus_mode` is `"none"` (the default for controls other than buttons and `LineEdit`s), can't
take focus, and that is an error. [`ui.layout`](#uilayout) marks the control with focus
`"focused": true`.

### ui.search

Finds text in a [`TextEdit`](scene-format.md#searching-and-highlighting-code), as Godot's
`TextEdit::search`: the next place it occurs from a position (the caret unless told), going
round past the end to the start, or with `backwards` from the position back, round past the
start. With `all`, every place it occurs, in order from the start.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The `TextEdit` |
| `text` | string | its `search_text` | What to find |
| `flags` | array of strings | its `search_flags` | `match_case`, `whole_words` (with a symbol, a space or the line's edge on each side) and `backwards` |
| `from` | `[line, column]` | the caret | Where to look from; a match may start there |
| `all` | boolean | `false` | Return every match instead |
| `max_results` | integer | `100` | With `all`, at most this many matches, 1 to 10000 |

Needs: `read`. From [examples/code-editor](../../examples/code-editor/):

```json
{"id": "script", "text": "player", "from": [12, 0], "flags": ["backwards"]}
```

```json
{"found":[11,1]}
```

`found` is `[line, column]`, from 0, the column counting characters, or `null` when the text
occurs nowhere. With `all`, the reply is `{"matches": [[line, column], ...], "count": n}`,
`count` covering every match. An empty `text`, no `text` for a `TextEdit` with no
`search_text`, and a `from` past the text are errors at the param's path. Case is ignored
unless `match_case`, for Latin, Greek and Cyrillic letters.

### ui.clipboard

Returns the clipboard [text fields](scene-format.md#text-fields) cut, copy and paste with, as
Godot's `DisplayServer.clipboard_get`; `text` sets it first. Headless runs and tests have the
session's own clipboard; a game running in a window shares the system's.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `text` | string | | Text to put on the clipboard, up to 65536 bytes |

Needs: `run`. After selecting all of a field's text and copying it (`ui_text_select_all`, then
`ui_copy`):

```json
{}
```

```json
{"text":"Mira 未来"}
```

### text.measure

Returns text's size in a font, as a `Label` lays it out: its width and height, the line height
and each line, wrapped to a width if asked. Nothing changes; use it to check that text fits
before setting it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `text` | string | required | The text, up to 65536 bytes; lines break at `"\n"` |
| `font` | string | the built-in font | A `.ttf`, `.otf` or [`.font.json`](scene-format.md#fonts) file |
| `font_size` | integer | `16` | In pixels, 1 to 1024 |
| `autowrap_mode` | `"off"`, `"arbitrary"`, `"word"` or `"word_smart"` | `"off"` | Where lines wrap, as `Label`'s |
| `width` | number | | The width to wrap to; required with `autowrap_mode` |
| `direction` | `"auto"`, `"ltr"` or `"rtl"` | `"auto"` | Each paragraph's base direction, as `Label`'s `text_direction`; `auto` takes its first letter's |
| `language` | string | | A BCP 47 tag, such as `"ar"` or `"ja"`, which picks [fallback fonts](scene-format.md#fonts) |

Needs: `read`.

```json
{"text": "The tower is quiet. Mira's lantern still burns.", "autowrap_mode": "word", "width": 200}
```

```json
{"ascent":17,"line_height":22,"lines":["The tower is quiet. Mira's","lantern still burns."],"size":[174,44]}
```

`size` is `[width, height]`: the widest line, and `line_height` times the lines. A `Label` adds
its `line_spacing` (3 by default) between lines, and `paragraph_spacing` between paragraphs.
`ascent` is from the top of a line to its baseline. `rtl` is `true` when the lines go right to
left, or one `true` or `false` per line when only some do. `missing_glyphs` counts characters no
font has and `system_glyphs` characters drawn from fonts installed on the machine. A font that
doesn't load is an error at `/font`, and a `language` that isn't a BCP 47 tag at `/language`:

```json
{"text": "الإصدار 1.2 (beta)", "font": "fonts/ui.font.json"}
```

```json
{"ascent":17,"line_height":22,"lines":["الإصدار 1.2 (beta)"],"rtl":true,"size":[142,22]}
```

## Input

Input follows Godot: events from keys, the mouse, touches and gamepads press
[actions](project-format.md#input-actions) that gameplay code asks about. Events wait in a queue
and apply at the start of the next tick, so `input.inject` then `step` plays out the same way
every run. The examples here are from [examples/input-actions](../../examples/input-actions/).

### input.inject

Queues input events for the next tick, as if a player sent them. Gameplay code sees them exactly
as it sees a real keyboard, mouse, touch screen or gamepad.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `events` | array of 1 to 256 events | required | Events in order |

Needs: `run`. Changes no entities, so there is nothing to undo; a failed command, or a `batch` or
dry run that steps past the events, leaves them queued.

```json
{"events": [{"type": "key", "physical_key": "D", "pressed": true}]}
```

```json
{"queued":1,"tick":0}
```

`queued` is how many events wait now; `tick` is the tick they apply at, which the next `step`
runs. Each event has a `type`:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `key` | `key` or `physical_key`, `pressed`, `echo`, `shift`, `ctrl`, `alt`, `meta` | A key pressed or released, by [name](project-format.md#input-actions). Give either name and the event has both, as a US keyboard's would. `echo` is a repeat while held |
| `mouse_button` | `button`, `position`, `pressed`, `double_click`, `factor`, the modifiers | A mouse button pressed or released. The wheel is a press and a release of `wheel_up` and the rest; `factor` is how far it turned |
| `mouse_motion` | `position`, `relative` | The mouse moved. `relative` defaults to the distance from the last position |
| `screen_touch` | `index`, `position`, `pressed`, `double_tap` | A finger touched or left the screen. `index` is which finger, from 0 |
| `screen_drag` | `index`, `position`, `relative` | A finger moved |
| `joypad_button` | `button`, `pressed` | A gamepad button pressed or released |
| `joypad_motion` | `axis`, `axis_value` | A stick or trigger moved: sticks from -1 to 1 with y down, triggers from 0 to 1. `0` lets go |
| `action` | `action`, `pressed`, `strength` | An action pressed or released by name, with a `strength` from 0 to 1 (default 1), as Godot's `Input.action_press` |
| `text` | `text` | Types text into the [text field](scene-format.md#text-fields) being edited: one key press per character, with the character in `unicode` and no key, as an input method or on-screen keyboard sends it |
| `ime` | `text`, `selection` | What an input method (for Japanese, Chinese or Korean) is composing, shown at the caret: `selection` is `[start, length]` in characters of the part being converted. `""` ends the composition; a `text` event commits the result |

A `key` event also takes `unicode`, the one character the key types (`{"type": "key", "key":
"A", "unicode": "a", "pressed": true}`); a key with no `unicode` edits a text field but types
nothing. Every event also takes `device`: which keyboard, mouse or gamepad, from 0. Positions are
`[x, y]` in window pixels from the top left, the space sprites and controls are drawn in. Fields
an event's type doesn't have are errors, and so are misspelled names, with the closest one.

- A press and a release before one `step` is a tap: during that tick the action is both just
  pressed and just released, so gameplay code that checks `is_action_just_pressed` never misses
  it. To hold a key, inject the press, `step` as long as it is held, then inject the release.
- A touch also clicks the left mouse button, unless `project.json` turns
  `input_devices.pointing.emulate_mouse_from_touch` off.
- A motion right after another from the same mouse, finger or stick merges into it, as Godot's
  accumulated input.
- In a running game, events apply at the next tick of the game loop.

### input.state

Returns what the game sees: actions pressed or changed at the last tick, the keys and mouse
buttons held, the mouse position, fingers down, gamepad buttons and axes away from rest, and
events still queued.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `actions` | array of strings | the ones pressed or just released, but text editing actions | Only these actions, pressed or not |

Needs: `read`. After a tap of Space, half a stick up and letting go of D:

```json
{"actions": ["jump", "move_right", "move_up"]}
```

```json
{"actions":{"jump":{"just_pressed":true,"just_released":true,"pressed":false},"move_right":{"just_released":true,"pressed":false},"move_up":{"just_pressed":true,"pressed":true,"strength":0.5}},"joypads":[{"axes":{"left_y":-0.625},"device":0}],"mouse":{"position":[0.0,0.0]},"tick":8}
```

- `tick` is the tick the input last applied at, the one before the world's tick counter. It is
  left out before the first step.
- `strength` is listed when it is between 0 and 1: a stick or trigger, or an action pressed with
  a `strength`. Past the deadzone (0.25 here), a stick at 0.625 reads 0.5.
- `just_pressed` and `just_released` are listed when true.
- `keys` are labels, as `key` names them; `mouse.buttons`, `touches` and `joypads` are listed
  when something is held. `queued` counts events waiting for the next tick.
- An action the game doesn't have is an error at its place in `actions`, with the closest name.

### input.actions

Returns input actions with their deadzone and the events bound to each: Godot's `ui_*` actions
and `project.json`'s `input`, as the game uses them.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `names` | array of strings | all but Godot's [text editing actions](project-format.md#input-actions) | Only these actions |

Needs: `read`.

```json
{"names": ["move_right", "jump"]}
```

```json
{"actions":{"jump":{"deadzone":0.2,"events":[{"physical_key":"Space","type":"key"},{"button":"a","type":"joypad_button"}]},"move_right":{"deadzone":0.25,"events":[{"physical_key":"D","type":"key"},{"key":"Right","type":"key"},{"axis":"left_x","axis_value":1.0,"type":"joypad_motion"}]}}}
```

Bindings are written as in `project.json`. A game that rebinds actions while it runs, e.g. from
an options menu, shows its bindings as they are now.

## Audio

Sound follows Godot's AudioServer: [players](scene-format.md#audio) mix into the buses of the
project's [bus layout](scene-format.md#bus-layouts), each with its volume and
[effects](scene-format.md#effects), and every bus sends to Master, which plays on the speakers.
Sessions mix sound as they step but play it nowhere, so an agent hears a game by measuring it:
`audio.state` shows the buses' levels and what each player is doing, and `audio.capture`
measures what a bus played, or writes it to a `.wav` file. Levels are in dB to a tenth, with
`null` for silence. The examples here are from [examples/audio](../../examples/audio/).

### audio.state

Returns the buses, with their volume, the peak level of the last mix step and what is turned
on, and each player with an ID.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ids` | array of strings | all | Only these players |

Needs: `read`. With the footsteps playing:

```json
{"ids": ["footsteps"]}
```

```json
{"buses":[{"name":"Master","peak_db":-5.4,"volume_db":0},{"effects":[{"enabled":false,"type":"low_pass_filter"}],"name":"Music","peak_db":-12,"volume_db":-6},{"effects":[{"type":"hard_limiter"}],"name":"Sfx","peak_db":-10.9,"volume_db":0}],"mix_rate":44100,"players":[{"bus":"Sfx","id":"footsteps","kind":"AudioStreamPlayer2D","length":0.1,"position":0.0493,"stream":"audio/steps.audio.json","voices":1,"volume":[0.5203,0.3797]}]}
```

- A bus lists `solo`, `mute` and `bypass_effects` when they are on, `send` when it isn't
  Master, and its effects in order, with `"enabled": false` for those turned off.
- `peak_db` is the louder channel's peak over the last mix step, after the bus's volume and
  effects, or `null` when the bus was silent.
- `voices` is how many playbacks of the player are mixing (up to its `max_polyphony`);
  `position` (of the newest one) and `length` are in seconds. A randomizer's `length` is that
  of the sound playing, and a player that hasn't played one has none. A playlist's `position`
  is in the stream playing and its `length` the whole list's.
- `clip` names the clip [interactive music](scene-format.md#interactive-music) is playing; its
  `position` is in that clip, and it has no `length`.
- A [generator](scene-format.md#generators-and-polyphonic-streams) lists
  `"generator": {"frames_available", "skips"}`: the frames a game can push before its buffer is
  full, and the mixes that ran out of frames and played silence since it started. A polyphonic
  player lists `"polyphonic": {"playing", "polyphony"}`: the streams it has going and how many it
  can.
- `volume` is the `[left, right]` gain mixed in: the player's `volume_db`, then for an
  `AudioStreamPlayer2D` its fade with distance and its pan from the listener.
- `paused` is listed when `stream_paused` holds the player, and `error` when its stream did not
  load, such as a missing or corrupt file.
- A player's `bus` is the one it plays on now: an `Area2D` with `audio_bus_override` that holds
  an `AudioStreamPlayer2D` sends it to its own bus ([buses by area](scene-format.md#buses-by-area)).
- A `capture` effect lists `frames_available`, the frames its buffer holds, and
  `discarded_frames` once a full buffer has dropped some. A `record` effect lists
  `"recording": true` while it records and `seconds` once it holds a recording; both show the
  state at the next tick, after any [`audio.record`](#audiorecord) waiting for it.
- With the project's `audio.enable_input`, `input` shows the [microphone](scene-format.md#microphone)
  input: `active` while a microphone player or the game keeps it on, `source` (the sound
  [`audio.input`](#audioinput) plays into it, the microphone's name, or `""` for silence, as
  headless), `peak_db` over the last tick (`null` when silent), and `error` when the microphone
  wouldn't open, such as when the player refused it. From
  [examples/microphone](../../examples/microphone/):
  `"input":{"active":true,"peak_db":-26,"source":"audio/song.wav"}`.

### audio.capture

Measures what a bus played over the last seconds: its peak and RMS level and the peak of each
slice of that time, oldest first, which shows a sound starting, fading or stopping. With `path`
it also writes the sound to a 16-bit `.wav` file, to listen to.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bus` | string | `"Master"` | The bus |
| `seconds` | number, 0 to 10 | `1` | How far back |
| `slices` | integer, 0 to 100 | `10` | Equal slices to report the peak of |
| `path` | string | | A `.wav` file to write, relative to the project |

Needs: `read`, and `filesystem` with `path`. The footstep click fading over a twentieth of a
second:

```json
{"bus": "Sfx", "seconds": 0.05, "slices": 5}
```

```json
{"peak_db":-7.6,"rms_db":-11.4,"seconds":0.05,"slices":[-7.6,-7.9,-8.8,-9.9,-11.1]}
```

Sessions keep the last 10 seconds of each bus; `seconds` in the reply is how much there was,
which is less right after a scene opens. A name that is no bus is an error with the closest one,
and so is a `path` that doesn't end in `.wav`. In a dry-run `batch`, `path` writes nothing.

### audio.play

Plays an `AudioStreamPlayer` or `AudioStreamPlayer2D` at the next tick, as Godot's `play()`: with
`max_polyphony` 1 it starts over, with more another voice joins, up to that many. Music,
generators, polyphonic streams and microphones always start over, as Godot's monophonic streams
do. With
`stream`, it plays that sound or `.audio.json` file on a player whose stream is
[polyphonic](scene-format.md#generators-and-polyphonic-streams) instead, as Godot's
`play_stream`, starting the player first if it isn't playing.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The player's entity |
| `from` | number | `0` | Seconds into the stream |
| `stream` | string | | A sound or `.audio.json` file to play on the polyphonic player |
| `volume_db` | number, -80 to 24 | `0` | With `stream`: its volume |
| `pitch_scale` | number, 0.01 to 16 | `1` | With `stream`: how fast it plays |

Needs: `run`. Changes no entities until the next `step`, so there is nothing to undo; a failed
`batch` takes the play back.

```json
{"id": "footsteps"}
```

```json
{"tick":6}
```

`tick` is the tick it plays at, which the next `step` runs; the player's `playing` turns on then.
An ID that names no entity is an error with the closest one, and so is an entity with neither
player component. Setting `playing` to `true` with [`component.set`](#componentset) plays it
too.

From [examples/generated-audio](../../examples/generated-audio/), a hit an octave up on the
polyphonic `sfx` player:

```json
{"id": "sfx", "stream": "audio/hit.wav", "pitch_scale": 2}
```

A `stream` that doesn't load or is a microphone is an error at `/stream`, and a player whose
stream isn't polyphonic one at `/id`; `volume_db` and `pitch_scale` go only with `stream`. When every slot of
the player's `polyphony` is playing, the new stream is dropped with a warning in the log.

### audio.stop

Stops an `AudioStreamPlayer` or `AudioStreamPlayer2D` at the next tick, as Godot's `stop()`,
fading every voice out over one mix step so it doesn't click.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The player's entity |

Needs: `run`. Like `audio.play`, it returns `{"tick": ...}` and a failed `batch` takes it back.

### audio.set_bus

Changes a bus while the game runs, as Godot's `AudioServer` setters, e.g. from an options menu:
its volume, mute, solo, effect bypass, or whether one of its effects is on. The
[bus layout file](scene-format.md#bus-layouts) is unchanged, and a reload of it starts the buses
over from the file.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bus` | string | required | The bus |
| `volume_db` | number, -80 to 24 | | Its gain in dB |
| `mute` | boolean | | Silences it |
| `solo` | boolean | | Plays only solo buses and the buses they send to |
| `bypass_effects` | boolean | | Skips its effects |
| `effect` | integer, 0 to 15 | | An effect's index on the bus, with `enabled` |
| `enabled` | boolean | | Turns that effect on or off |

Needs: `run`. Changes no entities, so there is nothing to undo; a failed command, `batch` or dry
run puts the buses back.

```json
{"bus": "Music", "mute": true}
```

```json
{"effects":[{"enabled":false,"type":"low_pass_filter"}],"mute":true,"name":"Music","peak_db":-12,"volume_db":-6}
```

The reply is the bus, as `audio.state` lists it. Volume changes ramp over one mix step. A bus
name or effect index that doesn't exist is an error, and so are `effect` without `enabled` and
`enabled` without `effect`.

### audio.spectrum

Reads a bus's [`spectrum_analyzer`](scene-format.md#effects) effect, as Godot's
`get_magnitude_for_frequency_range`: how loud each frequency range was in the newest window it
analyzed, so an agent can tell a hum from a hiss, or check a filter, without listening. The
examples here and under `audio.record` are from
[examples/audio-areas-and-analysis](../../examples/audio-areas-and-analysis/).

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bus` | string | `"Master"` | The bus |
| `effect` | integer, 0 to 15 | the bus's first `spectrum_analyzer` | The effect's index on the bus |
| `ranges` | array of `[from_hz, to_hz]`, 1 to 64 | ten octave bands from 22 Hz | The ranges to measure |
| `mode` | `"max"` or `"average"` | `"max"` | Each range's strongest frequency, or their mean |

Needs: `read`. A 220 Hz hum shifted up an octave, then with the shift turned off:

```json
{}
```

```json
{"db":[-58.7,-63.1,-68,-34,-11.5,-21.1,-24.2,-28.3,-33.4,-39.4],"ranges":[[22,44],[44,88],[88,177],[177,355],[355,710],[710,1420],[1420,2840],[2840,5680],[5680,11360],[11360,22720]]}
```

```json
{"ranges": [[200, 240], [420, 460]]}
```

```json
{"db":[-10.2,-60.6]}
```

`db` has one level per range, in dB to a tenth: the louder channel's magnitude, where a
full-scale sine measures about -6 dB, or `null` below -100 dB. `ranges` is listed only when the
default bands were used. The analyzer keeps only its newest window of `fft_size` frames, so it
reads 0 until that much sound has played through it. A bus with no `spectrum_analyzer`, or an
`effect` index that is another type, is an error naming what is there.

### audio.record

Starts or stops a bus's [`record`](scene-format.md#effects) effect, as Godot's
`set_recording_active`, and writes what it recorded to a `.wav` file in the effect's `format`.
Unlike `audio.capture`, which keeps the last 10 seconds of every bus, a recording keeps
everything from its start to its stop.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bus` | string | `"Master"` | The bus |
| `effect` | integer, 0 to 15 | the bus's first `record` | The effect's index on the bus |
| `recording` | boolean | unchanged | `true` starts a new recording, `false` stops |
| `path` | string | | A `.wav` file to write the recording to, relative to the project |

Needs: `run`, and `filesystem` with `path`. Recording 30 ticks of a bat in a cave:

```json
{"recording": true}
```

```json
{"recording":true,"seconds":0}
```

```json
{"recording": false, "path": "build/take.wav"}
```

```json
{"path":"build/take.wav","recording":false,"seconds":0.4992}
```

`seconds` is how much the recording holds. Starts and stops wait for the next tick, as
`audio.play` does, so a failed `batch` or a dry run takes them back; starting drops the last
recording. The mixer records whole steps of 512 frames, so `seconds` can run a step ahead of the
ticks. A `path` with nothing recorded, with `"recording": true` or not ending in `.wav` is an
error. In a dry run, `path` writes nothing.

### audio.input

Plays a sound or `.audio.json` file into the [microphone](scene-format.md#microphone) input from
the next tick, in place of the microphone, so an agent can test a game that listens: microphone
players play it 50 ms late and gameplay code reading the input hears it. It plays while input is
on, from where input turns on, and the input is silent once it ends. `""` goes back to the
microphone (silence when headless).

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `stream` | string | required | The sound or `.audio.json` file, or `""` for the microphone |

Needs: `run`. From [examples/microphone](../../examples/microphone/), singing into a game that
lights lanterns:

```json
{"stream": "audio/song.wav"}
```

```json
{"tick":5}
```

Like `audio.play`, it waits for the next tick, so a failed `batch` or a dry run takes it back.
It is an error when the project's `audio.enable_input` is off, and at `/stream` when the stream
doesn't load or is itself a microphone. [`audio.state`](#audiostate)'s `input` shows what feeds
the input.

## Services

Ads, analytics, in-app purchases, achievements and leaderboards, as Unity's services and Social
API: `project.json`'s [`services`](project-format.md#services) names the ad placements,
products, achievements and leaderboards, gameplay code shows, logs, buys, unlocks and submits
by those names, and what happens arrives as events at a later tick (see
[services.md](services.md)). Sessions run the services on the test backend, so ads load,
purchases go through and the player signs in with no store or platform account. The examples
here are from [examples/services](../../examples/services/), a shop that sells coins for
watched ads and purchases, and
[examples/achievements-and-leaderboards](../../examples/achievements-and-leaderboards/), a
race with lap achievements and a best-lap leaderboard.

### services.state

Returns the backends, the player's consent, each ad placement's format and status (`unloaded`,
`loading`, `loaded`, `showing` or `failed` with its `error`), the products with their prices
once fetched, what the player owns, purchases unfinished (completed but not yet granted and
finished) and pending, the latest analytics events and, in a game with achievements or
leaderboards, sign-in, each achievement's progress and each leaderboard's best score.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `events` | integer, 0 to 256 | `10` | How many of the latest analytics events |

Needs: `read`. After the shop's system logged an event for a watched ad:

```json
{"events": 1}
```

```json
{"ads":{"bonus":{"format":"rewarded","status":"loading"},"footer":{"format":"banner","status":"showing"}},"analytics":{"enabled":true,"events":[{"name":"coins_earned","params":{"coins":25,"source":"ad"},"sent":false,"tick":4}],"logged":1},"backends":{"ads":"test","analytics":["test"],"games":"test","purchases":"test"},"consent":{"analytics":"unknown","personalized_ads":"unknown"},"owned":[],"pending":[],"products":{"coins_100":{"available":true,"price":"$0.99","type":"consumable"},"remove_ads":{"available":true,"price":"$0.99","type":"non_consumable"}},"products_status":"fetched","tick":4,"unfinished":[]}
```

`tick` is the tick the `services` system last ran. The rewarded ad is `loading` again because it
auto-loads after it closes. The event was not `sent` because the player hasn't granted
analytics consent; `logged` counts every event so far. `products_status` is `none` (no
products), `fetching`, `fetched` or `failed` with `products_error`, e.g. `no store` in a game
with no store backend. `unfinished` and `pending` list `{"product", "transaction", "state"}`.

In the race, after the player finished a lap and the ten-lap achievement unlocked:

```json
{"games":{"achievements":{"first_finish":{"unlocked":true},"ten_laps":{"of":10,"steps":10,"unlocked":true}},"leaderboards":{"best_lap":{"best":62500}},"player":{"id":"test-player","name":"Player"},"sign_in":"signed_in"}}
```

(the rest left out). `sign_in` is `signed_out`, `signing_in`, `signed_in` or `failed` with
`sign_in_error`. Incremental achievements give `steps` of `of`. A leaderboard's `best` is the
player's best score, kept while signed out, and `unsent` is set until the platform has it.
`ui_showing` is set while the achievements or leaderboards screen is open.

### services.test

Sets how the test backend behaves, and loads, shows, hides, buys, finishes, restores, signs in,
unlocks, submits and loads scores as the game's code would. What happens arrives as events at the next tick or later, as from a real
backend.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `fill` | boolean | `true` | Ads load; `false` fails them with `no fill` |
| `load_ticks` | integer, 1 to 3600 | `1` | Ticks an ad takes to load |
| `show_ticks` | integer, 1 to 3600 | `1` | Ticks an interstitial or rewarded ad shows before it closes |
| `watch_to_end` | boolean | `true` | The player watches rewarded ads to the end and earns the reward; `false` skips them |
| `purchase_ticks` | integer, 1 to 3600 | `1` | Ticks a purchase or restore takes |
| `outcome` | `purchased`, `pending`, `cancelled` or `failed` | `purchased` | How purchases end. `pending` waits for `complete_pending`, as Ask to Buy does |
| `error` | string | `the payment was declined` | A failed purchase's error |
| `prices` | object of numbers | 0.99 each | Prices in US dollars by product ID; products are fetched again |
| `consent` | `{"analytics", "personalized_ads"}`, each `unknown`, `granted` or `denied` | | Sets the player's consent, as the game's `set_consent` would |
| `complete_pending` | `purchased`, `cancelled` or `failed` | | Ends each pending purchase so |
| `load`, `show`, `hide` | string | | An ad placement to load, show or hide (banners only) |
| `buy` | string | | A product to buy |
| `finish` | string | | A purchase's transaction to finish once granted |
| `restore` | boolean | | Restores purchases |
| `accept_sign_in` | boolean | `true` | The player signs in; `false` fails sign-in with `the player didn't sign in` |
| `player` | string | `Player` | The test player's name; their ID is `test-player` |
| `games_ticks` | integer, 1 to 3600 | `1` | Ticks signing in, submitting a score and loading scores take |
| `scores` | object of arrays of `{"name", "score"}` | | Other players' best scores by leaderboard, up to 100 each, ranked with the player's |
| `sign_in` | boolean | | Signs the player in, as the game's `sign_in` does when the player asks |
| `unlock` | string | | An achievement to unlock |
| `increment` | `{"achievement", "steps"}` | | Adds `steps` (default 1) to an incremental achievement |
| `submit` | `{"leaderboard", "score"}` | | Submits an integer score |
| `load_scores` | `{"leaderboard", "time", "collection", "count"}` | | Loads a leaderboard's best scores: `time` `all_time`, `week` or `today`; `collection` `global` or `friends`; `count` 1 to 25 (default 10) |
| `show_achievements` | boolean | | Shows the achievements screen |
| `show_leaderboards` | string | | Shows a leaderboard's screen, or every leaderboard's for `""` |

Settings apply first, then `consent`, `complete_pending`, `load`, `show`, `hide`, `buy`,
`finish`, `restore`, `sign_in`, `unlock`, `increment`, `submit`, `load_scores`,
`show_achievements` and `show_leaderboards`, in that order. Needs: `run`. Changes no entities, so there is nothing to
undo; a call that fails partway changes nothing.

```json
{"outcome": "pending", "buy": "remove_ads"}
```

```json
{"test":{"accept_sign_in":true,"error":"the payment was declined","fill":true,"games_ticks":1,"load_ticks":1,"outcome":"pending","player":"Player","prices":{},"purchase_ticks":1,"scores":{},"show_ticks":1,"watch_to_end":true},"tick":7}
```

`tick` is the next tick, when the first events can arrive; `test` lists every setting, and only
when the call changed one. The purchase is then pending until
`{"complete_pending": "purchased"}`. A call the game couldn't make fails with the same reason
the game would get, with its JSON Pointer:

```json
{"buy": "remove_ads"}
```

```
/buy: the player owns "remove_ads" already
```

In the race, two rivals' lap times put the player's in context, and eight laps' worth of
steps bring the ten-lap achievement near:

```json
{"scores": {"best_lap": [{"name": "Ana", "score": 61000}, {"name": "Bo", "score": 66500}]}, "increment": {"achievement": "ten_laps", "steps": 8}}
```

A misspelled name gets the closest one:

```json
{"unlock": "first_finsh"}
```

```
/unlock: no achievement "first_finsh"; did you mean "first_finish"?
```

Outside a session, as in an exported game, there is no test backend, and `services.test` fails.

## Storyboard

The game's facts as typed entries in `storyboard/*.json`; see the
[storyboard format](storyboard-format.md). Examples run against a copy of `tests/data/game`.

An agent calls `storyboard.brief` before a task, so it works from the game's recorded facts
instead of guessing them, records what it decides with `storyboard.edit`, and calls
`storyboard.check` after the task.

### storyboard.query

Finds storyboard entries. Filters combine; with none, every entry is returned. Each result has the
entry's ID, its version (which `storyboard.edit` needs) and its name and summary, or every field
with `"full": true`. `seq` is the journal's line count.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ids` | array of IDs | all | Only these entries. An unknown ID fails with `-32001` and a "did you mean" hint |
| `kind` | string | all | Only entries of this kind, e.g. `"character"` |
| `text` | string | | Only entries with this text in a field, ignoring case |
| `linked` | string | | Only entries whose `links` name this project path or a folder it is in, or whose references name this ID |
| `full` | boolean | `false` | Return every field, not only `name` and `summary` |

Needs: `read`.

```json
{"linked": "scenes/main.scene.json"}
```

```json
{"entries":[{"fields":{"name":"Pilot","summary":"The player: a courier pilot on her first run."},"id":"character/pilot","version":"f6ce5b73"},{"fields":{"name":"Landing pad","summary":"Where every run starts."},"id":"place/landing_pad","version":"4bdb955e"}],"seq":0}
```

### storyboard.brief

The facts a task needs, in one call: name the scenes, entities, files or entries it touches.
Returns, each with every field:

- the entries named, and the entries named by the `StoryRef` components in those scenes or on
  those entities;
- entries whose `links` name those files, those scenes, the prefabs their entities extend, or a
  folder one of them is in;
- rules, tones, palettes and budgets that apply everywhere (those with no `links` or `refs`);
- open tasks and live decisions that link to those files or name those entries.

`related` lists the other entries those name, with name and summary only. With `since`, `changed`
maps each entry changed after that journal `seq` to its new version, or `null` if it was removed,
so an agent that briefed earlier rereads only what moved.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `scenes` | array of paths | | Scene files, e.g. `["scenes/main.scene.json"]`. The open scene is read as it is now |
| `entities` | array of IDs | | Entities in the open scene |
| `files` | array of paths | | Other project files: assets, prefabs, source |
| `ids` | array of IDs | | Storyboard entries |
| `since` | integer | | Also list entries changed after this journal `seq` |

An unknown entity or entry, or a scene that does not exist, fails with `-32001` and a "did you
mean" hint. Needs: `read`.

```json
{"scenes": ["scenes/main.scene.json"]}
```

```json
{"entries":[{"fields":{"links":["scenes/main.scene.json"],"name":"Pilot","summary":"The player: a courier pilot on her first run."},"id":"character/pilot","version":"f6ce5b73"},{"fields":{"links":["prefabs/marker.prefab.json","scenes/main.scene.json"],"name":"Landing pad","summary":"Where every run starts."},"id":"place/landing_pad","version":"4bdb955e"},{"fields":{"name":"One hit","summary":"Any hazard ends the run; there is no health bar."},"id":"rule/one_hit","version":"7b97241a"}],"related":[],"seq":0}
```

After another agent's edit:

```json
{"files": ["prefabs/marker.prefab.json"], "since": 0}
```

```json
{"changed":{"character/pilot":"80d23357","term/courier":"d1f7edb3"},"entries":[{"fields":{"links":["prefabs/marker.prefab.json","scenes/main.scene.json"],"name":"Landing pad","summary":"Where every run starts."},"id":"place/landing_pad","version":"4bdb955e"},{"fields":{"name":"One hit","summary":"Any hazard ends the run; there is no health bar."},"id":"rule/one_hit","version":"7b97241a"}],"related":[],"seq":1}
```

### storyboard.check

Checks the storyboard against itself and the project. Errors: a storyboard file that is invalid,
a reference to an entry that does not exist or is of the wrong kind, a word a glossary `term`
says to `avoid` (as a whole word, ignoring case), and a `StoryRef` ID in a scene or prefab that
names no entry, and a texture (`.png` or `.bmp`) that breaks the style: larger on a side than a
`budget`'s `texture_size`, or with pixels in colors a `palette` doesn't have (see the
[storyboard format](storyboard-format.md#checks)). Every `.scene.json`, `.prefab.json` and
texture file in the project is read, except those in `build/` and in folders whose names start
with `.`; the open scene is checked as it is now. Warnings: a link to a file that does not exist,
since open work can link files still to be made. `slash-cli project validate` runs the same
check.

Takes no params. Needs: `read`.

```json
{}
```

```json
{"errors":[{"file":"storyboard/world.json","message":"uses \"driver\", which term/courier says to avoid; write \"courier\"","path":"/entries/character~1pilot/summary"},{"file":"scenes/main.scene.json","message":"unknown storyboard entry \"character/pilto\"; did you mean \"character/pilot\"?","path":"/entities/player/components/StoryRef/ids/0"},{"file":"art/crate.png","message":"has 12 pixels in 1 color not in palette/pico: #ff0000 at (3, 4), nearest \"red\" #ff004d","path":""}],"valid":false}
```

`errors` and `warnings` are left out when empty, so a clean project returns `{"valid":true}`.

### storyboard.edit

Creates, changes or removes entries, all in one step: every edit applies or none does. It writes
the files in canonical form, appends a line to the journal, and records an undo step.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `edits` | array | required | 1 to 1000 edits, at most one per entry |
| `edits[].id` | string | required | The entry's ID |
| `edits[].version` | string or `null` | required | The version `storyboard.query` returned, or `null` to create the entry |
| `edits[].set` | object | | Fields to set; a `null` value removes a field |
| `edits[].remove` | boolean | `false` | Remove the entry; give this or `set` |
| `why` | string | | Why, for the journal |
| `by` | string | the MCP client's name | Who, for the journal |

Needs: `edit`, `filesystem`. Takes `dry_run`.

```json
{"why": "Give the pilot a rival", "by": "claude-code", "edits": [
  {"id": "character/vex", "version": null, "set": {"name": "Vex", "summary": "A rival courier."}},
  {"id": "character/pilot", "version": "f6ce5b73", "set": {"refs": ["character/vex"]}}]}
```

```json
{"changes":{"character/pilot":"45b24094","character/vex":"a77d1847"},"seq":1}
```

`changes` holds each entry's new version, or `null` for a removed one. Fields are checked against
the entry's kind, and every entry a set field names must exist, with the right kind:

```json
{"error":{"code":-32602,"data":{"path":"/edits/0/set/home"},"message":"/edits/0/set/home: unknown storyboard entry \"place/landing_pd\"; did you mean \"place/landing_pad\"?"}}
```

An edit with an old version fails with `-32005`, changes nothing, and returns the entry as it is
now in `data.current`; edit that version instead:

```json
{"error":{"code":-32005,"data":{"current":{"fields":{"links":["scenes/main.scene.json"],"name":"Pilot","refs":["character/vex"],"summary":"The player: a courier pilot on her first run."},"id":"character/pilot","version":"45b24094"},"path":"/edits/0/version"},"message":"/edits/0/version: \"character/pilot\" changed since version f6ce5b73 was read; it is now 45b24094 (see current), so edit that version"}}
```

Removing an entry that another entry still names fails; change both in one call. `undo` and
`redo` apply the reverse edit and the edit again, with the same version checks, so an undo that
would overwrite someone else's later change is refused.

## Components

### component.set

Sets component fields on an entity, adding components it lacks. Returns only the fields whose
values changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `components` | object | required | Fields to set, keyed by component type. Fields left out are unchanged; missing components are added |

Needs: `edit`. Takes `dry_run`.

```json
{"id": "player", "components": {"Transform2D": {"rotation": 1.0, "position": [64, 32]}}}
```

```json
{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}
```

`position` was already `[64, 32]`, so only `rotation` is reported. A misspelled component name
gets a suggestion:

```json
{"code":-32001,"data":{"path":"/components/Transfrom2D"},"message":"/components/Transfrom2D: unknown component \"Transfrom2D\"; did you mean \"Transform2D\"?"}
```

### component.remove

Removes components from an entity. Returns the ones it had.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `components` | array of strings | required | Component types to remove (at least one) |

Needs: `edit`. Takes `dry_run`.

```json
{"id": "player", "components": ["Transform2D"]}
```

```json
{"id":"player","removed":["Transform2D"]}
```

## Query

### world.query

Finds entities by the components they have and their field values. Returns IDs unless `include`
asks for components, plus the total count.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `with` | array of strings | | Only entities with all of these components |
| `without` | array of strings | | Only entities with none of these components |
| `where` | array of conditions | | Field conditions; an entity must meet all of them |
| `include` | `true` or array of strings | | Components to return with each entity: `true` for all. Left out, only IDs are returned, which is cheapest |
| `omit_defaults` | boolean | `false` | Leave out fields equal to their defaults |
| `limit` | integer, 1 to 10000 | `100` | Most entities to return; `count` still covers all |

Each `where` condition is an object:

| Key | Type | Meaning |
| --- | --- | --- |
| `component` | string | Component type, e.g. `"Transform2D"` (required) |
| `field` | string | Field name. Add `.x`, `.y`, `.z`, `.w` or `.<index>` for one element of a vector, e.g. `"position.y"` (required) |
| `op` | `==`, `!=`, `<`, `<=`, `>`, `>=` | Comparison; `==` if left out. Ordering operators compare numbers or strings |
| `value` | any | Value to compare against (required) |

Needs: `read`.

```json
{"where": [{"component": "Transform2D", "field": "position.x", "op": ">", "value": 10}]}
```

```json
{"count":1,"entities":["player"],"truncated":false}
```

With components, and with defaults dropped:

```json
{"with": ["Transform2D"], "include": ["Transform2D"], "omit_defaults": true}
```

```json
{"count":1,"entities":[{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}},"id":"player"}],"truncated":false}
```

`truncated` is `true` when `limit` cut the list short:

```json
{"limit": 1}
```

```json
{"count":3,"entities":["camera"],"truncated":true}
```

## Snapshots

### world.snapshot

Saves a copy of every entity under a name, to compare against with `world.diff` or go back to
with `world.restore`. A name in use is replaced. A session keeps up to 100 snapshots, and they
last as long as the session.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | string, 1 to 64 characters | `"default"` | Snapshot name |

Needs: `read`.

```json
{}
```

```json
{"entities":3,"name":"default"}
```

### world.diff

Lists the entities added, removed or changed since a snapshot, by ID. `changed` holds every added
or changed entity with the new value of each field that differs: from the snapshot, or from its
default for a component the entity did not have (`{}` when every field is at its default). A
removed component is `null`. Empty sections are left out, so no changes give `{}`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `from` | string | `"default"` | Snapshot to compare from |
| `to` | string | the world now | Snapshot to compare to |

Needs: `read`. After a snapshot, moving `player` and destroying `spawn_point`:

```json
{}
```

```json
{"changed":{"player":{"Transform2D":{"position":[80.0,32.0]}}},"removed":["spawn_point"]}
```

An unknown name fails with `-32001` and the closest name:
`/from: no snapshot is named "defualt"; did you mean "default"?`.

### world.restore

Puts every entity back as it was in a snapshot. Undo reverts it; the tick counter keeps counting.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | string | `"default"` | Snapshot to restore |

Needs: `edit`. Takes `dry_run`.

```json
{}
```

```json
{"entities":3,"restored":"default"}
```

## Run

### step

Advances the simulation by running every system once per tick. Undo reverts the entities to how
they were before.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ticks` | integer, 1 to 100000 | `1` | Ticks to run |
| `tick_seconds` | number, above 0 and at most 1 | `1/60` | Seconds per tick |

Needs: `run`. Takes `dry_run`.

```json
{"ticks": 120}
```

```json
{"tick":120,"ticks":120}
```

`tick` is the world's tick counter after the step. Undo restores entities but not the counter.

### run.until

Steps the simulation until an entity matches a condition, or until none does, or the tick limit
passes, then returns once. Use it instead of stepping and querying after every tick: "when does
the ball reach the floor?", "do all enemies die within ten seconds?". Undo reverts the entities to
how they were before.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `with`, `without`, `where` | as in [`world.query`](#worldquery) | | The entities to wait for. At least one is required |
| `until` | `"any"` or `"none"` | `"any"` | Stop when any entity matches, or when none does |
| `ticks` | integer, 1 to 100000 | `600` | Most ticks to run |
| `tick_seconds` | number, above 0 and at most 1 | `1/60` | Seconds per tick |
| `include`, `omit_defaults`, `limit` | as in [`world.query`](#worldquery) | | What to return about the matching entities |

Needs: `run`. Takes `dry_run`.

The condition is checked before each tick, so one that already holds runs no ticks. The result is
`world.query`'s for the entities that match when it stops, plus `matched` (whether the condition
held), `ticks` (how many ran) and `tick` (the world's tick counter). With `"until": "none"` a
match means nothing is left, so `entities` is empty.

From [examples/run-until](../../examples/run-until/), where a `gravity` system makes `ball` fall
and a `despawn` system removes bodies below y = 1000:

```json
{"with": ["Body"], "include": ["Transform2D", "Body"], "omit_defaults": true, "tick_seconds": 0.015625,
 "where": [{"component": "Transform2D", "field": "position.y", "op": ">", "value": 600}]}
```

```json
{"count":1,"entities":[{"components":{"Body":{"velocity":[0.0,1104.0]},"Transform2D":{"position":[40.0,603.75]}},"id":"ball"}],"matched":true,"tick":69,"ticks":69,"truncated":false}
```

```json
{"with": ["Body"], "until": "none", "tick_seconds": 0.015625}
```

```json
{"count":0,"entities":[],"matched":true,"tick":89,"ticks":10,"truncated":false}
```

When the limit passes first, `matched` is `false`:

```json
{"with": ["Body"], "ticks": 10, "tick_seconds": 0.015625,
 "where": [{"component": "Transform2D", "field": "position.x", "op": "!=", "value": 40}]}
```

```json
{"count":0,"entities":[],"matched":false,"tick":79,"ticks":10,"truncated":false}
```

In a running game it runs between frames, so the game waits for it, as it does for `step`.

### play

Starts the game ticking in real time, running every system each tick. No params. Needs: `run`.
Only a running game (`slash-cli run --serve`, or `slash-mcp --live` connected to one) has `play`
and `pause`; other sessions advance the world with `step`. A running game starts playing.

```json
{"playing":true,"tick":123}
```

### pause

Stops the game ticking, so the world holds still while you inspect or edit it. The game keeps
serving requests, and `step` still advances it. No params. Needs: `run`.

```json
{"playing":false,"tick":123}
```

`play` and `pause` change nothing in the world, so there is nothing to undo.

## Replays

A replay is a run recorded into a [`.replay.json`](scene-format.md#replays) file: the entities it
started from, then each tick the host stepped, the input it queued and the commands that changed
the world, in order. Playing it back in any session lands on the same world, so a player's bug
report or an agent's play session can be stepped through again, queried at any tick and kept as
a test. [examples/replays](../../examples/replays/) records a run, plays it back and seeks the
moment something happened.

Recording starts the world afresh, as playback does: held input, focus, hover, particles,
physics contacts and playing sounds start over, and the entities are stored again in the order a
replay puts them back. Gameplay systems that keep state outside components add a reset with
`World::add_reset` so they replay exactly. What a recording leaves out: commands that only read,
dry runs, failed commands, `play`, `pause`, `display.set` and `storyboard.edit`. `undo`, `redo`,
`world.restore`, `scene.open`, `scene.new`, hot-reloaded prefabs and batches holding any of these
are kept as the entities they left, since playback has neither the history nor the files.
Text pasted from the system clipboard in a window is not kept.

### replay.record

Starts recording the session's world to a `.replay.json` file, written by `replay.stop`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | required | The file, inside the project, ending in `.replay.json` |

Needs: `read`, `filesystem`. One recording runs at a time, and not inside a batch.
`slash-cli run --record <file>` records a whole played run the same way.

```json
{"path": "replays/run.replay.json"}
```

```json
{"recording":"replays/run.replay.json","tick":0}
```

### replay.stop

Stops recording and writes the file. No params. Needs: `filesystem`. Returns the path and how
many ticks, kept commands and input events it holds.

```json
{"commands":0,"inputs":6,"path":"replays/run.replay.json","ticks":150}
```

### replay.run

Plays a replay back: puts back the entities it started from with its tick counter and next
generated ID, starts the world afresh, then steps the same ticks with the same input and
commands, to its end or to `tick`. It replaces the world, like `scene.open`, and clears the undo
history.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | required | The `.replay.json` file |
| `tick` | integer | the end | Stop here instead: everything recorded before this tick's step has happened. A command that steps past it, such as `step`, stops where it did |

Needs: `edit`, `run`, `filesystem`. Takes `dry_run`. Refused while a recording runs.

The result holds `tick` (the world's tick now), `start` and `end` (the recording's first and last
ticks) and `matched`:
whether every hash the recording took on the way matched, and at its end the entities too. A
hash is taken after each kept command and before every 60th tick the host stepped. When the run
went another way, `diverged` is the first tick where a hash differed, `changes` (at the end) is
what differs from the recorded end in the form of [`world.diff`](#worlddiff), and `failed`
names a command that failed, where playback stopped. A file that is not a valid replay is
refused with every error's JSON Pointer in `file_errors`.

From [examples/replays](../../examples/replays/):

```json
{"path": "replays/run.replay.json", "tick": 55}
```

```json
{"end":150,"matched":true,"start":0,"tick":55}
```

With a lower jump in the game's code, the hero no longer clears the crate:

```json
{"changes":{"added":["coin"],"changed":{"coin":{...},"hero":{"Transform2D":{...}}}},"diverged":60,"end":150,"matched":false,"start":0,"tick":150}
```

### replay.seek

Moves a replay's playback to a tick, by a number of ticks, or to the first tick where an entity
matches a condition, so a bug can be looked at the moment it happens and the ticks around it.
Going forward goes on from where the last `replay.run` or `replay.seek` left the world, which is
exact and costs only the ticks between. Going back plays the run again from its start, since
physics contacts and other state outside the components can't be put back. So does going
forward after anything else changed the world: a command other than a query, ticks the host
stepped or input it queued. It replaces the world, like `replay.run`, and clears the undo
history.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | the one playing | The `.replay.json` file |
| `tick` | integer | | Go to this tick. With a search, the last tick to search |
| `ticks` | integer | | Move this many ticks from where the playback is; negative goes back |
| `with`, `without`, `where` | as in [`world.query`](#worldquery) | | Search for the first tick where an entity matches |
| `until` | `"any"` or `"none"` | `"any"` | Find a tick where any entity matches, or where none does |
| `from` | integer | the start | Where a search starts |
| `include`, `omit_defaults`, `limit` | as in [`world.query`](#worldquery) | | What to return about the matching entities |

Needs: `edit`, `run`, `filesystem`. Takes `dry_run`. Refused while a recording runs. Give
`tick`, `ticks` or a search; `tick` and `ticks` can't go together, nor `ticks` and a search.

The world at a tick has had everything recorded at that tick happen, as in `replay.run`: a
command that steps, such as `step`, takes the playback to where it stopped. A search checks
each tick from `from` up to `tick` (the end if left out) and stops at the first that holds. The
result is `replay.run`'s, plus, for a search, `world.query`'s for the entities that match and
`found`.

From [examples/replays](../../examples/replays/), the first tick with no `Area2D` left (the coin
taken), then the tick before:

```json
{"with": ["Area2D"], "until": "none"}
```

```json
{"count":0,"end":150,"entities":[],"found":true,"matched":true,"start":0,"tick":96,"truncated":false}
```

```json
{"ticks": -1}
```

```json
{"end":150,"matched":true,"start":0,"tick":95}
```

## Tests

### test.run

Runs the project's [`.test.json` tests](scene-format.md#tests) headless and returns only the
checks that failed, so checking a change takes one call and no logs or screenshots. Each test
opens its scene file in a session of its own and runs its steps; it fails at the first step
whose command fails, whose reply doesn't hold what the step expects, or during which a warning
or an error is logged. The session that runs `test.run` keeps its world and history, and tests
can't write files. Tests start from scene files, so save a scene before testing an edit.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `paths` | string array | every `.test.json` file | Test files and folders in the project; `build/` is skipped |
| `tests` | string array | all | Run only the tests with these names |

Needs: `run`, `filesystem`. With neither `paths` nor `tests`, it also runs
[`storyboard.check`](#storyboardcheck) and adds its errors. A name in `tests` that no test has
fails with `-32001` and the closest name.

The result counts the tests run and those that passed, and lists each failed check:

| Field | Meaning |
| --- | --- |
| `file` | The test file, or for a storyboard error the file it is in |
| `test` | The test's name |
| `path` | JSON Pointer into the file: the expected value that didn't match, the bad param of a command that failed (`/tests/<name>/steps/<i>/params/...`), the step during which something was logged, or the scene that didn't open |
| `message` | What was wrong, quoting long values cut short |
| `tick` | The test's world's tick when it failed, after the step |
| `log` | The warning or error logged, as its log line's fields |

A test file that doesn't parse or match the schema is a failed check too, with `file`, `path`
and `message`, and the other files still run.

From [examples/headless-tests](../../examples/headless-tests/), with the floor's shape disabled
so the crate falls forever:

```json
{"tests": ["crate_lands"]}
```

```json
{"failed":[{"file":"tests/main.test.json","message":"expected 1 item, got 0 items: []","path":"/tests/crate_lands/steps/0/expect/entities","test":"crate_lands","tick":600}],"passed":0,"tests":1}
```

With every check passing:

```json
{"failed":[],"passed":3,"tests":3}
```

## History

### undo

Reverts the last change to the scene or the storyboard. No params. Needs: `edit`.

```json
{"can_redo":1,"can_undo":2,"undone":"entity.destroy"}
```

With nothing to undo, it fails with `-32002` and the message `nothing to undo`. Undoing a
`storyboard.edit` whose entries changed since fails with `-32005` and changes nothing.

### redo

Reapplies the last change that `undo` reverted. No params. Needs: `edit`.

```json
{"can_redo":0,"can_undo":3,"redone":"entity.destroy"}
```

### history.get

Lists the changes `undo` and `redo` would revert or reapply, by command name. The last `undo`
entry and the first `redo` entry are next. No params. Needs: `read`.

```json
{"redo":[],"undo":["batch","component.remove","entity.destroy"]}
```

## Several at once

### batch

Runs several commands in one call. If any fails, none of their changes to the scene are kept and
the error names the failing command. One undo reverts the whole batch.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `commands` | array of 1 to 1000 `{"command", "params"}` objects | required | Commands to run in order |

Needs no capability itself; each command inside it is checked as usual. Takes `dry_run`, which
the commands inside it cannot. A batch cannot contain another `batch`, `undo` or `redo`.

```json
{"commands": [
  {"command": "entity.create", "params": {"id": "enemy_01", "components": {"Transform2D": {"position": [200, 40]}}}},
  {"command": "component.set", "params": {"id": "player", "components": {"Transform2D": {"rotation": 1.0}}}}
]}
```

```json
{"results":[{"id":"enemy_01"},{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}]}
```

When one command fails, the error gives its index, its name and the JSON Pointer of the bad value
inside the batch:

```json
{"code":-32002,"data":{"command":"entity.create","index":1,"path":"/commands/1/params/id"},"message":"/commands/1 (entity.create) failed, so nothing was changed. /id: entity id \"a\" is already in use"}
```

## Observe

### events.subscribe

Starts queueing world events for `events.read`, so an agent learns what happened without
re-querying the world. A session keeps up to 16 subscriptions, each with up to 1000 queued
events; when a queue is full the oldest event is dropped.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kinds` | array of strings | all | Only these kinds of event (below) |
| `ids` | array of strings | all | Only events about these entity IDs |

| Kind | When | Fields |
| --- | --- | --- |
| `entity.created` | An entity with an ID is created, by a command or a system | `id` |
| `entity.destroyed` | An entity with an ID is destroyed | `id` |
| `command` | A command changed the world, including `step`, `undo` and `redo`, or a prefab reload (`"command": "asset.reload"`) or new gameplay code (`"command": "gameplay.reload"`) did | `command`, and `changes` as [`world.diff`](#worlddiff) returns them |
| `asset.reloaded` | A loaded asset or prefab file changed on disk | `data`: `path`, `type`, and `version`, or `error` if it no longer loads |
| `gameplay.reloaded` | The game's code was rebuilt and swapped in, or failed to load | `data` as [`gameplay.build`](#gameplaybuild)'s `reload` |
| anything else | Gameplay code called `World::emit`, e.g. `"coin_collected"` | `id` and `data`, if the game gave them |

Every event also has `seq`, which counts up across the session, and `tick`, the world's tick when
it happened. A failed command or a dry run queues nothing. Opening or creating a scene, `undo`,
`redo` and `world.restore` queue one `command` event instead of an event per entity. An unknown kind starting with `entity.` fails
with "did you mean". Needs: `read`.

```json
{}
```

```json
{"subscription":"events_1"}
```

### events.read

Returns and removes a subscription's queued events, oldest first.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `subscription` | string | required | Name from `events.subscribe` |
| `limit` | integer, 1 to 1000 | `100` | Most events to return |

Needs: `read`. After creating `enemy_01` and turning `player`:

```json
{"subscription": "events_1"}
```

```json
{"dropped":0,"events":[{"id":"enemy_01","kind":"entity.created","seq":1,"tick":0},{"changes":{"added":["enemy_01"],"changed":{"enemy_01":{"Transform2D":{"position":[200.0,40.0]}}}},"command":"entity.create","kind":"command","seq":2,"tick":0},{"changes":{"changed":{"player":{"Transform2D":{"rotation":1.0}}}},"command":"component.set","kind":"command","seq":3,"tick":0}],"more":false}
```

`dropped` counts the events lost since the last read because the queue was full, and `more` is
`true` when `limit` left events in the queue.

### events.unsubscribe

Stops a subscription and drops its queued events.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `subscription` | string | required | Name from `events.subscribe` |

Needs: `read`.

```json
{"unsubscribed":"events_1"}
```

### log.tail

Returns the most recent engine log lines as JSON objects, oldest first.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `lines` | integer, 1 to 1000 | `50` | How many lines |
| `level` | `"debug"`, `"info"`, `"warn"` or `"error"` | all | Only lines at this level or above |

Needs: `read`.

```json
{"lines": 2, "level": "warn"}
```

The result is `{"lines": [...]}`, each line an object such as
`{"level":"info","msg":"server listening","url":"http://127.0.0.1:34467"}`. A session captures
only the lines logged while it is open, so a fresh one returns `{"lines":[]}`.

### frame.capture

Draws the frame the window shows now, on the CPU: the world's sprites, tile maps, particles and
lights, then the collision shapes if asked, then the controls, over the project's
`rendering.clear_color`, at the window size
(`window.width` and `height` in `project.json`). The software device that draws it shades with
the same code as the GPU device's shaders, so the pixels are the same on every machine and with
no GPU. Nothing changes. [examples/frame-capture](../../examples/frame-capture/) calls it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `points` | array of `[x, y]`, 1 to 256 | | Window pixels to return the colors of |
| `region` | `[x, y, width, height]` | the whole window | Only this part of the window, in window pixels |
| `scale` | number, 0.05 to 4 | `1` | Image pixels per window pixel |
| `image` | boolean | `false` | Return the image as a base64 PNG in `png` |
| `path` | string | | A `.png` file to write, relative to the project |
| `time` | number | the tick's | Seconds of game time for materials' `time` nodes and [animated tiles](scene-format.md#animated-and-large-tiles) |
| `collisions` | boolean | `false` | Draw every `CollisionShape2D` and `CollisionPolygon2D` over the world, as Godot's Visible Collision Shapes |

Needs: `read`, and `filesystem` with `path`. A HUD with a health bar at 75%:

```json
{"points": [[80, 30], [20, 14], [68, 14]]}
```

```json
{"colors":[[0.2,0.4,0.8,1],[0.502,0.584,0.753,1],[0.169,0.31,0.588,1]],"drawn":{"controls":3},"height":90,"width":160}
```

`width` and `height` are the image's, after `region` and `scale`. `drawn` counts what was drawn
(`sprites`, `tiles`, `particles`, `lights`, `controls` and `shapes`), leaving out zeros. When the
project's `rendering.scaling` ([project-format.md](project-format.md#render-scale)) draws the
world at another size, `upscaler` names what scaled it and `render_size` is the world's size in
pixels, `[width, height]`. `colors` are
`[r, g, b, a]` from 0 to 1, one per point, read from the whole window whatever `region` says.
Over MCP, `png` is sent as an image content block rather than as text. A point or region outside
the window is an error at its path, and so is a `path` that doesn't end in `.png`. In a dry-run
`batch`, `path` writes nothing.

### frame.summary

Says what the window shows now, as data instead of an image: what `frame.capture` would draw,
read thing by thing. Each sprite, tile map layer, `CPUParticles2D`, `GPUParticles2D` and
control that shows is listed in drawing order, bottom first, with how much of it shows when not
all of it does, and what doesn't show is listed by why. Costs far fewer tokens than an image and needs no vision.
Nothing changes. [examples/frame-summary](../../examples/frame-summary/) calls it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ids` | array of entity IDs | all | Only these entities in the lists |
| `region` | `[x, y, width, height]` | the whole window | Only this part of the window, in window pixels |
| `map` | `[columns, rows]`, each 1 to 160 | | Add a text picture of the window (or region) |
| `limit` | integer, 1 to 1000 | `100` | At most this many entries in each list |

Needs: `read`. A player half behind a bush, a coin past the right edge, a ghost faded out and a
pause button with focus:

```json
{"ids": ["player", "coin", "ghost", "pause"]}
```

```json
{"clear":["ghost"],"drawn":[{"id":"player","kind":"Sprite2D","rect":[82,136,16,24],"shown":0.54},{"focused":true,"id":"pause","kind":"Button","rect":[130,4,60,30],"text":"Pause"}],"offscreen":["coin"],"viewport":[320,180]}
```

With `"map": [40, 12]` over the whole frame, the reply adds:

```json
{"map":{"legend":{"P":"player","b":"bush","g":"ground","h":"health","p":"pause","s":"score"},
        "rows":["................pppppppp................",
                ".sssss..ss......pppppppp...hhhhhhhhhhhh.",
                "........................................",
                "...",
                "...........bbbbb........................",
                "..........Pbbbbbb.......................",
                "gggggggggggggggggggggggggggggggggggggggg"]}}
```

- `drawn` lists what shows: `id`, `kind` (`Sprite2D`, `TileMapLayer`, `CPUParticles2D`,
  `GPUParticles2D`,
  `PopupMenu` for an open popup menu, named by the entity it belongs to, `Tooltip` for the
  tooltip shown, named by its control, or the control's type,
  such as `Label` or `Button`) and `rect`, its bounds in window pixels to a
  tenth, `[x, y, width, height]`, not cut to the window: a control's rect as
  [`ui.layout`](#uilayout) places it (only the part its clip leaves, inside a
  `ScrollContainer` or a parent with `clip_contents`), else the bounds of what it drew. `shown`,
  from 0.01 to 0.99, is the share of its pixels that show when not all of them do: the rest are
  outside the window (or region), under something drawn after it or cut off by a clip.
- What each shows: an `AnimatedSprite2D`'s `animation` and `frame`, a sprite sheet's `frame`, a
  layer's `tiles` and an emitter's `particles` (how many drew), and a control's `text` as laid
  out (lines joined with `\n`, cut short with its ellipsis, only the characters
  `visible_characters` shows), a button's `draw_mode` when it isn't `"normal"`, a slider's or
  scroll bar's `value`, a scroll container's `scroll` and `focused`, and a popup menu's `items`
  (their texts), `focused_item` and `scroll`, and the tooltip's `text`.
- `layer` names the [`CanvasLayer`](scene-format.md#canvas-layers-and-parallax) a thing draws
  in, when it is in one. Things in layers under the world come first in `drawn`, those over it
  last. A repeating `Parallax2D`'s children are listed once, their `rect` round every copy.
- `covered` lists what is in the window but wholly under what was drawn after it, `offscreen`
  what drew only outside the window (left out with a `region`) and `clear` what drew no pixel
  that can be seen, such as a sprite whose `modulate` alpha is below 0.25. `clipped` lists
  controls cut off entirely by a clip, such as the rows a `ScrollContainer` is scrolled past.
  Each is left out when empty.
- `problems` lists what can't draw as written, with the JSON Pointer of the field in the entity:
  a missing texture, material or tile set, a frame outside its sheet, and the mistakes
  [`ui.layout`](#uilayout) reports.
- `map` is `{"legend": {...}, "rows": [...]}`, like a `TileMapLayer`'s cells: each row is
  `columns` keys, each key what shows at the cell's center, `.` for nothing (the clear color).
  Keys are the start of each ID where that is free, one character while there are few enough
  things on the map, else two.
- `truncated` is `true` when `limit` left entries out.
- `camera`, when a [`Camera2D`](scene-format.md#2d-camera) is current, is `{"id": ..., "rect":
  [x, y, width, height]}`: the camera and the world rect the window shows through it, to a
  tenth. Rects in the other fields are in window pixels, as the camera draws them.

A thing's pixels are those where its alpha (its texture's there times its color's) is at least
0.25, and it shows at a pixel when at least a quarter of the pixel's color comes from it, as
blending mixes it under what was drawn after it: a sprite under a panel of alpha 0.6 still
shows, one under a panel of alpha 0.9 is covered. The map shows the last thing drawn that shows
at each cell's center. Materials and lights aren't run, so a sprite a material makes clear, or
one in the dark, still counts as showing. The part of each thing in the window (or region) is
read at every pixel up to 64 pixels across and down, and at evenly spaced pixels beyond, so
`shown` is exact for small things and close for large ones. A `region` outside the window is an error at its path, and so is an unknown ID, with the
closest one.

### canvas.convert

Turns points between window pixels and the world, through the current
[`Camera2D`](scene-format.md#2d-camera), as Godot's `get_global_mouse_position`: where a click
lands in the level, or where a thing in the level shows in the window. With an `id`, uses the
canvas that entity draws in instead: its
[`CanvasLayer`](scene-format.md#canvas-layers-and-parallax)'s, or the window itself for a
control in no layer. Nothing changes.
[examples/canvas-layers](../../examples/canvas-layers/) calls it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `points` | array of `[x, y]`, 1 to 256 | required | The points to turn |
| `to` | `"world"` or `"screen"` | required | `"world"`: from window pixels into the canvas; `"screen"`: from the canvas into window pixels |
| `id` | entity ID | the world | Use the canvas this entity draws in |

Needs: `read`. A click in the middle of a 320x180 window and one on the ground, with the camera
over the level at x 440:

```json
{"points": [[160, 90], [100, 165]], "to": "world"}
```

```json
{"points":[[600,90],[540,165]]}
```

- `points` are in the same order, to four decimals. With no camera, the world's pixels are the
  window's.
- `layer` names the `CanvasLayer` whose canvas was used, when `id` draws in one.
- An unknown `id` is an error at `/id`, with the closest ID.

## Display

A game served with `slash-cli run --serve` (or reached through `slash-mcp --live`) also has these
four, for its displays, how it presents frames, how evenly they arrive and its debug overlay.
Other sessions have no window or loop, so they leave them out.
[examples/frame-pacing](../../examples/frame-pacing/) calls the first three.

### display.list

Lists the displays, primary first, with the game's V-Sync mode, frame cap and render scale. A
headless game has no displays.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `modes` | boolean | `false` | Also list each display's fullscreen modes (`width`, `height`, `refresh_rate`, `pixel_density`) |

Each display has its `id`, `name`, whether it is `primary` and whether the game's `window` is on
it, its `bounds` (`[x, y, width, height]` in screen coordinates), its UI `scale`, the current
mode's `refresh_rate` and `pixel_density`, and `refresh_rates`: every rate its fullscreen modes
offer, fastest first, for an options menu. A refresh rate of `0` means the system doesn't say.
`scaling` is the render scale's settings, as `rendering.scaling` in
[project-format.md](project-format.md#render-scale), with `in_effect`, the upscaler the game's last
frame used (`none` when the world drew at full size), and its `render_size`. Needs: `read`. On SDL's offscreen video driver, which has one display and reports no refresh
rate:

```json
{}
```

```json
{"displays":[{"bounds":[0,0,1024,768],"id":1,"name":"1","pixel_density":1.0,"primary":true,"refresh_rate":0.0,"refresh_rates":[],"scale":1.0,"window":true}],"max_fps":120,"scaling":{"fsr_sharpness":0.2,"in_effect":"fsr","mode":"fsr","render_size":[512,384],"scale":0.5},"vsync_mode":"mailbox"}
```

### display.set

Changes the V-Sync mode, the frame cap or the render scale from the next frame, as a game's
options menu would, and returns the settings in effect.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `vsync_mode` | `"disabled"`, `"enabled"`, `"adaptive"` or `"mailbox"` | unchanged | How frames wait for the display; see [project-format.md](project-format.md) |
| `max_fps` | integer, 0 to 1000 | unchanged | Frames per second at most; `0` means no cap |
| `scaling` | object | unchanged | The render scale: any of `mode`, `scale`, `fsr_sharpness` and `upscaler`, as `rendering.scaling` in [project-format.md](project-format.md#render-scale); fields left out stay as they are |

A mode the GPU or window lacks falls back to `enabled`, which every GPU has, and the result
shows the mode in effect. SDL's GPU API has no adaptive mode yet, so `adaptive` always falls back
on a window. Needs: `run`. It changes nothing in the world, so there is nothing to undo. Here
Mesa's software Vulkan on an offscreen window has no mode that skips V-Sync:

```json
{"vsync_mode": "disabled", "max_fps": 0}
```

```json
{"max_fps":0,"scaling":{"fsr_sharpness":0.2,"mode":"bilinear","scale":1.0},"vsync_mode":"enabled"}
```

A scaling `mode` that isn't `bilinear` or `fsr` is an error at `/scaling/mode` (Godot's `fsr2`
and MetalFX modes need the 3D renderer), and an `upscaler` the game hasn't added is a `-32001`
error at `/scaling/upscaler` that lists the ones it has. Either way nothing changes.
[examples/render-scale](../../examples/render-scale/) changes the render scale while the game
runs.

### perf.stats

Frame pacing over the last 120 frames, with the settings that shape it. No params. Needs: `read`.

```json
{"catch_up_frames":0,"fps":120.0,"frame_ms":{"avg":8.33,"max":8.67,"min":7.99},"frames":932,"max_fps":120,"renderer":"vulkan","scaling":{"fsr_sharpness":0.2,"in_effect":"none","mode":"bilinear","scale":1.0},"tick_hz":60.0,"ticks":465,"vsync_mode":"mailbox"}
```

`frame_ms` is the time from one frame's start to the next: a `max` far above `avg` is a hitch.
`catch_up_frames` counts, since the game started, frames that ran more than one fixed tick
because the frame before was late. `frames` and `ticks` also count from the start, and
`renderer` names the render device (`null` when nothing is drawn). A headless game runs one tick
per frame and never waits, so its frame times only show how long its ticks take.

### debug.overlay

Shows or hides the debug panel of a game run with `slash-cli run`, and the collision shapes it
draws, for the person watching the window. The panel shows the frame rate and frame time, the
tick and whether the game plays, the entity count and what the last frame drew, and the mouse's
position with the IDs of the bodies and areas whose shapes are under it. Its Pause, Step and
Shapes buttons run `pause`, `play` and `step` through the session, as an agent would, and toggle
the shapes. F3 shows and hides it too, and `--debug-overlay` and `--debug-collisions` turn it and
the shapes on from the start ([cli.md](cli.md)). An agent sees the same shapes with
`frame.capture`'s `collisions`. [examples/debug-overlay](../../examples/debug-overlay/) uses
both.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `visible` | boolean | unchanged | Show the panel |
| `collisions` | boolean | unchanged | Draw the collision shapes, whether or not the panel shows |

Needs: `read`; it changes nothing in the world, and a recording leaves it out.

```json
{"collisions": true}
```

```json
{"collisions":true,"visible":false}
```

## Import

### project.import

Converts a Godot 4 or Unity project, in a folder inside this project, into this project, as
[`slash-cli import`](cli.md#import) does. A Godot project's scenes become `.scene.json` files
beside the originals, its `SpriteFrames` `.frames.json` files, each `AnimationPlayer`'s
animation libraries a `.anim.json` file, its tile sets `.tileset.json` files, its themes
`.theme.json` files, its bus layouts `.bus_layout.json` files and its settings and input map
`project.json`. A Unity project's scenes and prefabs become `.scene.json` files beside the
originals (prefabs that scenes instance), its sprites, tile maps, animation, cameras, 2D physics,
joints, 2D lights, shadow casters and sounds components measured in pixels, its uGUI and
TextMesh Pro UI controls, the sprites its tile maps show a `.tileset.json` file, each Animator
controller a `.anim.json` file, its audio mixers a `.bus_layout.json` file,
and its player, time, physics and input settings `project.json`. Textures, sounds and fonts are used
where they are. Each script, and each Animator controller's state machine, becomes a storyboard task to port
it to C++. A game exported with Godot 4, its `.pck` file or its executable with the pack inside,
is first unpacked back into the Godot project it was made from, then converted the same way, and
so is a game built with Unity, whose project AssetRipper rebuilds
([Unity builds](unity-import.md#unity-builds)).
[godot-import.md](godot-import.md) and
[unity-import.md](unity-import.md) say what converts to what. Registered by `slash-cli` and
`slash-mcp`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `source` | string | required | The folder holding the Godot project's `project.godot`, or the Unity project's `Assets/` and `ProjectSettings/`; a game exported with Godot 4: its `.pck` file or executable; or a game built with Unity: its folder, executable, `.app`, `.apk` or `.ipa` file or web build |
| `unpack_to` | string | | For an exported or built game, a folder of this project to write the Godot or Unity project unpacked from it to; the import then uses its assets there. Left out, only the assets come along, under the paths they had in the game |
| `dry_run` | boolean | `false` | Return the report and the files it would write; write nothing |

Needs: `read`, `filesystem`, and `build` for a Unity build, since AssetRipper runs as a program
of its own; a game served with `slash-cli run --serve` refuses Unity builds. It writes files and changes no scene, so there is nothing to undo;
importing again rewrites the converted files and keeps each task's status. The result counts
what converted, lists each script's task, and lists what didn't convert, once per kind with how
many, the first place it was seen and why. The full report, with the values scripts need, is
written to `import/godot.json` or `import/unity.json`. The settings it writes to `project.json`
(the window, physics, input and audio) apply to the session at the next command.

From [examples/import-godot](../../examples/import-godot/):

```json
{"source": "godot"}
```

```json
{"converted":{"animation_libraries":1,"assets":8,"audio_streams":2,"bus_layouts":1,"input_actions":3,"process_materials":0,"scenes":2,"sprite_frames":2,"themes":1,"tile_sets":1},"engine":"godot","files":13,"main_scene":"godot/main.scene.json","project":"Coin Run","report":"import/godot.json","scripts":[{"entities":0,"script":"godot/game_state.gd","task":"task/port-game-state"},{"entities":1,"script":"godot/main.gd","task":"task/port-main"},{"entities":2,"script":"godot/player.gd","task":"task/port-player"}],"unsupported":[{"count":1,"what":"autoload","where":"project.godot: GameState","why":"autoloads are gameplay code; see the storyboard task"},{"count":1,"what":"file type .svg","where":"icon.svg","why":"textures are PNG or BMP files; save it as PNG"},{"count":1,"what":"node type Timer","where":"main.tscn: Timer","why":"timers are gameplay code: count the ticks in a C++ system"}]}
```

From [examples/import-unity](../../examples/import-unity/), where `pixels_per_unit` is the
scale Unity's units convert at:

```json
{"source": "unity"}
```

```json
{"converted":{"animation_libraries":1,"assets":9,"bus_layouts":1,"input_actions":12,"prefabs":3,"scenes":1,"tile_sets":1},"engine":"unity","files":12,"main_scene":"unity/Assets/Scenes/Pond.scene.json","pixels_per_unit":32.0,"project":"Pond Hop","report":"import/unity.json","scripts":[{"entities":1,"script":"unity/Assets/Scripts/CameraFollow.cs","task":"task/port-camera-follow"},{"entities":6,"script":"unity/Assets/Scripts/Coin.cs","task":"task/port-coin"},{"entities":2,"script":"unity/Assets/Scripts/Frog.cs","task":"task/port-frog"},{"entities":1,"script":"unity/Assets/Scripts/Hud.cs","task":"task/port-hud"}],"unity_version":"2022.3.20f1","unsupported":[{"count":1,"what":"component EventSystem","where":"Assets/Scenes/Pond.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"},{"count":1,"what":"component StandaloneInputModule","where":"Assets/Scenes/Pond.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"},{"count":1,"what":"exposed mixer parameters","where":"Assets/Audio/Pond.mixer","why":"MusicVolume: scripts that set them set buses with audio.set_bus or gameplay code instead"},{"count":1,"what":"inactive object","where":"Assets/Scenes/Pond.unity: Coin (2)","why":"converted hidden, with its shapes disabled; there is no inactive state, so its other components still run"},{"count":1,"what":"input axis Mouse X","where":"ProjectSettings/InputManager.asset","why":"mouse movement axes: read the mouse's motion in code"},{"count":1,"what":"input binding","where":"Assets/Input/Controls.inputactions: Player/Croak","why":"\"<XRController>/trigger\": XRController bindings"},{"count":1,"what":"input interaction","where":"Assets/Input/Controls.inputactions: Player/Croak","why":"Hold: gameplay code times holds and taps"}]}
```

For an exported game, `unpacked` says what came back out of it: the pack's format and the
Godot version that exported it, whether it was inside an executable, the folder `unpack_to`
wrote, counts of scenes, resources, scripts (`scripts_from_tokens` of them from binary tokens),
textures, sounds and fonts, `renamed` for files that came back under another extension, and its
own `unsupported`. From [examples/unpack-godot](../../examples/unpack-godot/):

```json
{"source": "shipped/jumper.pck", "unpack_to": "godot"}
```

```json
{"converted":{"animation_libraries":0,"assets":2,"bus_layouts":0,"input_actions":1,"scenes":1,"sprite_frames":0,"themes":0,"tile_sets":0},"engine":"godot","files":12,"main_scene":"godot/main.scene.json","project":"Jumper","report":"import/godot.json","scripts":[{"entities":1,"script":"godot/player.gd","task":"task/port-player"}],"unpacked":{"files":8,"folder":"godot","fonts":0,"pack":{"embedded":false,"files":10,"format":3,"godot":"4.5.0"},"resources":0,"scenes":1,"scripts":1,"scripts_from_tokens":1,"sounds":1,"textures":1,"unsupported":[]},"unsupported":[]}
```

For a Unity build, `unpacked` names the build's `platform`, scripting `backend` and `unity`
version, AssetRipper's version, the folder `unpack_to` wrote, counts of scenes, prefabs, scripts,
`package_scripts` and files, the `log` of AssetRipper's run and its own `unsupported`. From
[examples/unpack-unity](../../examples/unpack-unity/):

```json
{"source": "shipped/Hopper", "unpack_to": "unity"}
```

```json
{"converted":{"animation_libraries":0,"assets":1,"bus_layouts":0,"input_actions":0,"prefabs":0,"scenes":1,"tile_sets":0},"engine":"unity","files":17,"main_scene":"unity/Assets/Scenes/Main.scene.json","pixels_per_unit":16.0,"project":"Hopper","report":"import/unity.json","scripts":[{"entities":1,"script":"unity/Assets/Scripts/Assembly-CSharp/Hopper.cs","task":"task/port-hopper"}],"unity_version":"2022.3.20f1","unpacked":{"assetripper":"2.0.0","build":{"backend":"il2cpp","platform":"linux","unity":"2022.3.20f1"},"files":5,"folder":"unity","log":".slash/import/unity/assetripper.log","package_scripts":4,"prefabs":0,"scenes":1,"scripts":1,"unsupported":[{"count":1,"what":"script without its code","where":"Assets/Scripts/Assembly-CSharp/Hopper.cs","why":"the build's code couldn't be read back (IL2CPP builds keep it compiled), so only the script's name and the objects using it came back"}]},"unsupported":[{"count":1,"what":"component EventSystem","where":"Assets/Scenes/Main.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"},{"count":1,"what":"component StandaloneInputModule","where":"Assets/Scenes/Main.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"}]}
```

With `dry_run`, `files` lists the paths it would write instead of counting them (a Unity build
is still unpacked into `.slash/import/unity/`). A missing folder fails with `-32001`; a folder
that is neither a Godot nor a Unity project, a Godot 3 project or a project from before Unity
2018.3, and a file that holds no Godot 4 pack, with `-32002`; a pack whose file list is encrypted
with the game's own key, a Unity build with encrypted bundles, a Unity build in a session without
`build` (`data.capability` is `"build"`) or in a running game, and a path outside the project,
with `-32003`; AssetRipper failing to start or to rebuild the project, with `-32004` and the end
of its output. `data.path` is `/source`, or `/unpack_to` for an `unpack_to` outside the project.

## Export

### export.run

Builds the project with an export preset, as [`slash-cli export`](cli.md#export) does, and
returns the same result. Registered by `slash-cli` and `slash-mcp`; a game served with
`slash-cli run --serve` leaves it out, because an export would stall its loop.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `preset` | string | required | A preset from `export_presets.json`, or a platform (`macos`, `windows`, `linux`, `web`, `android`, `ios`) for its defaults |
| `debug` | boolean | `false` | Debug build instead of Release |
| `test` | boolean | `false` | Run the exported game headless afterwards, where this machine can |

Needs: `filesystem`, `build`. It changes no scene, so there is nothing to undo.

```json
{"preset": "linux", "test": true}
```

```json
{"bytes":5335403,"executable":"build/export/linux/mygame","linux":{"arch":"x86_64"},"log":".slash/export/linux/export.log","output":"build/export/linux","platform":"linux","preset":"linux","seconds":66.6,"test":{"entities":0,"passed":true,"ticks":60}}
```

A bad preset name fails with `-32002` before anything is built; any other failure, including a
failed headless test, fails with `-32004`. Either way `data` carries the export's error details:
the failed `step`, the validation `errors` or the end of the build log in `tail`, and the `log`
path.

```json
{"code":-32002,"data":{"step":"options"},"message":"no preset or platform called \"macso\"; did you mean \"macos\"?"}
```

### sdk.check

Checks the project's [SDK packages](sdk-format.md#sdk-check): each `sdk.json`, the presets'
`sdks`, the keys in the environment and that every file a platform links exists, for each
architecture the presets build. Registered next to `export.run`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `platform` | string | every platform | Check only this platform's builds: `macos`, `windows`, `linux`, `web`, `android` or `ios` |
| `fetch` | boolean | `false` | Download the files not fetched yet, so they are checked too |

Needs: `read`, `filesystem`; `fetch` also needs `build` (`-32003` without it, with `data.path`
`/fetch`).

```json
{"platform": "linux"}
```

```json
{"ok":false,"problems":[{"message":"set the environment variable HELLO_APP_ID: The game's ID on the Hello dashboard; any text does for the stand-in.","path":"/keys/HELLO_APP_ID","sdk":"sdks/hello"}],"sdks":[{"folder":"sdks/hello","license":"MIT","name":"hello","platforms":["android","ios","linux","macos","web","windows"],"version":"1.0.0"}]}
```

Problems don't fail the command: `ok` says whether there were any. Each names the package's
folder in `sdk` (or `export_presets.json` for a preset's `sdks`), the JSON Pointer into its file
in `path` and the `message`. A package's downloads not fetched yet are listed in its `unchecked`,
with `maven` when files its `sdk.lock.json` pins aren't fetched yet; its `maven` is the number
of files the lock pins. A package with Maven dependencies and no up-to-date lock is a problem at
`/platforms/android/maven`.

### sdk.add

Adds an [SDK package](sdk-format.md#sdk-add) to `project.json`'s `sdks`. A folder in the project
is listed where it is; a git repository or an `https://` `.zip` or `.tar` archive is copied into
`sdks/<name>`. Listing a package twice does nothing.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `source` | string | required | A package folder in the project, a git repository URL, or an archive's `https://` URL |
| `name` | string | the package's name | Folder under `sdks/` for a copied package |
| `ref` | string | the default branch | Git branch or tag to clone |
| `sha256` | string | not checked | The archive's SHA-256; a mismatch adds nothing |
| `dry_run` | boolean | `false` | Check the package and return what would be added; change nothing |

Needs: `read`, `filesystem`; a URL also needs `build`. It changes no scene, so there is nothing to
undo; remove the folder from `sdks` to take a package out.

```json
{"source": "vendor/greeter"}
```

```json
{"copied":false,"folder":"vendor/greeter","keys":[],"license":"MIT","name":"greeter","platforms":["linux","macos","windows"],"version":"0.3.0"}
```

`keys` lists the environment variables to set before exporting. A folder outside the project
fails with `-32003`, a missing folder with `-32001`, a folder with no `sdk.json` or an invalid
one with `-32002`, a package whose
folder under `sdks/` exists with `-32005`, and a failed clone or download with `-32004`;
`data.path` is `/source`. A package with Maven dependencies and no `sdk.lock.json` adds
`"needs_lock": true`: run [`sdk.lock`](#sdklock) next.

### sdk.lock

Resolves the [Maven dependencies](sdk-format.md#maven-dependencies) of the project's SDK packages
(`maven` in their `android` section) as Gradle resolves them, and pins every file of each graph
by SHA-256 in the package's `sdk.lock.json`, which Android exports need. Downloads POMs and files
into `maven/` in `SLASH_SDKS_DIR`, or `~/.slash/sdks`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `sdk` | string | every package with Maven dependencies | Lock only this package |
| `dry_run` | boolean | `false` | Resolve and return what would be written; change nothing |

Needs: `read`, `filesystem`, `build`. It changes no scene, so there is nothing to undo.

```json
{"sdk": "ads"}
```

```json
{"sdks":[{"artifacts":14,"changed":true,"lock":"sdks/ads/sdk.lock.json","name":"ads"}]}
```

`artifacts` is the number of files pinned and `changed` whether the lock differs from the one
before. A package name that isn't in `project.json`'s `sdks` fails with `-32001` and a "did you
mean"; a module no repository has, or a range none of its versions meets, fails with the module's
name. `data.path` is `/sdk` when `sdk` was given.

## Gameplay code

A game's own components and systems are C++ in the folders and files `project.json`'s
[`gameplay.sources`](project-format.md#gameplay-code) names, registered in one
`SLASH_GAME_MODULE` function (see [AGENTS.md](../../AGENTS.md#gameplay-code)). Exports link them
in. `slash-cli`, `slash-mcp` and `slash-game` load them from the library this command builds, on
macOS and Linux, and every command first swaps in a library that changed since, so a build by
another program (such as [`slash-cli build`](cli.md#build)) goes in too.

### gameplay.build

Compiles the project's gameplay sources into `.slash/gameplay/game.so` and swaps the new code
into the world. The world keeps its entities; each component of the game's types keeps the
fields its new type still has. Only files that changed are compiled again. Registered by
`slash-cli` and `slash-mcp`; a running game leaves it out, and picks up a library another program
built before its next tick.

No params. Needs: `filesystem`, `build`. The swap is not undone by `undo`, but steps saved before
it still undo: values that no longer fit the code are left out.

```json
{}
```

```json
{"library":".slash/gameplay/game.so","log":".slash/gameplay/build.log","reload":{"changed":["Wallet"],"dropped":["Wallet.income: no longer in the code (2 entities)"]}}
```

`reload` says what the swap did: the game's component types `added`, `removed` and `changed`
(their fields or field types), and in `dropped` each value that could not be kept, with how many
entities lost it: a field the code no longer has, a value its new type can't hold, or a component
the old code kept without registering it. A failed swap leaves the old code running and fails
the command with `-32002` and the reason in `data.error`: a name used twice, a component or
system named like one of the engine's, no `SLASH_GAME_MODULE` function, or a library built
against other engine headers or another compiler.

Compiler errors fail with `-32002`, listing at most 20 in `data.errors`, each with `file`
(relative to the project when inside it), `line`, `column` and `message`; `data.log` is the full
build log. A project with no `gameplay.sources` fails with `-32002` and `data.step` `"options"`;
a build that fails without compiler errors, such as a link error, fails with `-32004` and the end
of the log in `data.tail`.

```json
{"code":-32002,"data":{"errors":[{"column":97,"file":"src/game.cpp","line":16,"message":"'struct {anonymous}::Wallet' has no member named 'bonsu'; did you mean 'bonus'?"}],"log":".slash/gameplay/build.log","step":"build"},"message":"the game's code has 1 error"}
```

## Adding a command

Register it in `engine/command/src/builtin.cpp` (or a registry of your own) with a name, a short
description, its capabilities, an `Effect` and a params schema with
`"additionalProperties": false`, add a test to `tests/test_command.cpp`, and add a section to this
page. See [AGENTS.md](../../AGENTS.md#commands-mcp-and-json-rpc).
