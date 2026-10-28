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
`run.until`, `world.restore`, `storyboard.edit`, `navigation.bake`, `lightmap.bake`, `voxel_gi.bake`, `occluder.bake`, `replay.run`, `replay.seek` and `batch`) to see what it would do. The command runs, then
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
`storyboard.edit` returns the versions it would write and `navigation.bake`, `lightmap.bake`, `voxel_gi.bake` and `occluder.bake` what they would bake. Commands that change nothing, such
as `entity.get`, reject it as an unknown param.

## Contents

| Group | Commands |
| --- | --- |
| Describe | [`engine.describe`](#enginedescribe), [`schema.get`](#schemaget), [`project.map`](#projectmap) |
| Scene | [`scene.get`](#sceneget), [`scene.new`](#scenenew), [`scene.open`](#sceneopen), [`scene.save`](#scenesave) |
| Entities | [`entity.create`](#entitycreate), [`entity.destroy`](#entitydestroy), [`entity.get`](#entityget), [`entity.place`](#entityplace) |
| Prefabs | [`prefab.save`](#prefabsave) |
| Assets | [`asset.list`](#assetlist) |
| Materials | [`material.eval`](#materialeval) |
| Animations | [`animation.eval`](#animationeval) |
| Models | [`model.inspect`](#modelinspect), [`model.cleanup`](#modelcleanup), [`skeleton.get`](#skeletonget), [`csg.get`](#csgget) |
| Paths | [`path.get`](#pathget) |
| Grid maps | [`gridmap.set`](#gridmapset), [`gridmap.get`](#gridmapget) |
| Terrains | [`terrain.generate`](#terraingenerate), [`terrain.brush`](#terrainbrush), [`terrain.get`](#terrainget) |
| Tile maps | [`tilemap.paint`](#tilemappaint), [`tilemap.set`](#tilemapset), [`tilemap.get`](#tilemapget), [`tilemap.convert`](#tilemapconvert) |
| Physics | [`physics.raycast`](#physicsraycast), [`physics.overlap`](#physicsoverlap), [`physics.contacts`](#physicscontacts), [`physics.gravity`](#physicsgravity) |
| Navigation | [`navigation.path`](#navigationpath), [`navigation.closest`](#navigationclosest), [`navigation.random`](#navigationrandom), [`navigation.map`](#navigationmap), [`navigation.agent`](#navigationagent), [`navigation.bake`](#navigationbake) |
| Particles | [`particles.eval`](#particleseval) |
| Lights | [`light.sample`](#lightsample) |
| Global illumination | [`lightmap.bake`](#lightmapbake), [`voxel_gi.bake`](#voxel_gibake) |
| Occlusion culling | [`occluder.bake`](#occluderbake) |
| UI and text | [`ui.layout`](#uilayout), [`ui.theme`](#uitheme), [`ui.press`](#uipress), [`ui.popup`](#uipopup), [`ui.select`](#uiselect), [`ui.drag`](#uidrag), [`ui.tooltip`](#uitooltip), [`ui.focus`](#uifocus), [`ui.search`](#uisearch), [`ui.clipboard`](#uiclipboard), [`text.measure`](#textmeasure) |
| Input | [`input.inject`](#inputinject), [`input.state`](#inputstate), [`input.actions`](#inputactions) |
| Audio | [`audio.state`](#audiostate), [`audio.capture`](#audiocapture), [`audio.analyze`](#audioanalyze), [`audio.play`](#audioplay), [`audio.stop`](#audiostop), [`audio.set_bus`](#audioset_bus), [`audio.spectrum`](#audiospectrum), [`audio.record`](#audiorecord), [`audio.input`](#audioinput) |
| Soundtrack | [`score.import`](#scoreimport), [`score.export`](#scoreexport), [`soundtrack.render`](#soundtrackrender), [`soundtrack.plan`](#soundtrackplan), [`soundtrack.arrange`](#soundtrackarrange), [`soundtrack.check`](#soundtrackcheck) |
| Services | [`services.state`](#servicesstate), [`services.test`](#servicestest) |
| Storyboard | [`storyboard.query`](#storyboardquery), [`storyboard.brief`](#storyboardbrief), [`storyboard.check`](#storyboardcheck), [`storyboard.review`](#storyboardreview), [`storyboard.edit`](#storyboardedit) |
| Components | [`component.set`](#componentset), [`component.remove`](#componentremove) |
| Query | [`world.query`](#worldquery) |
| Snapshots | [`world.snapshot`](#worldsnapshot), [`world.diff`](#worlddiff), [`world.restore`](#worldrestore) |
| Run | [`step`](#step), [`run.until`](#rununtil), [`play`](#play), [`pause`](#pause) |
| Replays | [`replay.record`](#replayrecord), [`replay.stop`](#replaystop), [`replay.run`](#replayrun), [`replay.seek`](#replayseek) |
| Tests | [`test.run`](#testrun), [`playtest.run`](#playtestrun) |
| History | [`undo`](#undo), [`redo`](#redo), [`history.get`](#historyget) |
| Several at once | [`batch`](#batch) |
| Observe | [`events.subscribe`](#eventssubscribe), [`events.read`](#eventsread), [`events.unsubscribe`](#eventsunsubscribe), [`log.tail`](#logtail), [`frame.capture`](#framecapture), [`frame.summary`](#framesummary), [`canvas.convert`](#canvasconvert) |
| Display | [`display.list`](#displaylist), [`display.set`](#displayset), [`perf.stats`](#perfstats), [`debug.overlay`](#debugoverlay) |
| Import | [`project.import`](#projectimport) |
| Export | [`export.run`](#exportrun), [`texture.forms`](#textureforms), [`sdk.check`](#sdkcheck), [`sdk.add`](#sdkadd), [`sdk.lock`](#sdklock) |
| Gameplay code | [`gameplay.build`](#gameplaybuild), [`build.check`](#buildcheck), [`api.search`](#apisearch) |
| AI | [`ai.providers`](#aiproviders), [`ai.complete`](#aicomplete) |
| Asset library | [`asset.search`](#assetsearch), [`asset.generate`](#assetgenerate), [`asset.refine`](#assetrefine), [`asset.info`](#assetinfo), [`asset.use`](#assetuse), [`asset.add`](#assetadd), [`asset.feedback`](#assetfeedback), [`asset.check`](#assetcheck), [`asset.style`](#assetstyle), [`asset.sync`](#assetsync) |


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
{"capabilities":["read","edit","run","filesystem","build","network"],"commands":["animation.eval","asset.list","batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","events.read","events.subscribe","events.unsubscribe","export.run","history.get","light.sample","log.tail","material.eval","particles.eval","physics.contacts","physics.gravity","physics.overlap","physics.raycast","prefab.save","redo","run.until","scene.get","scene.new","scene.open","scene.save","schema.get","step","storyboard.brief","storyboard.check","storyboard.edit","storyboard.query","text.measure","tilemap.get","tilemap.paint","tilemap.set","ui.layout","ui.theme","undo","world.diff","world.query","world.restore","world.snapshot"],"components":[{"defaults":{"children":[],"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0],"skew":0.0,"top_level":false},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down. The entities in children are placed in its space: they move, turn and scale with it, and draw after it.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"},{"defaults":{"ids":[]},"doc":"The storyboard entries this entity stands for.","name":"StoryRef"},{"defaults":{"centered":true,"flip_h":false,"flip_v":false,"frame":0,"hframes":1,"light_mask":1,"material":"","material_params":{},"modulate":[1.0,1.0,1.0,1.0],"normal_texture":"","offset":[0.0,0.0],"region":[0.0,0.0,0.0,0.0],"specular_color":[1.0,1.0,1.0,1.0],"specular_shininess":1.0,"specular_texture":"","texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_as_relative":true,"z_index":0},"doc":"A texture drawn at the entity's Transform2D. Sprites draw in order of z_index, then in the order their entities were made.","name":"Sprite2D"},{"defaults":{"animation":"default","frame":0,"frame_progress":0.0,"playing":true,"speed_scale":1.0,"sprite_frames":""},"doc":"Plays an animation from a .frames.json file on the entity's Sprite2D, which it adds if missing. Emits animation_finished when an animation that doesn't loop ends.","name":"AnimatedSprite2D"},{"defaults":{"animation":"","backwards":false,"library":"","playing":true,"speed_scale":1.0,"targets":{},"time":0.0},"doc":"Plays a keyframe animation from a .anim.json file on the entity's components, and on the entities its targets name. Emits animation_finished when an animation that doesn't loop ends, and the events of its event tracks.","name":"AnimationPlayer"},{"defaults":{"collision_enabled":true,"legend":{},"light_mask":1,"modulate":[1.0,1.0,1.0,1.0],"occlusion_enabled":true,"origin":[0,0],"rows":[],"texture_filter":"inherit","tile_set":"","visible":true,"z_as_relative":true,"z_index":0},"doc":"A grid of tiles from a .tileset.json file, drawn at the entity's Transform2D if it has one. Cells are text: rows of legend keys, \".\" for empty. Edit them with tilemap.paint and tilemap.set.","name":"TileMapLayer"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"offset":[0.0,0.0],"range_item_cull_mask":1,"range_layer_max":0,"range_layer_min":0,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1,"texture":"","texture_scale":1.0},"doc":"A light shining from the entity's Transform2D through a texture. It lights sprites and tile map layers whose light_mask shares a bit with range_item_cull_mask, in canvases its layer range holds; with shadow_enabled, LightOccluder2D polygons and tiles' occluders cast shadows from it.","name":"PointLight2D"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"max_distance":10000.0,"range_item_cull_mask":1,"range_layer_max":0,"range_layer_min":0,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1},"doc":"Light from one direction over everything, such as the sun. It shines down the screen, turned by the entity's Transform2D rotation. Masks, ranges and shadows work as PointLight2D's.","name":"DirectionalLight2D"},{"defaults":{"closed":true,"cull_mode":"disabled","occluder_light_mask":1,"polygon":[]},"doc":"A polygon that casts shadows from lights with shadow_enabled, placed by the entity's Transform2D.","name":"LightOccluder2D"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Tints everything lit in its canvas: the light the world, or the CanvasLayer it is in, has where no light reaches. One per canvas.","name":"CanvasModulate"},{"defaults":{"a":[0.0,0.0],"b":[0.0,10.0],"disabled":false,"height":30.0,"one_way_collision":false,"one_way_collision_margin":1.0,"points":[],"position":[0.0,0.0],"radius":10.0,"rotation":0.0,"shape":"rectangle","size":[20.0,20.0]},"doc":"A shape of the entity's physics body or area, moved by position and rotation and scaled by its Transform2D. On an entity with no body or area, a shape of its parent's.","name":"CollisionShape2D"},{"defaults":{"build_mode":"solids","disabled":false,"one_way_collision":false,"one_way_collision_margin":1.0,"polygon":[]},"doc":"A polygon shape of the entity's physics body or area (its parent's, on an entity with neither): any simple polygon, filled or only its edges.","name":"CollisionPolygon2D"},{"defaults":{"bounce":0.0,"collision_layer":1,"collision_mask":1,"constant_angular_velocity":0.0,"constant_linear_velocity":[0.0,0.0],"friction":1.0},"doc":"A physics body that doesn't move on its own, such as a wall. Changing its Transform2D moves it at once.","name":"StaticBody2D"},{"defaults":{"bounce":0.0,"collision_layer":1,"collision_mask":1,"constant_angular_velocity":0.0,"constant_linear_velocity":[0.0,0.0],"friction":1.0,"sync_to_physics":true},"doc":"A physics body that goes where its Transform2D says, such as a moving platform, pushing bodies in its way.","name":"AnimatableBody2D"},{"defaults":{"angular_damp":0.0,"angular_damp_mode":"combine","angular_velocity":0.0,"bounce":0.0,"can_sleep":true,"collision_layer":1,"collision_mask":1,"constant_force":[0.0,0.0],"constant_torque":0.0,"contact_monitor":false,"continuous_cd":"disabled","freeze":false,"freeze_mode":"static","friction":1.0,"gravity_scale":1.0,"inertia":0.0,"linear_damp":0.0,"linear_damp_mode":"combine","linear_velocity":[0.0,0.0],"lock_rotation":false,"mass":1.0,"sleeping":false},"doc":"A physics body moved by gravity, collisions, forces and joints. The physics step writes its Transform2D, velocities and sleeping.","name":"RigidBody2D"},{"defaults":{"collision_layer":1,"collision_mask":1,"floor_block_on_wall":true,"floor_constant_speed":false,"floor_max_angle":0.785398,"floor_normal":[0.0,0.0],"floor_snap_length":1.0,"floor_stop_on_slope":true,"max_slides":4,"motion_mode":"grounded","on_ceiling":false,"on_floor":false,"on_wall":false,"platform_floor_layers":4294967295,"platform_on_leave":"add_velocity","platform_velocity":[0.0,0.0],"platform_wall_layers":0,"safe_margin":0.08,"slide_on_ceiling":true,"up_direction":[0.0,-1.0],"velocity":[0.0,0.0],"wall_min_slide_angle":0.261799,"wall_normal":[0.0,0.0]},"doc":"A physics body moved by setting its velocity: each tick it slides along what it hits, as Godot's move_and_slide, and reports floors, walls and ceilings.","name":"CharacterBody2D"},{"defaults":{"angular_damp":1.0,"angular_damp_space_override":"disabled","collision_layer":1,"collision_mask":1,"gravity":980.0,"gravity_direction":[0.0,1.0],"gravity_point":false,"gravity_point_center":[0.0,1.0],"gravity_point_unit_distance":0.0,"gravity_space_override":"disabled","linear_damp":0.1,"linear_damp_space_override":"disabled","monitorable":true,"monitoring":true,"priority":0},"doc":"A region that emits body_entered, body_exited, area_entered and area_exited events, and can change the gravity and damping of rigid bodies in it. It goes where its Transform2D says.","name":"Area2D"},{"defaults":{"angular_limit_enabled":false,"angular_limit_lower":0.0,"angular_limit_upper":0.0,"disable_collision":true,"motor_enabled":false,"motor_target_velocity":0.0,"node_a":null,"node_b":null},"doc":"Pins two bodies together at the entity's Transform2D, letting them turn.","name":"PinJoint2D"},{"defaults":{"damping":1.0,"disable_collision":true,"length":50.0,"node_a":null,"node_b":null,"rest_length":0.0,"stiffness":20.0},"doc":"A spring from node_a at the entity's Transform2D to node_b, length pixels along the entity's y axis.","name":"DampedSpringJoint2D"},{"defaults":{"disable_collision":true,"initial_offset":25.0,"length":50.0,"node_a":null,"node_b":null},"doc":"Keeps node_b on a groove of node_a's, from the entity's Transform2D length pixels along its y axis; node_b slides along it and turns freely.","name":"GrooveJoint2D"},{"defaults":{"amount":8,"angle_curve":[],"angle_max":0.0,"angle_min":0.0,"angular_velocity_curve":[],"angular_velocity_max":0.0,"angular_velocity_min":0.0,"anim_loop":false,"anim_offset_curve":[],"anim_offset_max":0.0,"anim_offset_min":0.0,"anim_speed_curve":[],"anim_speed_max":0.0,"anim_speed_min":0.0,"color":[1.0,1.0,1.0,1.0],"color_initial_ramp":[],"color_ramp":[],"damping_curve":[],"damping_max":0.0,"damping_min":0.0,"direction":[1.0,0.0],"draw_order":"index","emission_colors":[],"emission_normals":[],"emission_points":[],"emission_rect_extents":[1.0,1.0],"emission_ring_inner_radius":0.0,"emission_ring_radius":1.0,"emission_shape":"point","emission_sphere_radius":1.0,"emitting":true,"explosiveness":0.0,"gravity":[0.0,980.0],"hframes":1,"hue_variation_curve":[],"hue_variation_max":0.0,"hue_variation_min":0.0,"initial_velocity_max":0.0,"initial_velocity_min":0.0,"lifetime":1.0,"lifetime_randomness":0.0,"light_mask":1,"linear_accel_curve":[],"linear_accel_max":0.0,"linear_accel_min":0.0,"local_coords":false,"material":"","material_params":{},"one_shot":false,"orbit_velocity_curve":[],"orbit_velocity_max":0.0,"orbit_velocity_min":0.0,"particle_flag_align_y":false,"preprocess":0.0,"radial_accel_curve":[],"radial_accel_max":0.0,"radial_accel_min":0.0,"randomness":0.0,"scale_amount_curve":[],"scale_amount_max":1.0,"scale_amount_min":1.0,"seed":0,"speed_scale":1.0,"spread":45.0,"tangential_accel_curve":[],"tangential_accel_max":0.0,"tangential_accel_min":0.0,"texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_as_relative":true,"z_index":0},"doc":"Emits particles from the entity's Transform2D, moves and draws them, as Godot's CPUParticles2D. Each takes a random value between each _min and _max when emitted; a _curve multiplies it as it ages. Emits finished when a one_shot's last particle dies.","name":"CPUParticles2D"},{"defaults":{"anchor_bottom":0.0,"anchor_left":0.0,"anchor_right":0.0,"anchor_top":0.0,"children":[],"clip_contents":false,"custom_minimum_size":[0.0,0.0],"focus_mode":"","focus_neighbor_bottom":null,"focus_neighbor_left":null,"focus_neighbor_right":null,"focus_neighbor_top":null,"focus_next":null,"focus_previous":null,"grow_horizontal":"end","grow_vertical":"end","layout_direction":"inherited","modulate":[1.0,1.0,1.0,1.0],"mouse_filter":"","offset_bottom":0.0,"offset_left":0.0,"offset_right":0.0,"offset_top":0.0,"pivot_offset":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0],"self_modulate":[1.0,1.0,1.0,1.0],"size_flags_horizontal":"fill","size_flags_stretch_ratio":1.0,"size_flags_vertical":"fill","texture_filter":"inherit","theme":"","theme_override_colors":{},"theme_override_constants":{},"theme_override_font_sizes":{},"theme_override_fonts":{},"theme_override_icons":{},"theme_override_styles":{},"theme_type_variation":"","tooltip_text":"","visible":true,"z_as_relative":true,"z_index":0},"doc":"A UI element's rect, as Godot's Control: placed in its parent's rect by anchors and offsets, or in the viewport when no Control lists it in children. Label, ColorRect and the other UI components draw in it; they lay out with these defaults when the entity has no Control. A container component places its children by size flags instead of their anchors and offsets. Controls draw above the world, or in the CanvasLayer that holds them, parents before children.","name":"Control"},{"defaults":{"autowrap_mode":"off","clip_text":false,"ellipsis_char":"…","horizontal_alignment":"left","language":"","lines_skipped":0,"max_lines_visible":-1,"text":"","text_direction":"auto","text_overrun_behavior":"no_trimming","uppercase":false,"vertical_alignment":"top","visible_characters":-1,"visible_characters_behavior":"chars_before_shaping"},"doc":"Text in the control's rect, as Godot's Label. Theme items: font, font_size, font_color, font_outline_color, outline_size, font_shadow_color, shadow_offset_x, shadow_offset_y, shadow_outline_size, line_spacing and paragraph_spacing.","name":"Label"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Fills the control's rect with one color, as Godot's ColorRect.","name":"ColorRect"},{"defaults":{},"doc":"Draws the theme's panel style box in the control's rect, as Godot's Panel.","name":"Panel"},{"defaults":{"expand_mode":"keep_size","flip_h":false,"flip_v":false,"stretch_mode":"scale","texture":""},"doc":"Draws a texture in the control's rect, as Godot's TextureRect.","name":"TextureRect"},{"defaults":{"axis_stretch_horizontal":"stretch","axis_stretch_vertical":"stretch","draw_center":true,"patch_margin_bottom":0,"patch_margin_left":0,"patch_margin_right":0,"patch_margin_top":0,"region_rect":[0.0,0.0,0.0,0.0],"texture":""},"doc":"Draws a texture cut into nine parts in the control's rect, as Godot's NinePatchRect: the corners keep their size, the sides stretch along and the middle both ways. Its minimum size is its margins.","name":"NinePatchRect"},{"defaults":{"allow_greater":false,"allow_lesser":false,"fill_mode":"begin_to_end","max_value":100.0,"min_value":0.0,"show_percentage":true,"step":0.01,"value":0.0},"doc":"A bar filled to show value between min_value and max_value, with the percentage on it, as Godot's ProgressBar. Theme items: background, fill, font, font_size, font_color, font_outline_color and outline_size.","name":"ProgressBar"},{"defaults":{"alignment":"begin","vertical":false},"doc":"Lays out its children in a row, or a column when vertical, as Godot's HBoxContainer and VBoxContainer: each at its minimum size along it, plus a share of the space left for those that expand. Theme constant: separation.","name":"BoxContainer"},{"defaults":{"columns":1},"doc":"Lays out its children in a grid, as Godot's GridContainer: left to right, a new row every columns children. Theme constants: h_separation and v_separation.","name":"GridContainer"},{"defaults":{},"doc":"Fits its children in its rect less margins, as Godot's MarginContainer. Theme constants: margin_left, margin_top, margin_right and margin_bottom.","name":"MarginContainer"},{"defaults":{"use_top_left":false},"doc":"Keeps its children at their minimum sizes in its center, as Godot's CenterContainer.","name":"CenterContainer"},{"defaults":{"alignment":"begin","last_wrap_alignment":"inherit","reverse_fill":false,"vertical":false},"doc":"Lays out its children in rows that wrap to its width, or columns that wrap to its height when vertical, as Godot's HFlowContainer and VFlowContainer. Theme constants: h_separation and v_separation.","name":"FlowContainer"},{"defaults":{},"doc":"Draws the theme's panel style box and fits its children inside the style's margins, as Godot's PanelContainer.","name":"PanelContainer"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":["input","gui","animation_player","animation_tree","physics2d","animated_sprite_2d","camera_2d","cpu_particles_2d","audio"]}
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

A [threaded system](../concepts.md#world-entities-and-components) also has `"threaded": true`,
its `stage` and the `reads` and `writes` it declared. Systems with the same stage run at the same
time:

```json
{"name":"cpu_particles_2d","doc":"Emits, moves and ages each CPUParticles2D's particles.","priority":1100,"reads":["Transform2D"],"stage":8,"threaded":true,"writes":["CPUParticles2D"]}
```

### schema.get

Returns a JSON Schema: for scene, prefab, [`.frames.json`](scene-format.md#sprite-frames),
[`.material.json`](scene-format.md#materials), [`.anim.json`](scene-format.md#animations),
[`.animtree.json`](scene-format.md#animation-trees),
[`.process_material.json`](scene-format.md#process-materials),
[`.tileset.json`](scene-format.md#tile-sets), [`.font.json`](scene-format.md#fonts),
[`.theme.json`](scene-format.md#themes), [`.audio.json`](scene-format.md#audio-streams),
[`.bus_layout.json`](scene-format.md#bus-layouts), [`.replay.json`](scene-format.md#replays),
[`.test.json`](scene-format.md#tests),
[`.navmesh.json` or `.navpoly.json`](scene-format.md#navigation-meshes-and-polygons),
[`.meshlib.json`](scene-format.md#mesh-libraries) or storyboard files, one component or one
command.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | `"scene"`, `"prefab"`, `"frames"`, `"material"`, `"animation"`, `"animtree"`, `"process_material"`, `"tileset"`, `"font"`, `"theme"`, `"audio"`, `"bus_layout"`, `"replay"`, `"test"`, `"navmesh"`, `"navpoly"`, `"meshlib"`, `"lightmap"`, `"voxel_gi"`, `"model"`, `"storyboard"`, `"component"` or `"command"` | required | Which schema |
| `name` | string | | Component or command name, for `component` and `command` |

Needs: `read`.

```json
{"kind": "command", "name": "step"}
```

```json
{"additionalProperties":false,"properties":{"tick_seconds":{"default":0.016666666666666666,"description":"Seconds per tick.","exclusiveMinimum":0,"maximum":1,"type":"number"},"ticks":{"default":1,"description":"Ticks to run.","maximum":100000,"minimum":1,"type":"integer"}},"type":"object"}
```

### project.map

Where things are in the project, so an agent finds code and content without reading every file.
It is made from the files on every call, so it can't go stale. Scenes and prefabs list their
entities' components (with those their prefabs give), the prefabs they extend, other project
files they name and the storyboard entries they stand for (their `StoryRef` IDs and entries
whose `links` name them). The game's C++ sources (project.json's `gameplay.sources`) are read as
text, not compiled: each lists its types and functions by line, the components it names, the
systems it adds with `add_system`, the events it emits with `emit` or compares an event's `kind`
with, the input actions it reads, the project files it names and includes, and the entries
linked to it. Across them, `components`, `systems`, `events` and `actions` say where each is
declared and used. Files in `build/` and in folders starting with `.` are left out; the open
scene is read as it is now.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `find` | string | | Only names containing this, ignoring case, and the files that name them or whose paths contain it; a file kept by a name lists only the names that match |
| `paths` | array of paths | | Only files in these files or folders, or that extend, use or include something in them, and what those name |
| `sections` | array | all | Only these parts: `scenes`, `prefabs`, `sources`, `components`, `systems`, `events`, `actions` |

Needs: `read`. A path that is not in the project fails with `-32001` and the closest one.

```json
{"find": "coin_collected"}
```

```json
{"events":{"coin_collected":{"emitted":["src/coins.cpp:37"],"read":["src/runner.cpp:13"]}},"project":{"main_scene":"scenes/level.scene.json","name":"Coin Run","sources":["src"]},"sources":{"src/coins.cpp":{"events":["coin_collected"],"lines":52},"src/runner.cpp":{"events":["coin_collected"],"lines":19}}}
```

What a change to a header touches, and the systems in those files; with no params, the whole
map ([examples/project-map](../../examples/project-map/) has one):

```json
{"paths": ["src/runner.hpp"], "sections": ["sources", "systems"]}
```

```json
{"project":{"main_scene":"scenes/level.scene.json","name":"Coin Run","sources":["src"]},"sources":{"src/coins.cpp":{"components":["Coin","Wallet"],"events":["area_entered","coin_collected"],"functions":{"collect":26},"includes":["src/runner.hpp"],"lines":52,"systems":["collect","move_runner"],"types":{"Coin":12,"Wallet":18},"uses":["prefabs/coin.prefab.json"]},"src/runner.cpp":{"actions":["jump"],"events":["cheered","coin_collected","jumped"],"functions":{"move_runner":8},"includes":["src/runner.hpp"],"lines":19,"story":["character/runner"]},"src/runner.hpp":{"functions":{"move_runner":8},"lines":10}},"systems":[{"components":["Coin","Wallet"],"loaded":false,"name":"collect","priority":960,"source":"src/coins.cpp:26"},{"loaded":false,"name":"move_runner","source":"src/runner.cpp:8"}]}
```

Without a filter, `systems` lists every system in the order it runs, with its priority; the
game's own add where their function is defined, the components its body names, and their `doc`.
`"loaded": false` marks one the sources add that isn't running, because the code isn't built or
failed to load. `actions` lists those project.json defines even when no code reads them. A
component's `declared` is its `SLASH_COMPONENT` line. Lists that would be empty are left out.

Because the sources are read as text, a macro that declares something is read as it looks, and
code in a branch the preprocessor drops is read too. Names that are built at run time, such as
an event kind put together from parts, don't appear.

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

### entity.place

Places 3D entities with constraints instead of exact coordinates, as agents assemble a level of
library assets: at a point or along a path, dropped onto what is below, and moved aside from
what is in the way. Each entity is dropped as Godot's editor snaps a node to the floor (Snap
Object to Floor): a ray from a meter above the bottom of its bounds, straight down at most 500
meters, puts the bottom of its bounds on the first surface it meets, which can be another prop.
Then, when its bounds overlap anything but what it stands on, it is moved to the nearest spot
that is free, trying rings of spots around where it was asked to go, each half its size further
out (16 rings), dropping it again at each. Surfaces and what is in the way are what a
[navigation bake](#navigationbake) reads: meshes, CSG roots, grid maps, terrains and static and
animatable bodies' shapes, kept apart by their bounding boxes. Entities are placed in order,
each kept apart from those before it; an entity's children move with it. Each one's
`Transform3D` position (and, with `rotate`, rotation) is set in its parent's space.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `entities` | array | required | Entity IDs, or `{"id", "at"}` with `at` the point in the world its origin goes to before it is dropped (default where it is); up to 256 |
| `along` | object | none | Spread the entities along a `Path3D`'s curve instead: `path` (an entity ID), `from` (meters along it to the first, default 0), `spacing` (meters between each; default spread evenly from end to end, or around a closed path) and `rotate` (turn each about y so its -z faces along the path, default `false`) |
| `ground` | boolean | `true` | Drop each onto what is below it |
| `on` | array of strings | anything | Entities that may be stood on, with those under them; everything else is only in the way |
| `apart` | boolean | `true` | Move each aside until its bounds overlap nothing but what it stands on |
| `margin` | number | `0` | Meters kept clear across the ground between bounds |

Needs: `edit`. Takes `dry_run`.

Each placed entity comes back with its `position` in the world, what it stands `on`, how far it
was `moved` aside and, with `rotate`, its `rotation`. `problems` lists entities that had nothing
below them within 500 meters, found no free spot (they stay where they were asked to go) or
would be past the end of an open path (they stay where they were). From
[examples/scene-assembly](../../examples/scene-assembly/README.md):

```json
{"entities": [{"id": "crate_a", "at": [0, 3, 0]}, {"id": "crate_b", "at": [0, 3, 0]}]}
```

```json
{"placed":[{"id":"crate_a","on":"ground","position":[0,0.4,0]},{"id":"crate_b","moved":0.8,"on":"ground","position":[0.8,0.4,0]}]}
```

```json
{"entities": ["post_1", "post_2", "post_3", "post_4", "post_5"],
 "along": {"path": "fence", "rotate": true}, "margin": 0.2}
```

```json
{"placed":[{"id":"post_1","on":"ground","position":[-6,0.6,4],"rotation":[0,0,0]},...,{"id":"post_5","on":"ground","position":[0,0.6,-4],"rotation":[0,-1.5708,0]}]}
```

```json
{"placed":[{"id":"kite","position":[30,2,0]}],"problems":[{"id":"kite","message":"nothing is below it to stand on within 500 m"}]}
```

An entity listed under another being placed fails with `-32602`, since that one moves it:

```json
{"code":-32602,"data":{"path":"/entities/1"},"message":"/entities/1: \"crate\" is under \"cart\", which moves it; place one of them"}
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
that extend a prefab; `bytes` is what a loaded asset holds in memory: a texture's decoded
pixels, a `.wav` file's samples, a JSON file's parsed values, and the file's size for types that
don't count their own. Prefab files, which entities keep parsed, have none. Files reload on their
own when they change (see [hot reload](../concepts.md#assets)), so this command is for checking
what is loaded and whether an edit was picked up.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `type` | string | all | Only assets of this type: `prefab`, `json`, `text` or one a game added |

Needs: `read`.

```json
{}
```

```json
{"assets":[{"bytes":120,"path":"data/tuning.json","refs":1,"type":"json","version":2},{"path":"prefabs/enemy.prefab.json","refs":3,"type":"prefab","version":1}]}
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

Returns the values an [animation](scene-format.md#animations)'s value and node tracks set at the
times given, as `AnimationPlayer` sets them, without playing it: a check that needs no stepping. Given
an entity, it uses its `AnimationPlayer`'s library and, unless `animation` names another, its
animation.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | | An entity with an `AnimationPlayer`. Give `id` or `path` |
| `path` | string | | A `.anim.json` file |
| `animation` | string | the entity's | The animation's name; required with `path` |
| `times` | array of numbers | required | Seconds into the animation, 1 to 100 of them, each from 0 to its length |
| `tracks` | array of strings | all | The value tracks to return, by path, or `"target:path"` for a track with a target, and the node tracks, by node |

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
[Node tracks](scene-format.md#node-tracks) go under `nodes`, by node, then property, one
`[x, y, z]` per time, rotations as Euler angles:

```json
{"length":1,"loop":"linear","tracks":{},"nodes":{"Elbow":{"rotation":[[0,0,0],[0,0,0.6],[0,0,1.2]]}}}
```

### animation.tree

Returns an [animation tree](scene-format.md#animation-trees)'s parameters: those games set,
each with its `default` and `doc`. Given an entity, each also has the `value` its
`AnimationTree` plays with, and the reply adds `state`, what the tree's nodes keep as they play
(each node's timings left out; a state machine's `playback` has its `current` state), `playing`,
the animations its last tick played with their times and weights, and `invalid` when it played
nothing.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | | An entity with an `AnimationTree`. Give `id` or `path` |
| `path` | string | | A `.animtree.json` file |

Needs: `read`.

```json
{"id": "arm"}
```

```json
{"parameters":{"shot/request":{"default":"none","doc":"\"fire\" plays the shot, \"abort\" stops it, \"fade_out\" fades it out; the tree sets it back to \"none\".","value":"none"},"speed/scale":{"default":1,"doc":"How fast its input plays; negative plays backward.","value":1}},"playing":[{"animation":"Wave-loop","time":0.5833,"weight":1},{"animation":"Point","time":0.0833,"weight":0.3333}],"state":{"point/backward":false,"shot/active":true,"shot/fade_in_remaining":0.1167,"shot/fade_out_remaining":0,"shot/internal_active":true,"shot/time_to_restart":-1,"wave/backward":false}}
```

Numbers are rounded to four decimals. An entity with no `AnimationTree`, or whose `tree_root` is
empty or doesn't load, fails at `/id`.

### animation.set_parameters

Sets parameters of an entity's `AnimationTree`, as Godot's `set("parameters/...")`, and returns
those that changed. Each value is checked against its node first, so a bad one changes nothing:
a number for an amount or a scale, a OneShot `request` of `"none"`, `"fire"`, `"abort"` or
`"fade_out"`, a Transition `transition_request` naming one of its inputs, a state machine's
`travel` or `start` naming one of its states (`"group/state"` in a grouped one), a 2D blend
position of `[x, y]`. `null` resets a parameter to its default.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with an `AnimationTree` |
| `parameters` | object | required | Values by parameter, at least one |

Needs: `edit`.

```json
{"id": "arm", "parameters": {"speed/scale": 2, "shot/request": "fire"}}
```

```json
{"changed":{"shot/request":"fire","speed/scale":2},"id":"arm"}
```

A parameter the tree hasn't fails with `-32001` at `/parameters/<name>` with "did you mean",
and one the tree keeps in `state` (as `shot/active`) or a value its node doesn't take with
`-32602`.

## Tile maps

These commands edit an entity's [`TileMapLayer`](scene-format.md#tile-map-layers) through its
[tile set](scene-format.md#tile-sets), and write its cells back in canonical form. Each takes
the cells as `cells`, a list of `[column, row]` (up to 4096), or `rect`,
`[column, row, width, height]` (up to 65536 cells); give one. Columns and rows are from -32768 to
32767. A layer whose tile set doesn't load, or whose legend or rows are wrong, fails with `path`
`/id` and the message names the bad value, as `"map"'s TileMapLayer/rows/2: ...`.

### model.inspect

Returns what a [model](scene-format.md#models) holds, as a `MeshInstance3D` draws it, without
drawing it: its meshes, materials, images, the nodes that place its meshes, its skins and
animations, and its bounds.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | A `.gltf`, `.glb`, `.obj`, `.fbx`, `.blend` or `.model.json` file, or a recipe's [variant](scene-format.md#variants-and-quality-tiers) after `::` |
| `id` | string | | Or an entity whose `MeshInstance3D` draws a model |
| `brief` | boolean | `false` | Only what it takes to place the model, as below |

Give `path` or `id`. Needs: `read`.

```json
{"path": "models/house.gltf"}
```

```json
{"bounds":{"max":[1.15,2.2,0.95],"min":[-1.15,0,-0.95]},"images":[],"materials":[{"name":"Plaster","standard":{"albedo_color":[0.9261,0.8756,0.7736,1],"roughness":0.9}},{"name":"Tiles","standard":{"albedo_color":[0.7977,0.3656,0.2717,1],"roughness":0.7,"vertex_color_use_as_albedo":true}},{"name":"Wood","standard":{"albedo_color":[0.4845,0.3318,0.206,1],"cull_mode":"disabled","roughness":0.8}}],"meshes":[{"materials":[0],"name":"Walls","triangles":8,"vertices":16},{"materials":[1],"name":"Roof","triangles":6,"vertices":18},{"materials":[2],"name":"Door","triangles":2,"vertices":4}],"parts":[{"mesh":0,"node":"Walls"},{"mesh":1,"node":"Roof"},{"mesh":2,"node":"Door"}]}
```

- Each mesh lists the material of each of its surfaces, by index into `materials`, or `-1` for
  Godot's default material, and counts their vertices and triangles.
  `lods`, for a mesh whose surfaces have [LODs](scene-format.md#mesh-lods), counts its
  triangles at each coarser LOD, a surface with fewer counted at its coarsest.
- Each material has the `StandardMaterial3D` fields that differ from the defaults, as the import
  sets them, and `textures`: the index into `images` its `albedo`, `metallic`, `roughness`,
  `normal` and `emission` textures sample (a glTF model's metallic and roughness are one image).
- Each image is the `path` of a file beside the model, or the `size` of one inside it.
- `parts` are the meshes the model draws: each with the scene node that places it, in tree order
  (an OBJ model's objects, in the file's order), and the index into `skins` of the skin that
  deforms it, if one does.
- `skins` and `animations` are there only for a model that has them: each skin's name and how
  many joints it has, and each [animation](scene-format.md#skeletons-and-model-animations)'s
  name, length in seconds, `loop` if it loops and how many tracks it has.
  [`skeleton.get`](#skeletonget) lists the bones.
- `bounds` are the corners of the box the parts fill at rest, in the model's own meters.
- `collision` is there only for a model with [collision](scene-format.md#model-collision): each
  shape's `name`, `shape` (`"convex"` or `"trimesh"`), and how many `points` and `triangles` it
  has.

With `brief`, it describes the model in a few numbers and names instead, so an agent can place
it, and things on it, without drawing it:

```json
{"path": "props/cabinet.model.json", "brief": true}
```

```json
{"materials":["pine","brass"],"parts":[{"center":[0,0.5,0],"materials":["pine"],"name":"body","size":[0.8,1,0.45],"triangles":28},{"center":[0,0.5,0.26],"materials":["pine","brass"],"name":"door","size":[0.8,1,0.08],"triangles":80}],"pivot":[0.5,0,0.4286],"points":[{"name":"handle","part":"door","position":[0.33,0.5,0.3]},{"name":"top","position":[0,1,0]}],"size":[0.8,1,0.525],"triangles":108}
```

- `size` is the bounds' size in meters, and `pivot` where the model's origin is in them on each
  axis, from 0 at the low side to 1 at the high one (0.5 on an axis the model is flat in):
  `[0.5, 0, 0.5]` stands on its origin, centered.
- `materials` are the materials' names (`""` for one without), and each part's `materials` those
  its surfaces use.
- Each part is the node placing a mesh, with the `center` and `size` of what it draws at rest, in
  the model's space, and its triangles.
- `points` are the model's attachment points, as Godot's `Marker3D` in an imported scene: nodes
  that place no mesh, have no children and are no skin's joint, such as a glTF file's empty nodes
  or a [recipe's points](scene-format.md#model-recipes). Each has its `position` and `rotation`
  (left out when zero) at rest in the model's space, and the `part` above it that it moves with.
  A [`BoneAttachment3D`](scene-format.md#bone-attachments)'s `bone_name` follows one, and a
  `Skeleton3D` turns a part by its name.
- `animations` are the names of its animations, for a model with any.
- `collision` lists its [collision](scene-format.md#model-collision) shapes, as the full form
  does, for a model with any. Collision nodes aren't `points`.

A model that doesn't load is an error at `/path` (or `/id`) saying why, as
[`scene validate`](cli.md#scene-validate) reports it.

### model.cleanup

Makes a generated or imported [model](scene-format.md#models) game-ready, as an artist's last
pass before it goes into a game, and writes it as one `.glb` file holding its buffer and images, as
Godot's glTF export writes one. The steps run in this order:

1. `up` turns the axis the file calls up to y, Godot's up.
2. `delight` takes the light baked into its albedo textures out of them, as Unity's
   De-Lighting tool does for scans: each texel the surfaces cover is placed on the model, the sky
   each place sees is traced against the model (16 rays, as far as half its size), the ambient
   and directional light that best explain the texels' brightness are fitted by least squares, and
   each texel is divided by that light, by `delight` of the way. A texture lit evenly, or on
   surfaces all facing one way, is left alone; an image another of a material's slots samples is
   kept, and the albedo gets a copy.
3. `triangles` simplifies each surface to its share of the budget with meshoptimizer's
   simplifier, as Godot's importer simplifies LODs, then makes the result's
   [LODs](scene-format.md#mesh-lods).
4. `scale`, or `height`, sizes it.
5. `pivot` moves it so its origin is at that point of its box.
6. With `uvs`, each surface whose vertices all share one texture coordinate (none, in effect) is
   unwrapped with xatlas, as lightmap UVs are.
7. `collision` makes what it collides as from the triangles it draws, as Godot's mesh import
   "Generate > Physics" shape types: `"trimesh"` the triangles, `"convex"` one convex hull of at
   most 32 corners, `"decompose"` convex hulls from V-HACD 4, as Godot's
   `Mesh::convex_decompose` makes them (Godot's defaults: about 10000 voxels, 32 corners a hull),
   the same hulls on every platform. It replaces the [collision](scene-format.md#model-collision)
   the model had; without it, that collision is turned, sized and moved with the model.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | The model: a `.gltf`, `.glb`, `.obj`, `.fbx`, `.blend` or `.model.json` file |
| `out` | string | `path` as `.glb` | The `.glb` file to write |
| `up` | string | `"y"` | The axis up is along in the file: `"y"`, `"-y"`, `"x"`, `"-x"`, `"z"` (most CAD tools, and Blender without glTF's conversion) or `"-z"` |
| `scale` | number | | Its size times this: `0.001` for a model in millimeters |
| `height` | number | | Or make it this many meters tall, after `up`; not with `scale` |
| `pivot` | array | | Where its origin goes in its box, 0 to 1 on each axis, as `model.inspect`'s brief `pivot`: `[0.5, 0, 0.5]` stands it on the ground, centered |
| `triangles` | integer | the storyboard's | The most triangles it may have, 0 for no limit; left out, the tightest [budget](storyboard-format.md#checks) covering `out` that sets `triangles` |
| `uvs` | boolean | `true` | Unwrap surfaces without texture coordinates |
| `collision` | string | | Collision to make: `"trimesh"`, `"convex"` or `"decompose"` |
| `max_hulls` | integer | `16` | The most hulls `"decompose"` makes, 1 to 256; only with it |
| `delight` | number | `0` | How much of the light baked into its albedo textures to take out, 0 to 1: 1 for scans and generated models lit in their textures |

Needs: `edit`, `filesystem`. Undo takes the file back. Models with skins or animations are refused
for now, as are a budget the simplifier can't get under (`/triangles`, or `/out` for the
storyboard's) and a `height` for a model flat along y; so are `"convex"` and `"decompose"` for
a model flat in some direction, which makes no hull (`/collision`), and a texture `delight`
can't read (`/delight`).

```json
{"path": "source/boulder.model.json", "out": "props/boulder.glb", "height": 0.8, "pivot": [0.5, 0, 0.5]}
```

```json
{"budget":"budget/props","out":"props/boulder.glb","pivot":[0.5,0.0,0.5],"size":[1.3899,0.8,0.9976],"triangles":[3968,600]}
```

`triangles` are the model's before and after, counting each mesh once for every part that draws
it; `budget` is the storyboard entry the limit came from; `size` and `pivot` are the result's, as
`model.inspect`'s brief form gives them; `unwrapped` counts the surfaces given texture
coordinates, when there are any; `collision` counts the result's collision shapes by kind, as
`{"convex": 6}`, when it has any; `delit` counts the textures `delight` changed and `light` is
the direction the light it took out came from, in the result's space, when it found one; and
`unchanged` is `true` when `out` already held the same bytes.

### skeleton.get

Returns the bones of the model an entity's `MeshInstance3D` draws, and the
[animations posing them](scene-format.md#skeletons-and-model-animations): its `AnimationPlayer`'s
animation from a model, then its `Skeleton3D`, then its [IK modifiers](scene-format.md#ik-modifiers).
Bones named get their pose as it is drawn.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity whose `MeshInstance3D` draws a model |
| `bones` | array of strings | the tree | Bones to pose |

Needs: `read`.

```json
{"id": "arm", "bones": ["Lower"]}
```

```json
{"bones":[{"name":"Lower","parent":"Upper","position":[0,1,0],"rotation":[0,0,1.5708],"scale":[1,1,1],"world_position":[2,1,0]}],"clips":[{"animation":"Bend","time":1}],"model":"models/arm.gltf"}
```

- Without `bones`, `bones` is every bone with its `parent` (left out for a root), parents first,
  and no poses: the names `Skeleton3D` and animations use.
- A posed bone's `position`, `rotation` (radians, as `Transform3D`'s and `Skeleton3D`'s) and
  `scale` are in its parent's space; `world_position` is where it is in the world.
- `clips` are the animations posing the model, each with its `time`, its `weight` when an
  `AnimationTree` blends several, and, when it comes from another model, that `model`.

A bone the model hasn't fails at `/bones/<n>` with "did you mean", and an entity that draws no
model, or a model with no nodes (an OBJ model), at `/id`.

### csg.get

Returns the mesh a [CSG root](scene-format.md#csg-shapes) makes from the shapes under it: its
size, its surfaces by material and what couldn't be made.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | A CSG root: a CSG shape whose parent isn't one |

Needs: `read`.

```json
{"id": "house"}
```

```json
{"aabb":{"max":[1.8,2.5,1.7],"min":[-1.8,-1.2,-1.7]},"surfaces":[{"material":{"albedo_color":[0.9,0.85,0.7,1]},"triangles":32},{"material":{"albedo_color":[0.45,0.3,0.2,1]},"triangles":34},{"triangles":36},{"material":{"albedo_color":[0.7,0.2,0.15,1]},"triangles":14}],"triangles":116,"vertices":196,"volume":15.6585}
```

- `aabb` and `volume` (cubic meters) are in the root's own space, as Godot's `get_aabb`; an
  empty result has a zero box and volume. A subtraction that cut nothing leaves the volume as
  it was.
- `surfaces` are in the order their materials first appear. Each has its triangles and the
  `material` it draws with: the `StandardMaterial3D` fields that differ from the defaults, or
  `model` and `model_material` for a `CSGMesh3D` model's own material, or neither for Godot's
  default material.
- `problems` are there only when a shape under the root couldn't be made or the result is empty,
  each with its entity's `id`, the `path` of the bad field and a `message`, as
  [`scene validate`](cli.md#scene-validate) reports them.

A shape under a root fails at `/id`, naming the root to ask for instead, and so does an entity
with no CSG shape.

### path.get

Returns a [`Path2D`'s or `Path3D`'s](scene-format.md#paths) curve, baked as Godot bakes it: its
length, the position and rotation at offsets along it and the point of it nearest to another,
so an agent can place things along a route without a follower.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `Path2D` or `Path3D` |
| `offsets` | array of numbers | none | Distances along the curve to sample, kept on it |
| `ratios` | array of numbers | none | Fractions of its length to sample, after `offsets` |
| `closest_to` | `[x, y]` or `[x, y, z]` | none | Also return the curve's nearest point to this |
| `cubic` | boolean | `false` | Cubic interpolation between baked points, as a follower's `cubic_interp` |
| `global` | boolean | `false` | Positions, rotations and `closest_to` in the world's space rather than the path's |

Needs: `read`.

```json
{"id": "road", "ratios": [0.5], "closest_to": [130, 20]}
```

```json
{"points":2,"baked_points":15,"length":50,"samples":[{"offset":25,"position":[0,25],"rotation":1.5708}],"closest":{"offset":20,"position":[0,20]}}
```

- `points` is how many points the path has and `baked_points` how many it was baked into.
- Each sample has its `offset` and `position`. A `Path2D`'s `rotation` is the angle in radians
  of the way it goes, as a `PathFollow2D` that `rotates` turns. A `Path3D`'s is the Euler angles
  of Godot's `sample_baked_with_rotation` with the tilt, as a `Transform3D`'s `rotation`, with
  its `up` vector and `tilt`.
- `closest` has the nearest point's `offset` along the curve and its `position`.

An entity with neither path fails at `/id`, and a `closest_to` with the wrong number of
coordinates at `/closest_to`.

### gridmap.set

Places one item of a [`GridMap`'s](scene-format.md#grid-maps) mesh library in cells, turned by
an orientation, or erases them with `null`, as Godot's `set_cell_item`. Writes the map in
canonical form and returns how many cells changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `GridMap` |
| `cells` | array of `[x, y, z]` | | The cells, up to 4096. Give `cells` or `box` |
| `box` | `[x, y, z, width, height, depth]` | | Every cell in it, up to 65536 |
| `item` | string or `null` | required | An item of the library, or `null` to erase |
| `orientation` | integer | `0` | One of Godot's 24 orthogonal indices, `0` to `23`: `16`, `10` and `22` turn the item 90, 180 and 270 degrees about y |

Needs: `edit`. Walling off a room, from [examples/gridmap](../../examples/gridmap/):

```json
{"id": "level", "box": [2, 0, 0, 1, 1, 2], "item": "wall"}
```

```json
{"changed":2,"id":"level"}
```

An item the library doesn't have fails at `/item` with the closest name; a GridMap with no
library at `/id`; and cells past -32768 to 32767 at `/cells` or `/box`.

```json
{"code":-32001,"data":{"path":"/item"},"message":"/item: the mesh library has no item \"wal\"; did you mean \"wall\"?"}
```

### gridmap.get

Returns a [`GridMap`'s](scene-format.md#grid-maps) cells as its legend and floors of rows, the
names of its library's items and any `problems`; or, with `cells`, what each given cell holds and
its center; or, with `points`, the cells points are in and what they hold.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `GridMap` |
| `cells` | array of `[x, y, z]` | | Up to 1024 cells |
| `points` | array of `[x, y, z]` | | Up to 1024 points |
| `global` | boolean | `false` | Centers and points in the world's space rather than the GridMap's |

Needs: `read`.

```json
{"id": "level"}
```

```json
{"cells":24,"floors":[{"origin":[0,0],"rows":["ffffpp","ffffrp","ffwfff","ffwfff"],"y":0}],"items":["floor","platform","ramp","wall"],"legend":{"f":"floor","p":"platform","r":"ramp:10","w":"wall"}}
```

```json
{"id": "level", "points": [[5, 1, 1], [9, 1.5, 3]]}
```

```json
{"points":[{"cell":[2,0,0],"item":"wall"},{"cell":[4,0,1],"item":"ramp","orientation":10}]}
```

- `cells` counts the cells in use. `problems` are there only when cells can't be made, each with
  the `path` of the bad field and a `message`, as [`scene validate`](cli.md#scene-validate)
  reports them.
- Each cell or point has its `cell`, its `item` (`null` for an empty cell) and its
  `orientation` when it isn't `0`. A cell asked for by `cells` has its `center`.

### terrain.generate

Makes a [heightmap terrain's](scene-format.md#heightmap-terrains) heights from fractal Perlin
noise, as HTerrain's generator: curved, shaped into an island or a canyon and eroded, or added to
the heights it has. Heights are in the heightmap's meters, before `map_scale.y`. Writes the
heightmap, widening `height_range` to whole meters that hold the heights, and returns the lowest
and highest.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with an `HTerrain` |
| `seed` | integer | `0` | The noise's seed |
| `offset` | `[x, z]` | `[0, 0]` | Moves the noise by cells |
| `base_height` | number | `-50` | The lowest the noise goes |
| `height_range` | number | `150` | How far above `base_height` it reaches |
| `scale` | number | `100` | About how many cells one hill spans |
| `roughness` | number | `0.4` | How much each finer octave adds, `0` to `1` |
| `curve` | number | `1` | Raises heights from 0 to 1 to this power, so over 1 flattens valleys |
| `octaves` | integer | `6` | Layers of finer noise, `1` to `10` |
| `erosion_steps` | integer | `0` | Passes of erosion, wearing peaks down |
| `erosion_weight` | number | `0.5` | How much each pass wears |
| `erosion_slope_factor`, `erosion_slope_direction`, `erosion_slope_invert` | | `0`, `[0, 0]`, `false` | Erosion that runs one way, `[x, z]`, or against it |
| `dilation` | number | `0` | Grows plateaus instead of wearing peaks, `0` to `1` |
| `island_weight` | number | `0` | How much the edges fall away into an island |
| `island_sharpness` | number | `0` | How sharply its shore falls |
| `island_height_ratio` | number | `-1` | The edges' height, `-1` (lowest) to `1` (highest, a canyon) |
| `island_shape` | number | `0` | `0` round to `1` square |
| `additive` | boolean | `false` | Add the noise to the current heights |

Needs: `edit`, `filesystem`. The same seed makes the same heights on every machine. From
[examples/terrain](../../examples/terrain/):

```json
{"id": "land", "seed": 4, "base_height": -6, "height_range": 40, "scale": 50, "island_weight": 1, "island_sharpness": 0.4}
```

```json
{"files":["terrain/land.height.png"],"heights":[-6,26.6482],"id":"land"}
```

`height_range` is in the reply only when it widened. A terrain that can't be made (a map that
doesn't load, or a resolution HTerrain refuses) fails at `/id` saying why, as
[`scene validate`](cli.md#scene-validate) reports it.

### terrain.brush

Paints a [heightmap terrain](scene-format.md#heightmap-terrains) with a round brush at each point
in turn, as HTerrain's painter. Writes the heightmap, splat map or color map it changes and
returns how many cells changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with an `HTerrain` |
| `mode` | string | required | `raise` or `lower` by `amount`, `smooth`, `flatten` to `height`, `level` to the mean under the brush, `splat` a texture, `color`, `hole` cuts holes and `unhole` fills them |
| `points` | array of `[x, z]` | required | Where each dab goes, up to 1024 |
| `radius` | number | `8` | From the middle to the edge |
| `falloff` | number | `0.5` | The outer part that eases to nothing, `0` to `1` |
| `opacity` | number | `1` | How strong each dab is, `0` to `1` |
| `amount` | number | `1` | `raise` and `lower`: meters at the middle |
| `height` | number | `0` | `flatten`: the height to go to |
| `texture` | integer | `0` | `splat`: which texture, `0` to `3` |
| `color` | `[r, g, b]` | `[1, 1, 1]` | `color`: from `0` to `1` |
| `slope` | `[min, max]` | `[0, 90]` | `splat` and `color`: only where the ground slopes between these degrees, easing off past them as HTerrain's slope limit does |
| `global` | boolean | `false` | Points and lengths in the world's space rather than the terrain's entity's, for a terrain not tilted |

Needs: `edit`, `filesystem`. Leveling a plot at 5 meters:

```json
{"id": "land", "mode": "flatten", "points": [[40, 80], [48, 84]], "radius": 10, "height": 5}
```

```json
{"changed":128,"files":["terrain/land.height.png"],"heights":[-5.9998,26.6484],"id":"land"}
```

- Points and lengths are in the entity's space, so `map_scale` stretches them into cells.
- A height change returns the lowest and highest `heights` and widens `height_range` as
  [`terrain.generate`](#terraingenerate) does; heights are kept to the heightmap's 65536 steps.
- A dab that changes nothing returns `"unchanged": true` and writes nothing.

A misspelled `mode` fails at `/mode` with the modes it takes, and an entity with no `HTerrain`
at `/id`.

### terrain.get

Returns a [heightmap terrain's](scene-format.md#heightmap-terrains) resolution, bounds, holes
and problems; or, with `points`, what the ground is at each.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with an `HTerrain` |
| `points` | array of `[x, z]` | | Up to 1024 points over the ground |
| `global` | boolean | `false` | Points and results in the world's space rather than the terrain's entity's, for a terrain not tilted |

Needs: `read`.

```json
{"id": "land"}
```

```json
{"aabb":{"max":[128,-10,128],"min":[0,-10,0]},"resolution":65}
```

```json
{"id": "land", "points": [[64, 60], [44, 82]]}
```

```json
{"points":[{"color":[0.302,0.2706,0.2392],"normal":[-0.6823,0.4357,0.587],"position":[64,17.1946,60],"splat":[0,1,0,0]},{"color":[0.1765,0.3137,0.1059],"normal":[0,1,0],"position":[44,5.0004,82],"splat":[0.8235,0.1765,0,0]}]}
```

- `aabb` holds the terrain's heights in the entity's space (or the world's with `global`).
  `holes` counts the cells cut, and `problems` are there only when it can't be drawn, each with
  the `path` of the bad field and a `message`, as [`scene validate`](cli.md#scene-validate)
  reports them.
- Each point has the ground's `position` (its height between cells, as HTerrain's
  `get_interpolated_height_at`) and `normal`, and, from the nearest cell, the four textures'
  `splat` weights (with more than one texture or a splat map), the `color` (with a color map),
  `hole: true` over a hole and `outside: true` past the edge, where it reads the nearest edge.

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

The same commands ask [3D physics](scene-format.md#3d-physics) when given points of three
numbers (`[x, y, z]`), a `CollisionShape3D` shape or an entity with a 3D body; their answers then
have three-number points, normals and gravity, in meters.

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
`[column, row]`, and one that hits a [`GridMap`](scene-format.md#grid-maps) gives the `cell` as
`[x, y, z]` and the `item` in it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `from` | `[x, y]` or `[x, y, z]` | required | Where the ray starts |
| `to` | `[x, y]` or `[x, y, z]` | required | Where it ends, with as many numbers as `from` |

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

In 3D, a ray across a 1-meter crate at the origin, standing 2 meters up:

```json
{"from": [0, 2, 5], "to": [0, 2, -5]}
```

```json
{"hit":{"fraction":0.45,"id":"crate","normal":[0,0,1],"point":[0,2,0.5]}}
```

Straight down onto a GridMap's ramp, from [examples/gridmap](../../examples/gridmap/):

```json
{"hit":{"cell":[4,0,1],"fraction":0.6667,"id":"level","item":"ramp","normal":[-0.7071,0.7071,0],"point":[9,1,3]}}
```

### physics.overlap

Returns the IDs of the bodies (or areas) whose shapes contain a point, or overlap a shape placed
somewhere, sorted.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `point` | `[x, y]` or `[x, y, z]` | | Find shapes containing this point. Give `point` or `shape` |
| `shape` | object | | `CollisionShape2D` fields, e.g. `{"shape": "circle", "radius": 8}`, or `CollisionShape3D` fields; fields left out take the component's defaults, so `{}` is a 20 by 20 rectangle |
| `position` | `[x, y]` or `[x, y, z]` | `[0, 0]` | Where the shape goes |
| `rotation` | number or `[x, y, z]` | `0` | The shape's rotation, in radians clockwise; in 3D, radians about each axis as `Transform3D`'s |
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

A misspelled shape name fails at `/shape/shape` with a "did you mean". A shape asks 3D physics
when `position` has three numbers, `rotation` is an array, or the shape is one only 3D has
(`box`, `sphere`, `cylinder`, `convex_polygon`, `concave_polygon`, `world_boundary`); a
`capsule` with neither is a 2D one.

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
`CharacterBody2D` or `CharacterBody3D` gets the gravity its gameplay code should add to its
velocity. A 3D body's gravity is `[x, y, z]` in meters per second squared.

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

## Navigation

Questions about the world's [navigation maps](scene-format.md#navigation), as Godot's
`NavigationServer2D` and `NavigationServer3D` queries. Points of two numbers ask the 2D map, in
pixels, and points of three the 3D one, in meters. Each command first rebuilds a map whose
regions or links changed since the last step. Numbers are rounded to four decimals. The examples
are the scene of [examples/navigation](../../examples/navigation/): a house of two rooms joined
the long way round by a corridor and the short way by a hatch, a link that costs one and a half
times as much to cross as to walk.

### navigation.path

Returns a path over the map from `from` to `to`, as Godot's `query_path`, and its length. It
starts at the point of the map closest to `from` and ends at the closest it reaches to `to`, so
a target off the map, or cut off from `from`, still gets a path; it is empty when the map is.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `from` | `[x, y]` or `[x, y, z]` | required | Where it starts |
| `to` | `[x, y]` or `[x, y, z]` | required | Where it goes, with as many numbers as `from` |
| `navigation_layers` | integer | `1` | The regions and links it may use, as bits |
| `postprocessing` | string | `"corridorfunnel"` | `"corridorfunnel"` pulls it tight, `"edgecentered"` goes through the middle of each edge, `"none"` keeps where the search first touched each polygon |
| `simplify_epsilon` | number | | Simplify the path, dropping points closer than this to the line, as Godot's `simplify_path` |
| `excluded_regions` | string array | `[]` | Region entities it may not use |
| `included_regions` | string array | `[]` | The only region entities it may use, when not empty; either leaves links out, as Godot's |
| `max_polygons` | integer | `4096` | Stop the search after this many polygons; `0` for no limit |
| `max_distance` | number | `0` | Stop the search this far from `from`; `0` for no limit |
| `return_max_length` | number | `0` | Cut the path to this length; `0` for no limit |
| `return_max_radius` | number | `0` | Cut the path where it goes this far from `from`; `0` for no limit |
| `metadata` | boolean | `false` | Also return each point's `owners` (a region or link ID) and `types` (`"region"` or `"link"`) |

Needs: `read`. Through the hatch:

```json
{"from": [1, 0, 1], "to": [10, 0, 2], "metadata": true}
```

```json
{"length":9.2361,"owners":["house","hatch","house","house"],"points":[[1,0,1],[3,0,2],[9,0,2],[10,0,2]],"types":["region","link","region","region"]}
```

With the hatch moved off layer 1, the long way round:

```json
{"length":21.3956,"points":[[1,0,1],[0.5714,0,4],[0,0,8],[4,0,8],[8,0,8],[9.3333,0,4],[10,0,2]]}
```

`from` and `to` of different sizes fail at `/to`, and an unknown region ID at its index, with a
"did you mean".

### navigation.closest

Returns the point of the map closest to a point and the region it is on, as Godot's
`map_get_closest_point` and `map_get_closest_point_owner`; in 3D also its polygon's normal,
which is not normalized, as Godot's. Regions that aren't enabled count. `point` is `null` when
the map is empty.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `point` | `[x, y]` or `[x, y, z]` | required | The point |

Needs: `read`. A point above the hatch's middle, between the rooms:

```json
{"point": [6, 1, 2]}
```

```json
{"normal":[0,16,0],"owner":"house","point":[4,0,2]}
```

### navigation.random

Returns a random point of the map's regions on `navigation_layers`, as Godot's
`map_get_random_point`: in a random polygon of a random region, or, with `uniformly`, anywhere
with the same chance. The same seed picks the same point on every machine.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `map` | string | `"3d"` if it has regions, else `"2d"` | `"2d"` or `"3d"` |
| `navigation_layers` | integer | `1` | The regions it may pick, as bits |
| `uniformly` | boolean | `false` | Any point with the same chance |
| `seed` | integer | `0` | The seed |

Needs: `read`.

```json
{"map": "2d", "uniformly": true, "seed": 7}
```

```json
{"point":[126.8636,10.6323]}
```

### navigation.map

Returns what each map is made of: its `iteration_id` (which goes up each time it is rebuilt, as
Godot's), its `regions`, `links` and `polygons` (a link's counts as one), `edge_connections`
(edges of different regions joined, shared or within the map's `edge_connection_margin`),
`free_edges` (edges joined to nothing), `merge_errors` (edges three or more polygons share, left
out when none do) and `errors`: the regions left out, each with its entity's `id` and a
`message` saying why.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `map` | string | both | `"2d"` or `"3d"` |

Needs: `read`.

```json
{}
```

```json
{"2d":{"edge_connections":0,"free_edges":6,"iteration_id":1,"links":0,"polygons":2,"regions":1},"3d":{"edge_connections":0,"free_edges":16,"iteration_id":1,"links":1,"polygons":8,"regions":1}}
```

### navigation.agent

Returns how a [`NavigationAgent2D` or `NavigationAgent3D`](scene-format.md#agents) is doing, as
Godot's `NavigationAgent` getters: its `next_position`, whether its `navigation_finished`, it
`target_reached` and its target is `target_reachable`, the straight `distance_to_target`, and its
`path` with the `path_index` of the point it is going to. An agent with `avoidance_enabled` also
returns the [`safe_velocity`](scene-format.md#avoidance) avoidance gave it last tick. Asking
brings the path up to date first, as `get_next_path_position` does.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with an agent |

Needs: `read`. The guard, before it sets off:

```json
{"id": "guard"}
```

```json
{"distance_to_target":10.2956,"navigation_finished":false,"next_position":[3,0,2],"path":[[1,0,1],[3,0,2],[9,0,2],[9.5,0,4],[10,0,6]],"path_index":1,"target_reachable":true,"target_reached":false}
```

One of the walkers in [examples/navigation-avoidance](../../examples/navigation-avoidance/), as
it passes the others:

```json
{"distance_to_target":201.4898,"navigation_finished":false,"next_position":[298.8978,280],"path":[[20,280],[87.5868,280],[101.1023,280],[114.684,280],[298.8978,280],[312.4132,280],[321.4111,280],[380,280]],"path_index":4,"safe_velocity":[78.6577,-8.9328],"target_reachable":true,"target_reached":false}
```

An unknown ID fails at `/id` with a "did you mean", and an entity with no agent fails there too.

### navigation.bake

Bakes a [region's](scene-format.md#regions) `.navmesh.json` or `.navpoly.json` file from the
geometry under it, as Godot's `bake_navigation_mesh` and `bake_navigation_polygon`, with the
[settings](scene-format.md#baking) in the file (Godot's defaults for a file that isn't there
yet). It replaces the file's `vertices` and `polygons`, keeping everything else as written, and
returns how many it wrote, how many obstructions a 2D bake cut out, how many
`obstacle_outlines` of [obstacles](scene-format.md#avoidance) with `affect_navigation_mesh` it
cut out, and `warnings` when there are any: settings that don't fit whole cells, meshes or tile
sets that didn't load and a region with nothing under it to bake. A 2D bake takes the
[tile map layers](scene-format.md#tile-navigation) under the region too: their tiles'
navigation outlines and collision polygons. A bake that comes out the same reports `"unchanged": true` and records no undo
step. Undo puts the file back as it was, removing it if the bake made it, and redo writes the
bake again; both fail if the file changed since. The map takes the new file at the next command
or tick.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `NavigationRegion3D` or `NavigationRegion2D` |

Needs: `edit` and `filesystem`. A courtyard in
[examples/navigation-baking](../../examples/navigation-baking/):

```json
{"id": "courtyard"}
```

```json
{"file":"nav/courtyard.navmesh.json","polygons":36,"vertices":34}
```

An unknown ID fails at `/id` with a "did you mean"; so do an entity with no region, a region
with no file named and a file of the other kind. A file that doesn't validate fails with its
first error's JSON Pointer.

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

A `GPUParticles2D` with [trails](scene-format.md#gpu-particle-trails) also returns `trails`, how
many draw (a particle's copies run on after it dies), and `trail_bounds`, the box round every
live particle and copy they draw through. With `list`, each particle has its `trail`: how many
of its copies are live, the `length` in pixels from it through them and where it `end`s.

```json
{"alive":4,"bounds":[58.7956,26.9748,146.1592,27.8562],"particles":[{"age":1.5,"color":[0.699,0.899,1,1],"position":[204.9548,54.831],"rotation":0,"scale":1,"trail":{"copies":12,"end":[159.7436,43.139],"length":46.7045}}],"trail_bounds":[24.3106,24.2416,180.6442,30.5893],"trails":4}
```

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

## Global illumination

### lightmap.bake

Bakes a [`LightmapGI`'s](scene-format.md#baked-lightmaps) light and [light
probes](scene-format.md#light-probes), as Godot's LightmapGI bake: the
`MeshInstance3D`s whose `gi_mode` is `"static"` under its parent (the whole world for a root),
lit by the lights there whose `light_bake_mode` isn't `"disabled"` and by its environment, with
shadows and bounces. It unwraps the meshes into lightmap UVs, traces their light and the probes'
on the CPU, then writes a `.lightmap.json` file and its texels in an `.hdr` file beside it, and
sets the LightmapGI's `light_data` to the file. It returns the files, how many meshes and lights
it baked, how many `probes`, the size of a slice, how many slices and `problems` when there are
any: models or textures that
don't load, meshes with no entity ID and model parts with a skin, which it leaves out. A bake that comes out the same
reports `"unchanged": true` and records no undo step. Undo puts both files back as they were,
removing them if the bake made them, and sets `light_data` back; redo writes the bake again.
Meshes draw with the new lightmap, and moving meshes with the new probes, at the next command or
tick.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `LightmapGI` |
| `file` | string | its `light_data`, else beside the scene, named after it | The `.lightmap.json` file to write, project path |

Needs: `edit` and `filesystem`. Takes `dry_run`. The courtyard of
[examples/lightmaps-3d](../../examples/lightmaps-3d/):

```json
{"id": "lightmap"}
```

```json
{"file":"main.lightmap.json","lights":1,"meshes":3,"probes":234,"size":[128,64],"slices":1,"texture":"main.lightmap.hdr"}
```

An unknown ID fails at `/id` with a "did you mean"; so does an entity with no `LightmapGI`. A
file that doesn't end in `.lightmap.json` fails at `/file` (or `/id`, for its `light_data`).

### voxel_gi.bake

Bakes a [`VoxelGI`'s](scene-format.md#voxelgi) cells, as Godot's VoxelGI bake: the
`MeshInstance3D`s whose `gi_mode` is `"static"` under its parent (the whole world for a root)
that touch its box, voxelized at its `subdiv` with their albedo, emission and normals. It writes
a `.voxelgi.json` file and sets the VoxelGI's `data` to it. Lights aren't baked: the renderer
injects them into the cells as the game runs. It returns the file, how many meshes it voxelized,
how many `cells` it made, the cells along each axis (`size`), the octree's `levels`, and
`problems` when there are any: models or textures that don't load, which it leaves out. A bake
that comes out the same reports `"unchanged": true` and records no undo step. Undo puts the file
back as it was, removing it if the bake made it, and sets `data` back; redo writes the bake
again. Meshes draw with the new cells at the next command or tick.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `VoxelGI` |
| `file` | string | its `data`, else beside the scene, named after it and the entity | The `.voxelgi.json` file to write, project path |

Needs: `edit` and `filesystem`. Takes `dry_run`. The room of
[examples/voxel-gi-3d](../../examples/voxel-gi-3d/):

```json
{"id": "gi"}
```

```json
{"cells":12104,"file":"main.gi.voxelgi.json","levels":7,"meshes":4,"size":[64,32,64]}
```

An unknown ID fails at `/id` with a "did you mean"; an entity with no `VoxelGI` fails at `/id`. A
file that doesn't end in `.voxelgi.json` fails at `/file` (or `/id`, for its `data`).

### occluder.bake

Bakes an [`OccluderInstance3D`'s](scene-format.md#occlusion-culling) shape from the scene's
meshes, as Godot's Bake Occluders: every visible `MeshInstance3D` whose `layers` share a bit with
its `bake_mask` and whose material isn't transparent (the entity's `StandardMaterial3D`, else
each model surface's own), placed in the occluder's space, each surface simplified with
meshoptimizer until it would stray `bake_simplification_distance` meters, then duplicate corners
and triangles merged. It sets the occluder's `shape` to `"array"` with the `vertices` and
`indices` it made, and returns how many `vertices` and `triangles` that is, and `problems` when
there are any: models that don't load, which it leaves out. A bake that comes out the same
reports `"unchanged": true` and records no undo step. Culling uses the new shape at the next
command or tick.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with an `OccluderInstance3D` |

Needs: `edit`. Takes `dry_run`. The tower of
[examples/occlusion-culling](../../examples/occlusion-culling/), the only mesh on its
`bake_mask`:

```json
{"id": "tower_occluder"}
```

```json
{"triangles":12,"vertices":8}
```

An unknown ID fails at `/id` with a "did you mean"; an entity with no `OccluderInstance3D` fails
at `/id`. With nothing to bake (no visible, opaque mesh on a `bake_mask` layer) it fails with
`-32002` and changes nothing, as Godot's bake reports no meshes.

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
measures what a bus played, or writes it to a `.wav` file. `audio.analyze` judges a piece of
music: its loudness, tempo, key and how cleanly it loops. Levels are in dB to a tenth, with
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
- `peak_db` is the loudest speaker's peak over the last mix step, after the bus's volume and
  effects, or `null` when the bus was silent.
- In 3.1, 5.1 or 7.1 ([surround](scene-format.md#surround-sound)), `speaker_mode` names the
  speakers (`surround_31`, `surround_51` or `surround_71`; left out in stereo), and each player
  and send adds `speakers`: its gain on the `center` speaker and the `lfe`, and `[left, right]`
  on the `rear` and `side` speakers that mode has. `volume` stays the front pair's.
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
  `AudioStreamPlayer2D` or `AudioStreamPlayer3D` its fade with distance and its pan from the
  listener.
- An `AudioStreamPlayer3D` ([3D sound](scene-format.md#3d-sound)) also lists `pitch_scale`, the
  pitch it plays at after the Doppler effect, and `filter_db`, the gain its high shelf takes off
  for distance and its emission angle (as Godot's filter, the highest frequencies lose twice that). An `Area3D` with a reverb bus adds
  `"sends": [{"bus", "volume"}]`, the buses it mixes into beside `bus`.
- `paused` is listed when `stream_paused` holds the player, and `error` when its stream did not
  load, such as a missing or corrupt file.
- A player's `bus` is the one it plays on now: an `Area2D` with `audio_bus_override` that holds
  an `AudioStreamPlayer2D` sends it to its own bus ([buses by area](scene-format.md#buses-by-area)),
  as an `Area3D` does an `AudioStreamPlayer3D`.
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

In surround, the peak and RMS are over every speaker, and `speakers` gives each speaker's RMS
level: `front_left`, `front_right`, `center`, `lfe`, then `rear_left` and `rear_right` in 5.1
and `side_left` and `side_right` in 7.1. The `.wav` file then has a channel for each speaker,
named as `WAVE_FORMAT_EXTENSIBLE` names them. From
[examples/surround-sound](../../examples/surround-sound/), a bee on the listener's right:

```json
{"peak_db":-3.7,"rms_db":-13.1,"seconds":0.1,"slices":[],"speakers":{"center":-10.8,"front_left":-17.9,"front_right":-15,"lfe":-8.3,"rear_left":-17.9,"rear_right":-14.3,"side_left":-18,"side_right":-13.9}}
```

Sessions keep the last 10 seconds of each bus; `seconds` in the reply is how much there was,
which is less right after a scene opens. A name that is no bus is an error with the closest one,
and so is a `path` that doesn't end in `.wav`. In a dry-run `batch`, `path` writes nothing.

### audio.analyze

Measures a sound so an agent can judge it without listening: loudness, peaks, clipping,
silence, tempo, key, each octave band's share of the energy and, for a looping file, how cleanly
the loop joins. It measures a sound file, a `.audio.json` sound as it loops, or what a bus
played. From [examples/music-analysis](../../examples/music-analysis/).

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | A sound (`.wav`, `.ogg`, `.mp3`) or a `.audio.json` file with a `source` |
| `bus` | string | | A bus to measure instead of a file |
| `seconds` | number, 0 to 10 | `10` | With `bus`, how far back; surround is measured folded to stereo |
| `loop` | boolean | the file's settings | Measure the loop's join; `true` loops a plain sound whole |

Needs: `read`. A four-bar groove's `.audio.json` file, which loops it every 16 beats:

```json
{"path": "music/groove.audio.json"}
```

```json
{"bands_db":[-7.5,-6.8,-6.4,-5,-12.2,-60.4,-56.1,-47.3,-38.4,-31.5],"beat_offset":0.006,"beats":16,"bpm":100,"clips":0,"key":"D minor","key_confidence":0.86,"loop":{"beats":16,"clicks":false,"level_change_db":0.1,"seconds":9.6,"step_db":null},"loudness_range":0.2,"lufs":-15.2,"max_short_term_lufs":-15,"peak_db":-1.7,"rms_db":-16,"seconds":9.6,"silence":{"end":0,"start":0},"tempo_confidence":0.91,"true_peak_db":-1.7}
```

- `lufs` is integrated loudness as EBU R 128 measures it (ITU-R BS.1770-4, with libebur128),
  `loudness_range` is in LU and `max_short_term_lufs` is the loudest three seconds. Each is
  `null` for a sound too quiet to gate (below -70 LUFS), and `max_short_term_lufs` for one
  shorter than three seconds.
- `true_peak_db` is the peak between samples, as a converter plays them (dBTP); `peak_db` and
  `rms_db` are the sample peak and RMS in dBFS.
- `clips` counts runs of three samples or more at full scale, and samples past it; `first_clip`
  is when the first was, in seconds.
- `silence` is the silence (below -60 dBFS) at the `start` and `end` in seconds, and `gaps` of
  half a second or more between, as `[from, to]` (at most 8).
- `bpm` is the tempo to a tenth, from where notes start, or `null` for a sound with no steady
  beat or under three seconds long. `beat_offset` is the first beat in seconds, `beats` the
  length in beats and `tempo_confidence` how sure the measure is, from 0 to 1. Compare them with
  a `.audio.json` file's `bpm` and `beat_count`.
- `key`, such as `"D minor"`, comes from the notes' pitch classes against Krumhansl and
  Kessler's key profiles, or is `null` when no key stands out; `key_confidence` is the
  correlation, from 0 to 1.
- `bands_db` is each octave band's share of the energy, centred on 31.5, 63, 125, 250 and 500 Hz
  and 1, 2, 4, 8 and 16 kHz, in dB: 0 is all of it, and `null` none.
- `loop` describes the join: its `seconds` and `beats`, `step_db` the jump between the last
  sample before it and the first after it (`null` for none), `level_change_db` the level over
  the beat after it against the beat before it, and `clicks` when the jump is far bigger than
  the sound's own changes from sample to sample. A WAV file loops from `loop_begin` to
  `loop_end` when its `loop_mode` is `forward`; an Ogg or MP3 file with `loop` goes back to
  `loop_offset` after `beat_count` beats, fading over 256 frames as it does when it plays, or
  at its end.
- `problems` lists what needs fixing: clipping, a true peak above the -1 dBTP that EBU R 128
  allows, a loop that clicks, or a silent sound.

The same groove with a loop that ends a tenth of a beat early, mid-note:

```json
{"loop":{"beats":15.9,"clicks":true,"level_change_db":0,"seconds":9.54,"step_db":-25.3},"problems":["the loop clicks where its end joins its start"]}
```

Exactly one of `path` and `bus` is needed. A `.audio.json` file with no `source` (a randomizer,
playlist or interactive music) is an error: play it and measure its bus. `loop` with `bus` is
an error too, since only a file loops.

### audio.play

Plays an `AudioStreamPlayer`, `AudioStreamPlayer2D` or `AudioStreamPlayer3D` at the next tick, as
Godot's `play()`: with
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
An ID that names no entity is an error with the closest one, and so is an entity with none of
the player components. Setting `playing` to `true` with [`component.set`](#componentset) plays it
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

Stops an `AudioStreamPlayer`, `AudioStreamPlayer2D` or `AudioStreamPlayer3D` at the next tick, as
Godot's `stop()`,
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

## Soundtrack

Music planned in the [storyboard](storyboard-format.md#music), written as
[`.score.json` scores](scene-format.md#scores), moved to and from Standard MIDI Files so any
music program can play or edit them, played into stems a game plays, and arranged into
interactive music, then checked without listening. From
[examples/soundtrack-plan](../../examples/soundtrack-plan/),
[examples/music-scores](../../examples/music-scores/),
[examples/soundtrack-render](../../examples/soundtrack-render/),
[examples/soundtrack-arrange](../../examples/soundtrack-arrange/) and
[examples/soundtrack-check](../../examples/soundtrack-check/).

### score.export

Writes a score as a Standard MIDI File (format 1): a first track with the tempo, meter, key and
a marker per section, then a track per score track with its name, instrument, volume and pan,
drums on channel 10.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | The `.score.json` file |
| `out` | string | `path` with `.mid` for `.score.json` | The `.mid` or `.midi` file to write |

Needs: `read`, `filesystem`.

```json
{"path": "music/night_market.score.json"}
```

```json
{"notes":119,"path":"music/night_market.mid","seconds":20.87,"tracks":4}
```

A score that doesn't validate is an error at `/path` naming the first problem, with its JSON
Pointer in the file:

```json
{"code":-32002,"data":{"path":"/path"},"message":"/path: music/sketch.score.json /tracks/0/bars/0: \"B5:0.5\": starts at beat 4, after the bar's 4 beats"}
```

So is a score with more than 15 tracks that aren't drums, since MIDI has 15 channels for them.

### score.import

Reads a Standard MIDI File (format 0 or 1) into a score: a track per MIDI track and channel that
plays notes, named by the track's name and playing the channel's first program change (channel
10 is `"drums"`), with the first tempo, time signature and key signature, a section per marker,
and each bar written as the shortest line of notes that plays it.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | The `.mid` or `.midi` file |
| `out` | string | `path` with `.score.json` for its extension | The `.score.json` file to write |
| `grid` | integer, 1 to 96 | `24` | Divisions of a beat notes snap to: thirty-second notes and sixteenth-note triplets |

Needs: `read`, `filesystem`. The file `score.export` wrote above, read back:

```json
{"path": "music/night_market.mid", "out": "music/night_market_again.score.json"}
```

```json
{"bars":8,"bpm":92,"key":"A minor","path":"music/night_market_again.score.json","sections":["intro","loop"],"tracks":[{"instrument":"flute","name":"melody","notes":15},{"instrument":"string_ensemble_1","name":"strings","notes":24},{"instrument":"acoustic_bass","name":"bass","notes":32},{"instrument":"drums","name":"drums","notes":48}]}
```

`meter` is there when it isn't 4/4, such as `"6/8"`. `problems` lists what the score can't
hold and what moved to fit it: tempo, meter and key changes after the first (the score keeps
the first), pitch bends, the sustain pedal, drum kits other than the standard one, notes moved
to the grid and markers that aren't on a bar line. A file with no notes, a format 2 or SMPTE
timed file, or one cut short is an error at `/path`.

### soundtrack.render

Plays a score through a SoundFont into one stem per track, each with a `.audio.json` file that
gives it the score's `bpm` and `beat_count` (and `bar_beats` when not 4), and writes a
`.audio.json` file that plays the stems together as a [synchronized
stream](scene-format.md#audio-streams), so a player's `stream_<index>_volume_db` fades one layer
at a time. Each track plays its General MIDI program from bank 0, drums from bank 128, at its
`volume_db`, `pan` and velocities, with [TinySoundFont](https://github.com/schellingb/TinySoundFont).

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | The `.score.json` file |
| `out` | string | `path` without `.score.json` | The folder for the stems; the music is this name with `.audio.json` |
| `tracks` | array of strings | every track | Only these tracks; the music still lists the other stems already rendered |
| `loop` | boolean | `false` | Stems exactly the score's length that loop (see below) |
| `format` | `"ogg"` or `"wav"` | `"ogg"` | Ogg Vorbis, or 16-bit PCM WAV |
| `quality` | number, 0 to 1 | `0.5` | Ogg Vorbis quality, about 64 kbit/s at 0 to 500 at 1 |
| `soundfont` | string | the built-in sounds | A SoundFont 2 (`.sf2`) file to play the instruments with |

Needs: `read`, `filesystem`. A four-bar loop:

```json
{"path": "music/battle.score.json", "loop": true}
```

```json
{"beat_count":16,"bpm":140,"path":"music/battle.audio.json","peak_db":-5.6,"seconds":6.86,"stems":[{"path":"music/battle/bass.ogg","peak_db":-14.4,"track":"bass"},{"path":"music/battle/strings.ogg","peak_db":-11.5,"track":"strings"},{"path":"music/battle/brass.ogg","peak_db":-8.6,"track":"brass"},{"path":"music/battle/drums.ogg","peak_db":-9.8,"track":"drums"}]}
```

It writes `music/battle/bass.ogg` and `music/battle/bass.audio.json`, and so on for each track
(a track's name with characters other than letters, digits, `-` and `_` as `_`), and
`music/battle.audio.json`:

```json
{"format": 1, "loop": true, "source": "music/battle/bass.ogg", "bpm": 140, "beat_count": 16}
{"format": 1, "synchronized": [{"stream": "music/battle/bass.audio.json"}, {"stream": "music/battle/strings.audio.json"}, ...]}
```

- Without `loop`, each stem rings on after the score's end until its notes have died away, for
  up to four seconds. With it, each stem is exactly `beat_count` beats long, as the engine loops
  it, and what rings past the end is added onto the start, so the loop joins without a click
  (an `.ogg` stem gets `"loop": true`, a `.wav` one `"loop_mode": "forward"`).
- `peak_db` is each stem's loudest sample in dBFS, and the stems' together when every track
  renders; `null` is silence. `problems` lists a stem or a mix above full scale, with what to
  lower, and a track with no notes.
- The built-in sounds are the engine's own: a synthesized voice for each of General MIDI's 128
  instruments, by family (pianos pluck, strings swell, organs hold), and a standard drum kit
  with every drum the scores name, all made in code, so a score plays with no SoundFont file and
  the same everywhere. A SoundFont file sounds better; FluidR3_GM (MIT) and MuseScore_General
  (MIT) are free ones.
- Check the result with [`audio.analyze`](#audioanalyze): a stem's `.audio.json` file measures
  as it loops.

A track whose instrument the SoundFont lacks, a file that isn't a SoundFont 2 file (at
`/soundfont`), a score that doesn't validate (at `/path`) and an unknown name in `tracks` (at
its index, with the closest name) are errors.

### soundtrack.plan

Drafts the storyboard's cue sheet: a [`cue`](storyboard-format.md#music) for each scene no cue
plays in, each story beat no cue plays during and each game state in `states` that no cue names,
returned as `edits` that [`storyboard.edit`](#storyboardedit) takes as they are. Change what
should differ before passing them on; nothing is written.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `states` | array of strings | | Game states the music follows, e.g. `["explore", "combat"]` |

- A scene's cue plays the theme whose `links` name the scene; else the theme of the place it is
  set in, else of a faction, else of a character. Where it is set and who is in it come from the
  scene's `StoryRef` IDs and what those name: a beat's `place` and `characters`, a character's
  `home` and `faction`, a place's `region`. A cue for a beat finds its theme the same way.
- A scene's cue also plays during the beats its `StoryRef` IDs name, or whose `links` name it,
  that no cue plays during yet.
- Cue IDs come from the scene's file name, the beat's ID or the state, with `_2` and so on if
  taken. A state's cue has no theme.
- `unthemed` lists the places, factions and characters cues found no theme for, so an agent knows
  which themes to write.

Every `.scene.json` file in the project is read, except those in `build/`; the open scene as it
is now. A storyboard that doesn't load fails with its first problem. Needs: `read`.

```json
{"states": ["combat"]}
```

```json
{"edits":[{"id":"cue/harbor","set":{"name":"Harbor","scenes":["scenes/harbor.scene.json"],"summary":"Plays in scenes/harbor.scene.json.","theme":"theme/sea"},"version":null},{"id":"cue/lighthouse","set":{"beats":["beat/storm"],"name":"Lighthouse","scenes":["scenes/lighthouse.scene.json"],"summary":"Plays in scenes/lighthouse.scene.json.","theme":"theme/sea"},"version":null},{"id":"cue/wreck","set":{"beats":["beat/wreck"],"name":"The wreck","summary":"Plays during beat/wreck."},"version":null},{"id":"cue/combat","set":{"name":"Combat","states":["combat"],"summary":"Plays in the combat state."},"version":null}],"unthemed":["place/reef"]}
```

### soundtrack.arrange

Writes the storyboard's [cues](storyboard-format.md#music) as one
[interactive stream](scene-format.md#interactive-music) that an `AudioStreamPlayer` plays and
gameplay code steers by setting its `parameters.switch_to_clip` to a clip's name. Each cue plays
its `music`: `.audio.json`, `.ogg`, `.wav` or `.mp3` files, or the music
[`soundtrack.render`](#soundtrackrender) makes of a `.score.json` file.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `out` | string | `"music/soundtrack.audio.json"` | The `.audio.json` file to write; layer files go in the folder of the same name |
| `cues` | array of cue IDs | every cue | Only these cues, in this order |
| `initial` | cue ID | the first | The cue the music starts with |

- A cue becomes a clip named after its ID (`cue/harbor` is `harbor`). With several files and
  no layers, the clip plays them together as a synchronized stream.
- A cue with `layers` and a file per layer, lowest first, becomes a clip per layer
  (`harbor_calm`, `harbor_tense`), each a synchronized stream of its file and those below it.
  Switching between them waits for the next beat and keeps the music's place.
- A cue that `plays` `once` is a sting. With `after`, it plays as the music leaves those cues,
  as a filler clip, before the next. Without, it plays when asked for, on the next beat, then the
  music goes back to the clip it interrupted, where that clip was.
- Every other switch waits for the next bar.
- `clips` says which cue and layer each clip plays and its stream; `layers` lists the layer
  files written. `problems` lists cues with no music (they are left out), files that don't exist
  yet, with the score that renders them, and layers without a file each.

Fails when no cue has music. Needs: `read`, `filesystem`.

```json
{"initial": "cue/harbor"}
```

```json
{"clips":[{"cue":"cue/bell","name":"bell","stream":"music/bell.audio.json"},{"cue":"cue/harbor","layer":"calm","name":"harbor_calm","stream":"music/harbor/strings.audio.json"},{"cue":"cue/harbor","layer":"tense","name":"harbor_tense","stream":"music/soundtrack/harbor_tense.audio.json"},{"cue":"cue/storm","name":"storm","stream":"music/storm.audio.json"},{"cue":"cue/win","name":"win","stream":"music/win.audio.json"}],"layers":["music/soundtrack/harbor_tense.audio.json"],"path":"music/soundtrack.audio.json"}
```

### soundtrack.check

Finds what keeps a game's music from playing as planned, by reading its files and measuring how
they play. The music is every interactive, playlist and synchronized `.audio.json` file, every
sound `.audio.json` file with a `bpm`, the streams cues play, and what those play; `build/` and
`.slash/` are skipped.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `paths` | string array | all the music | Music files and folders to check, with what they play, and the cues that name them |

Errors, which [`test.run`](#testrun) fails on:

- A cue's music that can't play: a file that doesn't exist, a score not rendered yet (with the
  file [`soundtrack.render`](#soundtrackrender) writes), a file that isn't music, or a cue with
  more layers than files. A file a music file names that doesn't exist.
- A sound whose `beat_count` beats at its `bpm` last longer than it does, so interactive music
  and playlists wait for beats that never come.
- A loop that clicks where its end joins its start, as
  [`audio.analyze`](#audioanalyze) finds it.
- An interactive clip whose stream has no `bpm` while switches from it wait for the beat or
  bar: they would happen at once.
- A piece that misses a [`loudness`](storyboard-format.md#music) target that covers it (its
  `links`, or all music with none): integrated loudness further from `lufs` than `tolerance`
  (2 LU if unset), or a true peak above `true_peak`. A piece is a clip of interactive music, a
  playlist's stream or music nothing else plays, measured as it plays for its beats or its
  length.

Warnings:

- A cue with no music yet, or a sting whose `after` names a cue with none.
- A compressed loop whose beats end before its sound does, so the loop leaves out the rest; a
  loop that isn't a whole number of bars; a sound that measures a tempo other than its `bpm`
  (or half or double it), when the measure is sure.
- Switches between clips with beats that come at once or on the next beat and start the next
  clip from its start: off the bar. A clip that goes on by itself (a sting) may come in on any
  beat, and `same_position` keeps the bar.
- `same_position` between clips whose `bpm` differ.
- Clips the music cuts between, directly or through a filler, whose keys are three or more steps
  apart round the circle of fifths (relative keys are none apart). A clip's key is its score's,
  the score it is a stem of, its layers', or else the key it measures in, when the measure is
  sure.

`checked` counts the `.audio.json` files read. Fails only when the storyboard doesn't load.
Needs: `read`.

```json
{}
```

```json
{"checked":13,"errors":[{"file":"music/chime.audio.json","message":"16 beats at 96 bpm last 10 s, longer than the sound's 3.34 s, so the music's beats run past its end; the sound holds 5 beats","path":"/beat_count"},{"file":"music/reef.audio.json","message":"measures -25.9 LUFS; loudness/music asks for -20 ± 3, so raise it 5.9 dB","path":""}],"warnings":[{"file":"music/soundtrack.audio.json","message":"harbor (E minor) and reef (Bb major) are 3 steps apart round the circle of fifths, and the music goes straight from one to the other; a filler clip, or a nearer key, joins them","path":"/clips/1"},{"file":"music/soundtrack.audio.json","message":"reef (Bb major) and storm (E minor) are 3 steps apart round the circle of fifths, and the music goes straight from one to the other; a filler clip, or a nearer key, joins them","path":"/clips/2"}]}
```

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
- rules, tones, palettes, budgets and loudness targets that apply everywhere (those with no
  `links` or `refs`);
- the theme of each cue among those, since its key, tempo, instruments and mood are what the
  cue's music is made from;
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
names no entry, a texture (`.png` or `.bmp`) that breaks the style: larger on a side than a
`budget`'s `texture_size`, or with pixels in colors a `palette` doesn't have, and a model with
more triangles than a `budget`'s `triangles` or an image inside it larger than its
`texture_size` (see the [storyboard format](storyboard-format.md#checks)). Every
`.scene.json`, `.prefab.json`, texture and model file in the project is read, except those in `build/` and in folders whose names start
with `.`; the open scene is checked as it is now. Warnings: a link to a file that does not exist,
since open work can link files still to be made, and, once the storyboard has a `cue`, each scene
no cue plays in and each story beat no cue plays during (see
[`soundtrack.plan`](#soundtrackplan)). `slash-cli project validate` runs the same check.

Takes no params. Needs: `read`.

```json
{}
```

```json
{"errors":[{"file":"storyboard/world.json","message":"uses \"driver\", which term/courier says to avoid; write \"courier\"","path":"/entries/character~1pilot/summary"},{"file":"scenes/main.scene.json","message":"unknown storyboard entry \"character/pilto\"; did you mean \"character/pilot\"?","path":"/entities/player/components/StoryRef/ids/0"},{"file":"art/crate.png","message":"has 12 pixels in 1 color not in palette/pico: #ff0000 at (3, 4), nearest \"red\" #ff004d","path":""}],"valid":false}
```

`errors` and `warnings` are left out when empty, so a clean project returns `{"valid":true}`.

### storyboard.review

Asks a model whether new material contradicts the storyboard: a line of dialogue, a scene's
interface text, a description, an image or entries just written. `storyboard.check` catches what
rules can (broken references, words to avoid, colors off the palette); this catches what only
reading can, such as a character called by the wrong relationship or a place she has never been.
The model comes from the project's [`providers.json`](providers.md), as for `ai.complete`.

The model reads the facts `storyboard.brief` would return for the material (the entries its
scenes' `StoryRef` components name, entries linked to its files, global rules and live work),
plus every entry whose name or alias the text mentions as a whole word, and the entries those
name. Findings are advisory: each quotes the material and names the entry it contradicts, and
one whose quote is not in the material, whose entry does not exist or whose item is out of
range is dropped and counted in `unverified`, so a finding never invents a fact. When no entry
applies to the material, no model is asked.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `text` | string | | New text, such as a line of dialogue |
| `paths` | array of paths | | Project files: scenes and prefabs (each control's `text`, `placeholder_text` and `tooltip_text`), other `.json` files (their strings with a space in them), text files (whole, up to 64 KiB) and images (`.png`, `.jpg`, `.gif`, `.webp`) for a model that sees them. The open scene is read as it is now |
| `entries` | array of IDs | | Storyboard entries to review against the rest, such as ones just written |
| `role` | string | the `default` role, or the only provider | A role or provider from `providers.json` |
| `max_tokens` | integer | the role's, then the provider's | The most tokens to reply with |

At least one of `text`, `paths` and `entries` is needed. Needs: `read`, `network`. It changes
nothing.

```json
{"paths": ["scenes/tavern.scene.json", "dialogue/act1.txt"]}
```

```json
{"advisory":true,"facts":4,"findings":[{"confidence":"high","entry":"character/oren","path":"/entities/greeting/components/Label/text","problem":"Oren is Mira's brother, not her sister.","quote":"My sister Oren","source":"scenes/tavern.scene.json"},{"confidence":"high","entry":"character/mira","problem":"Mira has never left Lowtown.","quote":"I took the ferry up to Hightown","source":"dialogue/act1.txt"}],"items":2,"model":"llama3.2","unverified":1,"usage":{"input_tokens":420,"output_tokens":96}}
```

Each finding has the `source` (`text`, a project path, or an entry's storyboard file), the JSON
Pointer `path` inside it where there is one, the `quote`, the `entry`, the `problem` in one
sentence and the model's `confidence` (`high`, `medium` or `low`). `facts` counts the entries
the model read and `items` the pieces of material; `"replayed": true` marks a reply from the
replay file. With no entry to check against, the reply has no `model` or `usage` and says why
in `note`.

Errors: no material, or a storyboard file that is invalid (run `storyboard.check`), with
`-32602`; a missing file or unknown entry with `-32001` and, for an entry, the closest ID; a path
outside the project, a file that is not text or an image, a text file over 64 KiB or invalid
JSON with `-32002`, each with `data.path` at the param; and the provider's errors as for
[`ai.complete`](#aicomplete).

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

Advances the simulation by running every system once per tick, and lists the music clips that
started. Undo reverts the entities to how they were before.

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

When music started a clip during the ticks, `music` lists each, oldest first, as a
[`clip_changed`](scene-format.md#clip-changes) event: the `tick` it started in, the player's
`id`, the `clip`, the clip `from` before it, the `transition` it followed, `filler` and
`automatic` when they hold, and the seconds it `waited` for its beat or bar. A `clip` of `""`
means the music stopped. At most 100 are listed, then `music_truncated` is true. From
[examples/music-timeline](../../examples/music-timeline/), as the victory sting goes back to
explore at its end by itself:

```json
{"music":[{"automatic":true,"clip":"explore","from":"victory","id":"music","tick":389,"transition":2,"waited":1.003}],"tick":420,"ticks":90}
```

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
match means nothing is left, so `entities` is empty. `music` lists the clips music started, as
[`step`](#step)'s does, so one call says whether the music followed the game: "does the boss
music start within a bar of the boss arriving?".

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
[`storyboard.check`](#storyboardcheck) and adds its errors, runs
[`soundtrack.check`](#soundtrackcheck) where the soundtrack's commands are registered (as in
`slash-cli` and `slash-mcp`) and adds its errors, and, where the session may, runs
[`asset.check`](#assetcheck) on the files the project took from the asset library, which
records what it sees as their feedback: its problems come back as `assets` without failing a
test, but for a model that fails the checks it was kept with, which fails at its `check` in
`library.json`. A name in `tests` that no test has fails with `-32001` and the closest name.

The result counts the tests run and those that passed, and lists each failed check:

| Field | Meaning |
| --- | --- |
| `file` | The test file, or for a storyboard or music error the file it is in |
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

### playtest.run

Runs many headless copies of the game at once and sums them up, for tuning values and
playtesting: "how high must the jump be to clear the pit?", "how often does the bot die with
enemy speed 120?". Each run is a session of its own started from the session's world as it is
now (unsaved edits included), or from `scene`, with one combination of the `vary` values set.
A scripted bot presses input actions before each tick, and the run stops at the first `end`
condition it meets or after `ticks`. Runs step on the job system's threads at the same time, and
the session's own world doesn't change. The game's systems come from its gameplay code (its
`SLASH_GAME_MODULE`), which every run gets.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `vary` | object of value arrays | | Values to try by `"<entity>/<Component>/<field>"`, with a part as in `"player/Transform2D/position/y"`. `"*/<Component>/<field>"` sets every entity with the component; a `.prefab.json` path in place of the entity sets the prefab, and so its instances, those spawned while the game runs too. Every combination runs, at most 8 keys |
| `runs` | integer, 1 to 64 | `1` | Runs of each combination. Combinations times runs is at most 256 |
| `seed` | integer | `0` | Run *n* of a combination presses by `chance` with seed + *n*, so runs differ and every call repeats |
| `bot` | array of presses | none | The input actions the bot presses, below |
| `end` | object of conditions | none | Conditions that end a run, by name, checked before each tick in name order: `with`, `without` and `where` as in [`world.query`](#worldquery), and `until` (`"any"` or `"none"`) as in [`run.until`](#rununtil). `"timeout"` is taken |
| `ticks` | integer, 1 to 100000 | `600` | Most ticks a run lasts |
| `tick_seconds` | number, above 0 and at most 1 | `1/60` | Seconds per tick |
| `count` | string array | every event gameplay code emits | Event kinds to count, such as `"entity.destroyed"` |
| `measure` | string array | none | Values to read when each run ends, as `vary`'s keys; `"*/<Component>"` counts the entities with it |
| `scene` | string | the open world | The scene file each run opens instead |

Each press in `bot` is an object with an `action` (an input action of the project's, with
"did you mean" when misspelled) and when to press it. With no trigger it is held from `from`
to `to`; otherwise it presses when every trigger it gives holds and lasts `for` ticks:

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `action` | string | | The input action |
| `from`, `to` | integer | `0`, the end | The ticks it may press in, counted from the run's start |
| `every` | integer | | Presses every this many ticks from `from` |
| `chance` | number, above 0 and at most 1 | | Presses at random with this chance each tick |
| `when` | condition | | Presses while entities match it, a condition as `end`'s |
| `for` | integer | `1`, or until `to` with no trigger | Ticks each press lasts |
| `strength` | number, above 0 and at most 1 | `1` | How far the action is pressed |

Needs: `run`. Runs can't write files, export or call models (they answer from the replay file).

The result has `runs`, the number made, and `results`, one per combination in order (the first
`vary` key's values change slowest), each with:

| Field | Meaning |
| --- | --- |
| `values` | The combination's values, by `vary` key |
| `ends` | With `end`: how many runs met each condition, and `timeout` for those that met none |
| `ticks` | `min`, `mean` and `max` of the ticks the runs lasted |
| `events` | Each event kind counted, per run on average |
| `measure` | Each `measure` key's value at the end: `min`, `mean` and `max` for numbers, otherwise how many runs ended with each value (as JSON text); `missing` counts runs whose entity or component was gone |
| `music` | When music started clips: by player ID, each clip's `runs` (how many runs it started in) and `first_tick` (`min`, `mean` and `max` of when it first did, in ticks from the run's start), and `transitions`, each `"<from> -> <to>"` per run on average |

Means are rounded to three decimals. A bad key, value, action or condition fails before anything
runs, with its JSON Pointer, such as `/vary/runner~1Runner~1jump_speed/1`.

From [examples/playtest](../../examples/playtest/), a runner and a 120-pixel pit, with a bot that
holds `move_right` and jumps at random:

```json
{"bot":[{"action":"move_right"},{"action":"jump","chance":0.04}],"end":{"cleared":{"where":[{"component":"Transform2D","field":"position.x","op":">=","value":600}],"with":["Runner"]},"fell":{"where":[{"component":"Transform2D","field":"position.y","op":">","value":64}],"with":["Runner"]}},"measure":["runner/Runner/coins"],"runs":16,"tick_seconds":0.015625,"ticks":400,"vary":{"runner/Runner/jump_speed":[400,500,600]}}
```

```json
{"results":[{"ends":{"cleared":0,"fell":16,"timeout":0},"events":{"coin_collected":0.75,"jumped":1.75},"measure":{"runner/Runner/coins":{"max":6,"mean":1.5,"min":0}},"ticks":{"max":143,"mean":123.313,"min":110},"values":{"runner/Runner/jump_speed":400}},{"ends":{"cleared":2,"fell":14,"timeout":0},"events":{"coin_collected":0.375,"jumped":1.688},"measure":{"runner/Runner/coins":{"max":5,"mean":1.125,"min":0}},"ticks":{"max":200,"mean":135.5,"min":109},"values":{"runner/Runner/jump_speed":500}},{"ends":{"cleared":6,"fell":10,"timeout":0},"events":{"coin_collected":0.5,"jumped":1.813},"measure":{"runner/Runner/coins":{"max":6,"mean":1,"min":0}},"ticks":{"max":200,"mean":153.25,"min":118},"values":{"runner/Runner/jump_speed":600}}],"runs":48}
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
| `asset.reloaded` | A loaded asset or prefab file changed on disk, or a file an asset read as it loaded did (both are reported) | `data`: `path`, `type`, and `version`, or `error` if it no longer loads |
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

Draws the frame the window shows now, on the CPU: the 3D world through its current `Camera3D`
([scene-format.md](scene-format.md#3d-nodes)), then the world's sprites, tile maps, particles and
lights, then the collision shapes if asked, then the controls, over the project's
`rendering.clear_color` (or a `WorldEnvironment`'s background), at the window size
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
| `frames` | integer, 1 to 120 | `1` | Draw the frame this many times and return the last, for [temporal antialiasing](project-format.md#temporal-antialiasing), which builds up over frames |

Needs: `read`, and `filesystem` with `path`. A HUD with a health bar at 75%:

```json
{"points": [[80, 30], [20, 14], [68, 14]]}
```

```json
{"colors":[[0.2,0.4,0.8,1],[0.502,0.584,0.753,1],[0.169,0.31,0.588,1]],"drawn":{"controls":3},"height":90,"width":160}
```

`width` and `height` are the image's, after `region` and `scale`. `drawn` counts what was drawn
(`sprites`, `tiles`, `particles`, `lights`, `controls`, `shapes`, 3D `meshes` and, of those,
`lods`: the meshes drawn at a coarser [LOD](scene-format.md#mesh-lods)), and `occluded`, the 3D
meshes [occluders](scene-format.md#occlusion-culling) hid, leaving out
zeros. When the
project's `rendering.scaling` ([project-format.md](project-format.md#render-scale)) draws the
world at another size, `upscaler` names what scaled it and `render_size` is the world's size in
pixels, `[width, height]` (with `fsr2`, `fsr3` or a game's temporal upscaler, the 3D world's). `colors` are
`[r, g, b, a]` from 0 to 1, one per point, read from the whole window whatever `region` says.
Over MCP, `png` is sent as an image content block rather than as text. A point or region outside
the window is an error at its path, and so is a `path` that doesn't end in `.png`. In a dry-run
`batch`, `path` writes nothing. Captures keep TAA's, FSR 2's and FSR 3's history from one to the next,
as a running game's frames do, so a still world's edges smooth out over captures and a moved
mesh's history follows it.

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
| `contrast` | boolean | `false` | Add how much each thing stands out from what is around it, and list those hard to see |
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
- With `contrast`, each entry in `drawn` has `contrast`: how much its edge stands out from what
  is around it, as [WCAG's contrast ratio](https://www.w3.org/TR/WCAG21/#dfn-contrast-ratio)
  of the two colors' lightness, from 1 (the same) to 21 (black on white), to a tenth. It is
  left out when nothing around the thing shows in the window, as for a background that fills
  it. `hard_to_see` lists those under 1.5: a dark sprite on a night sky, pale text on a pale
  panel. The frame is drawn for it as [`frame.capture`](#framecapture) draws it, lights and
  materials included, so it costs more. Along the thing's edge, each pixel where it makes at
  least three quarters of the color is compared with the nearest pixel across, down, left or
  right that it adds nothing to, past its smoothed edge, on the same grid as `shown`; the
  median of those is its contrast.
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

A scaling `mode` that isn't `bilinear`, `fsr`, `fsr2`, `fsr3`, `metalfx_spatial` or
`metalfx_temporal` is an error at `/scaling/mode`, and an `upscaler` the game hasn't added is a `-32001`
error at `/scaling/upscaler` that lists the ones it has. Either way nothing changes.
[examples/render-scale](../../examples/render-scale/) changes the render scale while the game
runs.

### perf.stats

How long the world's ticks and systems took over the last 120 ticks, in milliseconds, the slowest
systems first, and with `memory`, what each part of the engine holds in memory. In a running game
it also reports frame pacing over the last 120 frames, with the settings that shape it, and the
renderer's GPU textures. Needs: `read`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `top` | integer | `10` | How many of the slowest systems to list, by average time, and of each memory part's items, by bytes; `0` for none |
| `memory` | boolean | `false` | Also report memory in bytes: the process's and each part of the engine's |

```json
{"top": 3}
```

In any session:

```json
{"systems":[{"avg":0.412,"max":0.951,"name":"physics2d","stage":4},{"avg":0.088,"max":0.204,"name":"cpu_particles_2d","stage":8},{"avg":0.031,"max":0.07,"name":"gui","stage":1}],"tick_ms":{"avg":0.598,"max":1.302}}
```

In a running game (`slash-cli run --serve`):

```json
{"catch_up_frames":0,"fps":120.0,"frame_ms":{"avg":8.33,"max":8.67,"min":7.99},"frames":932,"max_fps":120,"renderer":"vulkan","scaling":{"fsr_sharpness":0.2,"in_effect":"none","mode":"bilinear","scale":1.0},"systems":[{"avg":0.412,"max":0.951,"name":"physics2d","stage":4}],"tick_hz":60.0,"tick_ms":{"avg":0.598,"max":1.302},"ticks":465,"vsync_mode":"mailbox"}
```

`tick_ms` is how long `World::step` took, systems and all. A system's times are its own run with
the events and deferred calls it left; threaded systems in the same `stage` run at once, so their
times can add up to more than the tick's. Times are real time, so they vary from run to run and
machine to machine, and a system that hasn't run yet reports zeros. For a timeline of every
thread, build with the `tracy` preset and connect the Tracy profiler
([AGENTS.md](../../AGENTS.md#profiling)).

With `memory`:

```json
{"memory": true, "top": 2}
```

```json
{"memory":{"parts":[{"bytes":3528000,"counts":{"buses":1,"players":2,"streams":2},"items":[{"bytes":3528000,"count":441000,"name":"history Master"}],"name":"audio"},{"bytes":1210552,"counts":{"assets":3},"items":[{"bytes":1048624,"count":1,"name":"texture"},{"bytes":161880,"count":2,"name":"sound"}],"name":"assets"},{"bytes":95232,"counts":{"components":1242,"types":9},"items":[{"bytes":41984,"count":600,"name":"Transform2D"},{"bytes":24576,"count":600,"name":"Sprite2D"}],"name":"components"},{"bytes":30912,"counts":{"bodies":12,"contacts":4,"joints":0,"shapes":14},"name":"physics2d"},{"bytes":8448,"counts":{"entities":601,"events":2,"prefabs":0},"name":"world"},{"bytes":0,"counts":{"emitters":0,"particles":0,"slots":0},"name":"cpu_particles_2d"}],"process":{"peak":96468992,"resident":94371840}},"systems":[{"avg":0.412,"max":0.951,"name":"physics2d","stage":4},{"avg":0.031,"max":0.07,"name":"gui","stage":1}],"tick_ms":{"avg":0.598,"max":1.302}}
```

`process` is the operating system's count of the process's memory in RAM (`resident`) and the
most it has been (`peak`): the resident set on macOS, Linux and Android, the working set on
Windows and the WebAssembly heap on the web. `parts` are the engine's own, the largest first,
each with its `bytes`, what it holds (`counts`) and, where it says, what the bytes are made of
(`items`, the largest first). Bytes are counted from the containers' capacities, so they are close
to what the allocator handed out but not exactly it, and they vary by platform.

| Part | Holds |
| --- | --- |
| `world` | Entity IDs, the event queues and the prefabs instances share |
| `components` | Each component type's storage and what its fields hold on the heap, one item per type |
| `assets` | Loaded assets (see `asset.list`'s `bytes`), one item per type |
| `physics2d` | What Box2D allocated, and its bodies, shapes, contacts and joints |
| `audio` | Each bus's last ten seconds for `audio.capture` (an item per bus) and the mixing buffers |
| `cpu_particles_2d`, `gpu_particles_2d` | Each emitter's particles, or the steps GPU particles replay |
| `text` | The glyph atlas pages text draws from |
| `csg` | The meshes CSG roots keep (`meshes`) and their `triangles` |
| `gridmap` | The octant meshes and shapes GridMaps keep (`gridmaps`, `octants`, `cells` and `triangles`) |
| `terrain` | The heights and range maps HTerrains keep (`terrains` and `cells`) |
| `model_collision` | The collision shapes kept for models that collide (`models` and `shapes`) |

A part that grows from one report to the next while the game keeps doing the same thing is the
place to look for a leak, and its items name what grew
([examples/memory-report](../../examples/memory-report/)). Parts keep memory they have grown
into, as EnTT keeps a storage's pages and a vector its capacity, so bytes don't fall back when
things go. Modules and games add parts with `World::add_memory_source`. In a running game,
`memory.gpu` adds the renderer's textures (`textures`, and the `bytes` their texels take).

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
it to C++, and each GDScript and C# file a C++ skeleton in the gameplay sources
([Godot](godot-import.md#scripts), [Unity](unity-import.md#scripts)) whose component the scenes
fill in; in a session with a
gameplay host, as slash-cli's and slash-mcp's have, those components are stand-ins that keep
their values until the code is built. A game exported with Godot 4, its `.pck` file or its executable with the pack inside,
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
importing again rewrites the converted files, keeps each task's status and leaves skeletons
that exist as they are. The result counts what converted, lists each script's task (and, for a
skeleton, its `code` file and `component`; `module` is the game module it wrote, if any), and
lists what didn't convert, once per kind with how
many, the first place it was seen and why. The full report, with the values scripts need, is
written to `import/godot.json` or `import/unity.json`. The settings it writes to `project.json`
(the window, physics, input and audio) apply to the session at the next command.

From [examples/import-godot](../../examples/import-godot/):

```json
{"source": "godot"}
```

```json
{"converted":{"animation_libraries":1,"animation_trees":0,"assets":8,"audio_streams":2,"bus_layouts":1,"input_actions":3,"mesh_libraries":0,"navigation_meshes":0,"process_materials":0,"scenes":2,"sprite_frames":2,"themes":1,"tile_sets":1},"engine":"godot","files":18,"main_scene":"godot/main.scene.json","module":"src/game.cpp","project":"Coin Run","report":"import/godot.json","scripts":[{"code":"src/imported/game_state.cpp","component":"GameState","entities":0,"script":"godot/game_state.gd","task":"task/port-game-state"},{"code":"src/imported/main.cpp","component":"Main","entities":1,"script":"godot/main.gd","task":"task/port-main"},{"code":"src/imported/player.cpp","component":"Player","entities":2,"script":"godot/player.gd","task":"task/port-player"}],"unsupported":[{"count":1,"what":"autoload","where":"project.godot: GameState","why":"autoloads are gameplay code; see the storyboard task"},{"count":1,"what":"file type .svg","where":"icon.svg","why":"textures are PNG, JPEG or BMP files; save it as PNG"},{"count":1,"what":"node type Timer","where":"main.tscn: Timer","why":"timers are gameplay code: count the ticks in a C++ system"}]}
```

From [examples/import-unity](../../examples/import-unity/), where `pixels_per_unit` is the
scale Unity's units convert at:

```json
{"source": "unity"}
```

```json
{"converted":{"animation_libraries":1,"assets":9,"bus_layouts":1,"input_actions":12,"prefabs":3,"scenes":1,"tile_sets":1},"engine":"unity","files":18,"main_scene":"unity/Assets/Scenes/Pond.scene.json","module":"src/game.cpp","pixels_per_unit":32.0,"project":"Pond Hop","report":"import/unity.json","scripts":[{"code":"src/imported/camera_follow.cpp","component":"CameraFollow","entities":1,"script":"unity/Assets/Scripts/CameraFollow.cs","task":"task/port-camera-follow"},{"code":"src/imported/coin.cpp","component":"Coin","entities":6,"script":"unity/Assets/Scripts/Coin.cs","task":"task/port-coin"},{"code":"src/imported/frog.cpp","component":"Frog","entities":2,"script":"unity/Assets/Scripts/Frog.cs","task":"task/port-frog"},{"code":"src/imported/hud.cpp","component":"Hud","entities":1,"script":"unity/Assets/Scripts/Hud.cs","task":"task/port-hud"}],"unity_version":"2022.3.20f1","unsupported":[{"count":1,"what":"component EventSystem","where":"Assets/Scenes/Pond.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"},{"count":1,"what":"component StandaloneInputModule","where":"Assets/Scenes/Pond.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"},{"count":1,"what":"exposed mixer parameters","where":"Assets/Audio/Pond.mixer","why":"MusicVolume: scripts that set them set buses with audio.set_bus or gameplay code instead"},{"count":1,"what":"inactive object","where":"Assets/Scenes/Pond.unity: Coin (2)","why":"converted hidden, with its shapes disabled; there is no inactive state, so its other components still run"},{"count":1,"what":"input axis Mouse X","where":"ProjectSettings/InputManager.asset","why":"mouse movement axes: read the mouse's motion in code"},{"count":1,"what":"input binding","where":"Assets/Input/Controls.inputactions: Player/Croak","why":"\"<XRController>/trigger\": XRController bindings"},{"count":1,"what":"input interaction","where":"Assets/Input/Controls.inputactions: Player/Croak","why":"Hold: gameplay code times holds and taps"}]}
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
{"converted":{"animation_libraries":0,"animation_trees":0,"assets":2,"audio_streams":0,"bus_layouts":0,"input_actions":1,"mesh_libraries":0,"navigation_meshes":0,"process_materials":0,"scenes":1,"sprite_frames":0,"themes":0,"tile_sets":0},"engine":"godot","files":15,"main_scene":"godot/main.scene.json","module":"src/game.cpp","project":"Jumper","report":"import/godot.json","scripts":[{"code":"src/imported/player.cpp","component":"Player","entities":1,"script":"godot/player.gd","task":"task/port-player"}],"unpacked":{"files":8,"folder":"godot","fonts":0,"pack":{"embedded":false,"files":10,"format":3,"godot":"4.5.0"},"resources":0,"scenes":1,"scripts":1,"scripts_from_tokens":1,"sounds":1,"textures":1,"unsupported":[]},"unsupported":[]}
```

For a Unity build, `unpacked` names the build's `platform`, scripting `backend` and `unity`
version, AssetRipper's version, the folder `unpack_to` wrote, counts of scenes, prefabs, scripts,
`package_scripts` and files, the `log` of AssetRipper's run and its own `unsupported`. From
[examples/unpack-unity](../../examples/unpack-unity/):

```json
{"source": "shipped/Hopper", "unpack_to": "unity"}
```

```json
{"converted":{"animation_libraries":0,"assets":1,"bus_layouts":0,"input_actions":0,"prefabs":0,"scenes":1,"tile_sets":0},"engine":"unity","files":20,"main_scene":"unity/Assets/Scenes/Main.scene.json","module":"src/game.cpp","pixels_per_unit":16.0,"project":"Hopper","report":"import/unity.json","scripts":[{"code":"src/imported/hopper.cpp","component":"Hopper","entities":1,"script":"unity/Assets/Scripts/Assembly-CSharp/Hopper.cs","task":"task/port-hopper"}],"unity_version":"2022.3.20f1","unpacked":{"assetripper":"2.0.0","build":{"backend":"il2cpp","platform":"linux","unity":"2022.3.20f1"},"files":5,"folder":"unity","log":".slash/import/unity/assetripper.log","package_scripts":4,"prefabs":0,"scenes":1,"scripts":1,"unsupported":[{"count":1,"what":"script without its code","where":"Assets/Scripts/Assembly-CSharp/Hopper.cs","why":"the build's code couldn't be read back (IL2CPP builds keep it compiled), so only the script's name and the objects using it came back"}]},"unsupported":[{"count":1,"what":"component EventSystem","where":"Assets/Scenes/Main.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"},{"count":1,"what":"component StandaloneInputModule","where":"Assets/Scenes/Main.unity: EventSystem","why":"not needed: SlashEngine's gui system sends input to controls itself"}]}
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
{"bytes":5335403,"executable":"build/export/linux/mygame","icon":"default","linux":{"arch":"x86_64"},"log":".slash/export/linux/export.log","output":"build/export/linux","platform":"linux","preset":"linux","seconds":66.6,"test":{"entities":0,"passed":true,"ticks":60}}
```

A bad preset name fails with `-32002` before anything is built; any other failure, including a
failed headless test, fails with `-32004`. Either way `data` carries the export's error details:
the failed `step`, the validation `errors` or the end of the build log in `tail`, and the `log`
path.

```json
{"code":-32002,"data":{"step":"options"},"message":"no preset or platform called \"macso\"; did you mean \"macos\"?"}
```

### texture.forms

Makes the [forms](project-format.md#texture-forms) exports ship the project's textures as, as
`project.json`'s `rendering.textures` says, and reports them: BC7 or ASTC 4x4 blocks, or RGBA8
texels, each in a KTX2 file. Forms are made once and kept in the asset library's cache, where
exports find them. Registered next to `export.run`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `platforms` | array | every platform | Platforms to ship for: `macos`, `windows`, `linux`, `web`, `android`, `ios` |
| `paths` | array | every texture | Textures, or folders ending in `/`, to report |

Needs: `read`, `filesystem`. It changes no scene, so there is nothing to undo.

```json
{"platforms": ["linux", "android", "web"]}
```

```json
{"made":2,"platforms":{"android":{"bytes":2100,"gpu_bytes":2192},"linux":{"bytes":2100,"gpu_bytes":2192},"web":{"bytes":3284,"gpu_bytes":2192}},"textures":[{"bytes":2882,"forms":[{"bytes":1184,"cached":false,"format":"bc7","gpu_bytes":1024,"psnr":39},{"bytes":1184,"cached":false,"format":"astc_4x4","gpu_bytes":1024,"psnr":40}],"mode":"vram_compressed","path":"sprites/hero.png","size":[32,32]},{"bytes":174,"forms":[],"mode":"vram_compressed","note":"its sides, 6x6, aren't multiples of 4, as BC7 and ASTC 4x4 blocks need, so it ships as it is","path":"sprites/spark.png","size":[6,6]},{"bytes":742,"forms":[],"mode":"lossless","path":"ui/icon.png","size":[16,16]}]}
```

Each texture has its compress `mode`, `size`, file `bytes` and its `forms`: each format's KTX2
file `bytes`, what it takes on the GPU in `gpu_bytes`, how close its colors stay in `psnr`
(decibels; left out when the texels are exact, and above about 40 is hard to tell apart) and
whether it came from the cache. A texture that ships as it is has no forms; one whose sides
aren't multiples of 4 says so in `note`, and a file that isn't an image has an `error`. Each
platform's `bytes` and `gpu_bytes` total what it ships and what the textures take on its GPU,
the files shipped as they are counted at 4 bytes a texel. `made` counts the forms made rather
than read from the cache. A path outside the project fails with `-32002` and the JSON Pointer of
the bad one in `data.path`. [examples/texture-forms](../../examples/texture-forms/) makes a
project's forms and reads them as a game does.

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
(relative to the project when inside it), `line`, `column` and `message`, and `fix` and `notes`
when the compiler gives them (as [`build.check`](#buildcheck) returns them); `data.log` is the
full build log. Every file is compiled, past those that fail, so all their errors come back. A project with no `gameplay.sources` fails with `-32002` and `data.step` `"options"`;
a build that fails without compiler errors, such as a link error, fails with `-32004` and the end
of the log in `data.tail`.

```json
{"code":-32002,"data":{"errors":[{"column":97,"file":"src/game.cpp","fix":[{"column":97,"end_column":102,"line":16,"text":"bonus"}],"line":16,"message":"'struct {anonymous}::Wallet' has no member named 'bonsu'; did you mean 'bonus'?"}],"log":".slash/gameplay/build.log","step":"build"},"message":"the game's code has 1 error"}
```

### build.check

Compiles the gameplay sources that changed since the last check or build, and those including a
header that changed, without linking or loading anything, and returns every error and warning as
data. Use it after each edit to the game's code: it compiles only what changed, and the objects
it compiles are the ones [`gameplay.build`](#gameplaybuild) links next. Registered by `slash-cli`
and `slash-mcp`, on macOS and Linux.

No params. Needs: `filesystem`, `build`.

```json
{}
```

```json
{"checked":["src/game.cpp"],"errors":[{"column":32,"file":"src/game.cpp","fix":[{"column":32,"end_column":37,"line":18,"text":"income"}],"line":18,"message":"'struct {anonymous}::Wallet' has no member named 'incme'; did you mean 'income'?"}],"ok":false,"warnings":[{"column":15,"file":"src/game.cpp","line":25,"message":"unused variable 'limit' [-Wunused-variable]"}]}
```

| Field | Meaning |
| --- | --- |
| `ok` | No errors |
| `checked` | The files compiled, relative to the project; `[]` when nothing changed |
| `errors`, `warnings` | At most 20 of each, left out when there are none: `file` (relative to the project when inside it), `line`, `column` and the compiler's `message` |
| `fix` | The compiler's suggested fix, when it has one, as edits: replace from `line`:`column` up to, but not including, `end_column` with `text`. Columns count bytes from 1; `end_line` and `file` appear when the edit ends on another line or is in another file |
| `notes` | At most three of the compiler's notes, such as where a name it suggests is declared: `file`, `line`, `column` and `message` |

A file that fails is compiled again by the next check until it compiles; one that compiled is
not, so its warnings come back only after it changes. An error repeated in each file that
includes a header is listed once. Checks fail as `gameplay.build` does when they can't run: a
project with no `gameplay.sources` with `-32002` and `data.step` `"options"`, a build that fails
without compiler errors with `-32004`, the end of the log in `data.tail` and the log in
`data.log` (`.slash/gameplay/check.log`).

### api.search

Finds C++ functions, types and macros by name or by what they do, in the engine's public headers
(`engine/<module>/include/`) and the project's gameplay sources, and returns each one's
signature, its doc comment, where it is declared and lines that use it: from the engine's
[examples](../../examples/), which its tests build and run, or from the project's other files.
Use it before writing a call instead of guessing a name. The sources are read as text, as
[`project.map`](#projectmap) reads them; the engine's are those exports build from
(`SLASH_ENGINE_DIR`, an installed copy, or the checkout the tools were built from). Registered by
`slash-cli` and `slash-mcp`, running games included.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `query` | string | required | A name (`World::find`, `is_action_pressed`) or words for what it does (`play a sound`) |
| `in` | array | both | Only `engine` or `project` |
| `kind` | string | all | Only `function`, `type` or `macro` |
| `limit` | integer | 8 | Most results, 1 to 50 |
| `examples` | integer | 2 | Most example lines per result, 0 to 10 |

Needs: `read`.

```json
{"query": "emit an event", "limit": 1}
```

```json
{"results":[{"at":"engine/world/include/slash/world/world.hpp:248","doc":"Emits a gameplay event, such as emit(\"player_died\", \"player\", {{\"cause\", \"lava\"}}). Kinds starting with \"entity.\" and the kind \"command\" are the engine's own.","examples":[{"at":"examples/physics-2d/main.cpp:64","code":"world.emit(\"coin_taken\", world.id_of(coin), {});"},{"at":"examples/project-map/src/coins.cpp:37","code":"world.emit(\"coin_collected\", event.id);"}],"include":"slash/world/world.hpp","kind":"function","name":"slash::world::World::emit","signature":"void emit(std::string kind, std::string_view id = {}, json::Value data = {})"}]}
```

| Field | Meaning |
| --- | --- |
| `name` | Qualified with its namespaces and classes; a macro's is its own |
| `kind` | `function`, `struct`, `class`, `union`, `enum`, `alias` or `macro` |
| `signature` | Its declaration on one line: a function's without its body, a type's head, a macro's parameters; `overloads` has a function's others |
| `doc` | The comment above it, left out when there is none |
| `at` | Where it is declared, `path:line`; `defined` is where a project function declared there is defined |
| `include` | How code includes an engine header: `#include "slash/world/world.hpp"` |
| `fields` | A struct's or class's public data members as declared, each with the first sentence of its comment, or an enum's values |
| `members` | A type's public member functions, by name |
| `examples` | The first line of each file that uses it, `at` and `code`: a file of the engine's examples that includes its header, or another file of the project |

A name finds itself first (`World::find` before other `find`s); words find the names, then the
classes, namespaces and headers, then the docs they appear in, so `play a sound` finds
`audio::play_stream` and the `AudioStreamPlayer` component. Words match across forms ("plays",
"playing"). A line counts as using a member function only when it calls it through its class or
on something named like it (`game_world.find` for `World::find`), so another class's `find` is
not an example. Private and protected members and `detail` namespaces are not the engine's API
and are left out. When nothing matches, `did_you_mean` names the closest name.

```json
{"did_you_mean":"chase","results":[]}
```

## AI

Language models from the project's [`providers.json`](providers.md), the same ones a game calls
through `ai::Provider`. Registered by `slash-cli` and `slash-mcp`, running games included.

### ai.providers

Lists the providers `providers.json` configures, by name, with each one's type, model and URL
(the vendor's when the file sets none), and for a provider that needs a key, the variable it
reads and whether it is set. Never returns a key. Then the roles, the asset library's
[generators](providers.md#generators) and the replay file with its mode (after
`SLASH_AI_REPLAY`). A project without the file has none.

No params. Needs: `read`.

```json
{}
```

```json
{"providers":[{"api_key_env":"ANTHROPIC_API_KEY","key_set":false,"model":"<model id>","name":"hosted","type":"anthropic","url":"https://api.anthropic.com"},{"model":"llama3.2","name":"local","type":"openai","url":"http://localhost:11434/v1"}],"replay":{"file":"tests/ai.replay.jsonl","mode":"auto"},"roles":{"default":{"provider":"local"},"dialogue":{"provider":"local"}}}
```

### ai.complete

Asks a model and returns its reply. Tool calls are returned, not run, so the caller decides what
the model may do. Needs: `read`, `network`. It changes nothing in the world.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `role` | string | the `default` role, or the only provider | A role or provider from `providers.json` |
| `prompt` | string | | What to ask, as a user message after `messages` |
| `system` | string | | Instructions, put first (after the role's own) |
| `images` | string[] | | Project paths of images sent with the prompt: `.png`, `.jpg`, `.gif` or `.webp` |
| `messages` | object[] | | The conversation so far, before the prompt: `role` (`system`, `user`, `assistant` or `tool`), `text`, and `images`, `tool_calls`, `tool_call_id`, `is_error` or `native` as the role takes them |
| `tools` | string[] | | Commands the model may ask to call, such as `world.query` |
| `output_schema` | object | | A JSON Schema the reply must match; `output` holds the parsed value |
| `max_tokens` | integer | the role's, then the provider's | The most tokens to reply with |
| `temperature`, `stop`, `seed` | | the role's | Sampling settings, where the model takes them |

A request needs `prompt` or `messages`. The reply has `text`, `stop` (`end`, `tool_calls`,
`max_tokens`, `stop_sequence`, `refused` or `other`), `usage`, the `provider` and `model` that
answered, `output` with an `output_schema`, and `"replayed": true` when it came from the replay
file:

```json
{"role": "dialogue", "prompt": "The guard, Bram: name and mood.", "output_schema": {"type": "object", "required": ["name", "mood"], "properties": {"name": {"type": "string"}, "mood": {"type": "string"}}}}
```

```json
{"model":"llama3.2","output":{"mood":"suspicious","name":"Bram"},"provider":"local","stop":"end","text":"{\"name\": \"Bram\", \"mood\": \"suspicious\"}","usage":{"input_tokens":41,"output_tokens":12}}
```

With `tools`, a reply that asks for calls lists them in `tool_calls` by `command`, and gives the
assistant `message` to pass back in `messages`, followed by a `tool` message per call with its
`tool_call_id` and the command's result as `text`:

```json
{"prompt": "Put a guard at the gate, at x 120, y 40.", "tools": ["entity.create"]}
```

```json
{"message":{"role":"assistant","tool_calls":[{"arguments":{"components":{"Transform2D":{"position":[120,40]}},"id":"guard"},"id":"call_1","name":"entity_create"}]},"model":"llama3.2","provider":"local","stop":"tool_calls","text":"","tool_calls":[{"arguments":{"components":{"Transform2D":{"position":[120,40]}},"command":"entity.create","id":"call_1"}],"usage":{"input_tokens":41,"output_tokens":12}}
```

Errors, with `data.path` pointing at the param where there is one: no `prompt` or `messages`
with `-32602`; an unknown role or command with `-32001` and the closest name, and a missing image
with `-32001`;
a provider whose key is not set with `-32003`; a request the replay file has no recording of, in
`replay` mode, with `-32001`; an image of another type, or a reply that does not match
`output_schema`, with `-32002`; and a provider's own error with its HTTP status and message,
coded as the status says (`-32002` for a bad request, `-32003` for a refused key, `-32001` for an
unknown model and `-32004` for the rest).

```json
{"code":-32001,"data":{"path":"/role"},"message":"no role or provider named \"dialog\"; did you mean \"dialogue\"?"}
```

## Asset library

Images, sounds and music kept once on the machine, so agents reuse them instead of generating
them again. The library is `~/.slash/library`, or `SLASH_LIBRARY_DIR`; every project on the machine
searches the same one. Each asset is stored by the SHA-256 of its bytes, with how it was made (the
request, generator, model and seed), its license, tags and a short description agents read
instead of the file. IDs in replies are the hash's first 12 hex digits, and any unique start of 6
or more finds an asset. Using an asset copies it into the project, so exports never depend on the
library, and records where it came from in the project's `library.json`:

```json
{
  "assets": {
    "art/slime.png": {"generator": "test", "id": "0b2fd1b5cf5a2939ad6f5c40396f95aedf766757807aea0e129ccbcaf338edc7", "license": "CC0-1.0", "model": "test"}
  }
}
```

Generators come from the project's [`providers.json`](providers.md#generators): OpenAI and
Gemini image models, a Stable Diffusion web UI on the machine, OpenAI's text to speech,
ElevenLabs' sound effects and music, and Stable Audio's music. The built-in `test` generator makes
placeholders with no model or key. Sessions that run tests write nothing
to the library. Registered by `slash-cli` and `slash-mcp`, running games included.
[examples/asset-library](../../examples/asset-library/) has two projects sharing one library.

#### Music

Music is an asset of type `music`, a `.wav`, `.ogg` or `.mp3` file kept with its `bpm`,
`beat_count`, `bar_beats` and `key`, so agents find a theme by its tempo and key and games time
transitions by its beats. The library measures each piece with [`audio.analyze`](#audioanalyze):

- Music a generator made keeps what it measures, and the tempo and key asked for only where it
  measures none, or where its tempo is twice or half the one asked for. A reply's `problems`
  says where they disagree, such as `"it measures 140 bpm, not the 100 bpm asked for"`.
- A composer's track added with [`asset.add`](#assetadd) keeps the `bpm`, `key` and
  `beat_count` its author gave, and `problems` says where the sound measures otherwise.
- A tempo whose beats nearly fill the sound is set so whole beats fill it, so the music loops on
  the beat.

Copied into a project, music gets the [`.audio.json`](scene-format.md#audio-streams) settings
that play it beside it (`music/harbor.wav` gets `music/harbor.audio.json`): its `source`, `bpm`,
`beat_count`, `bar_beats` when not 4, and with `loop`, `"loop_mode": "forward"` for a `.wav`
file or `"loop": true` for an `.ogg` or `.mp3` one. A settings file already there for the same
source keeps its other fields. The reply lists the files written in `files`.

A track can have stems, each music of its own as long as the mix, with the track's timing,
author and license, the tag `stem` and `track`, the mix's ID. [`asset.use`](#assetuse) with
`"stems": true` copies them into a folder named after the track (`music/boss/drums.wav` and
its settings), and the track's settings become a stream that plays them together, as
[`soundtrack.render`](#soundtrackrender) writes its stems:

```json
{"format": 1, "synchronized": [{"stream": "music/boss/drums.audio.json"}, {"stream": "music/boss/pads.audio.json"}]}
```

[examples/generated-music](../../examples/generated-music/) generates a theme's music and adds a
composer's track with its stems.

#### Team sharing

A team can share a library it hosts, named in [`providers.json`'s `library`](providers.md#team-library)
(or `SLASH_LIBRARY_TEAM` on one machine): a folder every teammate reaches, such as a network
share or a synced folder, or a library server that
[`slash-cli library serve`](cli.md#library-serve) runs. Each machine keeps its own library, and
the commands use the team's beside it:

- `asset.generate` and `asset.refine` answer an exact repeat from the team's library when this
  machine has none (the reply says `"reused": true, "team": true`), offer the team's close
  matches as candidates, and put what they make there, with the request that made it.
  `asset.add` puts the file there too.
- `asset.search` covers the team's assets, ranked by the team's copy of their feedback, which
  holds every teammate's.
- `asset.use`, `asset.info`, `asset.refine` and `asset.feedback` bring an asset the machine lacks
  from the team's library; `asset.info` shows the team's feedback and `shared`, whether
  teammates can find it.
- Feedback, from agents and checks, goes to the team's copy too.
- [`asset.sync`](#assetsync) shares what was made before the team was set up.

A library server needs the `network` capability. When the team's library can't be used (the
share isn't mounted, the server is down or refuses the token, or the session has no `network`),
commands work on the machine's library alone and their replies say why in `team_error`.
[examples/team-library](../../examples/team-library/) has three teammates sharing one.

### asset.search

Finds assets by words, tags and type. Words match descriptions, prompts and tags, a plural
matching its singular; `score` is the share of the words an asset matches, moved up or down 0.05
for each time it was [accepted](#assetfeedback) more than rejected (at most 0.2, and kept from 0
to 1). Results come best first, then the better received, then those whose tags matched more
words, then by ID; each shows how many times it was `accepted` and `rejected`, when it was. With
no `text`, every asset with the tags and type matches. Needs: `read`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `text` | string | | Words to find |
| `tags` | string[] | | Tags every result must have |
| `type` | string | every type | `image`, `sound`, `music` or `model` |
| `limit` | integer | 10 | Most results, up to 50 |

```json
{"text": "heart", "tags": ["ui"]}
```

```json
{"assets":[{"description":"Red heart icon, 8x8","height":8,"id":"19b7f428aefb","score":1.0,"tags":["ui","health"],"type":"image","width":8}]}
```

Music results have their `seconds`, `bpm`, `beat_count`, `bar_beats` and `key`, and a track the
names of its `stems`. Model results have their `size` (meters along x, y and z) and `triangles`.

### asset.generate

Makes an image, a sound or music with a generator and keeps it. An exact repeat of an earlier request
(the same generator, model and params, apart from `description`, `tags`, `path`, `similar` and
`context`) returns the stored asset with `"reused": true` and calls no model. Otherwise, when the
library has assets of the type that score at least 0.75 for the prompt and were not rejected
more than accepted, they come back as `candidates` and nothing is made; pass `"similar": false`
to generate anyway. Needs: `read`, `filesystem`, `network`.

The generator gets more than the prompt, so results fit the game: the
[storyboard](storyboard-format.md)'s `tone` entries, `palette` colors (for images) and `reference`
entries that cover `path` or every asset, up to three more `reference` entries that link assets
of the same type (as [`asset.style`](#assetstyle) drafts them), and the reasons
[feedback](#assetfeedback) gave for
assets like this one (scoring at least 0.5) and for a refine's parent: "What worked before:
..." and "Avoid what was wrong before: ...". The record keeps what was added as
`request.context`. An image copied into the project with `path` is checked against the palettes
and budgets that cover it, as `storyboard.check` would; the reply lists any `problems`, and the
result is recorded as the asset's feedback (source `storyboard.check`).

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `prompt` | string | required | What to make |
| `type` | string | `image` | `image`, `sound` or `music` |
| `generator` | string | the one that makes the type | A generator in `providers.json`, or `test` |
| `width`, `height` | integer | the generator's | An image's size, 1 to 4096 pixels; one alone makes a square. What a service makes at another size is fitted to it |
| `seconds` | number | the generator's | A sound's or music's length, up to 600 |
| `bpm` | number | | Music: the tempo to ask for, 20 to 400 |
| `key` | string | | Music: the key to ask for, such as `"D minor"`, `"F# major"` or `"Bbm"` |
| `bar_beats` | integer | 4 | Music: beats per bar, 2 to 32 |
| `loop` | boolean | false | Music: ask for a loop, and loop it in the settings written beside `path` |
| `theme` | string | | Music: a storyboard [theme](storyboard-format.md#music), such as `"theme/harbor"`: its `key`, `tempo` and `bar_beats` where these params give none, its `instruments` and `mood` added to the prompt, and its ID added to the tags |
| `seed` | integer | | For a repeatable result, where the generator takes one |
| `description` | string | the prompt | A sentence for agents to read instead of the file |
| `tags` | string[] | | Words to find it by |
| `path` | string | | Also copy it into the project here, as [`asset.use`](#assetuse) does |
| `similar` | boolean | true | `false` generates even when there are close matches |
| `context` | boolean | true | `false` sends the prompt alone, without the storyboard's style and notes from feedback |

The reply has the `id`, the image's `width` and `height` or the sound's `seconds`, and `path`
when it was copied; music's has its `bpm`, `beat_count`, `bar_beats` and `key`, and the
settings file in `files` (see [music](#music)):

```json
{"prompt": "green slime sprite", "generator": "test", "width": 32, "tags": ["enemy"], "path": "art/slime.png"}
```

```json
{"height":32,"id":"0b2fd1b5cf5a","path":"art/slime.png","width":32}
```

A close request, from another project:

```json
{"prompt": "a slime sprite, green", "generator": "test"}
```

```json
{"candidates":[{"description":"green slime sprite","height":32,"id":"0b2fd1b5cf5a","score":1.0,"tags":["enemy"],"type":"image","width":32}]}
```

Music in a storyboard theme (G major, 96 bpm, three beats to a bar):

```json
{"type": "music", "prompt": "The harbor at dawn.", "generator": "test", "theme": "theme/harbor", "seconds": 8, "path": "music/harbor.wav", "loop": true}
```

```json
{"bar_beats":3,"beat_count":12,"bpm":96,"files":["music/harbor.audio.json"],"id":"9bcb8cd89a27","key":"G major","path":"music/harbor.wav","seconds":7.5}
```

Music services take the tempo, meter, key and loop in words added to the prompt (see
[generators](providers.md#generators)).

The `test` generator draws a box, or a circle or triangle when the prompt names something round
or pointed, in the colors the prompt names, on a transparent background for a sprite, icon or
cutout; its sounds are tones whose pitch, wave and sweep follow the prompt's words. Its music is a
chord progression with a bass line, in the key asked for (A minor when the prompt has a word
such as "dark", "sad", "tense" or "boss", C major otherwise), at the tempo asked for (120 bpm by
default), in the whole bars nearest `seconds` (four by default), which loop on the beat. Its
license is `CC0-1.0`.

### asset.refine

Makes a new version of an asset, changed as the prompt says, with `parent` naming the one it came
from, which stays to compare and go back to. It takes `asset.generate`'s params apart from `type`
and `similar`; tags default to the parent's, and the description to the parent's followed by the
prompt. A repeat is reused as with `asset.generate`. The parent's generator is used when it was
`test`, otherwise the one that makes the type. Needs: `read`, `filesystem`, `network`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The asset to change |
| `prompt` | string | required, but for a model | What to change |
| `loop` | boolean | false | Music: loop it in the settings written beside `path` |
| `parameters` | object | | A model: new values for its [recipe's](scene-format.md#model-recipes) parameters, numbers or expressions |
| `parts` | object | | A recipe: library models to make its [model parts](scene-format.md#model-recipes) of instead, by the IDs (or starts of them) of the ones they replace |

A [model recipe](scene-format.md#model-recipes) is refined by its `parameters` instead, and no
generator is called: the new version is the recipe with those values, checked as it builds,
with the parent's description (followed by `prompt` when there is one), tags, license and
author. A name the recipe has no parameter for is reported with the closest one, and a value
that makes the recipe fail to build, or fail the parent's checks, is reported at `/parameters`. Only `description`, `tags`,
`prompt` and `path` go with it. A recipe made of other models takes `parts` too, or instead:
each model it is made of that `parts` names becomes the model it maps to, kept in the
request's `parts` by full IDs. Naming a model the recipe isn't made of, or something that isn't
a model, is reported at `/parts/<id>`. A `.glb` model has no parameters and no generator refines it yet:
edit the file and [`asset.add`](#assetadd) it again, which keeps it as a new version.

```json
{"id": "edc8a071", "parameters": {"length": 2.4}, "prompt": "long enough for four", "path": "props/long_bench.model.json"}
```

```json
{"id":"55d18dff10bf","parent":"edc8a0716505","path":"props/long_bench.model.json","size":[2.4,0.45,0.35],"triangles":36}
```

The picnic set made of the bench's deeper version, made of the long bench instead:

```json
{"id": "9552a4cb", "parts": {"193f1c58": "55d18dff"}, "description": "Pine picnic table with long benches"}
```

```json
{"id":"b74a378241a0","parent":"9552a4cbc07f","size":[2.4,0.75,1.55],"triangles":84}
```

```json
{"id": "0b2fd1b5", "prompt": "make it red", "description": "Red slime sprite", "path": "art/slime_red.png"}
```

```json
{"height":32,"id":"833c6cd1f6e5","parent":"0b2fd1b5cf5a","path":"art/slime_red.png","width":32}
```

The `test` generator recolors an image to a color the prompt names, mirrors, darkens or lightens
it, and makes a sound higher, lower, shorter, longer, louder, quieter or reversed. Its own music
it makes faster or slower (by a quarter), higher or lower (by a whole tone) and longer or shorter
(twice or half the bars), keeping the meter; the music services don't refine music. A prompt it
understands none of makes a variation.

### asset.info

Returns an asset's whole record: `type`, `format`, `bytes`, size or length (`seconds`,
`channels` and `sample_rate`; a model's `size`, `triangles`, `pivot` and the names of its `parts` and attachment `points`, as [`model.inspect`](#modelinspect)'s brief form gives them), `description`, `tags`, the `request` its generator was asked,
`generator`, `model`, `seed`, `license`, `author` and `parent`; music's `bpm`, `beat_count`,
`bar_beats` and `key`, a track's `stems` (`name` and `id` each) and a stem's `track`; a recipe's
`uses`, the library models its parts are made of; then `versions`, every version before it,
oldest first, `children`, the versions made from it, and `used_by`, the recipes on this machine
made of it. Needs: `read`.

```json
{"id": "833c6cd1"}
```

```json
{"bytes":303,"description":"Red slime sprite","format":"png","generator":"test","height":32,"id":"833c6cd1f6e5","license":"CC0-1.0","model":"test","parent":"0b2fd1b5cf5a","request":{"parent":"0b2fd1b5cf5a","prompt":"make it red","type":"image"},"seed":0,"tags":["enemy"],"type":"image","versions":["0b2fd1b5cf5a"],"width":32}
```

### asset.use

Copies an asset into the project and records it in `library.json`. The path keeps the asset's
format: a `.png` stays a `.png`. An image is checked against the storyboard's palettes and
budgets that cover the path, as with `asset.generate`'s `path`, and the reply lists any
`problems`. Music gets its settings beside it, and with `stems` its stems as layers (see
[music](#music)). A recipe made of other models comes with them: each is copied into a `parts`
folder beside it, named by its ID (`props/parts/193f1c58e2f7.model.json`), recorded in
`library.json` and listed in the reply's `parts`, and the copy's model parts name those paths.
Needs: `read`, `filesystem`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The asset, or the start of its ID |
| `path` | string | required | Where in the project |
| `loop` | boolean | false | Music: loop it in its settings |
| `stems` | boolean | false | Music: also copy its stems into a folder named after `path`, and make the settings play them together |

```json
{"id": "19b7f428", "path": "ui/heart.png"}
```

```json
{"path":"ui/heart.png"}
```

```json
{"id": "bce0fcbc", "path": "music/boss.wav", "stems": true, "loop": true}
```

```json
{"files":["music/boss/drums.wav","music/boss/drums.audio.json","music/boss/pads.wav","music/boss/pads.audio.json","music/boss.audio.json"],"path":"music/boss.wav"}
```

```json
{"id": "9552a4cb", "path": "props/picnic.model.json"}
```

```json
{"parts":["props/parts/193f1c58e2f7.model.json"],"path":"props/picnic.model.json"}
```

### asset.add

Adds a project's `.png`, `.bmp`, `.wav`, `.ogg`, `.mp3`, `.model.json` or `.glb` file to the
library, with its `description`, `tags`, `license` and `author`, and records it in
`library.json`. A [model recipe](scene-format.md#model-recipes) must build as written and name no
texture files, whose paths are a project's, and a `.glb` model must hold its own buffers and
images; models are kept with what [`model.inspect`](#modelinspect)'s brief form gives: `size`,
`triangles`, `pivot`, `parts` and `points`. A file the library already holds keeps its record,
with the new tags added; a sound added again as music becomes music. A file the project took
from the library (as `library.json` says) and changed since is a new version of that asset: its
`parent` is the one it came from, and its description, tags, license and author are that one's
unless the call gives them, and the reply's `used_by` lists the recipes made of the version
before, which [`asset.refine`](#assetrefine)'s `parts` moves to it. A recipe made of other model
files ([model parts](scene-format.md#model-recipes)) goes in with them: each must be a `.glb` or
`.model.json` file, and goes in first where the library lacks it (as a new version of the asset
`library.json` says it came from, or described as a part of the recipe, with its tags, license
and author); the library's copy of the recipe names each by its library ID
(`"asset:<id>.glb"`), since the project's paths mean nothing elsewhere, and the reply's
`model_parts` gives each file's ID. A model added with `checks` ([model
checks](scene-format.md#model-checks): size, pivot, triangles, materials, nodes, bones,
animations) must pass them, failing at `/checks/...`; the library keeps them, every new version
must pass them unless the call gives new ones (`/path: it fails the checks of the version
before: checks/nodes/0: ...`), and `library.json` keeps them beside the file for
[`asset.check`](#assetcheck). A recipe's own `checks` hold as well. Music is measured, keeping
the facts its author gives (see [music](#music)), and a track comes with its stems. Needs:
`read`, `filesystem`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | required | The file; a track's full mix |
| `description` | string | the last version's | A sentence for agents to read instead of the file; required for a file that isn't a new version |
| `tags` | string[] | | Words to find it by |
| `license` | string | | Its license, such as `CC0-1.0` |
| `author` | string | | Who made it |
| `type` | string | by the file | `music` for a track; `stems`, `bpm` and the rest imply it |
| `bpm` | number | measured | Music: its tempo, as its author gives it |
| `key` | string | measured | Music: its key, such as `"D minor"` |
| `bar_beats` | integer | 4 | Music: beats per bar |
| `beat_count` | integer | the whole beats in it | Music: its length in beats |
| `stems` | object | | Music: its stems by name (lowercase letters, digits, `_` and `-`), each a sound file within a second of the mix's length, such as `{"drums": "music/stems/drums.wav"}`; up to 32 |
| `checks` | object | the last version's | A model: what it and its versions must be, as a recipe's [`checks`](scene-format.md#model-checks) |

```json
{"path": "ui/heart.png", "description": "Red heart icon, 8x8", "tags": ["ui", "health"], "license": "CC-BY-4.0", "author": "Mira"}
```

```json
{"height":8,"id":"19b7f428aefb","type":"image","width":8}
```

```json
{"path": "delivery/boss.wav", "description": "Boss fight, drums and pads.", "author": "Ana", "license": "CC-BY-4.0", "bpm": 140, "key": "E minor", "stems": {"drums": "delivery/boss_drums.wav", "pads": "delivery/boss_pads.wav"}}
```

```json
{"beat_count":16,"bpm":140,"id":"bce0fcbcf493","key":"E minor","seconds":6.857,"stems":["drums","pads"],"type":"music"}
```

The game's copy of a bench, its seat made deeper by hand:

```json
{"path": "props/bench.model.json"}
```

```json
{"id":"193f1c58e2f7","parent":"edc8a0716505","size":[1.2,0.45,0.45],"triangles":36,"type":"model"}
```

A picnic set made of that bench:

```json
{"path": "props/picnic.model.json", "description": "Pine picnic table with a bench on each side", "tags": ["prop"], "license": "CC0-1.0"}
```

```json
{"id":"9552a4cbc07f","model_parts":{"props/bench.model.json":"193f1c58e2f7"},"size":[1.2,0.75,1.65],"triangles":84,"type":"model"}
```

### asset.feedback

Records whether an asset did the job and why, from an agent, a playtest or a check. The library
keeps an asset's last 20. Checks record their own: `storyboard.check` when an image is put in a
project or [`asset.check`](#assetcheck) runs, and `asset.check` for how scenes show it, each
only when its result changes. Feedback moves the asset in [`asset.search`](#assetsearch), keeps
assets rejected more than accepted out of `asset.generate`'s candidates, and its reasons go into
later prompts for assets like it. Needs: `read`, `filesystem`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | The asset, or the start of its ID |
| `verdict` | string | required | `accepted` or `rejected` |
| `reason` | string | | What worked or what was wrong, in a few words; up to 300 characters |
| `source` | string | `agent` | Who judged it, such as `playtest` |

```json
{"id": "19b7f428", "verdict": "rejected", "reason": "too small to read on a phone"}
```

```json
{"id":"19b7f428aefb","rejected":1}
```

Errors, with `data.path` pointing at the param: an unknown generator with `-32001` and the
closest name; no generator for the type, several and none named, or one that makes no such type,
with `-32001` or `-32002`; a size for a sound, `seconds` for an image, or a `path` of the wrong
type with `-32602`; an unknown or ambiguous ID with `-32001` or `-32002`; a path outside the
project with `-32003`; and, in a session that runs tests, any write to the library with
`-32003`. When `path` fails after the asset was made, the error's `data.id` names it, for
`asset.use`.

```json
{"code":-32001,"data":{"path":"/generator"},"message":"/generator: no generator named \"tset\"; did you mean \"test\"?"}
```

### asset.check

Checks the files the project took from the library (those `library.json` lists) as its scenes
show them, and records what it finds as each asset's [feedback](#assetfeedback), so search,
candidates and later prompts learn from it with no agent asking. [`test.run`](#testrun) runs it
on every whole-project run. Needs: `read`, `filesystem`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `scenes` | string array | every scene that draws one | The scenes to look at |
| `ticks` | integer, 0 to 600 | `0` | Ticks each scene runs before it is looked at |

Each scene opens in a session of its own that writes nothing, and its frame is summarized as
[`frame.summary`](#framesummary) does with `contrast`. A scene draws a library file when it
names it, or names a file that does (a prefab, sprite frames, a tile set, a theme), up to four
files deep. Where a sprite, tile map layer, particle emitter or control draws one, it is
checked:

- hard to see: its edge has less than 1.5:1 contrast with the colors around it;
- drawn at a quarter of its size or less, where a smaller image would look the same and cost
  less memory and download.

Images are also checked against the storyboard's palettes and budgets that cover their path,
models against the budgets that cover theirs, and models against the [checks](scene-format.md#model-checks) `library.json` keeps with them,
which needs no library: such a problem has the `check`'s JSON Pointer into `library.json`
(`/assets/props~1lamp.model.json/checks/nodes/0`), and `test.run` fails at it.
An asset with a problem is recorded `rejected`, the reason its first problem and the scene, and
one that shows clearly everywhere it shows is recorded `accepted`; an asset that doesn't show
gets nothing. A result the same as the last from that check isn't recorded again, and a session
that runs tests records nothing. Without a library on the machine, or for files from another
machine's library, it still reports.

The result counts the assets `checked` and `scenes` opened, and lists `problems`, each with the
asset's `id` and `path`, the `scene` and `entity` where it was seen (none for a storyboard
problem), and the `message`; a scene that doesn't open is a problem with its `scene` and
`message`. From [examples/asset-library](../../examples/asset-library/), a red slime with a dark
outline on a night level:

```json
{}
```

```json
{"checked":3,"problems":[{"entity":"red_slime","id":"833c6cd1f6e5","message":"hard to see: its edge has 1.2:1 contrast with the darker colors around it","path":"art/slime_red.png","scene":"levels/night.scene.json"}],"scenes":1}
```

### asset.style

Drafts edits that refine the storyboard's [style section](storyboard-format.md#refining-the-style-from-accepted-assets)
from the library assets the project uses (those `library.json` lists, each asset once) that a
person or agent last judged `accepted`; a check's verdict doesn't count. They are returned as
`edits` that [`storyboard.edit`](#storyboardedit) takes as they are, with `accepted`, the count
of assets read. Change what should differ before passing them on; nothing is written.

- Palettes: a `palette` covering an accepted image gains the colors it uses that the palette
  lacks (at most 16 a call), named in words; with no palette at all, `palette/main` takes the
  colors of images drawn in at most 16 flat colors, with `tolerance` 8, linked to their folders.
- Budgets: `triangles` and `texture_size` where no budget sets them for an accepted model or
  image, the most an accepted one has rounded up (two significant digits, a power of two), on
  the budget for everything, else a new `budget/assets`.
- References: a `reference/<file name>` for each accepted asset with a reason that no reference
  links, the description as its name and the reason as its summary.

Takes no params. Needs: `read`, `filesystem`. From [examples/asset-style](../../examples/asset-style/),
a cafe with a tone and nothing else, after a stool, a cup and a sign were accepted:

```json
{}
```

```json
{"accepted":3,"edits":[{"id":"palette/main","set":{"colors":{"dark_green":"#2a3a30","light_orange":"#e8d8b0"},"links":["art"],"name":"Main palette","summary":"The colors of the accepted images.","tolerance":8},"version":null},{"id":"budget/assets","set":{"name":"Asset budgets","summary":"What the accepted assets need at most.","texture_size":32,"triangles":96},"version":null},{"id":"reference/sign","set":{"links":["art/sign.png"],"name":"A chalkboard sign","summary":"two flat colors, a thick frame"},"version":null},{"id":"reference/cup","set":{"links":["props/cup.model.json"],"name":"A cup","summary":"reads at a glance on the counter"},"version":null},{"id":"reference/stool","set":{"links":["props/stool.model.json"],"name":"A cafe stool","summary":"eight-sided parts, flat oak and steel"},"version":null}]}
```

### asset.sync

Puts the assets the project's `library.json` names in the [team's library](#team-sharing)
where it lacks them, with the versions they were refined from and the requests that made them,
so teammates find them. Generated, refined and added assets go there by themselves; this is for
those made before the team was set up. Fails with `-32001` when no team is named. Needs: `read`,
`filesystem` (and `network` for a library server).

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `all` | boolean | `false` | Every asset in the machine's library instead |

The reply names the `team` and lists the IDs `shared`:

```json
{}
```

```json
{"shared":["9d1bc8586210"],"team":"team"}
```

## Adding a command

Register it in `engine/command/src/builtin.cpp` (or a registry of your own) with a name, a short
description, its capabilities, an `Effect` and a params schema with
`"additionalProperties": false`, add a test to `tests/test_command.cpp`, and add a section to this
page. See [AGENTS.md](../../AGENTS.md#commands-mcp-and-json-rpc).
