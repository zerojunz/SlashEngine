# Getting started

This walks through building SlashEngine, running a scene, changing it with commands, and
connecting an AI agent to it. It takes a few minutes once the build is done. Every result shown
here is real output from the engine.

SlashEngine is early: it runs worlds of entities and components, saves them as JSON scenes and
draws 2D sprites with animations, materials, tile maps and lights, but has no physics, audio or
3D yet (see the [roadmap](roadmap.md)). What works today is the part agents use most: scenes, the command API,
the servers that expose it, and exporting a game for macOS, Windows, Linux, the web, Android and
iOS.

## 1. Build

You need CMake 3.25 or newer, Ninja and a C++20 compiler (Apple Clang, Clang 16+, GCC 12+ or MSVC
19.3x). Everything else is downloaded by CMake at pinned versions. On Linux, SDL3 also needs the
X11 and Wayland development packages listed under `LINUX_SDL_DEPS` in
[.github/workflows/ci.yml](../.github/workflows/ci.yml).

```bash
cmake --preset dev               # configure; the first run fetches the dependencies
cmake --build --preset dev       # build the engine, slash-cli, slash-mcp and the tests
ctest --preset dev               # run the tests
```

The tools land in `build/dev/tools/`. The rest of this guide uses two shell variables for them:

```bash
cli=./build/dev/tools/slash-cli/slash-cli
mcp=./build/dev/tools/slash-mcp/slash-mcp
```

## 2. Run the engine

```bash
$cli version
```

```json
{"name":"SlashEngine","version":"0.1.0"}
```

Run a scene for 60 fixed ticks with no window or GPU. Logs go to stderr as JSON lines; the result
goes to stdout as one JSON object:

```bash
$cli run --headless --ticks 60 --scene tests/data/basic.scene.json
```

```json
{"entities":3,"headless":true,"quit_requested":false,"renderer":"null","ticks":60}
```

`$cli run` with no flags opens a window and ticks until you close it. On a machine with no
display, set `SDL_VIDEO_DRIVER=dummy` to try the windowed path anyway.

## 3. Look at a scene

A scene is a `.scene.json` file of entities keyed by stable IDs, each holding components. Fields
equal to their defaults are left out. This is `tests/data/basic.scene.json`:

```json
{
  "entities": {
    "camera": {
      "components": {
        "Transform3D": {
          "position": [0.0, 2.0, -5.0]
        }
      }
    },
    "player": {
      "components": {
        "Transform2D": {
          "position": [64.0, 32.0],
          "rotation": 0.5
        }
      }
    },
    "spawn_point": {}
  },
  "format": 1
}
```

Check a scene against the schema the engine generates from its own components:

```bash
$cli scene validate tests/data/basic.scene.json
```

```json
{"entities":3,"valid":true}
```

A broken scene lists every error with the [JSON Pointer](https://www.rfc-editor.org/rfc/rfc6901)
of the bad value, and suggests the right name for a misspelled one (exit code 1):

```bash
$cli scene validate tests/data/invalid.scene.json
```

```json
{"errors":[{"message":"expected number, got string","path":"/entities/player/components/Transform2D/position/1"},{"message":"unknown property \"rotaton\"; did you mean \"rotation\"?","path":"/entities/player/components/Transform2D/rotaton"},{"message":"unknown property \"Transfrom3D\"; did you mean \"Transform3D\"?","path":"/entities/player/components/Transfrom3D"},{"message":"expected 1, got 2","path":"/format"}],"valid":false}
```

`$cli scene format <file>` rewrites a valid scene in canonical form (sorted keys, defaults left
out), so diffs stay small. Run it before committing a scene you edited by hand.

## 4. Change a scene with commands

Every change to a world goes through a command. `slash-cli call` runs one command against a scene
and prints the result. Start by asking the engine what it has:

```bash
$cli call engine.describe
```

```json
{"capabilities":["read","edit","run","filesystem","build"],"commands":["animation.eval","asset.list","batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","events.read","events.subscribe","events.unsubscribe","export.run","history.get","input.actions","input.inject","input.state","light.sample","log.tail","material.eval","particles.eval","physics.contacts","physics.overlap","physics.raycast","prefab.save","redo","run.until","scene.get","scene.new","scene.open","scene.save","schema.get","step","storyboard.brief","storyboard.check","storyboard.edit","storyboard.query","text.measure","tilemap.get","tilemap.paint","tilemap.set","ui.focus","ui.layout","ui.press","ui.theme","undo","world.diff","world.query","world.restore","world.snapshot"],"components":[{"defaults":{"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0]},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"},{"defaults":{"ids":[]},"doc":"The storyboard entries this entity stands for.","name":"StoryRef"},{"defaults":{"centered":true,"flip_h":false,"flip_v":false,"frame":0,"hframes":1,"light_mask":1,"material":"","material_params":{},"modulate":[1.0,1.0,1.0,1.0],"normal_texture":"","offset":[0.0,0.0],"region":[0.0,0.0,0.0,0.0],"specular_color":[1.0,1.0,1.0,1.0],"specular_shininess":1.0,"specular_texture":"","texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_index":0},"doc":"A texture drawn at the entity's Transform2D. Sprites draw in order of z_index, then in the order their entities were made.","name":"Sprite2D"},{"defaults":{"animation":"default","frame":0,"frame_progress":0.0,"playing":true,"speed_scale":1.0,"sprite_frames":""},"doc":"Plays an animation from a .frames.json file on the entity's Sprite2D, which it adds if missing. Emits animation_finished when an animation that doesn't loop ends.","name":"AnimatedSprite2D"},{"defaults":{"animation":"","backwards":false,"library":"","playing":true,"speed_scale":1.0,"time":0.0},"doc":"Plays a keyframe animation from a .anim.json file on the entity's components. Emits animation_finished when an animation that doesn't loop ends, and the events of its event tracks.","name":"AnimationPlayer"},{"defaults":{"legend":{},"light_mask":1,"modulate":[1.0,1.0,1.0,1.0],"origin":[0,0],"rows":[],"texture_filter":"inherit","tile_set":"","visible":true,"z_index":0},"doc":"A grid of tiles from a .tileset.json file, drawn at the entity's Transform2D if it has one. Cells are text: rows of legend keys, \".\" for empty. Edit them with tilemap.paint and tilemap.set.","name":"TileMapLayer"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"offset":[0.0,0.0],"range_item_cull_mask":1,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1,"texture":"","texture_scale":1.0},"doc":"A light shining from the entity's Transform2D through a texture. It lights sprites and tile map layers whose light_mask shares a bit with range_item_cull_mask; with shadow_enabled, LightOccluder2D polygons cast shadows from it.","name":"PointLight2D"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"max_distance":10000.0,"range_item_cull_mask":1,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1},"doc":"Light from one direction over everything, such as the sun. It shines down the screen, turned by the entity's Transform2D rotation. Masks, ranges and shadows work as PointLight2D's.","name":"DirectionalLight2D"},{"defaults":{"closed":true,"cull_mode":"disabled","occluder_light_mask":1,"polygon":[]},"doc":"A polygon that casts shadows from lights with shadow_enabled, placed by the entity's Transform2D.","name":"LightOccluder2D"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Tints everything lit: the light a scene has where no light reaches. One per scene.","name":"CanvasModulate"},{"defaults":{"a":[0.0,0.0],"b":[0.0,10.0],"disabled":false,"height":30.0,"one_way_collision":false,"one_way_collision_margin":1.0,"points":[],"position":[0.0,0.0],"radius":10.0,"rotation":0.0,"shape":"rectangle","size":[20.0,20.0]},"doc":"A shape of the entity's physics body or area, moved by position and rotation and scaled by its Transform2D.","name":"CollisionShape2D"},{"defaults":{"build_mode":"solids","disabled":false,"one_way_collision":false,"one_way_collision_margin":1.0,"polygon":[]},"doc":"A polygon shape of the entity's physics body or area: any simple polygon, filled or only its edges.","name":"CollisionPolygon2D"},{"defaults":{"bounce":0.0,"collision_layer":1,"collision_mask":1,"friction":1.0},"doc":"A physics body that doesn't move on its own, such as a wall. Changing its Transform2D moves it at once.","name":"StaticBody2D"},{"defaults":{"bounce":0.0,"collision_layer":1,"collision_mask":1,"friction":1.0,"sync_to_physics":true},"doc":"A physics body that goes where its Transform2D says, such as a moving platform, pushing bodies in its way.","name":"AnimatableBody2D"},{"defaults":{"angular_damp":0.0,"angular_damp_mode":"combine","angular_velocity":0.0,"bounce":0.0,"can_sleep":true,"collision_layer":1,"collision_mask":1,"constant_force":[0.0,0.0],"constant_torque":0.0,"contact_monitor":false,"continuous_cd":"disabled","freeze":false,"freeze_mode":"static","friction":1.0,"gravity_scale":1.0,"inertia":0.0,"linear_damp":0.0,"linear_damp_mode":"combine","linear_velocity":[0.0,0.0],"lock_rotation":false,"mass":1.0,"sleeping":false},"doc":"A physics body moved by gravity, collisions, forces and joints. The physics step writes its Transform2D, velocities and sleeping.","name":"RigidBody2D"},{"defaults":{"collision_layer":1,"collision_mask":1,"floor_max_angle":0.785398,"floor_normal":[0.0,0.0],"floor_snap_length":1.0,"floor_stop_on_slope":true,"max_slides":4,"motion_mode":"grounded","on_ceiling":false,"on_floor":false,"on_wall":false,"platform_on_leave":"add_velocity","platform_velocity":[0.0,0.0],"slide_on_ceiling":true,"up_direction":[0.0,-1.0],"velocity":[0.0,0.0],"wall_normal":[0.0,0.0]},"doc":"A physics body moved by setting its velocity: each tick it slides along what it hits, as Godot's move_and_slide, and reports floors, walls and ceilings.","name":"CharacterBody2D"},{"defaults":{"collision_layer":1,"collision_mask":1,"monitorable":true,"monitoring":true},"doc":"A region that emits body_entered, body_exited, area_entered and area_exited events. It goes where its Transform2D says.","name":"Area2D"},{"defaults":{"angular_limit_enabled":false,"angular_limit_lower":0.0,"angular_limit_upper":0.0,"disable_collision":true,"motor_enabled":false,"motor_target_velocity":0.0,"node_a":null,"node_b":null},"doc":"Pins two bodies together at the entity's Transform2D, letting them turn.","name":"PinJoint2D"},{"defaults":{"damping":1.0,"disable_collision":true,"length":50.0,"node_a":null,"node_b":null,"rest_length":0.0,"stiffness":20.0},"doc":"A spring from node_a at the entity's Transform2D to node_b, length pixels along the entity's y axis.","name":"DampedSpringJoint2D"},{"defaults":{"disable_collision":true,"initial_offset":25.0,"length":50.0,"node_a":null,"node_b":null},"doc":"Keeps node_b on a groove of node_a's, from the entity's Transform2D length pixels along its y axis; node_b slides along it and turns freely.","name":"GrooveJoint2D"},{"defaults":{"amount":8,"angle_curve":[],"angle_max":0.0,"angle_min":0.0,"angular_velocity_curve":[],"angular_velocity_max":0.0,"angular_velocity_min":0.0,"anim_loop":false,"anim_offset_curve":[],"anim_offset_max":0.0,"anim_offset_min":0.0,"anim_speed_curve":[],"anim_speed_max":0.0,"anim_speed_min":0.0,"color":[1.0,1.0,1.0,1.0],"color_initial_ramp":[],"color_ramp":[],"damping_curve":[],"damping_max":0.0,"damping_min":0.0,"direction":[1.0,0.0],"draw_order":"index","emission_colors":[],"emission_normals":[],"emission_points":[],"emission_rect_extents":[1.0,1.0],"emission_ring_inner_radius":0.0,"emission_ring_radius":1.0,"emission_shape":"point","emission_sphere_radius":1.0,"emitting":true,"explosiveness":0.0,"gravity":[0.0,980.0],"hframes":1,"hue_variation_curve":[],"hue_variation_max":0.0,"hue_variation_min":0.0,"initial_velocity_max":0.0,"initial_velocity_min":0.0,"lifetime":1.0,"lifetime_randomness":0.0,"light_mask":1,"linear_accel_curve":[],"linear_accel_max":0.0,"linear_accel_min":0.0,"local_coords":false,"material":"","material_params":{},"one_shot":false,"orbit_velocity_curve":[],"orbit_velocity_max":0.0,"orbit_velocity_min":0.0,"particle_flag_align_y":false,"preprocess":0.0,"radial_accel_curve":[],"radial_accel_max":0.0,"radial_accel_min":0.0,"randomness":0.0,"scale_amount_curve":[],"scale_amount_max":1.0,"scale_amount_min":1.0,"seed":0,"speed_scale":1.0,"spread":45.0,"tangential_accel_curve":[],"tangential_accel_max":0.0,"tangential_accel_min":0.0,"texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_index":0},"doc":"Emits particles from the entity's Transform2D, moves and draws them, as Godot's CPUParticles2D. Each takes a random value between each _min and _max when emitted; a _curve multiplies it as it ages. Emits finished when a one_shot's last particle dies.","name":"CPUParticles2D"},{"defaults":{"anchor_bottom":0.0,"anchor_left":0.0,"anchor_right":0.0,"anchor_top":0.0,"children":[],"custom_minimum_size":[0.0,0.0],"focus_mode":"","focus_neighbor_bottom":null,"focus_neighbor_left":null,"focus_neighbor_right":null,"focus_neighbor_top":null,"focus_next":null,"focus_previous":null,"grow_horizontal":"end","grow_vertical":"end","layout_direction":"inherited","modulate":[1.0,1.0,1.0,1.0],"mouse_filter":"","offset_bottom":0.0,"offset_left":0.0,"offset_right":0.0,"offset_top":0.0,"pivot_offset":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0],"self_modulate":[1.0,1.0,1.0,1.0],"size_flags_horizontal":"fill","size_flags_stretch_ratio":1.0,"size_flags_vertical":"fill","texture_filter":"inherit","theme":"","theme_override_colors":{},"theme_override_constants":{},"theme_override_font_sizes":{},"theme_override_fonts":{},"theme_override_styles":{},"theme_type_variation":"","visible":true,"z_index":0},"doc":"A UI element's rect, as Godot's Control: placed in its parent's rect by anchors and offsets, or in the viewport when no Control lists it in children. Label, ColorRect and the other UI components draw in it; they lay out with these defaults when the entity has no Control. A container component places its children by size flags instead of their anchors and offsets. Controls draw above the world, parents before children.","name":"Control"},{"defaults":{"autowrap_mode":"off","clip_text":false,"ellipsis_char":"…","horizontal_alignment":"left","language":"","lines_skipped":0,"max_lines_visible":-1,"text":"","text_direction":"auto","text_overrun_behavior":"no_trimming","uppercase":false,"vertical_alignment":"top","visible_characters":-1,"visible_characters_behavior":"chars_before_shaping"},"doc":"Text in the control's rect, as Godot's Label. Theme items: font, font_size, font_color, font_outline_color, outline_size, font_shadow_color, shadow_offset_x, shadow_offset_y, shadow_outline_size, line_spacing and paragraph_spacing.","name":"Label"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Fills the control's rect with one color, as Godot's ColorRect.","name":"ColorRect"},{"defaults":{},"doc":"Draws the theme's panel style box in the control's rect, as Godot's Panel.","name":"Panel"},{"defaults":{"expand_mode":"keep_size","flip_h":false,"flip_v":false,"stretch_mode":"scale","texture":""},"doc":"Draws a texture in the control's rect, as Godot's TextureRect.","name":"TextureRect"},{"defaults":{"axis_stretch_horizontal":"stretch","axis_stretch_vertical":"stretch","draw_center":true,"patch_margin_bottom":0,"patch_margin_left":0,"patch_margin_right":0,"patch_margin_top":0,"region_rect":[0.0,0.0,0.0,0.0],"texture":""},"doc":"Draws a texture cut into nine parts in the control's rect, as Godot's NinePatchRect: the corners keep their size, the sides stretch along and the middle both ways. Its minimum size is its margins.","name":"NinePatchRect"},{"defaults":{"allow_greater":false,"allow_lesser":false,"fill_mode":"begin_to_end","max_value":100.0,"min_value":0.0,"show_percentage":true,"step":0.01,"value":0.0},"doc":"A bar filled to show value between min_value and max_value, with the percentage on it, as Godot's ProgressBar. Theme items: background, fill, font, font_size, font_color, font_outline_color and outline_size.","name":"ProgressBar"},{"defaults":{"action_mode":"release","alignment":"center","autowrap_mode":"off","button_group":"","button_mask":["left"],"button_pressed":false,"clip_text":false,"disabled":false,"expand_icon":false,"flat":false,"icon":"","icon_alignment":"left","keep_pressed_outside":false,"language":"","shortcut":[],"shortcut_feedback":true,"text":"","text_direction":"auto","text_overrun_behavior":"no_trimming","toggle_mode":false,"vertical_icon_alignment":"center"},"doc":"A button with text and an icon, as Godot's Button: a click or tap released over it, ui_accept while it has focus, or its shortcut presses it, emitting pressed (and toggled, data {\"toggled_on\": bool}, for toggle buttons), button_down and button_up events. Theme items: normal, hover, pressed, hover_pressed, disabled and focus styles, font_color, font_hover_color, font_pressed_color, font_hover_pressed_color, font_focus_color, font_disabled_color, icon_*_color for the same states, font, font_size, outline_size, font_outline_color, h_separation, icon_max_width, align_to_largest_stylebox, line_spacing and click_margin.","name":"Button"},{"defaults":{"action_mode":"release","button_group":"","button_mask":["left"],"button_pressed":false,"disabled":false,"flip_h":false,"flip_v":false,"ignore_texture_size":false,"keep_pressed_outside":false,"shortcut":[],"shortcut_feedback":true,"stretch_mode":"keep","texture_disabled":"","texture_focused":"","texture_hover":"","texture_normal":"","texture_pressed":"","toggle_mode":false},"doc":"A button drawn with a texture per state, as Godot's TextureButton: texture_normal, then texture_pressed, texture_hover and texture_disabled when they are set, and texture_focused over them while it has focus. Pressed as Button is, with the same events. Its minimum size is its texture's.","name":"TextureButton"},{"defaults":{"alignment":"begin","vertical":false},"doc":"Lays out its children in a row, or a column when vertical, as Godot's HBoxContainer and VBoxContainer: each at its minimum size along it, plus a share of the space left for those that expand. Theme constant: separation.","name":"BoxContainer"},{"defaults":{"columns":1},"doc":"Lays out its children in a grid, as Godot's GridContainer: left to right, a new row every columns children. Theme constants: h_separation and v_separation.","name":"GridContainer"},{"defaults":{},"doc":"Fits its children in its rect less margins, as Godot's MarginContainer. Theme constants: margin_left, margin_top, margin_right and margin_bottom.","name":"MarginContainer"},{"defaults":{"use_top_left":false},"doc":"Keeps its children at their minimum sizes in its center, as Godot's CenterContainer.","name":"CenterContainer"},{"defaults":{"alignment":"begin","last_wrap_alignment":"inherit","reverse_fill":false,"vertical":false},"doc":"Lays out its children in rows that wrap to its width, or columns that wrap to its height when vertical, as Godot's HFlowContainer and VFlowContainer. Theme constants: h_separation and v_separation.","name":"FlowContainer"},{"defaults":{},"doc":"Draws the theme's panel style box and fits its children inside the style's margins, as Godot's PanelContainer.","name":"PanelContainer"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":["input","gui","animation_player","physics2d","animated_sprite_2d","cpu_particles_2d"]}
```

Find entities. Queries return only IDs unless you ask for components, which keeps answers small:

```bash
$cli call world.query '{"with": ["Transform2D"]}' --scene tests/data/basic.scene.json
```

```json
{"count":1,"entities":["player"],"truncated":false}
```

Make several changes in one call with `batch`, and save the result with `--save`. Copy the scene
first so the test data stays untouched:

```bash
mkdir -p build/play && cp tests/data/basic.scene.json build/play/level.scene.json
$cli call batch '{"commands": [
  {"command": "entity.create", "params": {"id": "enemy_01", "components": {"Transform2D": {"position": [200, 40]}}}},
  {"command": "component.set", "params": {"id": "player", "components": {"Transform2D": {"rotation": 1.0}}}}
]}' --project build/play --scene level.scene.json --save
```

```json
{"results":[{"id":"enemy_01"},{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}]}
```

A batch is all or nothing: if any command in it fails, none of its changes are kept and the error
names the command that failed. Paths are relative to `--project` (default: the current folder) and
cannot leave it.

## 5. Connect an agent

`slash-cli init` sets a game folder up for Claude Code, Codex and Cursor:

```bash
$cli init mygame
```

```json
{"created":["project.json","AGENTS.md","CLAUDE.md",".mcp.json",".cursor/mcp.json",".codex/config.toml",".claude/skills/slashengine/SKILL.md",".github/workflows/export.yml","scenes/main.scene.json",".gitignore"],"skipped":[]}
```

`mygame` is now a game project: `project.json` names the game and its main scene. Each config launches `slash-mcp`, the engine's MCP server, with the project's main scene open.
Put `slash-mcp` on your `PATH` (or pass `--mcp-command` with its full path to `init`), then start
the agent in `mygame/`; it finds the server on its next start. To add the server to Claude Code by
hand instead:

```bash
claude mcp add slashengine -- slash-mcp --project .
```

The agent now sees every command as a tool. Tool names use `_` where commands use `.`
(`entity_create` for `entity.create`), because some clients reject dots.

## 6. Change a game while it runs

Start a game with its HTTP server on, and point the agent at it with `slash-mcp --live`:

```bash
$cli run --serve --scene scenes/main.scene.json     # in the game folder
claude mcp add slashengine-live -- slash-mcp --live --project .
```

The server listens on `127.0.0.1` only and needs a bearer token. Its URL and token are in
`.slash/server.json` while it runs. Commands from the agent wait in a queue and run between ticks,
so they never race the game loop. Tools that don't speak MCP can use the REST mirror instead:

```bash
URL=$(jq -r .url .slash/server.json)
TOKEN=$(jq -r .token .slash/server.json)
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{"with": ["Transform3D"]}' \
    "$URL/v1/commands/world.query"
```

```json
{"count":1,"entities":["camera"],"truncated":false}
```

## 7. Export the game

Build the game for players. On Linux, from the folder that holds `mygame`:

```bash
$cli export linux --project mygame --test
```

```json
{"bytes":5335402,"executable":"mygame/build/export/linux/mygame","log":"mygame/.slash/export/linux/export.log","output":"mygame/build/export/linux","platform":"linux","preset":"linux","seconds":64.2,"test":{"entities":0,"passed":true,"ticks":60}}
```

`--test` runs the exported game headless and reports whether it worked. Use `macos` or `windows`
on those machines, or `web` anywhere Emscripten is installed. To export every platform at once,
push the game to GitHub: `init` wrote `.github/workflows/export.yml`, which builds and tests each
one on its own runner. Presets in `export_presets.json` name output folders and signing settings;
see [reference/cli.md](reference/cli.md#export).

## Next

- [concepts.md](concepts.md) explains the world, commands, sessions and servers.
- [reference/commands.md](reference/commands.md) lists every command with examples.
- [reference/protocols.md](reference/protocols.md) covers REST, JSON-RPC and MCP in detail.
- [roadmap.md](roadmap.md) shows what comes next.
