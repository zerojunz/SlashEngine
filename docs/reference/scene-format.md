# Scene format

Scenes are `.scene.json` files and prefabs are `.prefab.json` files. This page describes format
version 1. The authoritative schemas are generated from the registered components and printed by
`slash-cli schema scene` and `slash-cli schema prefab` (or the `schema.get` command with
`{"kind": "scene"}`, or the MCP resource `slash://schema/scene`), so they always match the engine
you are running.

## Shape

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

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `entities` | Yes | Entities keyed by stable ID |
| `root` | No | The entity an instance of this scene becomes (see [Instanced scenes](#instanced-scenes)) |
| `entities.<id>` | | An object, or `null` to leave out an entity an instanced scene brings |
| `entities.<id>.extends` | No | A `.prefab.json` file, or a `.scene.json` file with a `root`, the entity starts from (see [Prefabs](#prefabs)) |
| `entities.<id>.components` | No | Components keyed by type name. Left out, the entity has none, or its prefab's |
| `entities.<id>.components.<Type>` | No | The component's fields, or `null` to remove a component the prefab has |
| `entities.<id>.components.<Type>.<field>` | No | A field value. Left out, the field keeps its default, or its prefab's value |

No other keys are allowed at any level: an unknown key is an error, with a "did you mean"
suggestion when it is close to a valid one.

## IDs

Entity IDs are 1 to 64 characters from `A-Z`, `a-z`, `0-9`, `_` and `-`, such as `player` or
`enemy_03`. They are stable: saving, loading and undo keep them, and commands refer to entities by
them. The entities an [instanced scene](#instanced-scenes) brings have IDs under the instance's,
joined with `/`, such as `hero/sprite` or `hero/gun/muzzle`, up to 255 characters.

## Field values

| C++ field type | JSON | Example |
| --- | --- | --- |
| `bool` | boolean | `true` |
| Integers | integer | `3` |
| `float`, `double` | number | `0.5` |
| `std::string` | string | `"door"` |
| `Vec2`, `Vec3`, `Vec4` | array of 2, 3 or 4 numbers | `[64.0, 32.0]` |
| `Vec2i` | array of 2 integers | `[-3, 2]` |
| `Quat` | array `[x, y, z, w]` | `[0.0, 0.0, 0.0, 1.0]` |
| Reference to another entity | that entity's ID, or `null` | `"player"` |
| `std::vector<T>` | array of `T` | `[1, 2, 3]` |
| `std::map<std::string, T>` | object of `T` by name, replaced whole when set | `{"flash": 1}` |
| `json::Value` | any JSON value | `[1, 0, 0]` |
| Struct declared with `SLASH_REFLECT` | object of its fields | `{"r": 1.0, "g": 0.5}` |

A reference must name an entity that exists in the same scene.

## Built-in components

| Component | Field | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| `Transform2D` | `position` | `Vec2` | `[0, 0]` | Position in pixels, as [x, y], in its [parent's](#parent-transforms) space; y points down |
| | `rotation` | `float` | `0` | Rotation in radians; positive turns clockwise |
| | `scale` | `Vec2` | `[1, 1]` | Scale factor on each axis |
| | `skew` | `float` | `0` | Radians its y axis leans from square to its x axis, -1.569 to 1.569 (under 89.9 degrees), as Godot's `skew` |
| | `top_level` | `bool` | `false` | Placed in the world rather than its parent's space, and drawn after the rest without its parent's tint or `z_index`, as Godot's `top_level` ([parent transforms](#parent-transforms)) |
| | `children` | Array of entity references | `[]` | Entities with a `Transform2D` placed in its space, in drawing order; each has one parent ([parent transforms](#parent-transforms)) |
| `Transform3D` | `position` | `Vec3` | `[0, 0, 0]` | Position in meters, as [x, y, z], in its parent's space; y points up ([3D nodes](#3d-nodes)) |
| | `rotation` | `Vec3` | `[0, 0, 0]` | Radians about x, y and z, turned z first, then x, then y, as Godot's default rotation order |
| | `scale` | `Vec3` | `[1, 1, 1]` | Scale factor on each axis |
| | `visible` | `bool` | `true` | `false` hides it and every entity under it |
| | `top_level` | `bool` | `false` | Placed in the world rather than its parent's space; a hidden parent still hides it |
| | `children` | Array of entity references | `[]` | Entities with a `Transform3D` placed in its space; each has one parent |
| `StoryRef` | `ids` | `std::vector<std::string>` | `[]` | The [storyboard](storyboard-format.md) entries this entity stands for, e.g. `["character/mira"]` |
| `Sprite2D` | `texture` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file; `""` draws nothing |
| | `centered` | `bool` | `true` | Center the texture on the position; `false` puts its top left corner there |
| | `offset` | `Vec2` | `[0, 0]` | Moves the texture from the position, in pixels |
| | `flip_h`, `flip_v` | `bool` | `false` | Mirror the texture left to right, or top to bottom, in place |
| | `hframes`, `vframes` | `int` | `1` | Columns and rows of frames in a sprite sheet, 1 to 16384 |
| | `frame` | `int` | `0` | The sheet's frame to draw, counted along rows from 0 |
| | `region` | `Vec4` | `[0, 0, 0, 0]` | Part of the texture to use, as [x, y, width, height] in pixels, before splitting it into frames; a zero size uses the whole texture |
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Color the texture and its children are multiplied by, as [r, g, b, a] from 0 to 1 |
| | `z_index` | `int` | `0` | Higher draws on top, -4096 to 4096, added to its parents'; equal values draw in tree order: the roots in the order the entities were made, each followed by its children |
| | `z_as_relative` | `bool` | `true` | Add `z_index` to its parents'; `false` makes it the whole of it, as Godot's |
| | `visible` | `bool` | `true` | Draw it and its children |
| | `texture_filter` | `std::string` | `"inherit"` | `"linear"`, `"nearest"` (sharp pixel art) or `"inherit"`, which uses the project's `rendering.texture_filter` |
| | `material` | `std::string` | `""` | Project path of a [`.material.json`](#materials) file, which colors the sprite's pixels; `""` draws the texture as it is |
| | `material_params` | `std::map<std::string, json::Value>` | `{}` | Values for the material's params by name, replacing their defaults, e.g. `{"flash": 1}` |
| | `light_mask` | `int` | `1` | [Lit](#2d-lights) by lights whose `range_item_cull_mask` shares a bit, 0 to 1048575 (Godot's 20 layers) |
| | `normal_texture` | `std::string` | `""` | Project path of a normal map laid out as the texture: OpenGL style, green up, blue toward the viewer. Lights need a `height` to show it |
| | `specular_texture` | `std::string` | `""` | Project path of a specular map laid out as the texture: highlight color and, in alpha, shininess. Needs a `normal_texture` |
| | `specular_color` | `Vec4` | `[1, 1, 1, 1]` | Multiplies the specular map's color |
| | `specular_shininess` | `float` | `1` | Multiplies the specular map's shininess, 0 to 1 |
| `AnimatedSprite2D` | `sprite_frames` | `std::string` | `""` | Project path of a [`.frames.json`](#sprite-frames) file |
| | `animation` | `std::string` | `"default"` | Name of the animation to play |
| | `playing` | `bool` | `true` | Advance the animation each tick |
| | `speed_scale` | `float` | `1` | Multiplies the animation's frames per second; negative plays backwards |
| | `frame` | `int` | `0` | The animation's current frame, from 0 |
| | `frame_progress` | `float` | `0` | How far through the current frame, from 0 to 1 |
| `AnimationPlayer` | `library` | `std::string` | `""` | Project path of a [`.anim.json`](#animations) file, or of a model whose [animations](#skeletons-and-model-animations) it plays |
| | `animation` | `std::string` | `""` | Name of the animation to play; `""` plays nothing |
| | `playing` | `bool` | `true` | Advance the animation each tick |
| | `speed_scale` | `float` | `1` | Multiplies the playback speed; negative plays backwards |
| | `time` | `double` | `0` | Seconds into the animation, from 0 to its length |
| | `backwards` | `bool` | `false` | Play toward the start, as Godot's `play_backwards`; pingpong loops flip it at each end |
| | `targets` | `std::map<std::string, entt::entity>` | `{}` | The entity each track [`target`](#animations) names, by name; `null` leaves that target's tracks unplayed |
| `AnimationTree` | `tree_root` | `std::string` | `""` | Project path of a [`.animtree.json`](#animation-trees) file |
| | `libraries` | `std::map<std::string, std::string>` | `{}` | Its animation libraries by name: `.anim.json` files or models. The tree names the animations of the library `""` by themselves, others' as `"library/animation"` |
| | `targets` | `std::map<std::string, entt::entity>` | `{}` | The entity each track `target` names, by name |
| | `active` | `bool` | `true` | Play the tree each tick; turning it off clears `state`, so it starts over |
| | `deterministic` | `bool` | `true` | Tracks no animation plays go back to their `RESET` values; off, each track's weights are scaled to add up to 1 and unplayed tracks stay |
| | `root_motion_track` | `std::string` | `""` | A model node whose motion is reported in `root_motion_*` instead of posed |
| | `root_motion_local` | `bool` | `false` | Report `root_motion_position` in the node's own frame |
| | `parameters` | `json::Object` | `{}` | Values for the tree's parameters, as `"speed/blend_amount"` or `"shot/request"`; requests go back to their defaults once the tree acts on them |
| | `state` | `json::Object` | `{}` | What the tree keeps as it plays (times, fades, the current input); set by the tree, and empty starts it over |
| | `playing` | `std::vector<TreeAnimation>` | `[]` | The animations the last tick played: `animation`, `time`, `weight` and `node_weights` (filtered model nodes). Set by the tree |
| | `invalid` | `std::string` | `""` | Why the last tick played nothing; `""` when it played |
| | `root_motion_position`, `root_motion_rotation`, `root_motion_scale` | `Vec3`, `Quat`, `Vec3` | zero, identity, zero | How the root motion track moved, turned and scaled in the last tick |
| | `root_motion_position_accumulator`, `root_motion_rotation_accumulator`, `root_motion_scale_accumulator` | `Vec3`, `Quat`, `Vec3` | zero, identity, one | Where the animations put the root motion track |
| `TileMapLayer` | `tile_set` | `std::string` | `""` | Project path of a [`.tileset.json`](#tile-sets) file |
| | `origin` | `Vec2i` | `[0, 0]` | The cell the first key of the first row stands for, as [column, row] |
| | `legend` | `std::map<std::string, std::string>` | `{}` | The tile each key in `rows` stands for, by name or as `"source:column,row"`, then `:<alternative>` and `:<turns>` if any ([turned cells](#alternative-tiles-and-turned-cells)) |
| | `rows` | `std::vector<std::string>` | `[]` | The [cells](#tile-map-layers), one string of keys per row from `origin` rightward; `.` is an empty cell |
| | `z_index` | `int` | `0` | Higher draws on top, -4096 to 4096, added to its parents'; layers and sprites draw in one order |
| | `z_as_relative` | `bool` | `true` | As `Sprite2D`'s |
| | `visible` | `bool` | `true` | Draw it and its children |
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Color every tile and its children are multiplied by, as [r, g, b, a] from 0 to 1 |
| | `texture_filter` | `std::string` | `"inherit"` | `"linear"`, `"nearest"` (sharp pixel art) or `"inherit"`, which uses the project's `rendering.texture_filter` |
| | `light_mask` | `int` | `1` | [Lit](#2d-lights) by lights whose `range_item_cull_mask` shares a bit |
| | `collision_enabled` | `bool` | `true` | Its tiles' [collision polygons](#tile-collision) make it a static body in 2D physics |
| | `occlusion_enabled` | `bool` | `true` | Its tiles' [occluders](#tile-occluders-and-custom-data) cast shadows |
| | `navigation_enabled` | `bool` | `true` | Each cell is a [region](#tile-navigation) of the 2D navigation map in its tile's navigation polygons |
| `PointLight2D` | `enabled` | `bool` | `true` | Shine |
| | `texture` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file whose color and alpha shape the light; `""` is a soft round light 256 pixels across |
| | `texture_scale` | `float` | `1` | Scales the texture |
| | `offset` | `Vec2` | `[0, 0]` | Moves the texture from the light, in pixels |
| | `color` | `Vec4` | `[1, 1, 1, 1]` | The light's color, as [r, g, b, a] from 0 to 1 |
| | `energy` | `float` | `1` | Multiplies the color |
| | `blend_mode` | `std::string` | `"add"` | How the light changes what it lights: `add` brightens, `sub` darkens and `mix` blends toward its color |
| | `height` | `float` | `0` | Pixels above the plane, for normal maps: at 0 it lights them only from the side |
| | `range_z_min`, `range_z_max` | `int` | `-1024`, `1024` | Lights sprites and layers whose `z_index` is in this range |
| | `range_layer_min`, `range_layer_max` | `int` | `0`, `0` | Lights the [canvases](#canvas-layers-and-parallax) whose layer is in this range, from -512 to 512: the world is layer 0, a `CanvasLayer` its `layer` |
| | `range_item_cull_mask` | `int` | `1` | Lights sprites and layers whose `light_mask` shares a bit |
| | `shadow_enabled` | `bool` | `false` | `LightOccluder2D` polygons and tiles' [occluders](#tile-occluders-and-custom-data) cast shadows from it |
| | `shadow_color` | `Vec4` | `[0, 0, 0, 0]` | What shadowed pixels get instead of the light; alpha is times the light's |
| | `shadow_filter` | `std::string` | `"none"` | Shadow edges: hard (`none`), or softened over 5 or 13 samples (`pcf5`, `pcf13`) |
| | `shadow_filter_smooth` | `float` | `0` | Spreads the filter's samples further apart |
| | `shadow_item_cull_mask` | `int` | `1` | Occluders whose `occluder_light_mask`, or tiles' occlusion layer's `light_mask`, shares a bit cast shadows |
| `DirectionalLight2D` | `enabled`, `color`, `energy`, `blend_mode` | | | As `PointLight2D`'s. It lights everything, down the screen turned by its `Transform2D` rotation |
| | `height` | `float` | `0` | For normal maps: from 0, along the plane, to 1, straight down onto it |
| | `max_distance` | `float` | `10000` | How far shadows reach past their occluders, in pixels |
| | `range_*`, `shadow_*` | | | As `PointLight2D`'s |
| `LightOccluder2D` | `polygon` | `std::vector<Vec2>` | `[]` | Points as [x, y] in pixels, placed by the entity's `Transform2D` |
| | `closed` | `bool` | `true` | Join the last point to the first |
| | `cull_mode` | `std::string` | `"disabled"` | `clockwise` or `counter_clockwise` leaves out edges that run that way around the light, as seen on screen |
| | `sdf_collision` | `bool` | `true` | [GPU particles](#gpu-particle-collision) whose process material has a `collision_mode` collide with it |
| | `occluder_light_mask` | `int` | `1` | Casts shadows from lights whose `shadow_item_cull_mask` shares a bit |
| `CanvasModulate` | `color` | `Vec4` | `[1, 1, 1, 1]` | Tints everything lit in its canvas (the world's, or the `CanvasLayer`'s that holds it): the light it has where no light reaches. One per canvas |
| `Camera2D` | `enabled` | `bool` | `true` | Can be the [current camera](#2d-camera): the first enabled one in drawing order |
| | `anchor_mode` | `std::string` | `"drag_center"` | `drag_center` puts the middle of the screen at the camera; `fixed_top_left` its top left corner |
| | `ignore_rotation` | `bool` | `true` | Keep the view upright when the camera's `Transform2D` turns |
| | `offset` | `Vec2` | `[0, 0]` | Moves the view, in pixels |
| | `zoom` | `Vec2` | `[1, 1]` | `[2, 2]` shows things twice as big; 0 counts as 1 |
| | `limit_enabled` | `bool` | `true` | Keep the view inside the limits |
| | `limit_left`, `limit_top`, `limit_right`, `limit_bottom` | `int` | `-10000000`, `-10000000`, `10000000`, `10000000` | Edges of the world the view stays inside, in pixels |
| | `limit_smoothed` | `bool` | `false` | With position smoothing, glide to the limits instead of stopping at them |
| | `position_smoothing_enabled` | `bool` | `false` | Glide after the entity instead of moving with it |
| | `position_smoothing_speed` | `float` | `5` | Each tick the view closes this times the tick's seconds of the gap |
| | `rotation_smoothing_enabled` | `bool` | `false` | Turn smoothly with the entity, when not ignoring rotation |
| | `rotation_smoothing_speed` | `float` | `5` | Each tick the view turns this times the tick's seconds of the way |
| | `drag_horizontal_enabled`, `drag_vertical_enabled` | `bool` | `false` | Follow the entity across, or up and down, only when it leaves the drag margins |
| | `drag_horizontal_offset`, `drag_vertical_offset` | `float` | `0` | Where the camera sits within its drag margins, from -1 to 1 |
| | `drag_left_margin`, `drag_top_margin`, `drag_right_margin`, `drag_bottom_margin` | `float` | `0.2` | How far the entity moves from the center before the camera follows, as a share of half the screen |
| `CollisionShape2D` | `shape` | `std::string` | `"rectangle"` | A [physics](#2d-physics) shape of the entity's body or area, or its [parent's](#parent-transforms) if it has neither: `rectangle`, `circle`, `capsule`, `segment` or `polygon` |
| | `size` | `Vec2` | `[20, 20]` | Rectangle width and height, in pixels |
| | `radius` | `float` | `10` | Circle and capsule radius, in pixels |
| | `height` | `float` | `30` | Capsule height end to end along y, in pixels |
| | `a`, `b` | `Vec2` | `[0, 0]`, `[0, 10]` | Segment ends, in pixels |
| | `points` | `std::vector<Vec2>` | `[]` | Polygon points in pixels; the shape is their convex hull |
| | `position` | `Vec2` | `[0, 0]` | Moves the shape from the entity, in pixels; the entity's `Transform2D` scale scales it |
| | `rotation` | `float` | `0` | Turns the shape, in radians clockwise |
| | `disabled` | `bool` | `false` | Leave the shape out |
| | `one_way_collision` | `bool` | `false` | Collide only with what comes from the shape's -y side, as platforms one jumps through from below |
| | `one_way_collision_margin` | `float` | `1` | How deep something may be in a one-way shape and still land on it, in pixels |
| `CollisionPolygon2D` | `polygon` | `std::vector<Vec2>` | `[]` | Any simple polygon, as [x, y] points in pixels from the entity |
| | `build_mode` | `std::string` | `"solids"` | `solids` fills it, split into convex parts; `segments` makes only its outline solid |
| | `disabled`, `one_way_collision`, `one_way_collision_margin` | | | As `CollisionShape2D`'s |
| `StaticBody2D` | `collision_layer` | `int` | `1` | The physics layers it is in, as bits: layer 1 is 1, layer 2 is 2, layer 3 is 4. Every body and area has it |
| | `collision_mask` | `int` | `1` | The layers it collides with, as bits. Every body and area has it |
| | `friction` | `float` | `1` | How much it resists sliding: 0 is ice. Two bodies use the smaller friction |
| | `bounce` | `float` | `0` | How much it bounces, 0 to 1. Two bodies add theirs, up to 1 |
| | `constant_linear_velocity` | `Vec2` | `[0, 0]` | Moves what touches it as if it moved this fast, in pixels per second, while it stays put: a conveyor belt |
| | `constant_angular_velocity` | `float` | `0` | The same for turning, in radians per second, clockwise |
| `AnimatableBody2D` | `collision_layer`, `collision_mask`, `friction`, `bounce` | | | As `StaticBody2D`'s. It goes where its `Transform2D` says, such as a moving platform |
| | `constant_linear_velocity`, `constant_angular_velocity` | | | As `StaticBody2D`'s, added to how it moves |
| | `sync_to_physics` | `bool` | `true` | Move there during the physics step, pushing bodies in the way and carrying those on it; `false` jumps there |
| `RigidBody2D` | `collision_layer`, `collision_mask`, `friction`, `bounce` | | | As `StaticBody2D`'s |
| | `mass` | `float` | `1` | In kilograms, from 0.001 |
| | `inertia` | `float` | `0` | Resistance to turning; 0 computes it from the shapes and the mass |
| | `gravity_scale` | `float` | `1` | Multiplies gravity; 0 floats |
| | `linear_velocity` | `Vec2` | `[0, 0]` | In pixels per second; the physics step writes it |
| | `angular_velocity` | `float` | `0` | In radians per second, clockwise; the physics step writes it |
| | `linear_damp_mode`, `angular_damp_mode` | `std::string` | `"combine"` | `combine` adds the damp below to the project's; `replace` uses only it |
| | `linear_damp`, `angular_damp` | `float` | `0` | Slows moving or turning, per second |
| | `constant_force` | `Vec2` | `[0, 0]` | A force applied every tick |
| | `constant_torque` | `float` | `0` | A torque applied every tick |
| | `lock_rotation` | `bool` | `false` | Never turn |
| | `freeze` | `bool` | `false` | Stop simulating it; it acts as `freeze_mode` says |
| | `freeze_mode` | `std::string` | `"static"` | What a frozen body acts as: `static`, or `kinematic`, moved by its `Transform2D` |
| | `continuous_cd` | `std::string` | `"disabled"` | `cast_ray` or `cast_shape` sweeps it against other moving bodies so it can't pass through them when fast |
| | `can_sleep` | `bool` | `true` | Stop simulating it while it rests |
| | `sleeping` | `bool` | `false` | Resting and not simulated; set `false` to wake it |
| | `contact_monitor` | `bool` | `false` | Emit `body_entered` and `body_exited` events as bodies start and stop touching it |
| `CharacterBody2D` | `collision_layer`, `collision_mask` | | | As `StaticBody2D`'s |
| | `motion_mode` | `std::string` | `"grounded"` | `grounded` has floors, walls and ceilings, for side views; `floating` has only walls, for top-down games |
| | `up_direction` | `Vec2` | `[0, -1]` | Which way is up, as a unit vector |
| | `velocity` | `Vec2` | `[0, 0]` | In pixels per second. Gameplay code sets it; each move slides it along what it hits |
| | `max_slides` | `int` | `4` | Slides per move at most, 1 to 16 |
| | `slide_on_ceiling` | `bool` | `true` | Slide along ceilings; `false` stops and falls |
| | `wall_min_slide_angle` | `float` | `0.261799` | In `floating` mode, hitting a wall within this many radians of head-on stops it instead of sliding; 0 always slides |
| | `floor_stop_on_slope` | `bool` | `true` | Don't slide down slopes while standing |
| | `floor_constant_speed` | `bool` | `false` | Walk up and down slopes at the speed it walks on flat ground, instead of slower up and faster down |
| | `floor_block_on_wall` | `bool` | `true` | Walking into a slope too steep to be a floor doesn't push it up the slope |
| | `floor_max_angle` | `float` | `0.785398` | Steepest floor, in radians from `up_direction` |
| | `floor_snap_length` | `float` | `1` | How far down it keeps to a floor while walking off steps and slopes, in pixels |
| | `platform_on_leave` | `std::string` | `"add_velocity"` | What it keeps of a moving floor's velocity on leaving it: `add_velocity`, `add_upward_velocity` or `do_nothing` |
| | `platform_floor_layers` | `int` | `4294967295` | The layers, as bits, of floors that carry it when they move; others it stands on without moving |
| | `platform_wall_layers` | `int` | `0` | The layers of walls that carry it when they move into it |
| | `safe_margin` | `float` | `0.08` | How close, in pixels, a body counts as touching; larger is steadier against fast moving bodies, smaller more exact |
| | `on_floor`, `on_wall`, `on_ceiling` | `bool` | `false` | What the last move touched; the physics step writes them |
| | `floor_normal`, `wall_normal` | `Vec2` | `[0, 0]` | The floor's and the wall's normals in the last move |
| | `platform_velocity` | `Vec2` | `[0, 0]` | The velocity of the moving floor it stands on |
| `Area2D` | `collision_layer` | `int` | `1` | As `StaticBody2D`'s |
| | `collision_mask` | `int` | `1` | The layers of bodies and areas it detects |
| | `monitoring` | `bool` | `true` | Emit `body_entered`, `body_exited`, `area_entered` and `area_exited` events |
| | `monitorable` | `bool` | `true` | Other areas detect it |
| | `priority` | `int` | `0` | Areas with space overrides apply from the highest priority down; equal ones in the order they were made |
| | `gravity_space_override` | `std::string` | `"disabled"` | How it changes the gravity of bodies in it: `disabled`, `combine`, `combine_replace`, `replace` or `replace_combine` (see [gravity and damping zones](#gravity-and-damping-zones)) |
| | `gravity_point` | `bool` | `false` | Pull toward `gravity_point_center` instead of along `gravity_direction` |
| | `gravity_point_center` | `Vec2` | `[0, 1]` | The point it pulls toward, in the entity's space |
| | `gravity_point_unit_distance` | `float` | `0` | With a point, the distance at which the pull is `gravity`, weakening with distance squared; 0 pulls the same everywhere |
| | `gravity_direction` | `Vec2` | `[0, 1]` | Which way it pulls, in the world, as a unit vector |
| | `gravity` | `float` | `980` | How strongly, in pixels per second squared |
| | `linear_damp_space_override`, `angular_damp_space_override` | `std::string` | `"disabled"` | How it changes the damping of bodies in it, as `gravity_space_override` |
| | `linear_damp` | `float` | `0.1` | Slows moving bodies in it, per second |
| | `angular_damp` | `float` | `1` | Slows turning bodies in it, per second |
| | `audio_bus_override` | `bool` | `false` | `AudioStreamPlayer2D` sounds in it play through `audio_bus_name` (see [buses by area](#buses-by-area)) |
| | `audio_bus_name` | `std::string` | `"Master"` | The bus sounds in it play through |
| `PinJoint2D` | `node_a`, `node_b` | Reference to another entity | `null` | The two bodies it joins, at the entity's `Transform2D`; a `null` `node_a` pins `node_b` to the world. Every joint has them |
| | `disable_collision` | `bool` | `true` | The two bodies don't collide with each other. Every joint has it |
| | `angular_limit_enabled` | `bool` | `false` | Limit how far `node_b` turns from `node_a` |
| | `angular_limit_lower`, `angular_limit_upper` | `float` | `0` | The limits in radians, from -3.1 to 0 and from 0 to 3.1 |
| | `motor_enabled` | `bool` | `false` | Turn `node_b` at `motor_target_velocity`; past an angle limit it stops pushing, as Godot's |
| | `motor_target_velocity` | `float` | `0` | In radians per second |
| `DampedSpringJoint2D` | `node_a`, `node_b`, `disable_collision` | | | As `PinJoint2D`'s; a spring from `node_a` at the entity's `Transform2D` to `node_b` |
| | `length` | `float` | `50` | Where `node_b` is attached, in the entity's pixels along its y axis (its scale stretches it) |
| | `rest_length` | `float` | `0` | The length it pulls toward; 0 is `length` |
| | `stiffness` | `float` | `20` | How hard it pulls |
| | `damping` | `float` | `1` | How fast it settles |
| `GrooveJoint2D` | `node_a`, `node_b`, `disable_collision` | | | As `PinJoint2D`'s; `node_b` slides along a groove of `node_a`'s and turns freely |
| | `length` | `float` | `50` | The groove's length in the entity's pixels, from its `Transform2D` along its y axis (its scale stretches it) |
| | `initial_offset` | `float` | `25` | Where `node_b` starts along the groove, in the entity's pixels |
| `CPUParticles2D` | `emitting` | `bool` | `true` | Emit new [particles](#particles); setting it again restarts a `one_shot` |
| | `amount` | `int` | `8` | Particles alive at most, 1 to 65536: each cycle emits this many |
| | `lifetime` | `float` | `1` | Seconds each particle lives and each cycle lasts, from 0.001 |
| | `one_shot` | `bool` | `false` | Emit one cycle, then set `emitting` to false |
| | `preprocess` | `float` | `0` | Seconds it runs ahead before its first tick, so it starts full |
| | `speed_scale` | `float` | `1` | Multiplies time for its particles |
| | `explosiveness` | `float` | `0` | 0 spreads a cycle's particles evenly over it; 1 emits them all at once |
| | `randomness` | `float` | `0` | Emits each particle later by up to its share of the cycle times this, 0 to 1 |
| | `lifetime_randomness` | `float` | `0` | Shortens each particle's life by up to this fraction, 0 to 1 |
| | `seed` | `int` | `0` | Varies the random values; the same seed and entity ID give the same particles |
| | `local_coords` | `bool` | `false` | Particles live in the entity's coordinates and move with it; `false` leaves them where they were emitted |
| | `draw_order` | `std::string` | `"index"` | `index` draws in emission slot order; `lifetime` draws the oldest first, so the youngest are on top |
| | `texture` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file each particle draws, centered on it; `""` draws a white pixel |
| | `texture_filter`, `z_index`, `z_as_relative`, `visible`, `material`, `material_params`, `light_mask` | | | As `Sprite2D`'s |
| | `hframes`, `vframes` | `int` | `1` | Columns and rows of frames in the texture, for `anim_speed` and `anim_offset` |
| | `anim_loop` | `bool` | `false` | Frames past the last start again from the first; `false` stays on the last |
| | `emission_shape` | `std::string` | `"point"` | Where particles start: `point` (the entity), `sphere` (in a circle), `sphere_surface` (on its edge), `rectangle`, `ring`, `points` or `directed_points` |
| | `emission_sphere_radius` | `float` | `1` | The circle's radius, in pixels |
| | `emission_rect_extents` | `Vec2` | `[1, 1]` | Half the rectangle's width and height, in pixels |
| | `emission_ring_radius`, `emission_ring_inner_radius` | `float` | `1`, `0` | The ring's outer and inner radius, in pixels |
| | `emission_points` | `std::vector<Vec2>` | `[]` | Points in pixels from the entity; each particle starts at one picked at random |
| | `emission_normals` | `std::vector<Vec2>` | `[]` | For `directed_points`, each point's direction: it turns the particle's velocity from x to it |
| | `emission_colors` | `std::vector<Vec4>` | `[]` | Colors multiplying each point's particles |
| | `particle_flag_align_y` | `bool` | `false` | Turn each particle so its y axis points along its velocity |
| | `direction` | `Vec2` | `[1, 0]` | Which way particles go |
| | `spread` | `float` | `45` | Degrees either side of `direction`, 0 to 180 |
| | `gravity` | `Vec2` | `[0, 980]` | In pixels per second squared |
| | `initial_velocity_min`, `_max` | `float` | `0` | Starting speed in pixels per second |
| | `angular_velocity_min`, `_max` | `float` | `0` | Degrees per second it turns, clockwise |
| | `orbit_velocity_min`, `_max` | `float` | `0` | Turns per second round the entity, anticlockwise on screen |
| | `linear_accel_min`, `_max` | `float` | `0` | Pixels per second squared along its velocity |
| | `radial_accel_min`, `_max` | `float` | `0` | Pixels per second squared away from the entity |
| | `tangential_accel_min`, `_max` | `float` | `0` | Pixels per second squared round the entity, clockwise |
| | `damping_min`, `_max` | `float` | `0` | Pixels per second squared it slows by, down to a stop |
| | `angle_min`, `_max` | `float` | `0` | Starting rotation in degrees, clockwise |
| | `scale_amount_min`, `_max` | `float` | `1` | Size, times the texture's |
| | `hue_variation_min`, `_max` | `float` | `0` | Turns its hue by this fraction of the color wheel, -1 to 1 |
| | `anim_speed_min`, `_max` | `float` | `0` | Times its frames play over its life |
| | `anim_offset_min`, `_max` | `float` | `0` | The frame it starts on, as a fraction of the frames, 0 to 1 |
| | `*_curve` | `std::vector<Vec2>` | `[]` | Each `_min` / `_max` pair but `initial_velocity` has one: points `[offset, value]` that multiply the value over each particle's life, offsets from 0 to 1, joined by straight lines; none is 1 |
| | `color` | `Vec4` | `[1, 1, 1, 1]` | Multiplies the texture, as [r, g, b, a] from 0 to 1 |
| | `color_ramp` | `std::vector<GradientPoint>` | `[]` | Colors over each particle's life, multiplying `color`: points `{"offset", "color"}`, blended between |
| | `color_initial_ramp` | `std::vector<GradientPoint>` | `[]` | Each particle multiplies its color by one picked at random from this gradient |
| `GPUParticles2D` | `emitting` | `bool` | `true` | Emit new [particles on the GPU](#gpu-particles); setting it again restarts a `one_shot` |
| | `amount` | `int` | `8` | Particles alive at most, 1 to 1000000: each cycle emits this many |
| | `amount_ratio` | `float` | `1` | The share of `amount` it emits, 0 to 1 |
| | `sub_emitter` | `entity` | `null` | Another `GPUParticles2D` its particles start particles in, as its process material's `sub_emitter_mode` says (see [sub-emitters](#sub-emitters)) |
| | `process_material` | `std::string` | `""` | Project path of a [`.process_material.json`](#process-materials) file: how particles start and move; `""` emits still white pixels |
| | `texture` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file each particle draws, centered on it; `""` draws a white pixel |
| | `lifetime` | `float` | `1` | Seconds each particle lives and each cycle lasts, from 0.001 |
| | `interp_to_end` | `float` | `0` | Ages every particle toward the end of its life, 0 to 1 |
| | `one_shot` | `bool` | `false` | Emit one cycle, then set `emitting` to false |
| | `preprocess` | `float` | `0` | Seconds it runs ahead when it starts, so it starts full |
| | `speed_scale` | `float` | `1` | Multiplies time for its particles |
| | `explosiveness` | `float` | `0` | 0 spreads a cycle's particles evenly over it; 1 emits them all at once |
| | `randomness` | `float` | `0` | Randomizes when each particle is emitted, 0 to 1 |
| | `use_fixed_seed` | `bool` | `false` | Use `seed` as it is, so every run and restart repeats; otherwise `seed` and the entity's ID vary the particles, and each `one_shot` restart differs |
| | `seed` | `int` | `0` | The random seed |
| | `fixed_fps` | `int` | `30` | Steps per second the particles move in, 0 to 1000; 0 steps once a tick (60 a second with trails) |
| | `interpolate` | `bool` | `true` | Draw particles where they are between steps |
| | `fract_delta` | `bool` | `true` | A particle emitted partway through a step moves for that part only, so emission is smooth |
| | `collision_base_size` | `float` | `1` | How near, in pixels, a particle comes to a shape before it [collides](#gpu-particle-collision); times half the texture's average side, `(width + height) / 4`, when it has one |
| | `local_coords` | `bool` | `false` | Particles live in the entity's coordinates and move with it; `false` leaves them where they were emitted |
| | `draw_order` | `std::string` | `"lifetime"` | `index` draws in emission slot order; `lifetime` draws the youngest on top, `reverse_lifetime` the oldest |
| | `hframes`, `vframes` | `int` | `1` | Columns and rows of frames in the texture, for the process material's `anim_speed` and `anim_offset` |
| | `anim_loop` | `bool` | `false` | Frames past the last start again from the first; `false` stays on the last |
| | `trail_enabled` | `bool` | `false` | Draw each particle as a ribbon along where it has been, its [trail](#gpu-particle-trails) |
| | `trail_lifetime` | `float` | `0.3` | Seconds of travel the trail covers, from 0.01 |
| | `trail_sections` | `int` | `8` | Copies of the particle down the trail, 2 to 128 |
| | `trail_section_subdivisions` | `int` | `4` | Segments the ribbon bends in between two copies, 1 to 1024 |
| | `texture_filter`, `z_index`, `z_as_relative`, `visible`, `material`, `material_params`, `light_mask` | | | As `Sprite2D`'s |
| `Control` | `children` | `std::vector<entity>` | `[]` | [Controls](#ui-and-text) placed in its rect and drawn over it, in this order; each control has one parent at most |
| | `anchor_left`, `anchor_top`, `anchor_right`, `anchor_bottom` | `float` | `0` | Where each edge is anchored in the parent's rect (or the viewport's): 0 at its left or top, 1 at its right or bottom, and past them outside 0 to 1, as Godot allows |
| | `offset_left`, `offset_top`, `offset_right`, `offset_bottom` | `float` | `0` | Pixels from each edge's anchor to the edge |
| | `grow_horizontal`, `grow_vertical` | `std::string` | `"end"` | Which way it grows when its minimum size is bigger than its anchors and offsets make it: `begin` (left or up), `end` or `both` |
| | `custom_minimum_size` | `Vec2` | `[0, 0]` | Its smallest size; it can be bigger to fit its content |
| | `rotation` | `float` | `0` | Radians, clockwise, around `pivot_offset`; its children turn with it |
| | `scale` | `Vec2` | `[1, 1]` | Drawn scaled around `pivot_offset`; layout doesn't change |
| | `pivot_offset` | `Vec2` | `[0, 0]` | The point it turns and scales around, from its top-left corner |
| | `visible` | `bool` | `true` | Draw it and its children |
| | `clip_contents` | `bool` | `false` | Cut off its drawing and its children's at its rect, and let the pointer reach only the part of its children inside it ([scrolling](#scrolling)); a `ScrollContainer` always does |
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Tints it and its children, as [r, g, b, a] |
| | `self_modulate` | `Vec4` | `[1, 1, 1, 1]` | Tints it only |
| | `top_level` | `bool` | `false` | Placed in the viewport instead of its parent's rect, left out by containers, with none of its parent's transform, `modulate`, `z_index` or clip, as Godot's `top_level`; it draws and takes the pointer after the rest of its parent's tree, and a hidden parent still hides it |
| | `z_index` | `int` | `0` | Draws over controls with a lower one, added to its parent's; -4096 to 4096 |
| | `z_as_relative` | `bool` | `true` | Add `z_index` to its parent's; `false` makes it the whole of it |
| | `theme_override_colors` | `std::map<std::string, Vec4>` | `{}` | [Theme](#theme-items) colors by name, e.g. `{"font_color": [1, 0.8, 0.2, 1]}` |
| | `theme_override_constants` | `std::map<std::string, int>` | `{}` | Theme constants by name, e.g. `{"outline_size": 4}` |
| | `theme_override_fonts` | `std::map<std::string, std::string>` | `{}` | Theme fonts by name: a `.ttf`, `.otf` or [`.font.json`](#fonts) file, e.g. `{"font": "fonts/title.ttf"}` |
| | `theme_override_font_sizes` | `std::map<std::string, int>` | `{}` | Theme font sizes in pixels by name, 1 to 1024, e.g. `{"font_size": 32}` |
| | `theme_override_icons` | `std::map<std::string, std::string>` | `{}` | Theme icons by name: a PNG, BMP or KTX2 file, a [built-in icon](#theme-items) such as `"builtin:checked"`, or `""` for none, e.g. `{"checked": "ui/tick.png"}` |
| | `theme_override_styles` | `std::map<std::string, json>` | `{}` | Theme [style boxes](#style-boxes) by name, e.g. `{"panel": {"bg_color": [0.1, 0.1, 0.2, 1], "corner_radius": 8}}` |
| | `theme` | `std::string` | `""` | A [`.theme.json`](#themes) file whose items it and its children use before the project's theme |
| | `theme_type_variation` | `std::string` | `""` | A theme type to look its items up in before its own: a variation with a `base_type`, e.g. `"Header"` |
| | `size_flags_horizontal`, `size_flags_vertical` | `std::string` | `"fill"` | How a [container](#containers) sizes it on this axis: `fill`, `expand_fill`, `expand`, `shrink_begin`, `shrink_center`, `shrink_end`, `expand_shrink_center` or `expand_shrink_end` |
| | `size_flags_stretch_ratio` | `float` | `1` | Its share of the space expanding children split in a container |
| | `texture_filter` | `std::string` | `"inherit"` | How its textures are sampled when scaled: `linear`, `nearest`, or `inherit` its parent's, then the project's `rendering.texture_filter` |
| | `mouse_filter` | `std::string` | `""` | Whether a click, tap or the mouse [stops at it](#buttons) (`stop`), goes on to its parent too (`pass`) or passes it by (`ignore`); `""` takes its type's: `ignore` for `Label` and `NinePatchRect`, `pass` for `TextureRect` and containers other than `PanelContainer`, `stop` for the rest |
| | `tooltip_text` | `std::string` | `""` | Shown in a box beside the pointer once it [rests on the control](#tooltips-and-drag-and-drop); a parent's shows over children that pass the mouse on |
| | `focus_mode` | `std::string` | `""` | Whether clicks and the keyboard or a gamepad give it [focus](#buttons) (`all`), clicks only (`click`) or nothing (`none`); `""` takes its type's: `all` for buttons, `none` for the rest |
| | `focus_neighbor_left`, `_top`, `_right`, `_bottom` | `entity` | `null` | The control `ui_left`, `ui_up`, `ui_right` or `ui_down` moves focus to; `null` finds the closest one that way |
| | `focus_next`, `focus_previous` | `entity` | `null` | The control `ui_focus_next` (Tab) or `ui_focus_prev` (Shift+Tab) moves focus to; `null` takes the next or previous in the tree |
| | `layout_direction` | `std::string` | `"inherited"` | Which way it [lays out](#right-to-left-layout): `ltr`, `rtl` (anchors, offsets, containers and alignment mirrored) or `inherited` from its parent, left to right at the root |
| `DragAndDrop` | `drag_data` | `json` | `null` | What [dragging the control](#tooltips-and-drag-and-drop) carries; `null`: it can't be dragged |
| | `can_drop` | `std::vector<json>` | `[]` | What it takes when dropped on it: data matching one of these, where an object needs only the members given and anything else must be equal |
| | `drag_preview` | `entity` | `null` | A control with no parent shown at the pointer while a drag from it goes on, even if hidden |
| `Label` | `text` | `std::string` | `""` | The text; lines break at `"\n"` |
| | `horizontal_alignment` | `std::string` | `"left"` | Each line's place across the rect: `left`, `center`, `right` or `fill`, which stretches spaces to the width on every line but a wrapped paragraph's last |
| | `vertical_alignment` | `std::string` | `"top"` | The lines' place down the rect: `top`, `center`, `bottom` or `fill`, which spreads them over it |
| | `autowrap_mode` | `std::string` | `"off"` | Wrap lines to the rect's width: `off`, `arbitrary` (anywhere), `word` (between words) or `word_smart` (inside a word too long for a line) |
| | `clip_text` | `bool` | `false` | Cut off text outside the rect |
| | `text_overrun_behavior` | `std::string` | `"no_trimming"` | Cut lines wider than the rect: `no_trimming`, `trim_char`, `trim_word`, `trim_ellipsis` or `trim_word_ellipsis` |
| | `ellipsis_char` | `std::string` | `"…"` | What a line cut with an ellipsis ends with; its first character is used |
| | `uppercase` | `bool` | `false` | Draw the text in capitals |
| | `lines_skipped` | `int` | `0` | Lines left out from the top |
| | `max_lines_visible` | `int` | `-1` | Lines shown at most; -1 for all |
| | `visible_characters` | `int` | `-1` | Characters shown from the start, for typing effects; -1 for all |
| | `visible_characters_behavior` | `std::string` | `"chars_before_shaping"` | `chars_before_shaping` lays out only the visible characters; `chars_after_shaping` lays out all of them and hides the rest, so lines don't move as more show |
| | `text_direction` | `std::string` | `"auto"` | The text's base direction: `auto` (its first letter's: right to left for Arabic or Hebrew), `ltr`, `rtl`, or `inherited` from the control's `layout_direction` |
| | `language` | `std::string` | `""` | The text's language as a BCP 47 tag, such as `"ar"`, `"ja"` or `"zh-Hant"`, which picks [fallback fonts](#fonts) and the glyphs a font has for it |
| `ColorRect` | `color` | `Vec4` | `[1, 1, 1, 1]` | Fills the control's rect, as [r, g, b, a] from 0 to 1 |
| `Panel` | | | | Draws the theme's `panel` style box in the control's rect |
| `TextureRect` | `texture` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file drawn in the rect |
| | `expand_mode` | `std::string` | `"keep_size"` | Its minimum size: `keep_size` (the texture's), `ignore_size` (none), `fit_width` (the rect's height as its width), `fit_height` (its width as its height), or `fit_width_proportional` / `fit_height_proportional`, keeping the texture's aspect |
| | `stretch_mode` | `std::string` | `"scale"` | How the texture fills the rect: `scale`, `tile`, `keep` (its own size), `keep_centered`, `keep_aspect` (scaled to fit inside), `keep_aspect_centered` or `keep_aspect_covered` (scaled to cover it, cut to the rect) |
| | `flip_h`, `flip_v` | `bool` | `false` | Draw it mirrored left to right, or upside down |
| `NinePatchRect` | `texture` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file, cut into nine parts |
| | `region_rect` | `Vec4` | `[0, 0, 0, 0]` | The part of the texture to use, [x, y, width, height] in pixels; a zero size uses the whole texture |
| | `patch_margin_left`, `_top`, `_right`, `_bottom` | `int` | `0` | Pixels of the texture on each side that keep their size; the sides stretch along, the middle both ways |
| | `axis_stretch_horizontal`, `axis_stretch_vertical` | `std::string` | `"stretch"` | How the middle parts fill: `stretch`, `tile`, or `tile_fit` (tiles stretched to fit a whole number) |
| | `draw_center` | `bool` | `true` | Draw the middle part |
| `ProgressBar` | `value` | `double` | `0` | How full it is, between `min_value` and `max_value`, rounded to `step` |
| | `min_value`, `max_value` | `double` | `0`, `100` | The values of an empty and a full bar |
| | `step` | `double` | `0.01` | The value is rounded to a multiple of it; 0 for none |
| | `allow_greater`, `allow_lesser` | `bool` | `false` | Let the value go past `max_value` or `min_value`; the bar stops at full or empty, the percentage doesn't |
| | `fill_mode` | `std::string` | `"begin_to_end"` | Which way it fills: `begin_to_end`, `end_to_begin`, `top_to_bottom` or `bottom_to_top` |
| | `show_percentage` | `bool` | `true` | Draw the percentage, rounded, centered on the bar |
| `Slider` | `value` | `double` | `0` | The [slider](#sliders)'s value, between `min_value` and `max_value`, a multiple of `step` from `min_value` |
| | `min_value`, `max_value` | `double` | `0`, `100` | The values at its ends |
| | `step` | `double` | `1` | The value is a multiple of it, and the arrow keys and wheel move it this much; 0 for any value |
| | `page` | `double` | `0` | The value stays this much under `max_value` |
| | `rounded` | `bool` | `false` | The value is a whole number |
| | `allow_greater`, `allow_lesser` | `bool` | `false` | Let the value go past `max_value` or `min_value` when set; the grabber stops at the end |
| | `editable` | `bool` | `true` | The pointer, keys and gamepads move it; otherwise it draws dimmed |
| | `scrollable` | `bool` | `true` | The mouse wheel steps it |
| | `tick_count` | `int` | `0` | Ticks drawn along it, evenly spaced from end to end; 0 to 4096 |
| | `ticks_on_borders` | `bool` | `false` | Draw the first and last ticks, at its ends |
| | `ticks_position` | `std::string` | `"bottom_right"` | Where the ticks are: `bottom_right` (below, or right of a vertical one), `top_left`, `both` or `center` |
| | `vertical` | `bool` | `false` | Godot's `VSlider`: its minimum at the bottom; otherwise an `HSlider` |
| `ScrollBar` | `value` | `double` | `0` | The [scroll bar](#scrolling)'s value, between `min_value` and `max_value - page` |
| | `min_value`, `max_value` | `double` | `0`, `100` | The values at its ends |
| | `page` | `double` | `0` | The part of the range in view: the grabber is this share of the bar long, and a click beside it moves the value this much |
| | `step` | `double` | `0` | The value is a multiple of it from `min_value`; 0 for any value |
| | `custom_step` | `float` | `-1` | What the `ui_*` actions and its arrows step it by; -1 for `step` |
| | `rounded`, `allow_greater`, `allow_lesser` | `bool` | `false` | As `Slider`'s |
| | `vertical` | `bool` | `false` | Godot's `VScrollBar`; otherwise an `HScrollBar` |
| `Button` | `text` | `std::string` | `""` | The [button](#buttons)'s text; lines break at `"\n"` |
| | `icon` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file drawn beside the text |
| | `flat` | `bool` | `false` | Draw no style box, only the text and icon |
| | `alignment` | `std::string` | `"center"` | The text's place across the button: `left`, `center` or `right` |
| | `text_overrun_behavior` | `std::string` | `"no_trimming"` | Cut text wider than the button, as `Label`'s |
| | `autowrap_mode` | `std::string` | `"off"` | Wrap the text to the button's width, as `Label`'s |
| | `clip_text` | `bool` | `false` | Let the button be narrower than its text, cutting the text off |
| | `icon_alignment` | `std::string` | `"left"` | The icon's place across the button: `left`, `center` (behind the text) or `right` |
| | `vertical_icon_alignment` | `std::string` | `"center"` | The icon's place down it: `top` and `bottom` put the text below or above it |
| | `expand_icon` | `bool` | `false` | Scale the icon to fit the button, keeping its aspect |
| | `disabled` | `bool` | `false` | Can't be pressed, and draws dimmed |
| | `toggle_mode` | `bool` | `false` | Each press turns `button_pressed` on or off |
| | `button_pressed` | `bool` | `false` | On, for a toggle button |
| | `action_mode` | `std::string` | `"release"` | Press it when a click or tap ends over it (`release`) or as it starts (`press`) |
| | `button_mask` | `std::vector<std::string>` | `["left"]` | The mouse buttons that press it: `left`, `right`, `middle` |
| | `keep_pressed_outside` | `bool` | `false` | Draw it pressed while the pointer that pressed it is outside |
| | `button_group` | `std::string` | `""` | Toggle buttons with the same group name are one choice: pressing one turns the others off |
| | `shortcut` | `std::vector<json>` | `[]` | What else presses it: [bindings](project-format.md#input-actions) such as `{"type": "key", "key": "S", "ctrl": true}`, or actions such as `{"type": "action", "action": "ui_cancel"}` |
| | `shortcut_feedback` | `bool` | `true` | Draw it `hover_pressed` for 0.2 seconds when its shortcut presses it |
| | `text_direction`, `language` | `std::string` | `"auto"`, `""` | As `Label`'s |
| `TextureButton` | `texture_normal` | `std::string` | `""` | Project path of the PNG, BMP or KTX2 file it draws; its size is the button's minimum size |
| | `texture_pressed`, `texture_hover`, `texture_disabled` | `std::string` | `""` | Drawn instead in those states, when set |
| | `texture_focused` | `std::string` | `""` | Drawn over the others while it shows focus |
| | `ignore_texture_size` | `bool` | `false` | Need no space for the texture |
| | `stretch_mode` | `std::string` | `"keep"` | How the texture fills the button, as `TextureRect`'s: `scale`, `tile`, `keep`, `keep_centered`, `keep_aspect`, `keep_aspect_centered` or `keep_aspect_covered` |
| | `flip_h`, `flip_v` | `bool` | `false` | Draw it mirrored left to right, or upside down |
| | `disabled` … `shortcut_feedback` | | | As `Button`'s |
| `CheckBox` | `text` … `language` | | | As `Button`'s, but `toggle_mode` is `true` and `alignment` `"left"`: a [check box](#check-boxes) drawn with a box before its text, or a radio button in a `button_group` |
| `CheckButton` | `text` … `language` | | | As `CheckBox`'s, drawn with a switch after its text |
| `OptionButton` | `items` | `std::vector<OptionButtonItem>` | `[]` | The [drop-down list](#menus)'s options: `text`, `icon`, `id` (-1 for its index), `disabled`, `separator` and `tooltip` |
| | `selected` | `int` | `-1` | The option shown, by index; -1 for none. Picking one sets it |
| | `fit_to_longest_item` | `bool` | `true` | As wide as the widest option, not only the one shown |
| | `allow_reselect` | `bool` | `false` | Picking the option already shown emits `item_selected` again |
| | `flat` … `language` | | | As `Button`'s but `text` and `icon`, which are the option's; `toggle_mode` is `true`, `alignment` `"left"` and `action_mode` `"press"` |
| `MenuButton` | `items` | `std::vector<PopupMenuItem>` | `[]` | The [menu](#menus) it opens: items as `PopupMenu`'s |
| | `switch_on_hover` | `bool` | `false` | While another menu button's menu is open, moving onto this one opens its menu instead |
| | `text` … `language` | | | As `Button`'s, but `flat` and `toggle_mode` are `true` and `action_mode` `"press"`; it takes no focus unless `focus_mode` says so |
| `PopupMenu` | `items` | `std::vector<PopupMenuItem>` | `[]` | The [menu](#menus)'s items: `text`, `icon`, `checkable` (`none`, `check` or `radio`), `checked`, `id` (-1 for its index), `disabled`, `separator`, `indent` and `tooltip` (shown once the pointer rests on the item) |
| | `position` | `Vec2` | `[0, 0]` | Its top left corner in the viewport; moved to fit inside it |
| | `visible` | `bool` | `false` | Open: set it to open the menu; closing it clears it |
| | `hide_on_item_selection`, `hide_on_checkable_item_selection` | `bool` | `true` | Close when an item, or a checkable item, is picked |
| | `allow_search` | `bool` | `true` | Typing the start of an item's text moves to it |
| | `theme` | `std::string` | `""` | Project path of a `.theme.json` file to take its theme items from first |
| | `layout_direction` | `std::string` | `"inherited"` | `inherited` (left to right), `ltr` or `rtl` |
| `LineEdit` | `text` | `std::string` | `""` | The [text field](#text-fields)'s one line; line breaks show as spaces |
| | `placeholder_text` | `std::string` | `""` | Shown, dimmed, while the text is empty |
| | `alignment` | `std::string` | `"left"` | The text's place across the field: `left`, `center`, `right` or `fill` (as `left`) |
| | `max_length` | `int` | `0` | Most characters it takes; 0 for no limit. Typing or pasting more emits `text_change_rejected` |
| | `editable` | `bool` | `true` | Typing changes the text; off, it can still be selected and copied, and draws `read_only` |
| | `keep_editing_on_text_submit` | `bool` | `false` | Keep editing after `ui_text_submit` (Enter) |
| | `expand_to_text_length` | `bool` | `false` | Its minimum width grows to fit the text |
| | `backspace_deletes_composite_character_enabled` | `bool` | `false` | Backspace deletes a whole character with its accents, an emoji sequence or a Hangul syllable, not its last code point |
| | `virtual_keyboard_enabled` | `bool` | `true` | Show the on-screen keyboard while editing, on phones and tablets |
| | `virtual_keyboard_show_on_focus` | `bool` | `true` | Show it as soon as the field takes focus, not only when tapped |
| | `virtual_keyboard_type` | `std::string` | `"default"` | `default`, `multiline`, `number`, `number_decimal`, `phone`, `email_address`, `password` or `url` |
| | `clear_button_enabled` | `bool` | `false` | Show a button at the end, while there is text, that clears it |
| | `shortcut_keys_enabled` | `bool` | `true` | Cut, copy, paste, select all, undo and redo by keyboard shortcut |
| | `selecting_enabled` | `bool` | `true` | Text can be selected |
| | `deselect_on_focus_loss_enabled` | `bool` | `true` | Losing focus drops the selection |
| | `drag_and_drop_selection_enabled` | `bool` | `true` | The selection can be [dragged](#tooltips-and-drag-and-drop) elsewhere in the field or to another |
| | `right_icon` | `std::string` | `""` | Project path of a PNG, BMP or KTX2 file drawn at the end |
| | `icon_expand_mode` | `std::string` | `"original_size"` | The right icon's size: `original_size`, `fit_to_text` (the font's height) or `fit_to_line_edit` (the field's height times `right_icon_scale`) |
| | `right_icon_scale` | `float` | `1` | For `fit_to_line_edit` |
| | `flat` | `bool` | `false` | Draw no style box |
| | `select_all_on_focus` | `bool` | `false` | Select all the text when it takes focus |
| | `caret_blink`, `caret_blink_interval` | `bool`, `float` | `false`, `0.65` | The caret blinks, shown then hidden for that many seconds |
| | `caret_column` | `int` | `0` | Characters before the caret |
| | `caret_force_displayed` | `bool` | `false` | Draw the caret even when not editing |
| | `caret_mid_grapheme` | `bool` | `false` | Let the caret stop between a letter and its accents, or inside an emoji sequence |
| | `secret` | `bool` | `false` | Show each character as `secret_character`, for passwords; cut and copy do nothing |
| | `secret_character` | `std::string` | `"•"` | One character |
| | `text_direction`, `language` | `std::string` | `"auto"`, `""` | As `Label`'s |
| `TextEdit` | `text` | `std::string` | `""` | The [text field](#text-fields-of-several-lines)'s lines, broken at `"\n"` |
| | `placeholder_text` | `std::string` | `""` | Shown, dimmed, while the text is empty |
| | `editable` | `bool` | `true` | Typing changes the text; off, it can still be selected and copied, and draws `read_only` |
| | `backspace_deletes_composite_character_enabled` | `bool` | `false` | As `LineEdit`'s |
| | `shortcut_keys_enabled`, `selecting_enabled`, `deselect_on_focus_loss_enabled`, `drag_and_drop_selection_enabled` | `bool` | `true` | As `LineEdit`'s |
| | `empty_selection_clipboard_enabled` | `bool` | `true` | Cut and copy with nothing selected take the caret's line |
| | `wrap_mode` | `std::string` | `"none"` | `boundary` wraps lines wider than the field |
| | `autowrap_mode` | `std::string` | `"word_smart"` | Where wrapped lines break: `word` (between words), `word_smart` (inside a word too long for a row) or `arbitrary` (anywhere) |
| | `tab_input_mode` | `bool` | `true` | Tab types a tab; off, Tab moves focus. `ui_focus_mode` (Ctrl+M) switches it |
| | `virtual_keyboard_enabled`, `virtual_keyboard_show_on_focus` | `bool` | `true` | As `LineEdit`'s; the keyboard is `multiline` |
| | `scroll_past_end_of_file` | `bool` | `false` | Let the last line scroll up to the top |
| | `scroll_vertical` | `double` | `0` | Rows scrolled down; the field writes it as it scrolls |
| | `scroll_horizontal` | `int` | `0` | Pixels scrolled right |
| | `scroll_fit_content_height`, `scroll_fit_content_width` | `bool` | `false` | Its minimum height grows to fit every row, or its minimum width the widest line |
| | `caret_type` | `std::string` | `"line"` | `line` between characters or `block` over one |
| | `caret_blink`, `caret_blink_interval` | `bool`, `float` | `false`, `0.65` | As `LineEdit`'s |
| | `caret_draw_when_editable_disabled` | `bool` | `false` | Draw the caret when not `editable` |
| | `caret_move_on_right_click` | `bool` | `true` | A right click outside the selection moves the caret |
| | `caret_mid_grapheme` | `bool` | `false` | As `LineEdit`'s |
| | `caret_line`, `caret_column` | `int` | `0` | The caret's line, and the characters before it in that line |
| | `highlight_current_line` | `bool` | `false` | Draw `current_line_color` behind the caret's row |
| | `draw_tabs`, `draw_spaces` | `bool` | `false` | Draw an arrow for each tab, a dot for each space |
| | `text_direction`, `language` | `std::string` | `"auto"`, `""` | As `Label`'s |
| | `highlight_all_occurrences` | `bool` | `false` | Mark the other copies of the selected text in view, whole words of the same case |
| | `search_text` | `std::string` | `""` | [Marked](#searching-and-highlighting-code) wherever it occurs in view |
| | `search_flags` | `std::vector<std::string>` | `[]` | How `search_text` matches: `match_case`, `whole_words` |
| | `gutters` | `std::vector<TextEditGutter>` | `[]` | [Columns](#gutters-and-line-numbers) left of the text: `name`, `type` (`string` or `icon`), `width` (`24`), `draw` (`true`) and `lines`, each `{"line": int, "text": string, "icon": string, "color": Vec4}` |
| `CodeHighlighter` | `number_color`, `symbol_color`, `function_color`, `member_variable_color` | `Vec4` | `[0, 0, 0, 1]` | [Colors](#searching-and-highlighting-code) for numbers, symbols, a word before `(` and a word after `.` in the `TextEdit` on its entity |
| | `keyword_colors` | `std::map<std::string, Vec4>` | `{}` | Words and their colors; opaque black colors nothing, as Godot's `Color()` |
| | `member_keyword_colors` | `std::map<std::string, Vec4>` | `{}` | Words colored as keywords unless they come after a `.` |
| | `color_regions` | `std::map<std::string, Vec4>` | `{}` | Regions by `"start end"` keys of symbols, such as `"/* */"`; a key with no end, such as `"#"`, runs to the line's end |
| `CodeEdit` | `gutters_draw_line_numbers` | `bool` | `false` | Number the lines of the `TextEdit` on its entity, in a [gutter](#gutters-and-line-numbers) |
| | `gutters_zero_pad_line_numbers` | `bool` | `false` | Pad the numbers with `0`s rather than spaces |
| | `gutters_line_numbers_min_digits` | `int` | `3` | The fewest digits the numbers take room for, `1` to `5` |
| | `gutters_draw_breakpoints_gutter`, `gutters_draw_bookmarks`, `gutters_draw_executing_lines` | `bool` | `false` | Show breakpoints, bookmarks and executing lines in the main gutter |
| | `breakpointed_lines`, `bookmarked_lines`, `executing_lines` | `std::vector<int>` | `[]` | The lines that have them, from 0 |
| | `line_length_guidelines` | `std::vector<int>` | `[]` | Columns, in widths of `0`, to draw a line down the text at |
| `BoxContainer` | `vertical` | `bool` | `false` | Lays its children out in a column instead of a row |
| | `alignment` | `std::string` | `"begin"` | Where they go when none expands: `begin`, `center` or `end` |
| `GridContainer` | `columns` | `int` | `1` | Children per row, 1 to 1024 |
| `MarginContainer` | | | | Fits its children in its rect less the theme's margins |
| `CenterContainer` | `use_top_left` | `bool` | `false` | Center its children on its top-left corner instead of its center |
| `FlowContainer` | `vertical` | `bool` | `false` | Columns that wrap to its height instead of rows that wrap to its width |
| | `alignment` | `std::string` | `"begin"` | Where each line's children go along it: `begin`, `center` or `end` |
| | `last_wrap_alignment` | `std::string` | `"inherit"` | Where the last line's children go when it isn't full: `inherit` (as `alignment`), `begin`, `center` or `end` |
| | `reverse_fill` | `bool` | `false` | Fill rows from the bottom up, or columns from the right |
| `PanelContainer` | | | | Draws the theme's `panel` style box and fits its children inside its margins |
| `ScrollContainer` | `horizontal_scroll_mode`, `vertical_scroll_mode` | `std::string` | `"auto"` | How it [scrolls](#scrolling) on each axis: `disabled` (no scrolling: it is as big as its children), `auto` (a scroll bar when they don't fit), `always`, `never` (scrolls, with no bar) or `reserve` (as `auto`, with room kept for the bar) |
| | `scroll_horizontal`, `scroll_vertical` | `int` | `0` | Pixels scrolled right and down; held to what its children leave at the next tick |
| | `scroll_horizontal_custom_step`, `scroll_vertical_custom_step` | `float` | `-1` | What its scroll bars step by; -1 for their own |
| | `scroll_deadzone` | `int` | `0` | Pixels a touch moves before it drags the scroll |
| | `follow_focus` | `bool` | `false` | Scroll to show a control inside it that gets focus |
| | `scroll_horizontal_by_default` | `bool` | `false` | The wheel scrolls across, and Shift with the wheel down |
| | `draw_focus_border` | `bool` | `false` | Draw its `focus` style box while it or a control inside it has focus |
| `AudioStreamPlayer` | `stream` | `std::string` | `""` | Project path of a `.wav`, `.ogg`, `.mp3` or [`.audio.json`](#audio-streams) file; `""` plays nothing |
| | `volume_db` | `float` | `0` | Volume in dB, -80 or more |
| | `pitch_scale` | `float` | `1` | Speed and pitch, times the sound's own, 0.01 to 16 |
| | `playing` | `bool` | `false` | Whether it [plays](#audio). Setting it starts the sound from the beginning or stops it; the sound ending sets it to false |
| | `autoplay` | `bool` | `false` | Plays once the audio system first sees it |
| | `stream_paused` | `bool` | `false` | Holds its sounds where they are; clearing it resumes them |
| | `max_polyphony` | `int` | `1` | Sounds it may play at once, 1 to 64; playing again past this stops the oldest |
| | `bus` | `std::string` | `"Master"` | The bus it plays through, from the project's [bus layout](#bus-layouts); Master if there is no such bus |
| | `mix_target` | `std::string` | `"stereo"` | Which speakers play it in [surround](#surround-sound): `stereo` (the front pair), `surround` (every speaker) or `center` (the center speaker and the LFE). Stereo plays it on both |
| | `parameters` | `std::map<std::string, json::Value>` | `{}` | Its stream's [parameters](#stream-parameters), as Godot's `parameters/`: `switch_to_clip` for interactive music, `stream_<index>_volume_db` for a synchronized stream |
| `AudioStreamPlayer2D` | `stream`, `volume_db`, `pitch_scale`, `playing`, `autoplay`, `stream_paused`, `max_polyphony`, `bus`, `parameters` | | | As `AudioStreamPlayer`'s; it plays at the entity's `Transform2D` |
| | `max_distance` | `float` | `2000` | Pixels from the listener beyond which it is silent, 1 or more |
| | `attenuation` | `float` | `1` | How fast it fades with distance: volume is (1 - distance / `max_distance`) to this power |
| | `panning_strength` | `float` | `1` | Times the project's `audio.panning_strength_2d`; 0 for no panning |
| | `area_mask` | `int` | `1` | `Area2D` layers, as bits, whose `audio_bus_override` changes its bus while it is inside one; 0 ignores areas |
| `AudioListener2D` | `current` | `bool` | `false` | Hear `AudioStreamPlayer2D` sounds from this entity's `Transform2D`, turned with it. The first current one by ID is used; with none, the middle of the viewport |

`engine.describe` lists every registered component with its defaults, including ones a game adds.

## Parent transforms

An entity listed in another's `Transform2D` `children` is placed in its parent's space, as a
Godot Node2D is in its parent's: its `position`, `rotation`, `scale` and `skew` are its own, and
its global transform is its parent's global transform times its own (Godot's
`get_global_transform`). An entity no one lists is a root, placed in the world.
[examples/parent-transforms](../../examples/parent-transforms/) has a boat with a mast, a flag, a
cannon and a deck.

```json
"boat": {"components": {"Transform2D": {"children": ["mast"], "position": [120.0, 120.0]}}},
"mast": {"components": {"Transform2D": {"children": ["flag"], "position": [-4.0, -18.0]}}},
"flag": {"components": {"Transform2D": {"position": [6.0, -9.0]}}}
```

- **Drawing.** Each root draws in the order entities were made, followed by its children in the
  order its list gives and theirs after them, so a child draws over its parent. `z_index` is
  added to the parents' (Godot's `z_as_relative`), and the sum is held to -4096 to 4096; with
  `z_as_relative` off, an entity's `z_index` is the whole of it, and its children add theirs to
  it. A parent's `Sprite2D` or `TileMapLayer` passes on its `visible` and `modulate`
  (`self_modulate` stays its own); a `CPUParticles2D` or `GPUParticles2D` passes on `visible`
  and `z_index`.
  Sprites, tile layers, particles, occluders and lights draw where their global transforms put
  them.
- **Top level.** An entity with `top_level` keeps its parent, but its transform is its global
  one, as Godot's `top_level`: moving the parent leaves it where it is, and what it holds is
  placed in its space. Like Godot's top-level items, it draws with what it holds after the rest
  of its tree and before roots made later, top-level entities in tree order, and takes neither
  its parents' `z_index` nor their `modulate`; hiding a parent still hides it. A body with
  `top_level` is where its own transform says.
- **Physics.** Bodies, areas and joints are where their global transforms put them, and a parent
  that moves moves the bodies in it. The physics step writes a `RigidBody2D`'s motion back as
  Godot sets a body's global transform: its own transform becomes what puts it where Box2D says.
  A `CharacterBody2D` slides in the world and is put back in its parent's space. A body or
  area takes the shapes of its own entity, then those of its children that have a
  `CollisionShape2D` or `CollisionPolygon2D` and no body or area of their own, placed by their
  transforms, as a Godot `CollisionObject2D` takes its shape nodes'; hits and events name the
  body. A character moves as the first of its shapes. Shapes and light textures don't lean
  with `skew`.
- **Sound and particles.** `AudioStreamPlayer2D`, `AudioListener2D`, `CPUParticles2D` and
  `GPUParticles2D` work from global transforms; particles with `local_coords` move with the emitter's.
- **Rules.** A child needs a `Transform2D`, has one parent and is never its own ancestor. `scene
  validate` reports each broken rule at the entry's JSON Pointer, and a command that would break
  one fails and changes nothing, as Godot's `add_child` refuses:

  ```json
  {"code":-32002,"data":{"path":"/entities/flag/components/Transform2D/children/0"},"message":"/entities/flag/components/Transform2D/children/0: \"boat\" is an ancestor of \"flag\", so it can't be its child"}
  ```

- **Commands.** [`entity.get`](commands.md#entityget) adds a child's `parent` and `global`
  transform. [`entity.destroy`](commands.md#entitydestroy) destroys the entity's `Transform2D` and
  `Control` children with it, as Godot frees a node's children, and takes destroyed entities out
  of the lists of parents that stay.
- **Not yet.** Node2D and Control entities in one tree.

## Paths

`Path2D` and `Path3D` are Godot's paths: a curve through `points`, each with a `position` and
`in` and `out` handles relative to it (`[0, 0]` or `[0, 0, 0]` for a sharp corner), joined by
cubic Bézier segments. The curve is baked as Godot bakes it, each segment halved until its
pieces are no longer than `bake_interval`, so its length and the points along it are Godot's.
An entity with a `PathFollow2D` or `PathFollow3D` in a path's `Transform2D` or `Transform3D`
`children` sits `progress` along its curve: each tick the `path_follow` system, after animations
and before physics, wraps its `progress` and sets its transform's position and rotation, as
Godot's `PathFollow2D` and `PathFollow3D` do.

```json
"track": {"components": {
  "Transform3D": {"children": ["cart"]},
  "Path3D": {"closed": true, "points": [
    {"position": [0, 0, 3], "in": [2, 0, 0], "out": [-2, 0, 0]},
    {"position": [-4, 0, 0], "in": [0, 0, 2], "out": [0, 0, -2]},
    {"position": [0, 1, -3], "in": [-2, 0, 0], "out": [2, 0, 0]},
    {"position": [4, 0, 0], "in": [0, 0, -2], "out": [0, 0, 2]}
  ]}
}},
"cart": {"components": {
  "Transform3D": {},
  "PathFollow3D": {"progress": 2.5, "rotation_mode": "oriented"}
}}
```

| Component | Fields (defaults) |
| --- | --- |
| `Path2D` | `points` (`position`, `in`, `out`), `bake_interval` `5` pixels |
| `Path3D` | `points` (`position`, `in`, `out`, `tilt` `0`: radians the up vector turns about the way the curve goes there), `closed` `false` (a segment back to the first point), `bake_interval` `0.2` meters, `up_vector_enabled` `true` (up vectors carried along the curve as a parallel transport frame; without them y is up) |
| `PathFollow2D` | `progress` `0` (pixels along the curve), `h_offset` and `v_offset` `0` (along and across it), `rotates` `true` (turned to face the way it goes; otherwise the offsets are the world's axes and the rotation is left alone), `cubic_interp` `true`, `loop` `true` |
| `PathFollow3D` | `progress` `0` (meters), `h_offset` and `v_offset` `0` (along its x and y), `rotation_mode` `"xyz"`, `use_model_front` `false` (+z faces the way it goes rather than -z), `cubic_interp` `true`, `loop` `true`, `tilt_enabled` `true` |

- **Progress.** With `loop`, progress goes round: past the end it comes back from the start, and
  below zero from the end. Without, it stops at the ends. The system writes the wrapped value
  back, so an `AnimationPlayer` track or gameplay code can keep adding to it.
- **Rotation.** `PathFollow3D`'s `rotation_mode` is Godot's: `"none"` leaves it unturned,
  `"y"` turns it only about y, `"xy"` about y and x, `"xyz"` as the curve's up vectors say, and
  `"oriented"` faces it the way the curve goes with y up. With `tilt_enabled`, the curve's tilt
  rolls it after. Its `scale` is left as it is.
- **Checks.** [`scene validate`](cli.md#scene-validate) and exports report a follower whose
  parent has no path of its kind, and `"oriented"` on a `Path3D` without up vectors, as Godot's
  configuration warnings.
- **Commands.** [`path.get`](commands.md#pathget) returns a curve's length, the position and
  rotation at offsets or ratios along it, and its nearest point to another.

A [`CSGPolygon3D`](#csg-shapes) in `"path"` mode pushes its polygon along a `Path3D`.
[examples/paths](../../examples/paths/) laps a cart round a rail.

## 3D nodes

The 3D world is Godot's 3D nodes as components. Units are meters, y points up, and a camera or
light looks down its entity's -z. A `Transform3D` with `children` places them in its space, as a
`Node3D` places its child nodes: they move, turn, scale and hide with it. A cycle is broken at
the entity made first, and `scene validate` reports it.

Games, `slash-cli run` and [`frame.capture`](commands.md#framecapture) draw the 3D world through
its current `Camera3D` (the first with `current` set, else the first one, in tree order), under
every canvas, so sprites and controls draw over it, as they do over Godot's 3D viewport. A world
with no `Camera3D` draws no 3D. Between ticks, entities are drawn part of the way from where
they were to where they are, as sprites are. `frame.capture`'s `drawn` counts `meshes`.

```json
"crate": {"components": {
  "Transform3D": {"position": [0, 0.5, 0], "rotation": [0, 0.5, 0]},
  "MeshInstance3D": {},
  "BoxMesh": {"size": [1, 1, 1]},
  "StandardMaterial3D": {"albedo_color": [0.85, 0.2, 0.15, 1], "roughness": 0.6}
}}
```

[examples/scene-3d](../../examples/scene-3d/) is a lit table with a crate, a ball and a ring,
[examples/shadows-3d](../../examples/shadows-3d/) a courtyard where the sun, a lamp and a
flashlight cast shadows, [examples/models-3d](../../examples/models-3d/) a house loaded from a
glTF file, [examples/obj-models](../../examples/obj-models/) a lamp post from an OBJ file,
[examples/fbx-models](../../examples/fbx-models/) a windmill from an FBX file and
[examples/blend-models](../../examples/blend-models/) a toadstool from a Blender file.

### Meshes

`MeshInstance3D` draws its entity's primitive mesh, colored by its `StandardMaterial3D` or, with
none, Godot's default material (white, rough and lit). Its `layers` (default `1`) are render
layers: a camera whose `cull_mask` and a light whose `light_cull_mask` share a bit with them see
it and light it, and a light whose `shadow_caster_mask` shares one gets its shadow.

Its `cast_shadow` (default `"on"`) is Godot's `GeometryInstance3D.cast_shadow`: `"on"` casts
shadows, culling faces as its material does; `"double_sided"` casts from both sides of every
face; `"shadows_only"` casts but isn't drawn; `"off"` casts none. A mesh on layers the camera
doesn't see still casts, as in Godot. Alpha-blended meshes cast no shadow, and alpha scissor
ones cast only where their pixels are kept.

Its `gi_mode` (default `"static"`) and `gi_lightmap_texel_scale` (default `1`) are Godot's: a
`"static"` mesh is baked into a [LightmapGI's lightmap](#baked-lightmaps) and lit by it, with
`gi_lightmap_texel_scale` times the texels; a `"dynamic"` mesh isn't baked but takes its ambient
light from the LightmapGI's [light probes](#light-probes), and a `"disabled"` one from neither. A
[VoxelGI](#voxelgi) voxelizes `"static"` meshes too, plots `"dynamic"` ones into its light as they
move, and lights every other mesh in its box.

The primitive meshes are Godot's, made as Godot makes them: the same corners, normals, tangents
and texture coordinates, so a texture wraps them as it does in Godot. An entity has at most one.
Meshes of equal settings share one mesh on the GPU.

| Component | Fields (defaults) |
| --- | --- |
| `BoxMesh` | `size` `[1, 1, 1]`; `subdivide_width`, `subdivide_height`, `subdivide_depth` `0` |
| `SphereMesh` | `radius` `0.5`, `height` `1`, `radial_segments` `64`, `rings` `32`, `is_hemisphere` `false` |
| `CylinderMesh` | `top_radius` `0.5`, `bottom_radius` `0.5` (`0` for a cone), `height` `2`, `radial_segments` `64`, `rings` `4`, `cap_top` and `cap_bottom` `true` |
| `CapsuleMesh` | `radius` `0.5`, `height` `2` (end to end), `radial_segments` `64`, `rings` `8` |
| `PlaneMesh` | `size` `[2, 2]`, `subdivide_width` and `subdivide_depth` `0`, `center_offset` `[0, 0, 0]`, `orientation` `"y"` (the axis it faces: `"x"`, `"y"` or `"z"`) |
| `QuadMesh` | as `PlaneMesh`, with `size` `[1, 1]` and `orientation` `"z"` |
| `PrismMesh` | `left_to_right` `0.5` (where the top edge sits across), `size` `[1, 1, 1]`, subdivisions as `BoxMesh` |
| `TorusMesh` | `inner_radius` `0.5`, `outer_radius` `1` (they can't be equal), `rings` `64`, `ring_segments` `32` |

Every primitive also has `flip_faces` (`false`), which turns it inside out, as for a skybox.

### Models

`MeshInstance3D`'s `mesh` (default `""`) names a model by project path: a glTF 2.0 `.gltf` file,
with its buffers and images in files beside it or in data URIs, a `.glb` file holding
everything, a Wavefront [`.obj` file](#obj-models), an [`.fbx` file](#fbx-models) or a
[Blender `.blend` file](#blender-models). The entity draws the model's meshes in
place of a primitive mesh, read as Godot's importers read them:

```json
"house": {"components": {
  "Transform3D": {"position": [0, 0, -3]},
  "MeshInstance3D": {"mesh": "models/house.gltf"}
}}
```

- Each node of the model's scene that has a mesh draws it where the node's transforms place it,
  in the entity's space. glTF's units are meters with y up, as Godot's are.
- Each glTF material becomes a `StandardMaterial3D` as Godot's importer makes it: the base color
  factor and texture as the albedo, the metallic and roughness factors times the
  metallic-roughness texture's blue and green channels, the normal map and its scale, the
  emissive factor or texture, `alphaMode` (`"BLEND"` is `"alpha"`, `"MASK"` is
  `"alpha_scissor"` at `alphaCutoff`), `doubleSided` as `cull_mode` `"disabled"`, and the base
  color texture's sampler as `texture_filter` and `texture_repeat`. Linear colors are turned to
  sRGB. `KHR_materials_emissive_strength`, `KHR_materials_unlit`, `KHR_texture_transform` and
  `KHR_materials_pbrSpecularGlossiness` (its diffuse color and glossiness) are read too.
- A primitive with vertex colors multiplies them into its albedo, and one with no material gets
  Godot's default material.
- A `StandardMaterial3D` on the entity replaces every material of the model, as Godot's
  `material_override`.
- Models keep Godot's clockwise front faces, a primitive with no normals gets one per face, and
  one with texture coordinates but no tangents gets MikkTSpace's, as Godot's importer makes
  them. `KHR_mesh_quantization` attributes are read.
- Skins deform their meshes and animations move the model's nodes, as
  [skeletons and model animations](#skeletons-and-model-animations) says.
- Points and lines are left out, since only triangles draw, and so are cameras, `KHR_lights_punctual`
  lights and morph targets. A file that requires any other extension, such as Draco or meshopt
  compression, is refused, naming it.

Images are PNG, JPEG, BMP or KTX2 files, as [textures](#textures) are. Entities drawing one model
share its meshes and textures on the device, and the model reloads when its file changes.
[`model.inspect`](commands.md#modelinspect) lists what a model holds without drawing it, and
`slash-cli scene validate` takes a `.gltf`, `.glb`, `.obj`, `.fbx` or `.blend` file and checks it as
`MeshInstance3D` loads it.

#### OBJ models

An `.obj` file is read as Godot's OBJ importer reads one as a scene, with its `.mtl` material
libraries beside it:

- Each `o` object is a mesh, named by its `o` line (`"Mesh"` before the first), and each run of
  faces between `usemtl` lines is a surface of it. `g` groups are left alone, as in Godot.
- Faces are fans of triangles from their first corner, and corners may count back from the
  latest vertex with negative indices. A vertex's color after its position (`v x y z r g b`) is
  kept, though Godot's material doesn't show it.
- A surface without normals gets smooth ones by its smoothing groups: corners at the same
  position in a group share the sum of their faces' normals. Faces are smooth until `s off`,
  which makes them flat; any other `s` line after it starts a new group. A surface with texture
  coordinates gets MikkTSpace's tangents; one without, Godot's stand-in tangents.
- `mtllib` names a library relative to the model, and `usemtl` a material in the latest one;
  faces before any `usemtl` in an object get Godot's default material. Each material becomes a
  `StandardMaterial3D` as Godot's importer makes it: `Kd` is the albedo color; `Ks`'s brightest
  channel, or `Ns` after it as `(1000 - Ns) / 1000`, is the `metallic` (Godot's importer sets no
  roughness); `d`, or `1 - Tr`, is the albedo's alpha, with `transparency` `"alpha"` below 0.99;
  `map_Kd` is the albedo texture, `map_Ks` the metallic one, `map_Ns` the roughness one and
  `map_bump` the normal map. Ambient colors and other keywords are left out. Texture paths are
  relative to the library, with backslashes read as slashes.
- A library that can't be read, or that Godot's importer would refuse (a color before any
  `newmtl`, or one with too few numbers), leaves its materials out, and a texture outside the
  project folder or not a texture file is left out of its material. The model still loads, and
  `scene validate` reports each of them with its line.

#### FBX models

An `.fbx` file, binary or ASCII, is read with ufbx as Godot 4.3's FBX importer reads one:

- Lengths become meters and axes y up, whatever units and axes the file was written in, and each
  node's geometric transform and pivots are applied to its mesh, as Godot's importer does.
- Each node with a mesh draws it where the node's transforms place it, in tree order. Polygons
  become triangles, and each material on a mesh is a surface of it. A mesh without normals gets
  smooth ones; one with texture coordinates but no tangents gets MikkTSpace's, one without,
  Godot's stand-in tangents.
- Vertex colors multiply into the albedo, since Godot's importer sets
  `vertex_color_use_as_albedo` on every material, and a surface with vertex colors and no
  material gets one that shows them.
- Each material becomes a `StandardMaterial3D` as Godot's importer makes it: the base color
  times its factor as the albedo color, turned to sRGB; the base color texture, its UV
  transform as `uv1_offset` and `uv1_scale`, `texture_repeat` only if it wraps both ways, and
  `transparency` from its alpha (`"alpha"` if some of it is partly clear, `"alpha_scissor"` if
  it is only clear or opaque); the PBR metalness and roughness (1 under a texture, which is read
  from its red channel); the normal map and its scale; and the emission color, with the emission
  factor as its energy, or the emission texture. Ambient occlusion and separate transparency
  textures are left out.
- Textures carried in the file are used as they are. Others are looked for as named, relative to
  the model, then by file name in the model's folder and the folders Godot looks in under it
  (`textures`, `Textures`, `images`, `Images`, `materials`, `Materials`, `maps`, `Maps`, `tex`,
  `Tex`), then in each folder above it up to the project's. A texture that isn't found, isn't a
  PNG, JPEG, BMP or KTX2 file, or doesn't decode is left out of its materials; the model still
  loads, and `scene validate` reports it, with the warnings ufbx gives about the file.
- The file's nodes are the model's, named as glTF nodes are (a node with no name is `Mesh` if it
  has a mesh, `Camera3D` if it has a camera, else `Node`). A mesh's first skin deformer is its
  skin: each cluster's bone is a joint, with the cluster's geometry-to-bone matrix as its inverse
  bind, and each vertex takes its four largest weights, or eight when a vertex of the skin has more
  than four, normalized.
- Each animation stack is an animation, baked into linear keys 30 times a second as Godot's
  importer bakes it (any curve interpolation, Euler rotations in any order), as long as the
  stack's end and named and looping as glTF animations are. Tracks that only hold a node where it
  is, and tracks on a skinned mesh's own node, are left out, as Godot leaves them out.
- Cameras, lights and blend shapes are left out.

#### Blender models

A `.blend` file is imported as Godot imports one: the engine never reads it, but runs the
artist's own Blender in the background (`blender --background --python-expr ...`), which opens
the file and exports it as a `.glb` file with Godot's default import options (y up, modifiers
applied, every object whether visible or not, materials, skins and actions, no vertex colors),
and reads that as a [glTF model](#meshes). No Blender code is linked into the engine or shipped
with a game.

- Blender is the program `SLASH_BLENDER_PATH` names, else the first of these that runs, as
  Godot looks: `blender` on `PATH`, then where Blender's installers put it (`/Applications` and
  `~/Applications`, Homebrew and MacPorts on macOS; `C:\Program Files\Blender Foundation` on
  Windows; `/usr/bin`, `/usr/local/bin`, `/opt/blender/bin` and the Snap Store on Linux). It
  must be Blender 3.0 or later.
- The glTF is kept in the project's `.slash/imported/` folder, named after the file and a hash
  of its bytes (`.slash/imported/models/tree.blend-<hash>.glb`), with what Blender printed in a
  `.log` file beside it. It is used until the `.blend` file changes; then the model reloads,
  Blender converts it again and the files made before are removed. A conversion that fails
  gives Blender's last lines and names its log.
- Running a program needs the `build` capability: a session without it reads files converted
  before and reports the rest. `slash-cli export` converts every `.blend` file in the project
  and ships the glTF under the `.blend` file's name, so games never need Blender, and
  `slash-cli scene validate` converts the files it checks.
- Textures packed into the `.blend` file, or found where it names them, are carried inside the
  `.glb` file.

#### Skeletons and model animations

A glTF or FBX model's nodes are its bones, as Godot's importers make a `Skeleton3D` of them: its
skins deform their meshes, its animations move its nodes, and an entity's `AnimationPlayer` and
`Skeleton3D` pose them. A `.blend` file's armatures and actions come through its glTF, and an FBX
file's skin deformers and animation stacks are read as [FBX models](#fbx-models) says.

```json
"hero": {"components": {
  "Transform3D": {},
  "MeshInstance3D": {"mesh": "models/hero.glb"},
  "AnimationPlayer": {"library": "models/hero.glb", "animation": "Walk-loop"},
  "Skeleton3D": {"bone_rotations": {"Head": [0, 0.4, 0]}}
}}
```

- Nodes are named as Godot names them: `.`, `:`, `@`, `/`, `"` and `%` become `_`, a name used
  before gets a number from 2 (`Arm`, `Arm2`), and a node with no name is `Mesh` if it has a mesh,
  else `Node`.
- A mesh with a skin is moved by its joints, as Godot's skeleton shader moves it: each vertex by
  up to eight joints (`JOINTS_0`, `JOINTS_1` and their weights, normalized), each joint by its
  node's transform times its inverse bind matrix, with normals and tangents turned the same way.
  It is placed by its joints, not by the node that holds it, as glTF says, and a vertex with no
  weight stays where it is. A skin whose joints aren't all in the model's scene is left out.
- Each animation is one Godot imports: as long as its last key, named as nodes are (`,` and `[`
  also become `_`; one with no name is `Animation`), and looping linearly when its name begins or
  ends with `loop` or `cycle` in any case (`Walk-loop`, `cycle_run`), as Godot's import hints.
  Position, rotation and scale keys play with their interpolation (`STEP` holds each key,
  `CUBICSPLINE` is sampled 30 times a second); morph target weights are left out.
- An `AnimationPlayer` whose `library` is the model plays its animations by name, as Godot's
  `AnimationPlayer` plays an imported scene's library. A [`.anim.json` animation](#animations) can
  come `from` a model's animation and add tracks of its own, such as footsteps. An animation from
  another model moves the nodes of the same names, so one rig's animations can play on every model
  made from it. The pose follows the player's `time` while paused too.
- A model is posed from its rest, then by its animation, then by its `Skeleton3D`, each time it
  is drawn, so undo, snapshots and dry runs always draw what the components say. Between ticks it
  is drawn part of the way from its last pose, as entities are.

| `Skeleton3D` field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bone_positions` | Map of `Vec3` | `{}` | A bone's position in its parent, by name, over its animation, as Godot's `set_bone_pose_position` |
| `bone_rotations` | Map of `Vec3` | `{}` | A bone's rotation in its parent, in radians as `Transform3D`'s, by name |
| `bone_scales` | Map of `Vec3` | `{}` | A bone's scale, by name |
| `show_rest_only` | `bool` | `false` | Draw the model at rest, whatever plays |

[`skeleton.get`](commands.md#skeletonget) lists a model's bones and where each is posed, and
[`model.inspect`](commands.md#modelinspect) its skins and animations. `scene validate` reports a
`Skeleton3D` bone the model hasn't (with "did you mean") and one on an entity that draws no
model. [examples/skinned-meshes](../../examples/skinned-meshes/) waves a skinned arm, from a
glTF file and from the same arm as an FBX file.

#### IK modifiers

`TwoBoneIK3D`, `FABRIK3D`, `CCDIK3D` and `JacobianIK3D` are Godot 4.6's IK modifiers: they turn a
model's bones after its animations and `Skeleton3D`, so a hand reaches a target while the arm
walks. Each goes on the entity that draws the model or on an entity in its `Transform3D`
`children` (Godot's modifier children of a `Skeleton3D`), and names bones by name and target and
pole entities by ID, which may be anywhere in the world.

```json
"goal": {"components": {"Transform3D": {"position": [1, 1, 0]}}},
"knee": {"components": {"Transform3D": {"position": [2, -1, 0]}}},
"arm": {"components": {
  "Transform3D": {},
  "MeshInstance3D": {"mesh": "models/arm.gltf"},
  "TwoBoneIK3D": {"settings": [{"root_bone": "Shoulder", "middle_bone": "Elbow",
                                "end_bone": "Hand", "target": "goal", "pole": "knee"}]}
}}
```

- They run in Godot's order: those on the drawing entity, then those on each of its children in
  list order; on one entity, `TwoBoneIK3D`, `FABRIK3D`, `CCDIK3D`, `JacobianIK3D`, then
  `PhysicalBoneSimulator3D`. Each setting in `settings` is one limb or chain, solved in turn.
- `TwoBoneIK3D` bends two bones so the end reaches the target, with the middle joint on the
  pole's side, as Godot's: straight at the target when it is out of reach. It needs a target
  and a pole. `pole_direction` rolls the middle bone so that axis faces the pole.
- `FABRIK3D`, `CCDIK3D` and `JacobianIK3D` turn every bone from `root_bone` to `end_bone`. Like
  Godot's, each joint turns at most `angular_delta_limit` a step and they take at most
  `max_iterations` steps a tick, carrying the chain from tick to tick, so it eases toward a
  moving target; the chain is kept beside the components, so saving, snapshots and undo leave it
  alone. `deterministic` solves from the pose each time instead.
- A chain's `joints` limit its bones by name: `rotation_axis` turns one only about an axis, and
  a `"cone"` limitation keeps it within `limitation_angle` (the cone's full width, in radians)
  of the way it points at rest.
- `extend_end_bone` reaches with a point `end_bone_length` meters past the end bone, along
  `end_bone_direction`; on `TwoBoneIK3D`, `use_virtual_end` makes that point the end, so the end
  bone is the middle bone.
- `influence` blends the result into the pose (0 leaves it, 1 replaces it), and `active` turns
  a modifier off.

| `TwoBoneIK3D` field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `settings` | Array of settings | `[]` | One limb each: `root_bone`, `middle_bone`, `end_bone`, `target`, `pole`, and those below |
| `active` | `bool` | `true` | Whether it turns bones |
| `influence` | `float` | `1` | How far its result replaces the pose, from 0 to 1 |
| `mutable_bone_axes` | `bool` | `true` | Follow bones the pose moves rather than their rest, as Godot's |

| Two-bone setting field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `use_virtual_end`, `extend_end_bone` | `bool` | `false` | Reach with a point past the end bone, as above |
| `end_bone_direction` | string | `"from_parent"` | The way the end bone points: `"from_parent"`, `"plus_x"`, `"minus_x"`, `"plus_y"`, `"minus_y"`, `"plus_z"` or `"minus_z"` |
| `end_bone_length` | `float` | `0` | Meters past the end bone |
| `pole_direction`, `pole_direction_vector` | string, `Vec3` | `"none"`, `[0, 0, 0]` | The middle bone's axis that faces the pole: `"none"`, an axis as above, or `"custom"` with the vector |

`FABRIK3D`, `CCDIK3D` and `JacobianIK3D` have `settings`, `active`, `influence` and
`mutable_bone_axes` as `TwoBoneIK3D`'s, and:

| Chain field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `max_iterations` | `int` | `4` | Most solver steps a tick |
| `min_distance` | `float` | `0.001` | Meters from the target at which it stops |
| `angular_delta_limit` | `float` | `0.0349` | Most radians a joint turns in one step (2°) |
| `deterministic` | `bool` | `false` | Solve from the pose each time rather than carrying the chain |

| Chain setting field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `root_bone`, `end_bone` | string | `""` | The bones the chain runs between |
| `target` | entity ID | none | The entity the end reaches for |
| `extend_end_bone`, `end_bone_direction`, `end_bone_length` | | `false`, `"from_parent"`, `0` | As a two-bone setting's |
| `joints` | Map of joints | `{}` | A bone's limits, by name: `rotation_axis` (`"all"`, `"x"`, `"y"`, `"z"` or `"custom"` with `rotation_axis_vector`), `limitation` (`""` or `"cone"`), `limitation_angle` (default a quarter turn), `limitation_rotation_offset`, and `limitation_right_axis` with `limitation_right_axis_vector` |

`skeleton.get` reports bones after their modifiers, and `scene validate` reports bones the model
hasn't (with "did you mean"), bones that aren't one under the other, names of directions and
axes, settings with no target or pole, and modifiers that turn no model's bones.
[examples/ik](../../examples/ik/) reaches with an arm.

A `PhysicalBoneSimulator3D` is a modifier too, run after the IK modifiers on its entity: it puts
bones where their physics bodies are ([ragdolls](#ragdolls)).

#### Bone attachments

A `BoneAttachment3D` is Godot's: an entity that goes where a bone of a model is posed, so what
hangs under it in its `Transform3D` `children` moves with the bone, such as a sword in a hand or
a hit box on a head. Put it in the `Transform3D` `children` of the entity that draws the model,
or name that entity in `external_skeleton` to follow a model from anywhere in the world.

```json
"arm": {"components": {
  "Transform3D": {"children": ["grip"]},
  "MeshInstance3D": {"mesh": "models/arm.gltf"}
}},
"grip": {"components": {
  "Transform3D": {"children": ["sword"]},
  "BoneAttachment3D": {"bone_name": "Hand"}
}},
"sword": {"components": {
  "Transform3D": {"position": [0, 0.4, 0]},
  "MeshInstance3D": {},
  "BoxMesh": {"size": [0.05, 0.8, 0.05]}
}}
```

- Each tick, after the animations and skeleton modifiers (in the `skeleton_modifiers` system,
  before physics), its `Transform3D` becomes what puts it at the drawing entity's transform times
  the bone's, scale included, as Godot's `BoneAttachment3D`. Bodies and areas under it move with
  it, so hit boxes follow the animation.
- Godot's `override_pose`, which poses the bone from the attachment, isn't there yet: pose a
  bone with `Skeleton3D` or an IK modifier.

| `BoneAttachment3D` field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bone_name` | string | `""` | The bone (or other node) of the model it follows |
| `external_skeleton` | entity ID | none | The entity drawing the model, in place of its `Transform3D` parent |

`scene validate` reports a bone the model hasn't (with "did you mean") and an attachment whose
`Transform3D` parent or `external_skeleton` draws no model file.
[examples/bone-attachment](../../examples/bone-attachment/) puts a sword in a waving hand.

### CSG shapes

Seven components are Godot 4.6's CSG nodes: shapes that add to, cut from or intersect each other
to build level geometry, as in Godot's editor. A shape's children (entities in its `Transform3D`
`children` that are CSG shapes too) change it in their order, each placed by its own transform;
a hidden child is left out. Only the root, a CSG shape whose parent isn't one, draws, collides
and bakes into navigation meshes: everything under it makes one mesh, with a surface per
material. Faces keep the `StandardMaterial3D` of the shape they came from, so a hole cut by a
box shows that box's material, and faces from a shape with none get Godot's default material.

```json
"house": {"components": {
  "Transform3D": {"position": [0, 1.2, 0], "children": ["door"]},
  "CSGBox3D": {"size": [3, 2.4, 3], "use_collision": true}
}},
"door": {"components": {
  "Transform3D": {"position": [0, -0.45, 1.4]},
  "CSGBox3D": {"size": [0.8, 1.5, 1], "operation": "subtraction"}
}}
```

| Component | Fields (defaults) |
| --- | --- |
| `CSGCombiner3D` | none of its own: it combines its children, as a group |
| `CSGBox3D` | `size` `[1, 1, 1]` |
| `CSGSphere3D` | `radius` `0.5`, `radial_segments` `12`, `rings` `6`, `smooth_faces` `true` |
| `CSGCylinder3D` | `radius` `0.5`, `height` `2`, `sides` `8`, `cone` `false` (a point at the top), `smooth_faces` `true` |
| `CSGTorus3D` | `inner_radius` `0.5`, `outer_radius` `1` (they can't be equal), `sides` `8`, `ring_sides` `6`, `smooth_faces` `true` |
| `CSGPolygon3D` | `polygon` (`[x, y]` points in the entity's xy plane; default a unit square), `mode` `"depth"` (pushed `depth` `1` along -z), `"spin"` (turned `spin_degrees` `360` around y in `spin_sides` `8` steps) or `"path"` (pushed along a [path](#paths), below), `smooth_faces` `false` |
| `CSGMesh3D` | `mesh` (a `.gltf`, `.glb`, `.obj`, `.fbx` or `.blend` file, its meshes placed as at rest; `""` for the entity's primitive mesh). The mesh must be closed |

Each also has the fields of Godot's `CSGShape3D`:

- `operation` (`"union"`): how it changes its parent, `"union"`, `"intersection"` or
  `"subtraction"`. A root's operation is unused.
- `use_collision` (`false`), `collision_layer` and `collision_mask` (`1`): a root with
  `use_collision` is a static body with its triangles as a concave shape, for
  [3D physics](#3d-physics). Shapes under a root leave these unused.
- `calculate_tangents` (`true`): a root makes tangents for normal maps, as Godot's does.
- `layers` (`1`) and `cast_shadow` (`"on"`): as a [`MeshInstance3D`'s](#meshes), for a root.
- `flip_faces` (`false`, not on `CSGCombiner3D`): turns the shape inside out.

Shapes are made as Godot makes them, face for face, then combined with
[Manifold](https://github.com/elalish/manifold) in Godot's order, so a level built in Godot
builds the same here. A root keeps its mesh until a shape under it changes; moving the root
alone doesn't remake it. [`csg.get`](commands.md#csgget) returns what a root makes.

In `"path"` mode a `CSGPolygon3D` pushes its polygon along the `Path3D` of the entity its
`path_node` names, as Godot's `MODE_PATH`: a ring of the polygon every `path_interval` (`1`)
meters (`path_interval_type` `"distance"`), or as many rings as the path has points divided by
`path_interval` (`"subdivide"`), each facing the way the curve goes. `path_rotation` turns the rings:
`"polygon"` not at all, `"path"` to face along the curve, `"path_follow"` (the default) with its
up vectors and tilt too; `path_rotation_accurate` (`false`) takes their direction from the curve
itself rather than the next ring. `path_simplify_angle` (`0`) merges rings that turn less than
that many degrees, `path_joined` (`false`) joins the last ring to the first with no ends, and
`path_continuous_u` (`true`) and `path_u_distance` (`1`) lay the texture along it. The path's
place in the world counts unless `path_local` (`false`), as in Godot, so leave the CSG root at
the origin, and a path that changes or moves remakes the mesh.

A shape that can't be made is left out and reported: a `polygon` with fewer than 3 points, no
area or edges that cross, a path mode `path_node` that names no `Path3D` or one with fewer than
2 points, a `CSGMesh3D` whose mesh doesn't load or isn't closed, a torus whose
radii are equal, or a root whose result is empty. An entity has at most one CSG shape and no
`MeshInstance3D` beside it, since the shape draws itself.
[examples/csg-shapes](../../examples/csg-shapes/) builds a hamlet.

### Grid maps

A `GridMap` is Godot 4.6's: items of a mesh library placed in a grid of cells, one item per
cell, to build levels from blocks. Cells are in the entity's `Transform3D` space. Like a
[tile map layer](#tile-map-layers), it writes its cells as text: a `legend` of short keys, and
`floors`, each the rows of keys at one height `y`. A row is a string of keys for the cells from
the floor's `origin` (`[x, z]`) toward +x, and each row after the first is one cell further
toward +z.

```json
"level": {"components": {
  "Transform3D": {},
  "GridMap": {
    "mesh_library": "blocks.meshlib.json",
    "bake_navigation": true,
    "legend": {"f": "floor", "p": "platform", "r": "ramp:10", "w": "wall"},
    "floors": [{"y": 0, "origin": [0, 0], "rows": ["ffffpp", "ffffrp", "ffwfff", "ffwfff"]}]
  }
}}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `mesh_library` | `""` | A [`.meshlib.json`](#mesh-libraries) file, below |
| `cell_size` | `[2, 2, 2]` | Meters along x, y and z |
| `cell_octant_size` | `8` | Cells along each side of the blocks drawn, collided and remade together |
| `cell_center_x`, `cell_center_y`, `cell_center_z` | `true` | Moves items half a cell along that axis, to the cell's center; without it the item sits on the cell's corner |
| `cell_scale` | `1` | Scales each cell's item |
| `collision_layer`, `collision_mask` | `1` | As a [static body's](#3d-physics) |
| `friction`, `bounce` | `1`, `0` | Its physics material |
| `bake_navigation` | `false` | Each cell whose item has a navigation mesh is a [navigation region](#regions) |
| `legend` | `{}` | key to item: the item's name, then `:` and an orientation, `1` to `23`, when it is turned |
| `floors` | `[]` | `y` `0`, `origin` `[0, 0]` and `rows` |

- **Keys and rows** are as a tile map layer's: one to three characters, all the same length,
  `.` an empty cell. Cells are from -32768 to 32767 on each axis, as in Godot. Commands write
  the map in canonical form: a floor for each `y` in use, lowest first, its origin the corner of
  its cells nearest -x and -z, rows ending at their last item, keys kept for what they stood for
  and new keys going to the most used items first, each the first letter of the item's name
  where that is free.
- **Orientation** is Godot's orthogonal index: one of the 24 ways a cell's item can face, as
  Godot's `GridMap::get_basis_with_orthogonal_index`. `0` is unturned; `16`, `10` and `22` turn
  it 90, 180 and 270 degrees about y.
- **Where a cell is.** A cell's item is turned by its orientation, scaled by `cell_scale` and
  placed at `cell × cell_size`, moved half a cell along each centered axis, as Godot's
  `map_to_local`. A point is in the cell `floor(point / cell_size)`.
- **Drawing.** Cells are grouped into octants of `cell_octant_size` cells a side, as Godot
  groups them, and each octant's items draw as one mesh per material and `mesh_cast_shadow`. A
  change to some cells remakes only their octants.
- **Collision.** With any item shapes, the GridMap is one static body of every cell's shapes, on
  its `collision_layer` and `collision_mask`. [`physics.raycast`](commands.md#physicsraycast)
  returns the `cell` it hit and its `item`.
- **Navigation.** With `bake_navigation`, each cell whose item has a `navigation_mesh` is a
  region on the item's `navigation_layers`, and neighboring cells join where their edges meet.
  [Baking](#baking) a navigation mesh takes the GridMap's meshes, or its shapes, as Godot's
  `GridMap::navmesh_parse_source_geometry` does.
- **Commands.** [`gridmap.set`](commands.md#gridmapset) places or erases an item in cells or a
  box of them, and [`gridmap.get`](commands.md#gridmapget) reads the map, cells or the cells
  points are in.
- **Checks.** `scene validate` and exports report rows that don't decode, legend items the
  library doesn't have (with the closest name), cells but no library, a library that doesn't
  load, item meshes that can't be made, a `cell_size` under 0.001 meters and a GridMap on a
  physics body's entity, each at its JSON Pointer.

#### Mesh libraries

A `.meshlib.json` file is Godot's `MeshLibrary`: items by name (1 to 64 characters, no `:`),
each what a cell holding it draws, collides as and walks on, in the cell's space.
`slash-cli schema meshlib` prints its JSON Schema, and `scene validate` and `scene format` take
it.

```json
{
  "format": 1,
  "items": {
    "wall": {
      "primitive": {"BoxMesh": {"size": [2, 2, 2]}},
      "material": {"albedo_color": [0.4, 0.42, 0.5, 1]},
      "shapes": [{"shape": "box", "size": [2, 2, 2]}]
    },
    "floor": {
      "primitive": {"BoxMesh": {"size": [2, 0.2, 2]}},
      "mesh_transform": {"position": [0, -0.9, 0]},
      "navigation_mesh": "nav/square.navmesh.json",
      "navigation_mesh_transform": {"position": [0, -0.8, 0]}
    }
  }
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `mesh` | `""` | A `.gltf`, `.glb`, `.obj`, `.fbx` or `.blend` file's meshes, as they are at rest |
| `primitive` | none | Or one [primitive mesh](#meshes) by its component's name, such as `{"BoxMesh": {}}` |
| `material` | none | `StandardMaterial3D` fields: takes the place of the model's materials, or colors the primitive; Godot's default material without one |
| `mesh_transform` | none | `position`, `rotation` and `scale` placing the mesh in the cell |
| `mesh_cast_shadow` | `"on"` | As a `MeshInstance3D`'s `cast_shadow` |
| `shapes` | `[]` | `CollisionShape3D` fields, each placed by its `position` and `rotation`; not `world_boundary` |
| `navigation_mesh` | `""` | A [`.navmesh.json`](#navigation-meshes-and-polygons) file |
| `navigation_mesh_transform` | none | Places the navigation mesh in the cell |
| `navigation_layers` | `1` | The navigation layers its regions are in |

[examples/gridmap](../../examples/gridmap/) lays out a room with a ramp up to a platform.

### Heightmap terrains

An `HTerrain` is a heightmap terrain, as the HTerrain plugin for Godot (zylann.hterrain), since
Godot has no terrain node of its own: `resolution` by `resolution` heights a cell apart, from the
entity's origin toward +x and +z in its `Transform3D` space, stretched by `map_scale`. Its
heights, ground texture weights and colors are image files that the
[terrain commands](commands.md#terraingenerate) write, so no one edits them by hand.

```json
"land": {"components": {
  "Transform3D": {},
  "HTerrain": {
    "heightmap": "terrain/land.height.png",
    "splatmap": "terrain/land.splat.png",
    "colormap": "terrain/land.color.png",
    "resolution": 513,
    "height_range": [0, 100],
    "map_scale": [2, 1, 2],
    "textures": [{"albedo_bump": "ground/grass.png", "uv_scale": 20},
                 {"albedo_bump": "ground/rock.png", "normal_roughness": "ground/rock_normal.png"}]
  }
}}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `heightmap` | `""` | A 16-bit gray PNG whose name ends in `.height.png` |
| `resolution` | `513` | Heights along each side, which every map's size matches: a power of two plus one, `65` to `4097` |
| `height_range` | `[0, 100]` | The heights in meters the heightmap's `0` and `65535` stand for |
| `splatmap` | `""` | A PNG whose red, green, blue and alpha weigh the four textures |
| `colormap` | `""` | A PNG that tints the textures; alpha under 0.5 is a hole |
| `textures` | `[]` | Up to 4 ground textures, below |
| `depth_blending` | `true` | Where textures meet, the one whose bump stands higher shows |
| `specular` | `0.5` | How bright highlights are, `0` to `1` |
| `map_scale` | `[1, 1, 1]` | Meters a cell covers along x and z, and what heights are multiplied by |
| `centered` | `false` | Its middle, not its corner, is at the entity |
| `chunk_size` | `32` | Cells along each side of a chunk drawn at one level of detail: `16`, `32` or `64` |
| `lod_scale` | `2` | How far each level of detail reaches, in chunks of its size, `2` to `5` |
| `collision_enabled` | `true` | Collides as a static height field |
| `collision_layer`, `collision_mask` | `1` | As a [static body's](#3d-physics) |
| `friction`, `bounce` | `1`, `0` | Its physics material |
| `render_layers` | `1` | As a `MeshInstance3D`'s `layers` |
| `cast_shadow` | `"on"` | As a `MeshInstance3D`'s |

Each texture is HTerrain's Classic4 ground layer:

| Field | Default | Meaning |
| --- | --- | --- |
| `albedo_bump` | `""` | An sRGB image, its alpha the bump depth blending compares; `""` is white |
| `normal_roughness` | `""` | An OpenGL-style normal map with roughness in alpha; `""` is flat and rough |
| `uv_scale` | `20` | Cells one repeat of the textures covers |
| `colormap_opacity` | `1` | How much the color map tints it |

- **Maps that don't exist yet.** A heightmap file that doesn't exist reads as flat ground at
  `height_range`'s low end, a splat map as the first texture everywhere and a color map as
  white with no holes, so a new terrain needs only paths. Each map is `resolution` pixels
  square, its top row at -z.
- **Heights.** A heightmap value `v` is `low + (high - low) * v / 65535` meters, times
  `map_scale.y`. The commands widen `height_range` to whole meters that hold the heights they
  write.
- **Drawing.** As HTerrain's quad tree (`util/quad_tree_lod.gd`), a quad splits into four finer
  ones while the camera is nearer its middle than `chunk_size` cells times 2 to the level of
  detail times `lod_scale`, so detail falls off with distance. A chunk beside a coarser one
  skips every other vertex on that side, as HTerrain's mesher does, so no cracks show. The
  textures are mixed by the splat map (sharpened by their bumps with `depth_blending`), tinted
  by the color map and lit through the terrain's normals, as HTerrain's Classic4 shader.
- **Collision.** With `collision_enabled`, the terrain is a static height field on its
  `collision_layer` and `collision_mask`, as HTerrain's `HeightMapShape3D` collider, with holes
  where the color map has them. [`physics.raycast`](commands.md#physicsraycast) hits it.
- **Navigation.** [Baking](#baking) a navigation mesh takes the terrain's triangles at full
  detail, leaving out those touching a hole.
- **Commands.** [`terrain.generate`](commands.md#terraingenerate) makes heights from noise,
  [`terrain.brush`](commands.md#terrainbrush) raises, lowers, smooths, flattens, levels, paints
  textures and colors and cuts holes, and [`terrain.get`](commands.md#terrainget) reads heights,
  normals, texture weights and colors at points. Each writes the map files it changes, and
  `undo` puts them back.
- **Exports.** A `.height.png` file ships as it is, as do the splat and color maps a scene or
  prefab's `HTerrain` names, whatever `rendering.textures` says: terrains read their pixels.
- **Checks.** `scene validate` and exports report a `resolution` that isn't a power of two plus
  one, a `chunk_size` that isn't 16, 32 or 64, more than four textures, an `HTerrain` on a
  physics body's entity, and maps that don't load, aren't `resolution` pixels square or hold
  only compressed texels, each at its JSON Pointer.

[examples/terrain](../../examples/terrain/) makes an island, paints its slopes and cuts a hole.

### StandardMaterial3D

Godot's `StandardMaterial3D`, as the mesh's material override. Colors are sRGB; textures are
project paths of PNG, JPEG, BMP or KTX2 files, and `""` leaves one out. Textures repeat, as
Godot's do in 3D, unless `texture_repeat` is off.

| Field | Default | Meaning |
| --- | --- | --- |
| `albedo_color`, `albedo_texture` | `[1, 1, 1, 1]`, `""` | The surface's color, times the texture |
| `vertex_color_use_as_albedo` | `false` | Multiply by the mesh's vertex colors |
| `metallic`, `metallic_texture`, `metallic_texture_channel` | `0`, `""`, `"red"` | How metallic, times a channel of the texture (`"red"`, `"green"`, `"blue"`, `"alpha"` or `"gray"`) |
| `metallic_specular` | `0.5` | Specular strength of the non-metallic part |
| `roughness`, `roughness_texture`, `roughness_texture_channel` | `1`, `""`, `"red"` | How rough, as `metallic` |
| `emission_enabled`, `emission`, `emission_energy_multiplier`, `emission_texture` | `false`, `[0, 0, 0, 1]`, `1`, `""` | Light the surface gives off, unlit: the color (plus the texture) made linear, times the multiplier, as Godot |
| `normal_enabled`, `normal_scale`, `normal_texture` | `false`, `1`, `""` | A tangent-space normal map |
| `shading_mode` | `"per_pixel"` | `"unshaded"` draws the albedo with no lighting |
| `transparency`, `alpha_scissor_threshold` | `"disabled"`, `0.5` | `"alpha"` blends; `"alpha_scissor"` drops pixels under the threshold |
| `blend_mode` | `"mix"` | How alpha meshes blend: `"mix"`, `"add"`, `"sub"`, `"mul"` or `"premul_alpha"` |
| `cull_mode` | `"back"` | Faces not drawn: `"back"`, `"front"` or `"disabled"` (both sides drawn) |
| `uv1_scale`, `uv1_offset` | `[1, 1, 1]`, `[0, 0, 0]` | Texture coordinates are scaled, then moved (x and y are used) |
| `disable_fog` | `false` | The environment's fog leaves it alone |
| `texture_filter` | `"linear_with_mipmaps"` | `"nearest"` and `"nearest_with_mipmaps"` keep texels sharp |
| `texture_repeat` | `true` | Textures tile past their edges; `false` stretches the edge texels, as a glTF sampler that clamps |

Opaque and alpha scissor meshes draw nearest first, then alpha meshes farthest first, as Godot
sorts them. A texture that can't load is left out, with a warning and a problem in
`frame.summary`.

### Camera3D

| Field | Default | Meaning |
| --- | --- | --- |
| `projection` | `"perspective"` | Or `"orthogonal"` |
| `keep_aspect` | `"height"` | The axis `fov` or `size` spans: `"height"` or `"width"` |
| `fov` | `75` | Degrees across that axis, for a perspective camera |
| `size` | `1` | Meters across that axis, for an orthogonal camera |
| `near`, `far` | `0.05`, `4000` | The nearest and farthest it sees, in meters |
| `h_offset`, `v_offset` | `0`, `0` | Moves the view along the camera's x and y, in meters |
| `current` | `false` | The world is seen through this camera |
| `cull_mask` | `1048575` | The render layers it sees (layers 1 to 20) |
| `doppler_tracking` | `"disabled"` | `idle_step` or `physics_step`: measure how fast it moves, for [3D sound](#3d-sound) heard through it |

The camera's scale is ignored, as Godot's is. Depth is reversed, so far scenes keep their
precision.

### Lights

`DirectionalLight3D` shines down its entity's -z everywhere, as the sun; `OmniLight3D` shines
from its entity in every direction; `SpotLight3D` shines from its entity in a cone down its -z.
Up to 8 directional lights and 512 omni and spot lights shine at once, the directional ones
first and each kind in tree order, and hidden ones don't. The project's
[rendering method](project-format.md#rendering-methods) picks how: `forward_plus`, the default,
lights each pixel with only the omni and spot lights whose range reaches it, and
`gl_compatibility`, as the OpenGL drivers always do, with the first 64. Their fields are Godot's:

| Field | Default | Lights | Meaning |
| --- | --- | --- | --- |
| `light_color` | `[1, 1, 1, 1]` | all | sRGB |
| `light_energy` | `1` | all | Brightness |
| `light_specular` | `0.5` | all | How strong its highlights are |
| `light_negative` | `false` | all | Takes light away instead |
| `light_cull_mask` | `4294967295` | all | The render layers it lights (layers 1 to 20) |
| `omni_range`, `omni_attenuation` | `5`, `1` | omni | How far it reaches, in meters, and how it falls off |
| `spot_range`, `spot_attenuation` | `5`, `1` | spot | The same for a spot light |
| `spot_angle`, `spot_angle_attenuation` | `45`, `1` | spot | Degrees from its axis to the cone's edge, and how it fades to it |
| `shadow_enabled` | `false` | all | Casts shadows |
| `shadow_bias` | `0.1` (spot `0.03`) | all | Pushes shadow map depths away from the light, against shadow acne |
| `shadow_normal_bias` | `1` (directional `2`) | all | Moves lit points along their normals, in shadow map texels, against acne |
| `shadow_reverse_cull_face` | `false` | all | Shadow maps draw the faces meshes cull, and cull the rest |
| `shadow_opacity` | `1` | all | How dark its shadows are: `0` casts none, `1` a full one |
| `shadow_caster_mask` | `4294967295` | all | The render layers whose meshes cast its shadows |
| `directional_shadow_mode` | `"parallel_4_splits"` | directional | `"orthogonal"` covers the view with one shadow map; `"parallel_2_splits"` and `"parallel_4_splits"` with cascades, sharper near the camera |
| `directional_shadow_split_1`, `_2`, `_3` | `0.1`, `0.2`, `0.5` | directional | Where the cascades end, as fractions of the shadows' reach |
| `directional_shadow_blend_splits` | `false` | directional | Blends each cascade into the next, so their seams don't show |
| `directional_shadow_fade_start` | `0.8` | directional | Where shadows start fading out, as a fraction of their reach |
| `directional_shadow_max_distance` | `100` | directional | How far from the camera shadows reach, in meters (the camera's `far` if nearer) |
| `directional_shadow_pancake_size` | `20` | directional | How far behind each cascade casters still cast, in meters |
| `light_angular_distance` | `0` | directional | Degrees across its sun's radius in the [sky](#skies), 0 to 90 |
| `sky_mode` | `"light_and_sky"` | directional | `"light_only"` lights meshes and shows no sun in the sky; `"sky_only"` shows a sun and lights nothing |
| `light_bake_mode` | `"dynamic"` | all | How a [LightmapGI](#baked-lightmaps) bakes it: `"disabled"` leaves it out, `"dynamic"` bakes only its bounces and it lights live, `"static"` bakes it whole and it doesn't light lightmapped meshes live. [VoxelGIs](#voxelgi) leave out omni and spot lights that are `"disabled"` |
| `light_indirect_energy` | `1` | all | Times its light in baked bounces |
| `light_size` | `0` | omni, spot | Its radius when baked, for soft shadows (a directional light's is `light_angular_distance`) |
| `shadow_blur` | `1` | all | How much its baked shadows soften |
| `light_volumetric_fog_energy` | `1` | all | `forward_plus`: how much it lights [volumetric fog](#volumetric-fog), times `light_energy` |

Shadows are Godot's Compatibility renderer's shadow maps, drawn on every device and by
`frame.capture`. A directional light's cascades share the directional atlas with the other
directional lights that cast (up to 4); omni and spot lights (up to 16) share the positional
atlas, an omni light's cube taking six tiles and a spot light's one. Edges are filtered by the
project's `rendering.lights_and_shadows` ([project-format.md](project-format.md#shadows)).

### WorldEnvironment

The first `WorldEnvironment` made sets the background a 3D frame clears to or the sky behind
its meshes, the fog, the ambient light every lit surface gets and what happens to colors after,
as Godot's `Environment`. Without one, the background is the project's `rendering.clear_color`,
there is no ambient light and colors stay as they are.

| Field | Default | Meaning |
| --- | --- | --- |
| `background_mode` | `"clear_color"` | `"clear_color"` (the project's), `"color"` or `"sky"` (the entity's [sky material](#skies)) |
| `background_color` | `[0, 0, 0, 1]` | The background for `"color"` |
| `background_energy_multiplier` | `1` | Brightens the background or sky |
| `sky_custom_fov` | `0` | Degrees across the target's height the sky is seen with; `0` for the camera's |
| `sky_rotation` | `[0, 0, 0]` | Turns the sky, in radians about x, y and z, as `Transform3D`'s rotation |
| `sky_radiance_size` | `256` | Texels across each face of the cube the [sky's light](#sky-light) is drawn into: 32, 64, 128, 256, 512, 1024 or 2048 |
| `ambient_light_source` | `"background"` | `"background"` (a color background's color and energy, or a sky's light), `"color"`, `"sky"` (the sky's light) or `"disabled"` |
| `ambient_light_color`, `ambient_light_energy` | `[0, 0, 0, 1]`, `1` | The ambient light for `"color"`, and what the sky's light mixes with |
| `ambient_light_sky_contribution` | `1` | How much of the ambient light is the sky's, over `ambient_light_color`, from 0 to 1 |
| `reflected_light_source` | `"background"` | What lit surfaces reflect: `"background"` (the sky, if it is the background), `"sky"` or `"disabled"` |
| `tonemap_mode` | `"linear"` | `"linear"`, `"reinhard"`, `"filmic"`, `"aces"` or `"agx"` |
| `tonemap_exposure` | `1` | Multiplies lit colors before they are tonemapped |
| `tonemap_white` | `1` | The color tonemapped to white, at least 1 with `forward_plus` (`"agx"` uses its own) |
| `tonemap_agx_white`, `tonemap_agx_contrast` | `16.29`, `1.25` | `forward_plus`: `"agx"`'s white, at least 2, and how steep its curve is |
| `glow_enabled` | `false` | Bright colors bleed into what is around them |
| `glow_levels` | `[0, 0.8, 0.4, 0.1, 0, 0, 0]` | `forward_plus`: how much each of glow's seven levels adds, from the sharpest to the softest; Godot's `glow_levels/1` to `/7` |
| `glow_normalized` | `false` | `forward_plus`: `glow_levels` are divided by their sum |
| `glow_intensity` | `0.3` | How strong the glow is |
| `glow_strength` | `1` | `forward_plus`: multiplies each level as it is blurred |
| `glow_blend_mode` | `"screen"` | `forward_plus`: `"additive"`, `"screen"`, `"softlight"`, `"replace"` or `"mix"` |
| `glow_mix` | `0.05` | `forward_plus`: how much glow replaces the colors with `"mix"` |
| `glow_bloom` | `0` | How much of every color glows, from 0 to 1 |
| `glow_hdr_threshold`, `glow_hdr_scale` | `1`, `2` | Colors glow from the threshold, fully at the threshold plus the scale |
| `glow_hdr_luminance_cap` | `12` | The brightest glow |
| `adjustment_enabled` | `false` | Brightness, contrast, saturation and color correction apply |
| `adjustment_brightness`, `adjustment_contrast`, `adjustment_saturation` | `1`, `1`, `1` | Multiply colors; contrast 0 is flat gray, saturation 0 gray |
| `adjustment_color_correction` | `""` | A lookup texture colors pass through: 1 pixel tall, or as wide as its height squared |
| `ssr_enabled` | `false` | `forward_plus`: [screen-space reflections](#screen-space-reflections) reflect what is on the screen in shiny surfaces |
| `ssr_max_steps` | `64` | How many steps a reflected ray takes at most, from 1 to 512 |
| `ssr_fade_in` | `0.15` | How reflections fade in where they leave the surface, as an easing curve |
| `ssr_fade_out` | `2` | How reflections fade out toward the screen's edges, as an easing curve |
| `ssr_depth_tolerance` | `0.5` | How far behind what is on the screen a ray still hits it, in meters, from 0.01 to 128 |
| `ssao_enabled` | `false` | `forward_plus`: [screen-space ambient occlusion](#ambient-occlusion) darkens corners and what is close to a surface |
| `ssao_radius` | `1` | How far around a pixel occludes it, in meters, from 0.01 to 16 |
| `ssao_intensity` | `2` | How dark occlusion is, from 0 to 16 |
| `ssao_power` | `1.5` | The occlusion's curve: higher darkens only what is most occluded, from 0.01 to 10 |
| `ssao_detail` | `0.5` | How much the pixels right beside a pixel occlude it, for small creases, from 0 to 5 |
| `ssao_horizon` | `0.06` | Samples below this angle above a surface don't occlude it, from 0 to 1 |
| `ssao_sharpness` | `0.98` | How much the blur keeps to edges, from 0 to 1 |
| `ssao_light_affect`, `ssao_ao_channel_affect` | `0`, `0` | How much occlusion darkens direct light too: their product, as Godot 4.6 mixes them, each from 0 to 1 |
| `fog_enabled` | `false` | [Fog](#fog) fades what is far away, or low down, into its color |
| `fog_mode` | `"exponential"` | `"exponential"` thickens with distance by `fog_density`; `"depth"` from `fog_depth_begin` to `fog_depth_end` |
| `fog_light_color`, `fog_light_energy` | `[0.518, 0.553, 0.608, 1]`, `1` | The fog's color, sRGB, and how bright it is |
| `fog_sun_scatter` | `0` | How much directional lights brighten the fog looked through toward them |
| `fog_density` | `0.01` | How thick exponential fog is |
| `fog_aerial_perspective` | `0` | How much the sky colors the fog in front of it, from 0 to 1 (over the sky only, as Godot's Compatibility renderer) |
| `fog_sky_affect` | `1` | How much fog covers the sky or background, from 0 to 1 |
| `fog_height`, `fog_height_density` | `0`, `0` | Fog thickens below this height, by this much; `0` for none |
| `fog_depth_curve`, `fog_depth_begin`, `fog_depth_end` | `1`, `10`, `100` | How depth fog grows, where it begins and where it is thickest; an end of `0` or less is the camera's `far` |
| `volumetric_fog_enabled` | `false` | `forward_plus`: [volumetric fog](#volumetric-fog) fills the view, lit by the lights and shaded by their shadows |
| `volumetric_fog_density` | `0.05` | How thick it is |
| `volumetric_fog_albedo` | `[1, 1, 1, 1]` | The color it scatters light with, sRGB |
| `volumetric_fog_emission`, `volumetric_fog_emission_energy` | `[0, 0, 0, 1]`, `1` | The light it gives off itself, sRGB, and how bright |
| `volumetric_fog_anisotropy` | `0.2` | Which way it scatters light, from -0.9 (back toward the light) through 0 (every way) to 0.9 (on, so it glows looked through toward a light) |
| `volumetric_fog_length` | `64` | How far from the camera it reaches, in meters; what is past it is seen through all of it |
| `volumetric_fog_detail_spread` | `2` | How its slices spread over its length: above 1, more of them are near the camera, from 0.5 to 6 |
| `volumetric_fog_ambient_inject` | `0` | How much the ambient light (and the sky's, as meshes take it) lights it, from 0 to 16 |
| `volumetric_fog_gi_inject` | `1` | How much the light of the [VoxelGIs](#voxelgi) it is in lights it, from 0 to 16 |
| `volumetric_fog_sky_affect` | `1` | How much it covers the sky, from 0 to 1 |
| `volumetric_fog_temporal_reprojection_enabled`, `volumetric_fog_temporal_reprojection_amount` | `true`, `0.9` | Each frame blends in this much of the frame before's fog, from 0.5 to 0.99, smoothing it |

How colors are finished depends on the project's
[rendering method](project-format.md#rendering-methods), as in Godot.

With `forward_plus`, as Godot's Forward+ renderer, the background and meshes draw linear into
a texture of 16-bit floats the frame's size, so colors keep their brightness above 1. Glow
blurs the parts above `glow_hdr_threshold` into up to seven textures, the first half the
frame's size and each next one half the last, drawing only as far as the last level of more
than 0.01. A pass over the frame then exposes every pixel, blends glow in by
`glow_blend_mode`, tonemaps it, adjusts and corrects it, and turns it to sRGB, before the 2D
draws go over it. Alpha blending happens in linear colors.

With `gl_compatibility`, as Godot's Compatibility renderer, meshes are exposed and tonemapped
as they draw when there is no glow and no adjustments, and the background is left alone. With
either, the meshes draw exposed but not tonemapped into a texture of the frame's size, and a pass
over the frame screens glow in, tonemaps, adjusts and corrects every pixel, the background's
too. Glow blurs the texture's bright parts through four textures each half the size of the last.
The textures hold 8 bits a channel, so drawn colors reach 1 at most: glow needs
`glow_hdr_threshold` below 1, or `glow_bloom`, and `glow_levels`, `glow_strength`,
`glow_blend_mode` and `glow_mix` do nothing.

A color correction texture one pixel tall maps each channel by its value along x, as Godot's 1D
LUTs (a `GradientTexture1D`) do. One as wide as its height squared is a 3D LUT: height slices of
height by height pixels side by side, blue choosing the slice and red and green the pixel in it,
as Godot imports a `Texture3D` from horizontal slices. A texture of another shape, or one that
can't load, is reported and left out.

### CameraAttributesPractical

How a camera exposes and focuses the 3D world, as Godot's `CameraAttributesPractical`. A
`Camera3D` uses the one on its own entity, as Godot's `Camera3D.attributes`, or else the one on
the first `WorldEnvironment`'s, as `WorldEnvironment.camera_attributes`.

| Field | Default | Meaning |
| --- | --- | --- |
| `exposure_multiplier` | `1` | Brightens the lights, emission, the background and the sky, and so the sky's light, but not `ambient_light_color` or fog |
| `exposure_sensitivity` | `100` | The camera's ISO, which the auto exposure sensitivities are over |
| `auto_exposure_enabled` | `false` | `forward_plus`: exposes each frame by how bright it is |
| `auto_exposure_scale` | `0.4` | The brightness frames are exposed toward: higher is brighter |
| `auto_exposure_speed` | `0.5` | How much of the way to a new brightness the exposure moves each second of game time |
| `auto_exposure_min_sensitivity`, `auto_exposure_max_sensitivity` | `0`, `800` | The least and most brightness the exposure follows, in ISO: a scene darker than the least stays dark, and one brighter than the most stays bright |
| `dof_blur_far_enabled` | `false` | `forward_plus`: blurs what is past `dof_blur_far_distance` |
| `dof_blur_far_distance`, `dof_blur_far_transition` | `10`, `5` | Meters from the camera where the far blur starts, and over how many more it grows to full. A negative transition grows it as a lens does instead, by how far past the distance something is |
| `dof_blur_near_enabled` | `false` | `forward_plus`: blurs what is nearer than `dof_blur_near_distance` |
| `dof_blur_near_distance`, `dof_blur_near_transition` | `2`, `1` | Meters from the camera where the near blur starts, and over how many fewer it grows to full. A negative transition grows it as a lens does instead, by how far short of the distance something is |
| `dof_blur_amount` | `0.1` | How much it blurs, from 0 to 1: the widest blur is 64 pixels times this |

Auto exposure measures each frame as Godot's Forward+ renderer does: each pixel's brightest
channel is averaged in blocks of 8 by 8, then the blocks' averages, down to one value, and the
frame is exposed by `auto_exposure_scale` over it, glow too. The brightness moves toward each
new frame's by `auto_exposure_speed` per second of game time, so the same frames at the same
times expose the same way. It starts from the first frame's own brightness when auto exposure
turns on, when the camera follows other attributes, or when game time goes back, as after a
restore. `gl_compatibility` has no auto exposure, as in Godot.

Depth of field blurs as Godot 4.6's Forward+ renderer does, by each pixel's distance in the depth
buffer, between the sky and meshes and glow. Each pixel's blur size is worked out from the
distances, then the frame is blurred by the project's bokeh shape (`rendering.camera.depth_of_field`
in [project.json](project-format.md)) and mixed with the sharp frame by that size, so what is in
focus stays sharp. The box and hexagon blur in two passes, at half size for the `very_low` and
`low` qualities; the circle gathers a spiral of samples at half size. Frames smaller than 8 by 8
aren't blurred, and `gl_compatibility` has no depth of field, as in Godot.

### Skies

A `"sky"` background draws the sky material on the `WorldEnvironment`'s entity behind the meshes,
worked out for each pixel's view direction as Godot's sky shaders do: a
`ProceduralSkyMaterial`, a `PhysicalSkyMaterial` or a `PanoramaSkyMaterial`. An entity with none
has Godot's default procedural sky; one with two is reported, and the first of those three
draws. Colors are sRGB and textures project paths, and `background_energy_multiplier` brightens
the sky.

The sky shows the sun of each `DirectionalLight3D` whose `sky_mode` isn't `"light_only"`, up to
four in tree order, where the light shines from, in its `light_color` and `light_energy`, sized
by its `light_angular_distance`. `sky_rotation` turns the sky and `sky_custom_fov` sees it
through another field of view. The sky also lights the meshes, as its [light](#sky-light).

`ProceduralSkyMaterial` is a gradient from the horizon up to the sky's top and down to the
ground's bottom, with each sun on it:

| Field | Default | Meaning |
| --- | --- | --- |
| `sky_top_color`, `sky_horizon_color` | `[0.385, 0.454, 0.55, 1]`, `[0.6463, 0.6558, 0.6708, 1]` | The sky straight up and at the horizon |
| `sky_curve` | `0.15` | How quickly the sky turns from the horizon's color to the top's |
| `sky_energy_multiplier` | `1` | Brightens the sky above the horizon |
| `sky_cover`, `sky_cover_modulate` | `""`, `[1, 1, 1, 1]` | A panorama over the sky, such as clouds, tinted; its alpha says how much shows |
| `ground_bottom_color`, `ground_horizon_color` | `[0.2, 0.169, 0.133, 1]`, `[0.6463, 0.6558, 0.6708, 1]` | The ground straight down and at the horizon |
| `ground_curve`, `ground_energy_multiplier` | `0.02`, `1` | The same for the ground |
| `sun_angle_max`, `sun_curve` | `30`, `0.15` | Degrees from a sun's edge its glow reaches, and how it fades |
| `use_debanding` | `true` | A little noise, so gradients don't band |
| `energy_multiplier` | `1` | Brightens all of it |

`PhysicalSkyMaterial` scatters the first sun's light through the air, after Preetham:

| Field | Default | Meaning |
| --- | --- | --- |
| `rayleigh_coefficient`, `rayleigh_color` | `2`, `[0.3, 0.405, 0.6, 1]` | How much the air scatters light, and the color it scatters |
| `mie_coefficient`, `mie_eccentricity`, `mie_color` | `0.005`, `0.8`, `[0.69, 0.729, 0.812, 1]` | How much haze scatters, how much of it toward the sun, and its color |
| `turbidity` | `10` | How hazy the air is |
| `sun_disk_scale` | `1` | Scales the sun's disk |
| `ground_color` | `[0.1, 0.07, 0.034, 1]` | Below the horizon |
| `energy_multiplier` | `1` | Brightens it |
| `use_debanding` | `true` | As the procedural sky's |
| `night_sky` | `""` | A panorama behind the sky, such as stars |

`PanoramaSkyMaterial` is an equirectangular texture all round, x once round the horizon and y
from straight up to straight down, as Godot's:

| Field | Default | Meaning |
| --- | --- | --- |
| `panorama` | `""` | The texture; with none the sky is black |
| `filter` | `true` | Samples it smoothly; `false` takes its nearest texel |
| `energy_multiplier` | `1` | Brightens it |

### Sky light

The sky lights meshes and shines in them, as Godot's sky radiance. The sky material on the
`WorldEnvironment`'s entity (or the default procedural sky) is drawn into a cube of
`sky_radiance_size` texels a side, with its suns but without fog, and blurred into smaller
levels for rougher surfaces: Godot's `RADIANCE_SIZE` levels, down to four texels a side, each
filtered by GGX samples. It is drawn again only when the sky changes.

Ambient light comes from it when `ambient_light_source` is `"sky"`, or `"background"` with a sky
background: each surface gets the blurriest level's light from where its normal faces, times
`background_energy_multiplier`, mixed with `ambient_light_color` times `ambient_light_energy` by
`ambient_light_sky_contribution`. Reflections come from it when `reflected_light_source` is
`"sky"`, or `"background"` with a sky background: a smooth surface reflects the sharp level and a
rough one a blurrier level, as Godot's `StandardMaterial3D` does, and dark ambient light dims
them, as Godot's specular occlusion does. Either can use the sky while the background is a
color: the sky then lights the meshes without being drawn behind them.

### Fog

Fog is the Compatibility renderer's. After lighting and before exposure, each mesh fades toward
the fog's color by distance (exponential fog, or depth fog between its begin and end) and below
`fog_height`, and directional lights brighten the fog toward them by `fog_sun_scatter`. The sky
fades by `fog_sky_affect`, after `fog_aerial_perspective` mixes its colors into the fog's. A
`"color"` or `"clear_color"` background is fogged as a sky of that color, which the background's
energy multiplies twice, as Godot's does. A material's `disable_fog` leaves it clear.

### Volumetric fog

With `forward_plus`, `volumetric_fog_enabled` fills the view with fog that the lights shine
through and the shadows fall in, as Godot 4.6's Forward+ renderer does. Each frame, the view out
to `volumetric_fog_length` is cut into froxels (cells across, down and into the screen, set by
the project's `rendering.environment.volumetric_fog`, see
[project-format.md](project-format.md#volumetric-fog)), more of them near the camera by
`volumetric_fog_detail_spread`. A compute pass works out each froxel's light: its emission, the
directional lights through their shadow maps, the omni and spot lights that reach it (through
the meshes' clusters, with their shadows), the ambient light by
`volumetric_fog_ambient_inject`, the light of the first eight `VoxelGI`s whose boxes hold it by
`volumetric_fog_gi_inject` (every level of each, as Godot's fog reads them), each scattered toward the camera by
`volumetric_fog_anisotropy` (the Henyey-Greenstein phase) and times each light's
`light_volumetric_fog_energy`. With temporal reprojection, each froxel is sampled at a point
that moves each frame and blends in the frame before's light where it was then. The volume is
blurred across and down, then gathered along each column into the light the fog in front of
each point adds and how much of it the fog lets through.

Meshes are seen through the volume at their depth, after the [fog](#fog), as Godot blends them,
and the sky through all of it by `volumetric_fog_sky_affect` (a `"color"` or `"clear_color"`
background is left as it is unless the fog draws it as a sky). Transparent meshes take it too;
a material's `disable_fog` leaves it out. Frames with no meshes and reflection probes' cubes go
without it, and `gl_compatibility` has none, as in Godot. Every device draws the same passes (the
software device on the CPU, so `frame.capture` shows it), and a target's first frame starts from
its own density instead of fading in from none.

#### Fog volumes

A `FogVolume` adds fog to the volumetric fog in a shape, as Godot 4.6's: mist in a hollow, a
glowing cloud, or a box of clear air in fog where its material's density is below 0. It is
filled as the `FogMaterial` on its entity says, and with none as Godot's default material does:
a density of 1, white, filling the shape and stopping at its edge. A material's density fades
toward the shape's edges by `edge_fade`, and with height above the entity by `height_falloff`.

As Godot's, each volume fills the froxels between its corners' froxels (the whole view across
while the camera is near it), up to 32 a frame, those after left out and reported, and adds to
each froxel in Godot's fixed point: its density in 1024ths, and the light it scatters and its
glow held to what 11, 11 and 10 bits a channel carry. A `"world"` volume fills every froxel, as
big as twice the froxels in each direction, so a material on one fades by `edge_fade` (0 keeps
it whole). Volumes only show with `volumetric_fog_enabled`, in `forward_plus`, to cameras whose
`cull_mask` has one of their `layers`.
[examples/fog-volumes-3d](../../examples/fog-volumes-3d/) fills a cellar with them.

| `FogVolume` field | Default | Meaning |
| --- | --- | --- |
| `size` | `[2, 2, 2]` | The shape's size, centered on the entity, in its space; at 0 or more |
| `shape` | `"box"` | `"ellipsoid"`, `"cone"` (pointing up), `"cylinder"` (upright), `"box"` or `"world"` (everywhere) |
| `layers` | `1` | The render layers it is on, which cameras' `cull_mask` see |

| `FogMaterial` field | Default | Meaning |
| --- | --- | --- |
| `density` | `1` | Added to the fog's own; below 0 takes fog away |
| `albedo` | `[1, 1, 1, 1]` | The color it scatters light with, sRGB |
| `emission` | `[0, 0, 0, 1]` | The light it gives off itself, sRGB |
| `height_falloff` | `0` | How fast it thins above the entity: it halves every 1 / `height_falloff` meters |
| `edge_fade` | `0.1` | How it fades toward the shape's edges, as an exponent on the depth into it: 0 doesn't fade |

### Ambient occlusion

With `forward_plus`, `ssao_enabled` darkens what light can't easily reach (corners, creases and
what is close to a surface) as Godot 4.6's Forward+ renderer does with Intel's ASSAO. Before the
meshes' color pass, a prepass draws the opaque meshes' depth and view-space normals. The depth is
made linear and split into four slices of every other pixel; each slice's occlusion is gathered
from the depth within `ssao_radius` of each pixel, blurred along edges by `ssao_sharpness`, and the
slices are put back together at the frame's size. The color pass multiplies each opaque mesh's
ambient light by the occlusion, which dims its reflections too, as Godot's specular occlusion
does, and its direct light by `ssao_light_affect` times `ssao_ao_channel_affect` of it. Occlusion fades out with distance, and the project's
`rendering.environment.ssao` ([project-format.md](project-format.md#ssao)) sets its quality, size,
blur and fade. Transparent meshes get none, and `gl_compatibility` has no SSAO, as in Godot. The
software device draws the same passes on the CPU, so `frame.capture` shows it too.

### Screen-space reflections

With `forward_plus`, `ssr_enabled` reflects what is on the screen in shiny surfaces, as Godot
4.6's Forward+ renderer does. From the prepass's depth and normals (halved first at half size,
keeping the nearest of each 2x2 pixels), a pyramid of the nearest depths is made. Each opaque
pixel's reflected ray steps across it, up to `ssr_max_steps` times, dropping a level where it
might hit something and climbing one where it can't, and hits what it passes behind by no more
than `ssr_depth_tolerance`. Where it lands it reads the frame before, moved to where it is now,
fading in near the surface by `ssr_fade_in` and out toward the screen's edges by `ssr_fade_out`.
Rougher pixels read blurrier levels of a pyramid of the reflections, and the result is resolved
to the frame's size along its depths and normals. The color pass puts the reflections over the
sky's (see [Sky light](#sky-light)), weighed by how much the ray found, and then the material's
Fresnel. A target's first frame is drawn once without them, for the next to reflect.

Only what is on the screen is reflected: what is off it, or hidden behind something nearer,
isn't, as in Godot. Surfaces from roughness 0.7 up and transparent meshes take none, and
`gl_compatibility` has no SSR. The project's `rendering.environment.screen_space_reflection`
([project-format.md](project-format.md#screen-space-reflections)) sets its resolution. The
software device draws the same passes on the CPU, so `frame.capture` shows them too.

### Reflection probes

A `ReflectionProbe` is a box whose meshes reflect a cube drawn from inside it, as Godot 4.6's
Forward+ `ReflectionProbe`: shiny floors and metal in a room reflect its walls instead of the sky.
With `forward_plus`, the renderer draws the world into the probe's cube from `origin_offset`
through six 90-degree views, out to the box's sides or `max_distance` if that is further, with
the `cull_mask` layers' meshes, the frame's lights (shadows with `enable_shadows`), sky and fog,
but no post-processing, SSAO, SSR or probes. The cube is filtered into blurrier levels as the
[sky's light](#sky-light) is. `"once"` draws it when the probe first shows and again when it moves
or its box, offset, distance, layers or shadows change; `"always"` draws it every frame, which
costs six frames' worth of drawing. With `interior`, the cube shows no sky, fog or ambient light,
only the project's clear color behind the meshes, as for a room.

| Field | Default | Meaning |
| --- | --- | --- |
| `update_mode` | `"once"` | `"once"` or `"always"` |
| `intensity` | `1` | How bright its reflections are |
| `blend_distance` | `1` | How far in from the box's sides it fades in, in meters |
| `max_distance` | `0` | How far the cube sees, when further than the box's sides |
| `size` | `[20, 20, 20]` | The box, centered on the entity, in meters |
| `origin_offset` | `[0, 0, 0]` | Where the cube is drawn from, inside the box |
| `box_projection` | `false` | Aims reflections at the box's walls, for a room shaped like the box |
| `interior` | `false` | Draws the cube without the sky, fog or ambient light |
| `enable_shadows` | `false` | Draws shadows into the cube |
| `cull_mask` | `1048575` | The layers whose meshes the cube shows |
| `reflection_mask` | `1048575` | The layers whose meshes reflect it |
| `mesh_lod_threshold` | `1` | Kept for Godot; meshes have no LOD here |
| `ambient_mode` | `"environment"` | Ambient light inside it: `"disabled"` (the environment's), `"environment"` (the cube's blurriest level) or `"color"` |
| `ambient_color` | `[0, 0, 0, 1]` | The ambient light for `"color"`, in sRGB |
| `ambient_color_energy` | `1` | `ambient_color`'s energy |

Each opaque pixel inside a probe's box, on a mesh in its `reflection_mask`, reflects the cube's
level for its roughness in place of the sky's, times `intensity`, and with `ambient_mode` takes
its ambient light from it too. Toward the box's sides the probe fades out over `blend_distance`.
Where probes overlap they blend as Godot's `reflection_process` does: probes are sorted largest
first, smaller ones take the pixel first and larger ones fill what is left, and the sky fills the
rest. The probe's rotation turns its box; its scale is ignored, as Godot's. Probes share one atlas
of cubes, `rendering.reflections.reflection_atlas` in project.json
([project-format.md](project-format.md#reflection-probes)), which sets their size and how many a
frame draws (at most 42; the smallest past that reflect nothing). `gl_compatibility` draws no
probes yet. The software device draws the same captures and reflections on the CPU, so
`frame.capture` shows them too.

### Baked lightmaps

A `LightmapGI` bakes light into textures, as Godot 4.6's `LightmapGI`: light that bounces off
walls and floors, soft shadows and sky light, worked out once rather than every frame.
[`lightmap.bake`](commands.md#lightmapbake) traces it on the CPU, as Godot's `LightmapperRD`
does on the GPU, and writes a `.lightmap.json` file and its `.hdr` texels, naming the file in
`light_data`. It bakes the `MeshInstance3D`s whose `gi_mode` is `"static"`, visible under the
entity's parent (the whole world for a root), lit by the lights there whose `light_bake_mode`
isn't `"disabled"`:

- Each mesh is unwrapped into lightmap UVs with xatlas, as Godot's `lightmap_unwrap`, at 0.2
  meters a texel times `texel_scale` and its `gi_lightmap_texel_scale`. Meshes alike are
  unwrapped once, and the unwraps are packed into slices no larger than `max_texture_size`.
- Every texel takes the direct light of the lights, with shadows softened by their size, and
  rays traced from it (`quality`: 32, 128, 512 or 2048) bring the light of `bounces` bounces
  and of the environment. Emissive materials light their surroundings.
- The denoiser smooths the noise, as Godot's JNLM, and seams between charts are blended.

A bake is the same on every machine and for any number of threads, and takes seconds for a
room at `"medium"`. A model part with a skin isn't baked, since its bones move it.

| Field | Default | Meaning |
| --- | --- | --- |
| `quality` | `"medium"` | `"low"`, `"medium"`, `"high"` or `"ultra"`: 32, 128, 512 or 2048 rays a texel |
| `supersampling`, `supersampling_factor` | `false`, `2` | Bakes at the factor times the texels, then shrinks them |
| `bounces` | `3` | Times light bounces, 0 to 16 |
| `bounce_indirect_energy` | `1` | Times the light each bounce carries |
| `use_texture_for_bounces` | `true` | Bounces take direct light from the lightmap rather than tracing the lights again |
| `use_denoiser`, `denoiser_strength`, `denoiser_range` | `true`, `0.1`, `10` | Smooths the noise |
| `bias` | `0.0005` | How far rays start off surfaces, against self-shadowing |
| `texel_scale` | `1` | Times every mesh's texels across |
| `max_texture_size` | `16384` | The widest and tallest a slice may be |
| `environment_mode` | `"scene"` | What rays that leave the world bring: `"disabled"`, `"scene"` (the [WorldEnvironment's](#worldenvironment) ambient light or sky) or `"custom_color"` |
| `environment_custom_color`, `environment_custom_energy` | `[1, 1, 1, 1]`, `1` | The light for `"custom_color"` |
| `camera_attributes` | `""` | An entity whose `CameraAttributesPractical`'s `exposure_multiplier` it bakes with |
| `generate_probes_subdiv` | `"8"` | Where [light probes](#light-probes) go: `"disabled"` for the bounds' corners only, or `"4"`, `"8"`, `"16"` or `"32"` cells along the longest side of an octree over the static meshes |
| `interior` | `false` | Moving meshes inside its probes' bounds take only interior LightmapGIs' light |
| `light_data` | `""` | The `.lightmap.json` file it bakes into and draws from |

Each baked mesh then draws with its texels as its ambient light, in place of the
WorldEnvironment's and the probes' (it still reflects the sky and probes), bicubically filtered
and scaled by the camera's exposure over the one it was baked with. A `"static"` light isn't
drawn on lightmapped meshes, since the lightmap holds its light; a `"dynamic"` one still is. A
mesh that changed since the bake draws without its lightmap, with a problem saying to bake again.
Every device draws lightmaps, including the software one, so `frame.capture` shows them.

A `.lightmap.json` file is Godot's `LightmapGIData` in text (`slash-cli schema lightmap`): the
`texture` (an `.hdr` file of the slices one above the other), each slice's `width` and `height`,
the `slices`, the `baked_exposure`, the unwraps in `meshes` by a hash of each mesh (lightmap UVs,
the vertices they came from and the triangles, as base64), and the `users`: each baked entity, the
model `part`, its `mesh`, `slice` and `uv_rect`, then its `probes` (below). `scene validate` checks a LightmapGI's
`light_data` and `camera_attributes`, and exports ship both files.

#### Light probes

A LightmapGI also bakes light probes, as Godot 4.6's do, which light the meshes that move: a
`MeshInstance3D` whose `gi_mode` is `"dynamic"` takes its ambient light from them, at its bounds'
middle, in place of the WorldEnvironment's and the reflection probes'. Lights still light it live,
`"static"` ones included.

- Probes go at the corners of the bounds around the static meshes and at the corners of the
  octree cells (`generate_probes_subdiv` along the longest side) that their triangles touch, as
  Godot's `LightmapGI::bake` places them. An entity with a `LightmapProbe` component (no fields)
  adds one where light changes quickly, as in a doorway; generated probes within half a cell of
  it are left out.
- Each probe traces rays (`quality`: 64, 256, 512 or 2048) for its light from every direction,
  kept as nine spherical harmonics coefficients, with the lightmap's bounces.
- The probes are joined into Delaunay tetrahedra (Godot's `Delaunay3D`, with an exact in-sphere
  test so they never overlap and are the same everywhere) and a BSP tree finds the one around a
  point. A moving mesh
  takes its four probes' light blended by where it is, turned with the LightmapGI and scaled by
  the camera's exposure over the one it was baked with, then eased toward it as it moves (15
  times the frame's time, Godot's `update_speed`). Where LightmapGIs' bounds overlap, their
  light blends by how far inside each the mesh is, and an `interior` one stands in for the
  rest. Outside every LightmapGI's bounds, it takes the scene's ambient light.

The `.lightmap.json` file keeps them in `probes`: the `bounds` (position, then size), `interior`,
and as base64 the `points`, their `sh` light (27 floats each), the `tetrahedra` (four probe
indices each) and the `bsp` tree (24-byte nodes: a plane, then the node or tetrahedron on each
side). `scene validate` reports light, tetrahedra or nodes that don't fit the points.

### VoxelGI

A `VoxelGI` lights what is in a box with light that bounces, as Godot 4.6's `VoxelGI`: the static
meshes in it are voxelized into cells once, and the lights are injected into the cells as the game
runs, so lights that move or change bounce too. Meshes in the box take their ambient light and
reflections from it, by tracing cones through the cells. It is Forward+ only, as Godot's.

[`voxel_gi.bake`](commands.md#voxel_gibake) voxelizes the `MeshInstance3D`s whose `gi_mode` is
`"static"`, visible under the entity's parent (the whole world for a root), that touch its box, as
Godot's `Voxelizer`: each triangle goes down an octree to cells `subdiv` across the box's longest
side, and each cell takes the albedo, emission and normal of the triangles in it. It writes a
`.voxelgi.json` file and names it in `data`. A bake is the same on every machine.

As the game runs, the renderer:

- Injects the lights into the cells whenever they, the VoxelGI's place or its settings change,
  as Godot's `voxel_gi.glsl` does on the GPU, here on the CPU on the job pool: the omni and spot
  lights whose `light_bake_mode` isn't `"disabled"` and whose range reaches the box, then every
  directional light, at most 32, each at its `light_energy` times its `light_indirect_energy`,
  shadowed through the cells where it casts shadows. A second bounce is traced through the first
  with `use_two_bounces`, and the light spreads up the octree by `propagation`.
- Plots the `"dynamic"` meshes whose bounds touch the box into the cells' light, again each frame
  one moves or changes, as Godot's dynamic objects: each is drawn from the six faces of its box
  into maps 16 times finer than the cells, the albedo, normal and emission of its nearest surfaces
  lit by the same lights, then shrunk down to the cells and blended over each level of the light.
  So a moving mesh bounces light onto what is near it, and glows on it where it is emissive. Moving
  meshes don't shadow the cells, a skinned model part is plotted at rest, and emission is plotted
  at an exposure of 1, as Godot's without physical light units.
- Gives each mesh the VoxelGIs its bounds touch, at most two in the order they were made, as
  Godot pairs them. Lightmapped meshes, `"dynamic"` meshes lit by [light probes](#light-probes),
  terrains and meshes drawn only into shadows take none.
- Traces cones from each pixel through the cells' light, as Godot's `gi.glsl`: four cones of 45
  degrees for its ambient light, or six narrower ones with project.json's
  `rendering.global_illumination.voxel_gi.quality` set to `"high"`, and one as wide as its
  roughness for its reflection. Where the cones find cells, their light goes over the
  environment's ambient light and sky reflections by how much they cover, before
  [reflection probes](#reflection-probes); where they find none, the environment shows through,
  except in an `interior` VoxelGI.

Every Forward+ device traces VoxelGIs, including the software one, so `frame.capture` shows
them; Compatibility draws without them.

| Field | Default | Meaning |
| --- | --- | --- |
| `subdiv` | `"128"` | Cells along the box's longest side: `"64"`, `"128"`, `"256"` or `"512"` |
| `size` | `[20, 20, 20]` | The box, centered on the entity, in its space |
| `camera_attributes` | `""` | An entity whose `CameraAttributesPractical`'s `exposure_multiplier` emission is baked with; a camera's over it scales the light |
| `data` | `""` | The `.voxelgi.json` file it bakes into and draws from |
| `dynamic_range` | `2` | The brightest light the cells hold, 1 to 8 |
| `energy` | `1` | Times the light meshes take |
| `bias` | `1.5` | Cells cones start out from surfaces, against lighting themselves |
| `normal_bias` | `0` | Cells cones start out along surfaces' normals |
| `propagation` | `0.5` | How much light spreads up into bigger cells, 0 to 1 |
| `use_two_bounces` | `true` | Light bounces twice inside the cells, not once |
| `interior` | `false` | Leaves out the environment's light: meshes take only the cells' |

A `.voxelgi.json` file is Godot's `VoxelGIData` in text (`slash-cli schema voxel_gi`): the
`bounds` it voxelized (position, then size, in the VoxelGI's space), its `octree_size` (cells
along each axis), the `baked_exposure`, the `level_counts` of the octree from the root, and its
`cells` as base64, level by level, each sorted as Godot sorts them: a cell's place, albedo,
emission and normal. `scene validate` checks a VoxelGI's `subdiv`, `size`, `camera_attributes`
and `data`, and exports ship the file.

### Decals

A `Decal` projects textures down its -Y onto the surfaces in a box, as Godot 4.6's Forward+
`Decal`: paint on a wall, a puddle on the street, a scorch mark. Where its albedo texture
covers a surface, the surface takes its color by `albedo_mix`, its normal texture's bumps, and its
ORM texture's ambient occlusion, roughness and metallic (red, green and blue); its emission
texture glows on top, whatever the albedo covers. A decal with neither an albedo nor an emission
texture draws nothing. Textures load as sprites' do, at most 16384 texels across.

As Godot's, decals go into the frame's clusters with the lights: up to 512 a frame, those nearest
the camera (less their `sorting_offset`) drawn last, on top. Each frame's textures are packed into
one atlas, as Godot's, with mipmaps that project.json's `rendering.textures.decals.filter` samples
(see [Texture forms](project-format.md#texture-forms)). The atlas holds 2^25 texels over its
levels, so a texture 2048 texels across fits with room to spare; decals whose textures overflow it
are left out and reported, those before them kept. Every Forward+ device draws decals, including
the software one, so `frame.capture` shows them, on unshaded meshes too; Compatibility draws
without them, as Godot's. A mesh takes a decal when its `layers` share a bit with the decal's
`cull_mask`, and a camera sees a decal on its `cull_mask` layers.
[examples/decals-3d](../../examples/decals-3d/) paints a street with three.

| Field | Default | Meaning |
| --- | --- | --- |
| `size` | `[2, 2, 2]` | The box, centered on the entity, in its space; textures span its x and z |
| `texture_albedo` | `""` | Its color, sRGB with alpha, which covers the surface by alpha |
| `texture_normal` | `""` | Its bumps, Y down as most tools draw normal maps, where the albedo covers |
| `texture_orm` | `""` | Ambient occlusion, roughness and metallic in red, green and blue, where the albedo covers |
| `texture_emission` | `""` | Light it gives off, sRGB |
| `emission_energy` | `1` | Times the emission |
| `modulate` | `[1, 1, 1, 1]` | sRGB color times the albedo and emission; its alpha fades the decal |
| `albedo_mix` | `1` | How much of the surface's color the albedo replaces, 0 to 1; its normal, ORM and emission apply by alpha either way |
| `normal_fade` | `0` | Fades the decal on surfaces turned away from its -Y: 0 doesn't, 1 shows it only on surfaces facing it head on |
| `upper_fade` | `0.3` | How sharply it fades toward the top of its box, as an exponent: 0 doesn't fade |
| `lower_fade` | `0.3` | The same toward the bottom |
| `distance_fade_enabled` | `false` | Fade it out with distance from the camera |
| `distance_fade_begin` | `40` | Meters from the camera the fade starts |
| `distance_fade_length` | `10` | Meters over which it fades out; past them it isn't drawn |
| `cull_mask` | `0xFFFFF` | The mesh layers it projects onto |
| `layers` | `1` | The render layers it is on, which cameras' `cull_mask` see |
| `sorting_offset` | `0` | Meters taken off its distance when ordering decals, so it draws over closer ones |

### Checks

`scene validate` and exports report an entity with two primitive meshes (at
`/entities/<id>/components`), a `MeshInstance3D` with none (at its `MeshInstance3D`), a
`TorusMesh` whose radii are equal, and a `mesh` that isn't a `.gltf`, `.glb`, `.obj` or `.fbx` file in the
project, doesn't load, or sits beside a primitive mesh (at its `mesh`), as drawing would, and
[CSG shapes](#csg-shapes) that can't be made, [grid maps](#grid-maps) whose cells can't
be made, [path followers](#paths) that can't follow, [LightmapGIs](#baked-lightmaps) whose
`light_data` doesn't load and [VoxelGIs](#voxelgi) whose `data` doesn't load.
Exports
check every `.gltf`, `.glb`, `.obj` and `.fbx` file as `scene validate` does, and ship them with their
`.bin` buffers, `.mtl` libraries and images. `frame.summary` doesn't describe 3D meshes
yet.

## Canonical form

Every scene has exactly one canonical text form, which `scene.save` and `slash-cli scene format`
write:

- Keys sorted alphabetically at every level.
- Two-space indent, one entity per block, short arrays of numbers on one line.
- Fields equal to their defaults removed, and entities with no components written as `{}`.
- Numbers normalized: `1` in a float field becomes `1.0`, `2.50` becomes `2.5`.
- A newline at the end of the file.

For example, this valid but non-canonical file:

```json
{"format":1,"entities":{"b":{"components":{"Transform2D":{"scale":[1,1],"position":[1,2.50]}}},"a":{}}}
```

formats to:

```json
{
  "entities": {
    "a": {},
    "b": {
      "components": {
        "Transform2D": {
          "position": [1.0, 2.5]
        }
      }
    }
  },
  "format": 1
}
```

`slash-cli scene format --check` prints `{"canonical":false}` and exits with 1 for a file that is
not canonical, which makes it usable as a CI or pre-commit check.

## Validation errors

`slash-cli scene validate` and `scene.open` report every error at once. Each error carries the
JSON Pointer of the bad value:

```json
{"errors":[{"message":"expected number, got string","path":"/entities/player/components/Transform2D/position/1"},{"message":"unknown property \"rotaton\"; did you mean \"rotation\"?","path":"/entities/player/components/Transform2D/rotaton"},{"message":"unknown property \"Transfrom3D\"; did you mean \"Transform3D\"?","path":"/entities/player/components/Transfrom3D"},{"message":"expected 1, got 2","path":"/format"}],"valid":false}
```

An invalid scene never loads partly: the open scene stays as it was.

## Editing scenes

- Agents should change scenes through commands (`entity.create`, `component.set`, `batch`, then
  `scene.save`) rather than editing the file. Commands are checked, undoable and return only what
  changed.
- When a scene is edited by hand, run `slash-cli scene validate` and then `slash-cli scene format`
  before committing it.
- Don't edit a scene file by hand while a server has it open: the server holds the scene in
  memory and its next `scene.save` overwrites the edit.

## Prefabs

A prefab is one entity's components in a `.prefab.json` file, like a Godot scene of one node
that other scenes instance; for several entities, instance a scene
([instanced scenes](#instanced-scenes)). An entity that `"extends"` it starts with the prefab's
components, and its own fields override the prefab's. With `Health` as a game's component, `prefabs/enemy.prefab.json`:

```json
{
  "components": {
    "Health": {
      "current": 50.0,
      "max": 50.0
    },
    "Transform2D": {
      "scale": [2.0, 2.0]
    }
  },
  "format": 1
}
```

and an entity in a scene:

```json
"enemy_01": {
  "components": {
    "Health": {
      "current": 20.0
    },
    "Transform2D": {
      "position": [200.0, 40.0]
    }
  },
  "extends": "prefabs/enemy.prefab.json"
}
```

`enemy_01` has `Health` with `current` 20 and `max` 50, and a `Transform2D` at `[200, 40]` with
the prefab's scale. A `null` component removes one the prefab has: `"Health": null` gives an
instance no `Health`. Changing the prefab file changes every instance, except for the fields an
instance overrides.

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `components` | No | Components keyed by type name, as in a scene entity |
| `extends` | No | Another prefab this one starts from, so prefabs can build on each other |

- Paths are relative to the project folder and use `/`, such as `"prefabs/enemy.prefab.json"`;
  they cannot leave the folder, contain `.` or `..` parts, or use a name Windows keeps for a
  device, such as `con` or `aux`.
- Saving writes only what differs from the prefab: a field equal to the prefab's value is left
  out, even when the prefab's value is not the default.
- A missing prefab, a prefab that extends itself through a chain, or a `null` that names a
  component the prefab lacks is a load error at the entity's `/extends` or component path.
- `slash-cli scene validate` and `scene format` accept `.prefab.json` files too. They read the
  prefabs a file extends from its project: the nearest folder above it with a `project.json`.
- Exports copy and validate every prefab with the scenes.
- `entity.create` with `"extends"` makes an instance and `prefab.save` writes an entity to a
  prefab file (see the [command reference](commands.md)).

## Instanced scenes

A scene can be instanced by other scenes, as Godot's PackedScene: an entity that `"extends"` a
`.scene.json` file becomes the scene's root entity, and the scene's other entities come with it.
A scene names its root in `"root"`, which `scene.save` sets
([commands](commands.md#scenesave)). `scenes/player.scene.json`:

```json
{
  "entities": {
    "gun": {
      "components": {
        "Transform2D": {
          "position": [8.0, 0.0]
        }
      }
    },
    "player": {
      "components": {
        "Transform2D": {
          "children": ["sprite", "gun"]
        }
      }
    },
    "sprite": {
      "components": {
        "Sprite2D": {
          "texture": "art/player.png"
        }
      }
    }
  },
  "format": 1,
  "root": "player"
}
```

and a level that instances it:

```json
"hero": {
  "components": {
    "Transform2D": {
      "position": [64.0, 32.0]
    }
  },
  "extends": "scenes/player.scene.json"
},
"hero/sprite": {
  "components": {
    "Sprite2D": {
      "flip_h": true
    }
  }
},
"hero/gun": null
```

`hero` has the player's components with its own position. The scene's other entities are
`hero/sprite` and `hero/gun`: the instance's ID, `/`, and their ID in the scene. References
inside the scene follow them, so `hero`'s `Transform2D` `children` are `hero/sprite` and
`hero/gun`, and a joint's `node_a` naming `sprite` names `hero/sprite`.

- An entry keyed with `/` holds only that entity's overrides, as an instance's own entry does,
  and `null` leaves the entity out. It cannot have `extends`: the instanced scene decides that.
  An entry for an entity no instanced scene brings is a load error that says why.
- A scene can instance scenes in turn, so IDs nest: a gun scene instanced as `gun` in the player
  scene gives `hero/gun/muzzle`. Scenes that instance each other in a loop are a load error.
- Saving writes the instance's `extends` and overrides, an entry for each brought entity that
  differs from the scene, and `null` for each one destroyed.
- Changing the scene file changes every instance, keeping their overrides; entities added to the
  scene appear in each instance, and removed ones go.
- `entity.create` with `"extends"` makes an instance, and `entity.destroy` on an instance takes
  the entities it brought. A prefab file holds one entity, so it can extend another prefab but
  not a scene, and `prefab.save` refuses an instance of a scene.
- Only a scene with a `"root"` can be instanced. The root must be an entity of its own, not one
  an instanced scene brings.

## Textures

Fields that name a texture take a PNG, JPEG (`.jpg` or `.jpeg`), BMP or KTX2 file. PNG, JPEG and
BMP files are RGBA8 texels. JPEG files, baseline or progressive, decode with stb_image's integer
decoder, so they give the same texels on every machine. A
KTX2 file holds texels the way GPUs take them: RGBA8 (Vulkan format 37 or 43), BC7 (145 or 146)
or ASTC 4x4 (157 or 158), in one 2D level that is not supercompressed; only the first mip level
is read. BC7 and ASTC take a byte a texel, a quarter of RGBA8's memory, in blocks of 4x4 texels,
so their sides are multiples of 4.

A device that samples a file's format gets its blocks as they are: BC7 on most desktop GPUs, and
ASTC on phones, Apple GPUs and browsers with `WEBGL_compressed_texture_astc`. Any other device
gets the texels decoded, exactly as the format specifies, so a texture looks the same everywhere;
the software device behind `frame.capture` always decodes. `perf.stats` counts each texture's
bytes in the form the device keeps it.

## Sprite frames

A `.frames.json` file holds named flipbook animations, like Godot's SpriteFrames resource. An
entity with `AnimatedSprite2D` plays one of them on its `Sprite2D`, adding a `Sprite2D` if it
has none: each tick the `animated_sprite_2d` system moves the animation on and sets the sprite's
`texture`, `hframes`, `vframes` and `frame`. `slash-cli schema frames` prints the schema, and
`slash-cli scene validate` and `scene format` accept `.frames.json` files.

```json
{
  "animations": {
    "idle": {
      "fps": 2,
      "frames": ["sprites/idle_0.png", "sprites/idle_1.png"]
    },
    "jump": {
      "fps": 10,
      "frames": [2, 3],
      "hframes": 4,
      "loop": false,
      "texture": "sprites/hero.png"
    },
    "walk": {
      "fps": 8,
      "hframes": 4,
      "texture": "sprites/hero.png"
    }
  },
  "format": 1
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `animations` | Yes | Animations keyed by name |
| `animations/<name>/frames` | No | The frames in order. Each is a texture path (the whole image) or a frame number in the animation's sheet, counted along rows from 0. Left out, every frame of the sheet in order |
| `animations/<name>/texture` | No | The sprite sheet that frame numbers pick from |
| `animations/<name>/hframes`, `vframes` | No | Columns and rows of frames in the sheet, 1 to 16384 (default 1) |
| `animations/<name>/fps` | No | Frames per second, 0 or more (default 5) |
| `animations/<name>/loop` | No | Start again after the last frame (default `true`) |

- A frame changes every `1 / (fps * speed_scale)` seconds of game time, so animations play at
  the same speed at any tick rate. A negative `speed_scale` plays backwards.
- An animation that doesn't loop stops on its last frame (its first, when playing backwards),
  sets `playing` to `false` and emits an `animation_finished` event with
  `{"animation": "<name>"}`, which `events.subscribe` can wait for.
- A frame number needs the animation's `texture`, and must be inside its sheet. An animation
  needs `frames`, or a `texture` to play every frame of.
- An unknown `animation` name logs one warning, with the closest name ("did you mean"), and the
  sprite keeps its last frame.
- Files reload when they change, like every asset.

## Materials

A `.material.json` file colors each pixel of a sprite, like a Godot `ShaderMaterial` with a
`canvas_item` shader, written as a node graph, or as a game's own [shader](#shaders) in HLSL. `Sprite2D`'s `material`
names the file and `material_params` sets its params, so one material serves many sprites.
`slash-cli schema material` prints the schema, `slash-cli scene validate` and `scene format`
accept `.material.json` files, and the [`material.eval`](commands.md#materialeval) command
returns the colors a material draws. [examples/materials](../../examples/materials/) has three.

```json
{
  "color": {"op": "vec4", "args": [
    {"op": "mix", "args": ["base.rgb", "flash_color.rgb", "flash"]}, "base.a"]},
  "format": 1,
  "nodes": {
    "base": {"op": "mul", "args": [{"op": "texture", "args": ["texture", "uv"]}, "color"]}
  },
  "params": {
    "flash": {"type": "float", "value": 0, "min": 0, "max": 1},
    "flash_color": {"type": "color", "value": [1, 1, 1, 1]}
  }
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `color` | No | The pixel's color, a vec4 from 0 to 1. Left out, the texture times the vertex color, as a sprite with no material draws |
| `nodes` | No | Named nodes, `{"op": ..., "args": [...]}`, for values used more than once: each is computed once |
| `params` | No | Values sprites set through `material_params`, by name |
| `params/<name>/type` | Yes | `float`, `vec2`, `vec3`, `vec4`, `color` (a vec4; `[r, g, b]` means alpha 1) or `texture` |
| `params/<name>/value` | No | The default: a number, an array of numbers, or a texture path (`""` samples white). Left out, 0, or white for colors and textures |
| `params/<name>/min`, `max` | No | Numbers: the range every component must be in |
| `params/<name>/repeat` | No | Textures: tile past the edges instead of stretching them (default `false`) |
| `params/<name>/filter` | No | Textures: `inherit` (the sprite's filter, the default), `linear` or `nearest` |
| `params/<name>/doc` | No | What the param does, for whoever sets it |
| `blend` | No | How the color combines with what is under it, as Godot's blend modes: `mix` (default), `add`, `sub`, `mul` or `premul_alpha` |
| `light_mode` | No | How [2D lights](#2d-lights) shade it, as Godot's: `normal` (default), `unshaded` (lights and `CanvasModulate` leave it alone) or `light_only` (visible only where lights reach) |
| `shader` | No | A game's own shader instead of `color` and `nodes`: an `.hlsl` file's project path (see [Shaders](#shaders)) |
| `compiled` | No | The shader compiled for each backend. Exports write it; never written by hand |

An arg is a number, an array of 2 to 4 numbers, a node written in place, or a name: a node in
`nodes`, a param, or an input. A name can pick components after a dot, from `xyzw` or `rgba`:
`"base.rgb"`, `"uv.yx"`, `"color.aaaa"`. Names of nodes and params use lowercase letters, digits
and `_`, don't start with a digit, and can't be an input's.

| Input | Type | Godot's | Value |
| --- | --- | --- | --- |
| `uv` | vec2 | `UV` | The texture coordinate, from 0 to 1 across the sprite's frame |
| `color` | vec4 | `COLOR` | The vertex color: the sprite's `modulate` |
| `texture` | texture | `TEXTURE` | The sprite's texture, for the `texture` op |
| `texture_pixel_size` | vec2 | `TEXTURE_PIXEL_SIZE` | One over the texture's width and height |
| `fragcoord` | vec2 | `FRAGCOORD.xy` | The pixel's position in the target, in pixels from the top left |
| `time` | float | `TIME` | Seconds of game time, wrapping every hour |

Ops work on each component, as in shaders. Args of one op have one size, and a float stands in
for any size: `["uv", 2]` doubles both components. Each op's result has the size of its args
unless the table says otherwise.

| Ops | Args | Result |
| --- | --- | --- |
| `add`, `sub`, `mul`, `div`, `min`, `max` | a, b | |
| `mod` | a, b | `a - b * floor(a / b)`, as GLSL's `mod` |
| `pow` | a, b | `a` to the power `b`; a negative `a` gives 0 |
| `step` | edge, x | 1 where `x >= edge`, else 0 |
| `abs`, `sign`, `floor`, `ceil`, `fract`, `sqrt`, `exp`, `log` | a | |
| `round` | a | `floor(a + 0.5)` |
| `sin`, `cos`, `tan`, `asin`, `acos` | a | In radians |
| `atan` | y_over_x, or y, x | With two args, the angle of (x, y), as `atan2` |
| `saturate`, `one_minus`, `negate` | a | Clamped to 0 to 1; `1 - a`; `-a` |
| `mix` | a, b, t | `a + (b - a) * t` |
| `clamp` | x, min, max | |
| `smoothstep` | edge0, edge1, x | 0 below `edge0`, 1 above `edge1`, smooth between |
| `dot`, `distance` | a, b | A float; both args the same size |
| `length` | a | A float |
| `normalize` | a | `a` with length 1; zero stays zero |
| `vec2`, `vec3`, `vec4` | floats and vectors | A vector made of the args' components in order, as `vec4(rgb, 1)`; one float fills every component |
| `texture` | texture, uv | The vec4 color of `texture` (the input) or a texture param at `uv` |
| `rgb_to_hsv`, `hsv_to_rgb` | a vec3 | The color as hue, saturation and value, each from 0 to 1, or back |

- The color is clamped to 0 to 1 before blending, and NaN draws as 0.
- A material runs on the GPU as one program of at most 64 instructions (most ops take one; a
  `vecN` takes one per arg) and 32 constants (each different number, number param and the time
  take one), with 28 values alive at once and 3 texture params besides the sprite's texture. It has at most 64 nodes, nested in place at most
  16 deep. A graph past a limit fails with what to change.
- Mistakes fail when the file loads, each with the JSON Pointer of the bad value and the closest
  name: an unknown op, name or param, args of the wrong size or number, a cycle between nodes, a
  value outside its param's range. A sprite whose material fails to load, or whose
  `material_params` name a param the material doesn't have or give a bad value, is left out
  with one warning in the log.
- `material.eval` computes colors on the CPU the way the GPU does, so an agent checks a material
  without a screenshot.
- Material files reload when they change, like every asset, and exports check and ship them.

### Shaders

For what a graph can't say, `shader` names a game's own shader: an `.hlsl` file with a
`fragment()` function, a `vertex()` function or both, as Godot's `canvas_item` shaders, written
in HLSL. The functions take nothing and return nothing; they read and write Godot's built-ins,
which are globals, and the material's params, by name. Helper functions, loops and `discard` are
plain HLSL. [examples/custom-shaders](../../examples/custom-shaders/) has a dissolve and a flag
that sways.

```hlsl
// shaders/dissolve.hlsl, with params amount (float) and edge_color (color).
void fragment() {
    float n = texture(noise_map, UV).r;
    if (n < amount) {
        discard;
    }
    COLOR.rgb = lerp(edge_color.rgb, COLOR.rgb, smoothstep(amount, amount + 0.1, n));
}
```

```json
{
  "format": 1,
  "params": {
    "amount": {"max": 1, "min": 0, "type": "float", "value": 0},
    "edge_color": {"type": "color", "value": [1, 0.5, 0, 1]},
    "noise_map": {"repeat": true, "type": "texture", "value": "noise.png"}
  },
  "shader": "shaders/dissolve.hlsl"
}
```

| Built-in | Type | In | Value |
| --- | --- | --- | --- |
| `VERTEX` | float2 | both | The vertex position in the canvas, in pixels; `vertex()` can move it |
| `UV` | float2 | both | The texture coordinate, from 0 to 1 across the sprite's frame |
| `COLOR` | float4 | both | In `vertex()`, the vertex color (the sprite's `modulate`); in `fragment()`, the texture times it, and what the pixel draws |
| `TIME` | float | both | Seconds of game time, wrapping every hour |
| `TEXTURE` | texture | both | The sprite's texture |
| `TEXTURE_PIXEL_SIZE` | float2 | both | One over the texture's width and height |
| `FRAGCOORD` | float4 | `fragment()` | The pixel's center in the target, in pixels from the top left (`xy`); `z` is 0 and `w` 1 |
| `NORMAL_MAP` | float3 | `fragment()` | The sprite's `normal_texture` texel (green up), or `(0.5, 0.5, 1)`, which lights shade with |
| `SPECULAR_SHININESS` | float4 | `fragment()` | The specular color and shininess lights use: the sprite's `specular_texture` texel times its specular color |

- `texture(t, uv)` samples `TEXTURE` or a texture param, as Godot's `texture()`, with the
  texture's filter and repeat; `PI`, `TAU` and `E` are defined.
- Params are globals of their type: a `float` param is a `float`, `vec2` a `float2`, `vec3` a
  `float3`, `vec4` and `color` a `float4`, and `texture` a texture. A shader takes at most 31
  number params (the time takes the last constant) and 3 textures. Names HLSL or the code
  around the shader uses (keywords, types, intrinsic functions such as `length`, and the code's
  own globals) are refused with the param's path.
- A `vertex()` that moves `VERTEX`, `UV` or `COLOR` moves or recolors the corners, and
  `fragment()` gets those, interpolated. Lights shade the color `fragment()` leaves, as the
  material's `light_mode` says; writing `NORMAL_MAP` or `SPECULAR_SHININESS` makes lights use
  them even when the sprite has no normal or specular texture, as in Godot.
- `slash-cli` and `slash-mcp` compile shaders with glslang, SPIRV-Cross and Tint when they first
  draw, for Vulkan, Metal, Direct3D 12, OpenGL ES, WebGL 2 and WebGPU, and the CPU, which the software device
  ([`frame.capture`](commands.md#framecapture)) and `material.eval` run. Games can't compile
  shaders, so `slash-cli export` writes each material's shader, compiled for the platform, into
  the staged material file as `compiled`, and leaves the `.hlsl` files out.
- A mistake is reported with the shader file and line, as
  `shaders/dissolve.hlsl:4: 'amout' : unknown variable`: by `slash-cli scene validate` on the
  material (at `/shader`), by exports, by `material.eval`, and in the log for a sprite whose
  shader doesn't compile, which is left out. Shader files reload when they change.
- `material.eval` runs `fragment()` alone, with derivatives (`ddx`, `ddy`, `fwidth`) of 0, and a
  discarded pixel comes back as `[0, 0, 0, 0]`. Functions such as `sin` and `pow` run with the C
  library on the CPU, which can differ from a GPU's in the last bits.
- Not yet: passing a texture to a function of your own (sample it in `fragment()` instead), and
  Godot's shader language itself.

## Animations

A `.anim.json` file holds keyframe animations by name, like a Godot `AnimationLibrary` of
`Animation` resources. An entity's `AnimationPlayer` names the file (or a
[model](#skeletons-and-model-animations), to play its animations as they are) in `library` and
plays one of them: each tick the `animation_player` system moves `time` on, sets every field the
animation's value tracks name on the entity's own components (adding a component it lacks), and
emits the events its event tracks name. A track with a `target` acts on another entity instead,
the one the player's `targets` maps that name to, as a Godot track names a node by its path from
the player's root node. A node track moves a node or bone of the model the entity's
`MeshInstance3D` draws, as Godot's position, rotation and scale tracks move a `Node3D` or a
`Skeleton3D` bone (see [node tracks](#node-tracks)). `slash-cli schema animation` prints the schema,
`slash-cli scene validate` and `scene format` accept `.anim.json` files, and the
[`animation.eval`](commands.md#animationeval) command returns the values an animation sets.
[examples/keyframe-animation](../../examples/keyframe-animation/) has two.

```json
{
  "animations": {
    "walk": {
      "length": 0.5,
      "loop": "linear",
      "tracks": [
        {"interpolation": "cubic", "path": "Sprite2D/offset/y",
         "keys": [{"time": 0, "value": 0}, {"time": 0.25, "value": -4}]},
        {"path": "Sprite2D/frame",
         "keys": [{"time": 0, "value": 0}, {"time": 0.25, "value": 1}]},
        {"event": "footstep",
         "keys": [{"time": 0, "data": {"foot": "left"}}, {"time": 0.25, "data": {"foot": "right"}}]},
        {"target": "sword", "path": "Transform2D/rotation",
         "keys": [{"time": 0, "value": 0}, {"time": 0.25, "value": 0.3}]}
      ]
    }
  },
  "format": 1
}
```

and the entity that plays it, whose child `sword` the last track turns:

```json
"hero": {
  "components": {
    "AnimationPlayer": {"animation": "walk", "library": "anim/hero.anim.json",
                        "targets": {"sword": "sword"}},
    "Transform2D": {"children": ["sword"]}
  }
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `animations` | Yes | Animations by name |
| `animations/<name>/from` | No | A model's animation it plays on the nodes of the model the entity's `MeshInstance3D` draws: `{"model": "models/hero.glb", "animation": "Walk"}` ([model animations](#skeletons-and-model-animations)) |
| `animations/<name>/length` | Unless `from` | Seconds, more than 0. Left out, the length of the model's animation it comes from |
| `animations/<name>/loop` | No | What happens at the end, as Godot's loop modes: `none` stops there, `linear` starts over, `pingpong` turns round. Left out, the model animation's (`linear` if it loops), else `none` |
| `animations/<name>/tracks` | Unless `from` | Value tracks, event tracks and node tracks |
| `tracks/<n>/path` | One of these | A value track: the field it sets, as a component, a field and optionally a part of the field |
| `tracks/<n>/event` | One of these | An event track: the event kind it emits |
| `tracks/<n>/node` | One of these | A [node track](#node-tracks): the node of the drawn model it moves |
| `tracks/<n>/property` | Node tracks | What it moves: `position`, `rotation` (Euler angles in radians, as `Transform3D`'s) or `scale` |
| `tracks/<n>/target` | No | The entity it acts on: a name the `AnimationPlayer`'s `targets` maps to an entity. Left out, the player's own entity |
| `tracks/<n>/keys` | Yes | At least one key, in time order, each later than the one before and no later than the length |
| `keys/<n>/time` | Yes | Seconds from the start |
| `keys/<n>/value` | Value and node tracks | A value the field takes; for a node track, `[x, y, z]` |
| `keys/<n>/data` | No | Event tracks: the event's data, any JSON |
| `keys/<n>/transition` | No | Easing on the way to the next key, as Godot's easing curve: 1 (default) is linear, above 1 eases in, between 0 and 1 eases out, below 0 eases in and out, 0 holds the value until the next key |
| `tracks/<n>/interpolation` | No | Value tracks: `linear` (default), `nearest` (each key's value until the next), `cubic` (a smooth curve through the keys), or `linear_angle` and `cubic_angle`, which turn the short way round, for rotations in radians. Node tracks: `linear` or `nearest` |
| `tracks/<n>/update` | No | Value tracks: `continuous` sets the value every tick, `discrete` sets a key's value when playback reaches it. `auto` (default) is continuous for float fields and discrete for others: integers, strings, booleans and entity references |
| `tracks/<n>/loop_wrap` | No | Value and node tracks in a looping animation: before the first key and after the last, move between the last key and the first across the loop's end (default `true`); `false` holds them |

A path's part picks one number out of a vector or color field, by `x`, `y`, `z` or `w`, `r`, `g`,
`b` or `a`, or by index (`Sprite2D/modulate/a`, `Transform2D/position/y`), or a field of a
struct field. Values are absolute, as in Godot: a track on `Transform2D/position` places the
entity, so animate `Sprite2D/offset` to move a sprite around its entity instead.

- Playback passes the keys after where it was, up to and including where it gets to, as Godot's:
  at 60 ticks a second, a key at 0.25 seconds is passed on the 15th tick. When playback starts at
  the start of an animation, a key there is passed too. Event keys emit their event then, with
  the ID of the entity the track acts on and the key's `data`, and discrete tracks set their
  key's value.
- The names in `targets` are entity references, so they follow the entities when a scene is
  instanced: a hero scene whose player targets `sword` targets `knight/sword` in the knight
  instance. Two tracks may set one path on different targets. A target missing from `targets`,
  `null` or naming a destroyed entity logs one warning, and its tracks don't play.
- An animation that doesn't loop stops at its end (at the start when playing backwards), sets
  `playing` to `false` and emits `animation_finished` with `{"animation": "<name>"}`. Playing it
  again from there starts it over. A linear loop passes keys at both its end and its start as it
  wraps; a pingpong loop passes a key at each end once and flips `backwards`. Whole loops in a
  single tick pass no keys.
- Fields a new animation doesn't set keep their last values, as in Godot.
- Given the components, mistakes fail with the JSON Pointer of the bad value: keys out of order
  or past the length, an unknown component or field (with "did you mean"), a part a field doesn't
  have, a key value the field doesn't take, a string or boolean on a continuous track, two
  tracks on one path, or an event kind the engine uses itself (`command`, `entity.*`).
  `slash-cli scene validate` and exports check all of these, and in a scene or prefab, that
  each `AnimationPlayer`'s library loads and has its `animation` (with "did you mean"), that
  each animation `from` a model names one the model has, and that its `targets` name every target
  its tracks use. A player whose file, animation or track can't
  be used logs one warning and plays what it can.
- Animation files reload when they change, like every asset, and exports check and ship them.

### Node tracks

A node track moves one node of the model the playing entity's `MeshInstance3D` draws, a bone
or any other node, as a Godot `position_3d`, `rotation_3d` or `scale_3d` track moves a node or a
`Skeleton3D` bone. It plays as a model's own animations do: the player poses the model, an
[animation tree](#animation-trees) blends it with the model's animations and uses it for root
motion, and nodes no track moves keep their rests. Rotations are Euler angles in radians, as
`Transform3D`'s, and turn the shorter way round from key to key, as Godot slerps them.

```json
"wave": {
  "length": 1,
  "loop": "linear",
  "tracks": [
    {"node": "Elbow", "property": "rotation",
     "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 0.5, "value": [0, 0, 1.2]}]},
    {"node": "Hand", "property": "scale", "interpolation": "nearest",
     "keys": [{"time": 0, "value": [1, 1, 1]}, {"time": 0.5, "value": [1.2, 1.2, 1.2]}]}
  ]
}
```

- Node tracks have no `target`, `update` or key `transition`, and an animation `from` a model
  moves the model's nodes by its own tracks, so it has none. Two tracks may not move one node's
  same property.
- `scene validate` checks that an entity whose player plays node tracks draws a model, and that
  each node they name is one of its nodes (with "did you mean").
- [`animation.eval`](commands.md#animationeval) returns their values under `nodes`.

## Animation trees

A `.animtree.json` file is a tree of animation nodes, as Godot's `AnimationNodeBlendTree`,
`AnimationNodeStateMachine` and the nodes they hold: animations blended, added, faded in and out,
switched between and played as states. An entity's
[`AnimationTree`](#built-in-components) names the file in `tree_root` and the libraries its animations
come from in `libraries` (`.anim.json` files or models). Each tick the `animation_tree` system,
after `animation_player`, processes the tree with the entity's `parameters`, keeps what the nodes
need in `state`, and mixes the animations they play as Godot's `AnimationMixer` does: value
tracks blend by weight into the entity's (or a target's) components, event tracks emit their
events, and a model's animations pose the skeleton of the model its `MeshInstance3D` draws in
place of its `AnimationPlayer`'s. `slash-cli schema animtree` prints the schema,
`slash-cli scene validate` and `scene format` accept `.animtree.json` files, and the
[`animation.tree`](commands.md#animationtree) and
[`animation.set_parameters`](commands.md#animationset_parameters) commands list and set
parameters. [examples/animation-tree](../../examples/animation-tree/) points a waving arm.

```json
{
  "format": 1,
  "root": {
    "type": "BlendTree",
    "nodes": {
      "wave": {"type": "Animation", "animation": "Wave-loop"},
      "point": {"type": "Animation", "animation": "Point"},
      "speed": {"type": "TimeScale", "inputs": {"in": "wave"}},
      "shot": {"type": "OneShot", "inputs": {"in": "speed", "shot": "point"},
               "fadein_time": 0.2, "fadeout_time": 0.2,
               "filter_enabled": true, "filters": ["Shoulder"]}
    },
    "output": "shot"
  }
}
```

`root` is an `Animation`, a `BlendTree`, a blend space or a `StateMachine`. A `BlendTree` holds nodes by name and connects them by the names in each
node's `inputs`; `output` names the node whose result it gives. The node types are Godot's, with
its field names:

| Type | Inputs | Fields | Parameters |
| --- | --- | --- | --- |
| `Animation` | | `animation` (its name in the library `""`, or `"library/name"`), `play_mode`, `advance_on_start`, `use_custom_timeline` with `timeline_length`, `start_offset`, `loop_mode` and `stretch_time_scale` | |
| `BlendTree` | | `nodes`, `output` | its nodes' |
| `Blend2`, `Blend3` | `in`, `blend` / `in`, `-blend`, `+blend` | `sync`; filters on `Blend2` | `blend_amount` |
| `Add2`, `Add3` | `in`, `add` / `in`, `-add`, `+add` | `sync`, filters | `add_amount` |
| `Sub2` | `in`, `sub` | `sync`, filters | `sub_amount` |
| `OneShot` | `in`, `shot` | `fadein_time`, `fadeout_time`, their `_curve`s, `mix_mode` (`blend`, `add`), `break_loop_at_end`, `autorestart` with its delays, `sync`, filters | `request`: `"fire"`, `"abort"`, `"fade_out"` |
| `TimeScale` | `in` | | `scale` |
| `TimeSeek` | `in` | `explicit_elapse` | `seek_request`: seconds |
| `Transition` | a list of `{name, node, auto_advance, break_loop_at_end, reset}` | `xfade_time`, `xfade_curve`, `allow_transition_to_self`, `sync` | `transition_request`: an input's name |
| `BlendSpace1D`, `BlendSpace2D` | | `points` (`{position, node}`, each node an `Animation`, a `BlendTree`, a blend space or a `StateMachine`), `blend_mode` (`interpolated`, `discrete`, `discrete_carry`), `min_space`, `max_space`, `sync`; 2D: `auto_triangles` (Delaunay, as Godot's) or `triangles` | `blend_position`: a number, or `[x, y]` |
| `StateMachine` | | `states` (by name, `{node, position}`), `transitions`, `state_machine_type` (`root`, `nested`, `grouped`), `allow_transition_to_self`, `reset_ends` | `travel`, `start`: a state's name; `next`, `stop`: `true`; `conditions/<name>` |

Filters (`filter_enabled` and `filters`) name the tracks a node blends: a model node's name for
the tracks that move it, a value track's path, `@` and an event track's event, each after its
target and `:` if it has one (`"sword:Transform2D/rotation"`). The rest pass through, as Godot's
filters.

A `StateMachine` plays one of its `states` at a time, as Godot's `AnimationNodeStateMachine`, and
moves between them through its `transitions`:

```json
{
  "type": "StateMachine",
  "states": {
    "idle": {"node": {"type": "Animation", "animation": "Idle"}, "position": [0, 0]},
    "run": {"node": {"type": "Animation", "animation": "Run"}, "position": [200, 0]},
    "jump": {"node": {"type": "Animation", "animation": "Jump"}, "position": [100, -100]}
  },
  "transitions": [
    {"from": "Start", "to": "idle", "advance_mode": "auto"},
    {"from": "idle", "to": "run", "xfade_time": 0.2},
    {"from": "run", "to": "idle", "xfade_time": 0.2},
    {"from": "idle", "to": "jump", "advance_mode": "auto", "advance_condition": "jumping"},
    {"from": "jump", "to": "idle", "advance_mode": "auto", "switch_mode": "at_end",
     "xfade_time": 0.1}
  ]
}
```

- `Start` and `End` are the machine's own states and play nothing; an auto transition from
  `Start` picks the first state. In `states` they take only a `position`.
- A transition's `advance_mode` is `enabled` (the default: travels take it), `auto` (also taken
  on its own once its `advance_condition`, if it has one, is `true` in the parameter
  `conditions/<name>`, and its `advance_expression`, below, holds) or `disabled`. Of the auto transitions that can be taken, the lowest
  `priority` goes. `switch_mode` is `immediate`, `sync` (the next state goes on from where the
  last one was) or `at_end` (once the current state has `xfade_time` left; `break_loop_at_end`
  ends a loop). `xfade_time` and `xfade_curve` cross-fade the two states, and `reset` (the
  default) starts the next one over. Transitions taken one after another happen in one tick
  until one cross-fades.
- `travel` names a state to go to by the cheapest way through transitions that aren't disabled,
  as Godot's `travel()`: A* over the states' `position`s, each step costing its length times its
  `priority`, a direct transition first. With no way there, it jumps there. `start` jumps to a
  state and starts it over, `next` takes the next transition (a travel's next step or an auto
  transition) now, without waiting for `at_end`, and `stop` plays nothing for a tick and leaves
  the machine stopped, so the next `travel` sets out from `Start`; the tree sets them back once
  it acts on them. `state` keeps the machine's `playback` (`"move/playback"`): `current`,
  `fading_from` with the cross-fade's `fading_time` and `fading_pos`, the travel's `path`, and
  `playing`.
- `state_machine_type`: a `root` machine (the default) starts over when its parent seeks it to
  its start, as the tree's first tick and a Transition switching to it do; a `nested` one
  carries on, starting over only once it reached `End`; a `grouped` one is a state of another
  machine and plays as part of it. Its transitions from `Start` and to `End` stand for the
  parent's into and out of it, so there are as many of each, and a travel goes into it by a
  path, `"combat/shoot"`, through its `End` when it leaves.
- A state's node's parameters are named after the state: `"move/run/blend_position"`.
- An auto transition's `advance_expression` must also run to a value that counts as true, as
  Godot's: `"speed > 0.1 and not crouching"`. It reads the tree's `variables`, which the file
  declares at its top with their defaults (a number, `true`, `false` or `null`) and games set as
  parameters of their own (`"speed"`, beside the root's `travel`), as Godot's expressions read the
  properties of the tree's `advance_expression_base_node`. `triggers` names variables that start
  `false` and go back to `false` once a transition that reads them is taken on its own, as Unity's
  Animator triggers: set `jump` and the next transition whose expression holds uses it up.

  ```json
  {"format": 1, "variables": {"speed": 0.0}, "triggers": ["jump"], "root": {"type": "StateMachine",
   "states": {"...": {}},
   "transitions": [{"from": "run", "to": "jump", "advance_mode": "auto",
                    "advance_expression": "jump and speed < 4"}]}}
  ```

  Expressions are Godot's `Expression`: numbers, strings, `true`, `false`, `null`, `PI`, `TAU`,
  `INF` and `NAN`; the operators `**`, `~`, unary `-`, `* / %`, `+ -`, `<< >>`, `&`, `^`, `|`,
  comparisons, `not` (`!`), `and` (`&&`) and `or` (`||`), bound as Godot binds them; and the
  functions `abs`, `sign`, `min`, `max`, `clamp`, `floor`, `ceil`, `round`, `sqrt`, `pow`, `lerp`,
  `fmod`, `is_equal_approx`, `is_zero_approx`, `int`, `float` and `bool`. Values follow Godot's
  rules: whole numbers divide as integers, and comparing a number with `null` or a string fails.
  An expression that fails isn't taken. `validate` reports one that doesn't parse, with the
  character it stops at, and a name that isn't a variable or trigger, with "did you mean".
  [examples/animation-expressions](../../examples/animation-expressions/) jumps on a trigger.

- A parameter's name is its node's path in the tree and its own: `"shot/request"`,
  `"locomotion/walk/blend_amount"` for a node inside a blend tree inside the root's, and
  `"blend_position"` for a root blend space; the tree's variables and triggers go by their own
  names. Games set them in `parameters`; requests go back to
  their defaults once the tree acts on them. Everything else the nodes keep (times, fades, the
  current input, `shot/active`) is in `state`. A blend space's points' nodes are named by their
  index: `"space/0/..."`.
- The first tick after the tree starts (or `state` is emptied) seeks every node to its start, as
  Godot's tree does: nothing moves until the second tick. Animations at zero weight don't move on
  unless a node's `sync` keeps them playing.
- With `deterministic` (the default), every track any animation in the tree has is set each
  tick: tracks no playing animation moves blend back toward their rest values, the first key of
  the `RESET` animation in the library `""` (model nodes' rest poses for model animations). Off,
  only played tracks are set and their weights are scaled to add up to 1.
- Root motion: `root_motion_track` names a model node whose motion is taken out of the pose and
  reported each tick in `root_motion_position`, `root_motion_rotation` and `root_motion_scale`
  (the change) and the `_accumulator`s (where the animations put it), as Godot's, for gameplay
  code to move the entity by. `root_motion_local` turns the position into the node's own frame.
- An `Animation` node whose animation doesn't loop emits `animation_started` when it starts and
  `animation_finished` when it reaches its end, with `{"animation": "<name>"}`, as Godot's tree
  signals, before the events of event tracks.
- A tree that can't play, such as an animation the libraries lack or an input connected to
  nothing, plays nothing and says why in `invalid`, as Godot's `invalid_reasons`. A file that
  doesn't load logs one warning.
- `slash-cli scene validate` and exports check, in a scene or prefab, that each tree and library
  loads, that the tree's animations and filters' tracks are in its libraries (with "did you
  mean"), that `root_motion_track` is a node some animation moves, that `parameters` names
  parameters the tree has with values they take (a `travel` a state the machine has), that
  `targets` has every target the tracks
  name, and, for a deterministic tree, that every value track has a key in a `RESET`
  animation. Tree files reload when they change, and exports check and ship them.

## Tile sets

A `.tileset.json` file cuts atlas textures into tiles, names the terrains that connect them and
gives tiles collision polygons, occluders and custom data, like Godot's `TileSet` with
`TileSetAtlasSource`s. A [`TileMapLayer`](#tile-map-layers) draws
its tiles. `slash-cli schema tileset` prints the schema, `slash-cli scene validate` and
`scene format` accept `.tileset.json` files, and exports check and ship them.
[examples/tilemaps](../../examples/tilemaps/) has one with grass, water and the tiles between.

```json
{
  "format": 1,
  "tile_size": [16, 16],
  "terrain_sets": {"ground": {"terrains": ["grass", "water"]}},
  "physics_layers": [{"friction": 0.8}],
  "sources": {
    "ground": {
      "texture": "tiles/ground.png",
      "tiles": {
        "1,1": {"name": "grass", "terrain": "grass"},
        "1,0": {"name": "grass_n", "terrain": "grass",
                "peering": {"n": "water", "ne": "water", "nw": "water"}},
        "3,0": {"name": "water", "terrain": "water"},
        "3,2": {"name": "rock",
                "collision": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}],
                "alternatives": {"1": {"modulate": [1, 1, 1, 0.5]}}}
      }
    }
  }
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `tile_shape` | No | The cells' shape, as Godot's: `square` (default), `isometric` (diamonds), `half_offset_square` (bricks) or `hexagon`. See [tile shapes](#tile-shapes) |
| `tile_layout` | No | How cells of the shapes other than squares are numbered, as Godot's: `stacked` (default), `stacked_offset`, `stairs_right`, `stairs_down`, `diamond_right` or `diamond_down` |
| `tile_offset_axis` | No | Which way those shapes' rows run: `horizontal` (default), offset rows, or `vertical`, offset columns |
| `tile_size` | No | A cell's width and height in pixels, the box its shape fills; default `[16, 16]` |
| `terrain_sets` | No | Terrain sets by name |
| `terrain_sets/<name>/mode` | No | How tiles connect, as Godot's terrain modes: `match_corners_and_sides` (default) by their sides and corners, `match_corners` by their corners only, `match_sides` by their sides only |
| `terrain_sets/<name>/terrains` | Yes | Terrain names, unique in the file |
| `physics_layers` | No | Physics layers, as Godot's: each tile's collision polygons are on one, by index from 0 |
| `physics_layers/<n>/collision_layer` | No | The layer bits its polygons are on; default `1` |
| `physics_layers/<n>/collision_mask` | No | The layer bits its polygons collide with; default `1` |
| `physics_layers/<n>/friction`, `bounce` | No | Its physics material, as Godot's `PhysicsMaterial`: friction from 0 (default `1`) and bounce from 0 to 1 (default `0`) |
| `occlusion_layers` | No | Occlusion layers, as Godot's: each tile's [occluders](#tile-occluders-and-custom-data) are on one, by index from 0 |
| `occlusion_layers/<n>/light_mask` | No | Lights whose `shadow_item_cull_mask` shares a bit take shadows from its occluders; default `1` |
| `occlusion_layers/<n>/sdf_collision` | No | [GPU particles](#gpu-particle-collision) collide with its occluders; default `false` |
| `navigation_layers` | No | Navigation layers, as Godot's: each tile's [navigation outlines](#tile-navigation) are on one, by index from 0 |
| `navigation_layers/<n>/layers` | No | The `navigation_layers` bits of the regions its outlines make; default `1` |
| `custom_data_layers` | No | Values every tile has, as Godot's custom data layers, each with a `name` (unique in the file) and a `type`: `bool`, `int`, `float`, `string`, `vector2`, `vector2i` or `color` |
| `sources` | Yes | Atlas sources by name, at least one |
| `sources/<name>/texture` | Yes | Project path of a PNG, BMP or KTX2 file |
| `sources/<name>/normal_texture` | No | Project path of a normal map laid out as the texture, which [2D lights](#2d-lights) shade its tiles with, as a Godot `CanvasTexture`'s |
| `sources/<name>/specular_texture` | No | Project path of a specular map laid out as the texture: highlight color and, in alpha, shininess. Needs a `normal_texture` |
| `sources/<name>/specular_color` | No | Multiplies the specular map's color, as [r, g, b, a] from 0 to 1; default `[1, 1, 1, 1]` |
| `sources/<name>/specular_shininess` | No | Multiplies the specular map's alpha, from 0 to 1; default `1` |
| `sources/<name>/region_size` | No | Each tile's width and height in the texture; default `tile_size` |
| `sources/<name>/margins` | No | Pixels before the first column and row; default `[0, 0]` |
| `sources/<name>/separation` | No | Pixels between columns and rows; default `[0, 0]` |
| `sources/<name>/tiles` | No | Tiles by `"column,row"` in the texture's grid, as `"2,0"` |
| `tiles/<cell>/name` | No | What legends and `tilemap.set` call the tile: unique in the file, no `:`. A tile without one is `"source:column,row"` |
| `tiles/<cell>/terrain` | No | The terrain it paints |
| `tiles/<cell>/peering` | No | The terrain each side and corner must touch for the tile to fit, by compass direction: `n`, `ne`, `e`, `se`, `s`, `sw`, `w`, `nw` for squares, as Godot's terrain peering bits; the [shape](#tile-shapes) says which directions it has. A direction left out must have the tile's own terrain; `null` means none |
| `tiles/<cell>/probability` | No | How often painting picks the tile over others that fit as well; default `1`, and `0` only when nothing else fits |
| `tiles/<cell>/texture_origin` | No | Pixels to move its texture by, up and to the left, as Godot's `texture_origin`: `[0, 8]` draws it 8 pixels higher in its cell. It turns with the cell; collision polygons stay put. Default `[0, 0]` |
| `tiles/<cell>/size_in_atlas` | No | Its width and height in the texture's cells, as Godot's: `[2, 1]` is two cells wide. Default `[1, 1]`. See [animated and large tiles](#animated-and-large-tiles) |
| `tiles/<cell>/animation_frames` | No | Each frame's duration in seconds, above 0, 1 to 256 frames; default `[1]`, one frame |
| `tiles/<cell>/animation_columns` | No | Frames per row before they wrap to the next, 0 to 256; default `0`, all in one row |
| `tiles/<cell>/animation_separation` | No | Texture cells to skip between frames, across and down; default `[0, 0]` |
| `tiles/<cell>/animation_speed` | No | Multiplies the animation's speed, above 0; default `1` |
| `tiles/<cell>/animation_mode` | No | `default`, every cell in step, or `random_start_times`, each cell from its own point of the loop |
| `tiles/<cell>/collision` | No | Its [collision polygons](#tile-collision) |
| `tiles/<cell>/occlusion` | No | Its [occluders](#tile-occluders-and-custom-data) |
| `tiles/<cell>/navigation` | No | Its [navigation outlines](#tile-navigation) |
| `tiles/<cell>/custom_data` | No | Its [values](#tile-occluders-and-custom-data) by custom data layer name |
| `tiles/<cell>/alternatives` | No | [Alternative tiles](#alternative-tiles-and-turned-cells) by number, from `"1"` to `"4095"` |

A tile's pixels start at `margins + column × (region_size + separation)` on each axis, and
span `region_size × size_in_atlas + separation × (size_in_atlas - 1)`. Peering names only
directions the shape has and the terrain set's mode connects by, and only terrains of the tile's
own set. Mistakes fail with the JSON Pointer of the bad value and, for a misspelled terrain, the
closest one. Tile sets reload when they change, like every asset.

### Tile shapes

Cells are squares unless `tile_shape` says otherwise, and the other shapes follow Godot's
`TileSet` exactly: where each cell sits, which cells are its neighbors and which sides and
corners its peering names. [examples/isometric-and-hex-tiles](../../examples/isometric-and-hex-tiles/)
paints an island on diamonds and sets tiles on a board of hexagons.

| Shape | Sides | Corners | Cells overlap |
| --- | --- | --- | --- |
| `square` | `n`, `e`, `s`, `w` | `ne`, `se`, `sw`, `nw` | not at all |
| `isometric` | `ne`, `se`, `sw`, `nw` | `n`, `e`, `s`, `w` | by half a cell down each row (across each column with a vertical axis) |
| `hexagon`, offset rows | `ne`, `e`, `se`, `sw`, `w`, `nw` | `n`, `ene`, `ese`, `s`, `wsw`, `wnw` | by a quarter |
| `hexagon`, offset columns | `n`, `ne`, `se`, `s`, `sw`, `nw` | `nne`, `e`, `sse`, `ssw`, `w`, `nnw` | by a quarter |
| `half_offset_square` | as a hexagon | as a hexagon | not at all |

- `tile_layout` numbers the cells of the shapes other than squares, as Godot's layouts. In
  `stacked` every other row (or column, with a vertical `tile_offset_axis`) sits half a cell
  over, starting with row 1; `stacked_offset` starts with row 0. `stairs_right` and `stairs_down`
  step each row, or each column, half a cell over from the last. `diamond_right` and
  `diamond_down` turn the grid so that columns and rows both run diagonally; in `diamond_down`,
  the usual isometric layout, columns run down to the right and rows down to the left.
- A cell has a neighbor across each of its sides; a square or a diamond also has one across each
  corner, and a hexagon has none there. Painting terrain fits a side to the cell across it, and
  a corner to the cells around it, so a hexagon's corner follows the two sides beside it.
- [`tilemap.convert`](commands.md#tilemapconvert) turns cells into the points at their centers
  and points into the cells they are in, as Godot's `map_to_local` and `local_to_map`. The
  C++ calls are `canvas::map_to_local`, `canvas::local_to_map` and `canvas::neighbor_cell`.

### Tile map layers

A `TileMapLayer` holds its cells as text, so an agent reads a map in one call and a diff shows
the rows that changed:

```json
"ground": {
  "components": {
    "TileMapLayer": {
      "legend": {"G": "grass_n", "g": "grass", "r": "rock", "w": "water"},
      "origin": [0, -1],
      "rows": ["wwww", "wGGw", "wggr", "", "..ww"],
      "tile_set": "tiles/ground.tileset.json"
    }
  }
}
```

- Each legend key is one to three characters from letters, digits and punctuation other than
  `.`, `"` and backslash, all keys the same length. Each row is a string of keys for the cells
  from `origin` rightward, and `.` (or `..`, `...`) is an empty cell. Cells are from -32768 to
  32767 on each axis, as in Godot.
- Commands write the layer in canonical form: `origin` is the top left of the cells in use, rows
  end at their last tile, keys keep standing for the tiles they stood for, and new keys go to
  the most used tiles first, each the first letter of the tile's name where that is free. Keys
  are one character while a layer uses up to 91 tiles, then two, then three.
- [`tilemap.paint`](commands.md#tilemappaint) paints terrain as Godot's terrain painting does:
  each painted cell, and each terrain cell around them, gets the tile of its terrain whose
  peering fits its neighbors best, first where its own terrain stops and then which terrain lies
  beyond. Empty cells and terrains of other sets count as none, so painting in empty space draws
  edges. Among equal fits the cell's position picks, weighted by `probability`, so the same
  paint always gives the same tiles. [`tilemap.set`](commands.md#tilemapset) places tiles
  without changing their neighbors, and [`tilemap.get`](commands.md#tilemapget) reads the rows.
- A layer draws at its entity's `Transform2D` if it has one, each tile's region centered on its
  cell, less its `texture_origin` (a square cell `(c, r)` spans `c × tile_size` to `(c + 1) × tile_size`; other shapes are
  placed as [tile shapes](#tile-shapes) says). Cells draw down the screen by their centers, then
  left to right, as Godot draws a layer with Y-sort on, so a tile taller than its cell covers the
  cells behind it. Tiles from one texture draw together, so a layer whose tiles share an atlas
  is one GPU draw. Lights shade a source's tiles with its normal and specular maps,
  flipped and turned with each cell.
- `slash-cli scene validate` and exports check every layer's legend and rows against its tile
  set; a layer that takes part of itself from a prefab is checked in the prefab. A layer that
  can't draw, or a tile outside its texture, logs one warning and draws the rest.

### Tile collision

A tile's `collision` lists polygons in pixels around the tile's center (`[-8, -8]` is a 16-pixel
tile's top left), as Godot's tile physics polygons. A [`TileMapLayer`](#tile-map-layers) whose
tiles have them is one static body in [2D physics](#2d-physics), each cell's polygons placed at
its center. [examples/tile-collision](../../examples/tile-collision/) rolls, bounces and drops
balls on a level of tiles.

```json
"0,0": {"name": "ledge",
        "collision": [{"points": [[-8, -8], [8, -8], [8, -4], [-8, -4]], "one_way": true}]}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `collision/<n>/points` | Yes | The polygon's corners as `[x, y]`, at least three; a concave polygon is split into convex pieces |
| `collision/<n>/layer` | No | The physics layer it is on, by index; default `0` |
| `collision/<n>/one_way` | No | Stop only what comes from the tile's -y side (after the cell's turns), as a shape with `one_way_collision` |
| `collision/<n>/one_way_margin` | No | How deep in it, in pixels, something may be and still pass; default `1` |

- Polygons collide by their physics layer's `collision_layer` and `collision_mask`, as any
  body's shapes, with its friction and bounce. As in Godot, a tile set needs a physics layer
  before its tiles have polygons: a polygon on a physics layer the tile set doesn't have, with
  no area, or crossing itself fails validation.
- The layer's body follows its entity's `Transform2D`, and its parents'. Editing cells,
  `tile_set` or the tile set file rebuilds it, and `collision_enabled: false` removes it.
- [`physics.raycast`](commands.md#physicsraycast) reports the `cell` a ray hit beside the layer's
  `id`; `physics.overlap` and `physics.contacts` report the layer's ID. The debug overlay's
  collision shapes draw tile polygons.

### Alternative tiles and turned cells

A tile's `alternatives` are other versions of it, numbered from 1, as Godot's alternative tiles.
Each has its own flips, color, collision and texture origin, and a legend or `tilemap.set` names
one after the tile: `"rock:1"` or `"ground:3,2:1"`.

| Key | Required | Meaning |
| --- | --- | --- |
| `alternatives/<n>/flip_h`, `flip_v` | No | Draw it mirrored left to right, top to bottom |
| `alternatives/<n>/transpose` | No | Draw it with its axes swapped, before the flips |
| `alternatives/<n>/modulate` | No | Color it is multiplied by, as [r, g, b, a] from 0 to 1 |
| `alternatives/<n>/collision` | No | Its own collision polygons, as a tile's; an alternative without them has none |
| `alternatives/<n>/texture_origin` | No | Its own texture origin, as a tile's; `[0, 0]` if left out, whatever the tile's is |
| `alternatives/<n>/occlusion` | No | Its own occluders, as a tile's; an alternative without them has none |
| `alternatives/<n>/navigation` | No | Its own navigation outlines, as a tile's; an alternative without them has none |
| `alternatives/<n>/custom_data` | No | Its own custom data, as a tile's; a layer it leaves out has the type's default, whatever the tile's is |

A cell can also turn whatever tile it holds, as Godot's cell transform flags: after the tile, and
its alternative if any, `:h` flips it left to right, `:v` top to bottom and `:t` swaps its axes,
written in that order (`"slope:h"`, `"rock:1:hv"`, `"ground:3,2:vt"`). A quarter turn clockwise
is `:ht`, counterclockwise `:vt`. The tile draws turned, its collision polygons, occluders and
navigation polygons turn with it, and an alternative's own flips combine with the cell's. Commands give each turned or alternative tile
a legend key of its own.

### Animated and large tiles

A tile can span several of its texture's cells and play frames, as Godot's
`TileSetAtlasSource`:

```json
"0,0": {"name": "water", "animation_frames": [0.4, 0.4, 0.4]},
"0,1": {"name": "torch", "animation_frames": [0.25, 0.25, 0.25, 0.25],
        "animation_columns": 2, "animation_mode": "random_start_times"},
"3,0": {"name": "bridge", "size_in_atlas": [2, 1], "texture_origin": [-8, 0]}
```

- `size_in_atlas` makes a tile that many texture cells wide and high. It is still one cell of a
  layer, keyed by its top left texture cell, and draws centered on its layer cell, so a 2x1 tile
  covers half of each neighbor; a `texture_origin` of `[-8, 0]` (for 16-pixel cells) moves it to
  cover its cell and the next instead. Its collision polygons are its own, as any tile's.
- Frame `f` is the tile's texture cell plus `(f mod columns, f / columns)` steps of
  `size_in_atlas + animation_separation`, or `(f, 0)` steps with `animation_columns` `0`. The
  water above plays texture cells `0,0`, `1,0` and `2,0`; the torch `0,1`, `1,1`, `0,2` and
  `1,2`.
- A tile, its size and its frames cover texture cells no other tile does, as Godot's
  `has_room_for_tile`: a tile on another's frame fails at its key, and at most 65,536 cells a
  tile. Every frame must fit in the texture, or the layer reports it when it draws.
- Tiles play on game time: the loop lasts the frames' durations added up, over
  `animation_speed`. With `random_start_times`, each cell starts from its own point of the loop,
  picked from the layer's entity ID and the cell, the same on every run. Alternatives play
  their tile's frames. [`frame.capture`](commands.md#framecapture) draws any moment with `time`.

### Tile occluders and custom data

Tiles carry occluders that cast shadows and values that gameplay code reads, as Godot's tile
set occlusion and custom data layers.
[examples/tile-data-and-occluders](../../examples/tile-data-and-occluders/) lights a dungeon
whose walls cast shadows and whose lava does damage.

```json
"occlusion_layers": [{}],
"custom_data_layers": [{"name": "damage", "type": "int"}, {"name": "kind", "type": "string"}],
"sources": {"dungeon": {"texture": "dungeon.png", "tiles": {
  "1,0": {"name": "wall", "custom_data": {"kind": "wall"},
          "occlusion": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}]},
  "2,0": {"name": "lava", "custom_data": {"damage": 10, "kind": "lava"}}}}}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `occlusion/<n>/points` | Yes | The occluder's corners as `[x, y]` in pixels around the tile's center, at least two |
| `occlusion/<n>/layer` | No | The occlusion layer it is on, by index; default `0` |
| `occlusion/<n>/closed` | No | Join the last point to the first; default `true`, which needs three points or more |
| `occlusion/<n>/cull_mode` | No | `disabled` (default), `clockwise` or `counter_clockwise`, as a `LightOccluder2D`'s |
| `custom_data/<layer>` | No | The tile's value in that layer, of the layer's type: `vector2` and `vector2i` are `[x, y]`, `color` is `[r, g, b, a]` |

- Each cell's occluders sit at its center, moved by the layer's `Transform2D`, and cast shadows
  as a `LightOccluder2D` with the occlusion layer's `light_mask` as its `occluder_light_mask`.
  A layer casts none while it is hidden or its `occlusion_enabled` is `false`.
- A cell's turns turn its occluders. When they mirror it (an odd number of `h`, `v` and `t`),
  `clockwise` and `counter_clockwise` swap, so the same edges face the light. Godot's turned
  copies lose their cull mode; this keeps it.
- A tile's value in a layer it doesn't set is the type's default: `false`, `0`, `0.0`, `""`,
  `[0, 0]` or `[0, 0, 0, 1]`, as Godot's `get_custom_data` returns. An alternative's values are
  its own. [`tilemap.get`](commands.md#tilemapget) with `cells` returns each cell's tile and all
  its values; C++ asks `TileSet::custom_data(tile, "damage")`.
- An occluder on an occlusion layer the tile set doesn't have, a value of the wrong type and a
  value for a layer it doesn't have fail validation; a misspelled layer name gets the closest
  one.

### Tile navigation

Tiles carry navigation outlines, as Godot's tile set navigation layers: each cell of a
`TileMapLayer` is a region of the 2D [navigation](#navigation) map, so agents find paths over a
tile map with no navigation file of its own.
[examples/navigation-tiles](../../examples/navigation-tiles/) sends a mouse through a maze of
tiles and a duck across its pond.

```json
"navigation_layers": [{}, {"layers": 2}],
"sources": {"maze": {"texture": "maze.png", "tiles": {
  "0,0": {"name": "floor",
          "navigation": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}]},
  "1,0": {"name": "water",
          "navigation": [{"layer": 1, "points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]}]}}}}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `navigation/<n>/points` | Yes | The outline's corners as `[x, y]` in pixels around the tile's center: three or more, with area, not crossing itself |
| `navigation/<n>/layer` | No | The navigation layer it is on, by index; default `0` |

- A tile's outlines on each navigation layer bake into polygons the first time a cell needs
  them, as Godot's tile set editor bakes the outlines drawn in it: merged, with no agent radius.
- Each cell whose tile has polygons on a navigation layer is a region there, at the cell's
  center moved by the layer's `Transform2D`, with the navigation layer's `layers` as its
  `navigation_layers`, as Godot's `TileMapLayer` makes them. Regions join where their edges meet,
  so outlines that reach a tile's edges join the cells beside it.
- A cell's turns turn its polygons, and those a turn mirrors are reversed so they wind as
  before, as Godot's turned copies are.
- Navigation layers only filter paths: polygons that share an edge join whatever their layers,
  and an edge three polygons share joins none, as in Godot. A tile with outlines on two layers
  that reach the same side of its cell can cut paths off there, so keep each cell to one
  layer's polygons where it meets its neighbors.
- Paths and [`navigation.closest`](commands.md#navigationclosest) name the layer's entity as a
  cell's owner, and [`navigation.map`](commands.md#navigationmap) counts each cell's region.
  The map is rebuilt when a layer's cells, tile set or transform change, not each tick.
- A layer whose `navigation_enabled` is `false` has no regions. A tile set that doesn't load
  leaves its layers out, and `navigation.map` reports why under `errors`.
- [Baking](#baking) a region takes the layers under it: their tiles' outlines are where agents
  may walk and their collision polygons are cut out.
- An outline on a navigation layer the tile set doesn't have, or with no area, fails
  validation.

## 2D camera

A `Camera2D` chooses the part of the world the window shows, as Godot's. Without one the window
shows world pixels from (0, 0) at its top left corner. With one, the view is seen from the
camera's entity: put the camera in the player's `Transform2D` `children` and the view follows
the player. [examples/camera-2d](../../examples/camera-2d/) follows a player through a level
wider than the window.

```json
"cam": {
  "components": {
    "Camera2D": {"drag_horizontal_enabled": true, "limit_left": 0, "limit_right": 1600,
                 "position_smoothing_enabled": true},
    "Transform2D": {"position": [0.0, -32.0]}
  }
}
```

- The current camera is the first enabled `Camera2D` with a `Transform2D`, in drawing order
  (`z_index`, then parents before children, then the order entities were made). The others do
  nothing until it is disabled or removed.
- `anchor_mode` `drag_center` puts the camera's position in the middle of the window, and
  `fixed_top_left` at its top left corner. `zoom` scales the view around that point, `offset`
  moves it, and with `ignore_rotation` off the view turns with the entity.
- The limits keep the view inside a rectangle of the world. A level narrower than the view is
  centered in it.
- With `drag_horizontal_enabled` or `drag_vertical_enabled`, the camera holds still while its
  entity moves inside the drag margins (a share of half the window on each side of the middle)
  and follows once it leaves them. A drag offset puts the camera at that point of the margins
  at once.
- Position smoothing glides the view after its entity, closing `position_smoothing_speed` times
  each tick's seconds of the gap; it stops at the limits unless `limit_smoothed` glides it there
  too. Rotation smoothing does the same for the angle.
- The `camera_2d` system (priority 1050, after physics) moves the drag margins and smoothing on
  each tick, for the current camera only. An edit to a camera or its entity shows in the next
  frame without waiting for a tick, and games draw between ticks with the view in between.
- Sprites, tile layers, particles, lights and shadows and the debug overlay's collision shapes
  are drawn through the camera; controls stay where they are on the screen, as if on Godot's
  default `CanvasLayer`, and what a [`CanvasLayer`](#canvas-layers-and-parallax) holds is placed
  by the layer. 2D sounds with no `AudioListener2D` are heard from the middle of the view.
- [`frame.summary`](commands.md#framesummary) reports rects on the screen, and its `camera` field
  names the current camera and the world rect it shows.
  [`canvas.convert`](commands.md#canvasconvert) turns window pixels into the world through the
  camera, and back.

## Canvas layers and parallax

A `CanvasLayer` draws the entities its `children` list in a canvas of its own, as Godot's: a HUD
that stays put while the camera moves, or a background behind the world. A `Parallax2D` scrolls
its entity with the camera, slower or faster than the world, and can draw what it holds again
and again to fill the window. [examples/canvas-layers](../../examples/canvas-layers/) has a
parallax background and a HUD.

```json
"hud": {"components": {"CanvasLayer": {"children": ["heart", "score"]}}},
"far": {
  "components": {
    "Parallax2D": {"repeat_size": [320.0, 0.0], "scroll_scale": [0.25, 1.0]},
    "Transform2D": {"children": ["hills"]}
  }
}
```

| `CanvasLayer` field | Default | Meaning |
| --- | --- | --- |
| `layer` | `1` | Its place in the drawing order: higher draws over lower; the world is 0 |
| `visible` | `true` | Hides everything in it when false |
| `offset` | `[0, 0]` | Moves it, in pixels |
| `rotation` | `0` | Turns it around its top left, in radians |
| `scale` | `[1, 1]` | Scales it from its top left |
| `follow_viewport_enabled` | `false` | Seen through the camera too, after its own offset, rotation and scale |
| `follow_viewport_scale` | `1` | Following the camera, scales it around the middle of the window |
| `children` | `[]` | The entities that draw in it, with their own children |

- Canvases draw in order of `layer`, lowest first. The world's canvas is layer 0 and draws
  before a `CanvasLayer` of layer 0, and layers of one layer draw in the order their entities
  were made. Within a canvas, things draw as in the world: by `z_index`, then parents before
  children.
- `children` lists the 2D entities and controls that draw in the layer, and their own
  `Transform2D` and `Control` children draw there too. A `CanvasLayer` has no `Transform2D` of
  its own. An entity in a layer can't also be another entity's child, nor be in two layers
  (the first made wins), and layers don't hold each other; `scene validate` reports each at
  the JSON Pointer of the list item. `entity.destroy` on a layer takes its children with it.
- What a layer holds is placed by its `offset`, `rotation` and `scale`, not by the camera. With
  `follow_viewport_enabled` the camera moves it after them, as it does the world, and
  `follow_viewport_scale` then scales it around the middle of the window, so a layer at 0.5
  looks farther away than the world.
- Lights shade the canvases their `range_layer_min` to `range_layer_max` holds, the world's
  (layer 0) by default, and a `CanvasModulate` tints the canvas it is in. A layer that no light
  reaches and that holds no `CanvasModulate` draws unlit, as in Godot. Lights and occluders in a
  layer are placed in its canvas, and shadows fall across every canvas a light reaches.
- Controls in a layer lay out in the window as other controls do, then the layer places them;
  they take clicks and touches where they show. Controls in no layer stay on the screen, above
  the world's sprites. Popup menus, tooltips and drag previews draw over every canvas.
- [`frame.summary`](commands.md#framesummary) and [`ui.layout`](commands.md#uilayout) name the
  layer each thing is in as `layer`, and [`canvas.convert`](commands.md#canvasconvert) with an
  `id` turns points into and out of the canvas that entity draws in.

| `Parallax2D` field | Default | Meaning |
| --- | --- | --- |
| `scroll_scale` | `[1, 1]` | How far it moves on the screen for each pixel the camera moves: 0 stays put, 1 moves with the world, 0.25 looks far away |
| `scroll_offset` | `[0, 0]` | Where it sits when the view's top left is at `[0, 0]` |
| `repeat_size` | `[0, 0]` | Draws what it holds again every so many pixels, along each axis that isn't 0 |
| `repeat_times` | `1` | Copies drawn past the first on each repeating axis, half of them (rounded down) before it, for a camera zoomed out |
| `autoscroll` | `[0, 0]` | Pixels a second `scroll_offset` moves by, wrapped within the repeat |
| `limit_begin`, `limit_end` | `[-10000000, -10000000]`, `[10000000, 10000000]` | The views it scrolls for: past them it stops |
| `follow_viewport` | `true` | Off for one in a `CanvasLayer` that doesn't follow the camera |
| `ignore_camera_scroll` | `false` | Leaves `screen_offset` alone, for code or an animation to set |
| `screen_offset` | `[0, 0]` | The top left of the current camera's view in the world, set each tick |

- The `parallax_2d` system (priority 1060, right after `camera_2d`) sets each `Parallax2D`'s
  `Transform2D` `position` each tick, as Godot's `Parallax2D::_update_scroll`: it takes
  `screen_offset` from the current camera (none leaves it as it is), keeps it inside the limits
  less a window, scales it by `scroll_scale` and adds `scroll_offset`. Set `scroll_offset`, not
  `position`, to place one.
- With a `repeat_size` it wraps its position within one repeat and draws everything it holds
  again beside it, so a picture as wide as the window always fills it. Something drawn between
  ticks jumps where its repeat wrapped instead of sliding back across the window.
- `autoscroll` drifts only a `Parallax2D` with a `repeat_size`, as in Godot.

## 2D lights

Lights follow Godot's 2D lighting. Draws in a [canvas](#canvas-layers-and-parallax) are unshaded
until an enabled light's layer range holds it or it has a `CanvasModulate`. Then each lit pixel
of a sprite or tile layer starts from the color its material made, times its canvas's
`CanvasModulate` color (the ambient light, white if there is none), and each light whose
`range_item_cull_mask` shares a bit with the draw's `light_mask`, whose z range holds its
`z_index` and whose layer range holds its canvas's layer, adds its color times that pixel
color, subtracts it or mixes toward it. [examples/lights-2d](../../examples/lights-2d/) has a torch, the moon, a shadow and a normal
map.

```json
"torch": {
  "components": {
    "PointLight2D": {"color": [1.0, 0.75, 0.45, 1.0], "energy": 1.4, "shadow_enabled": true},
    "Transform2D": {"position": [96.0, 90.0]}
  }
}
```

- A `PointLight2D`'s texture is centered on it (plus `offset`), `texture_scale` times its size
  and turned and scaled by its `Transform2D`; it reaches only where the texture does. The light
  adds `color` × `energy` × the texture's color and alpha.
- A `DirectionalLight2D` reaches everywhere. At rotation 0 its light travels down the screen;
  positive rotations turn it clockwise.
- With a `normal_texture` (a sprite's, or a tile set source's), a pixel takes a light by the
  angle between its normal and the light: for a point light, toward its position raised
  `height` pixels; for a directional light, its direction tipped `height` of the way toward the
  viewer. A `specular_texture` adds Blinn-Phong highlights. Without a normal map, a pixel faces
  the viewer and takes each light whole.
- `LightOccluder2D` polygons, and tiles' [occluders](#tile-occluders-and-custom-data), cast
  shadows from lights with `shadow_enabled`, when its `occluder_light_mask` shares a bit with
  the light's `shadow_item_cull_mask`. Shadows begin at
  the first edge a ray from the light crosses, so an occluder with `cull_mode` `disabled`
  shadows its own sprite. For a polygon wound clockwise on screen, `counter_clockwise` leaves
  out the edges facing the light, so the shadow starts at its far side and its sprite is lit.
  A directional light's shadows reach `max_distance` pixels past their occluders.
- A frame takes up to 64 lights, in the order their entities were made; more, a light whose
  texture doesn't load, or a second `CanvasModulate` in a canvas is left out with one warning in
  the log.
- A material's `light_mode` opts a sprite out of lighting (`unshaded`, for glowing signs and UI)
  or shows it only where lights reach (`light_only`).
- [`light.sample`](commands.md#lightsample) computes the color a white pixel gets at points, or
  at an entity in its canvas, and which lights reach them, the way the GPU does, so an agent
  checks lighting without a screenshot.
  [examples/lit-tiles-and-layers](../../examples/lit-tiles-and-layers/) lights a tile set's
  normal map and a HUD layer.

## 2D physics

2D physics follows Godot's, on [Box2D](https://box2d.org) v3. An entity is a body or an area by
having one of `StaticBody2D`, `AnimatableBody2D`, `RigidBody2D`, `CharacterBody2D` or `Area2D`,
placed by its `Transform2D`, and takes its shapes from its `CollisionShape2D` and
`CollisionPolygon2D`, then from its [children's](#parent-transforms) that have no body or area
of their own, as Godot's shape nodes. Lengths are pixels and y points down. The `physics2d`
system steps the world each tick at priority 950, after gameplay systems and keyframe
animation; gravity and damping come from `physics_2d` in [`project.json`](project-format.md).
[examples/physics-2d](../../examples/physics-2d/) has every kind of body, an area and a joint.

```json
"hero": {
  "components": {
    "CharacterBody2D": {"collision_layer": 2},
    "CollisionShape2D": {"height": 32.0, "radius": 8.0, "shape": "capsule"},
    "Transform2D": {"position": [40.0, 200.0]}
  }
}
```

- A `StaticBody2D` doesn't move on its own; changing its `Transform2D` moves it there at once.
  An `AnimatableBody2D` goes where its `Transform2D` says, set by gameplay code or an
  `AnimationPlayer`; with `sync_to_physics` it moves there during the step, so it pushes bodies
  in its way and carries those standing on it.
- A `RigidBody2D` is moved by gravity, collisions, forces and joints, and the step writes back
  its `Transform2D`, `linear_velocity`, `angular_velocity` and `sleeping`. Setting those moves
  or wakes it. `physics2d::apply_impulse` and `apply_torque_impulse` push it at the next step.
- A `CharacterBody2D` moves as Godot's `move_and_slide`, each tick: gameplay code sets its
  `velocity`, then the step moves it, slides it along what it hits up to `max_slides` times,
  snaps it to the floor within `floor_snap_length` and writes `on_floor`, `on_wall`,
  `on_ceiling`, the normals and the velocity it ended with. Standing on a moving body carries
  it along. It moves as its `CollisionShape2D`, or its `CollisionPolygon2D`'s convex hull; a
  polygon's corners are rounded by a pixel.
- An `Area2D` collides with nothing and detects the bodies and areas in its `collision_mask`
  (an area only if it is `monitorable`), emitting `body_entered`, `body_exited`,
  `area_entered` and `area_exited` events with the other entity's ID in `body` or `area`. A
  `RigidBody2D` with `contact_monitor` emits `body_entered` and `body_exited` the same way.
  Events report each pair once and stay quiet when [`undo`](commands.md#undo) or
  [`world.restore`](commands.md#worldrestore) brings bodies back.
- Two bodies collide when either one's `collision_mask` has a bit of the other's
  `collision_layer`, as in Godot. Joints with `disable_collision` stop their two bodies
  colliding with each other.
- A shape with `one_way_collision` stops only what comes from its -y side, after the shape's
  and the entity's rotation, and lets through what is deeper in it than
  `one_way_collision_margin` pixels: platforms one jumps up through.
- A `CollisionShape2D`'s size is scaled by the entity's `Transform2D` scale; a circle or capsule
  radius takes the larger of the two. A `polygon` shape is its points' convex hull. A
  `CollisionPolygon2D` takes any simple polygon, split into convex pieces, or only its outline
  with `build_mode` `segments`. A shape with no area, or a polygon that crosses itself, is left
  out with one warning in the log.
- A joint joins `node_a` and `node_b` at its own entity's `Transform2D`, as Godot's joints are
  placed by their node; a `null` `node_a` joins `node_b` to the world. It is made where the
  entity is once both bodies exist, and stays there when a parent carries the entity away, as
  Godot's; changing the entity's own `Transform2D` makes it again there.
- Results are the same on every platform for the same scene and ticks, so tests and replays
  can check exact positions.
- [`physics.raycast`](commands.md#physicsraycast), [`physics.overlap`](commands.md#physicsoverlap)
  and [`physics.contacts`](commands.md#physicscontacts) answer what is where and what touches
  what; C++ code calls `physics2d::raycast`, `overlap_point`, `overlap_shape` and `contacts`.

### Character options

`CharacterBody2D`'s options are Godot's, and `move_and_slide` follows Godot 4's step for step:

- `floor_constant_speed` keeps its speed along a slope the same as on flat ground; without it
  the slope slows it going up and speeds it going down.
- `floor_block_on_wall` stops it climbing a slope steeper than `floor_max_angle` by walking
  into it. It is on by default, as in Godot.
- `wall_min_slide_angle`, in `floating` mode, stops it against a wall it hits nearly head-on
  instead of sliding along it.
- `safe_margin` is how close something must be to count as touching: floors, walls and
  one-way shapes within it are found, and it keeps that much room from them.
- `platform_floor_layers` and `platform_wall_layers` say which moving bodies carry it, by
  their `collision_layer`: floors in any layer by default, walls in none.

### Gravity and damping zones

An `Area2D` with a space override changes the gravity and damping of the bodies in its
`collision_mask` while they overlap it, whether or not it is `monitoring`, as Godot's areas.
Areas apply from the highest `priority` down, then the project's gravity and damping fill in
what no area stopped:

| Override | Its value | Then |
| --- | --- | --- |
| `combine` | Added to what higher areas gave | Lower areas and the project's add theirs |
| `combine_replace` | Added to what higher areas gave | Lower areas and the project's are ignored |
| `replace` | Replaces what higher areas gave | Lower areas and the project's are ignored |
| `replace_combine` | Replaces what higher areas gave | Lower areas and the project's add theirs |

```json
"planet_pull": {
  "components": {
    "Area2D": {"gravity": 600.0, "gravity_point": true, "gravity_point_center": [0.0, 0.0],
               "gravity_point_unit_distance": 30.0, "gravity_space_override": "replace",
               "priority": 1},
    "CollisionShape2D": {"radius": 80.0, "shape": "circle"},
    "Transform2D": {"position": [370.0, 100.0]}
  }
}
```

- Gravity pulls along `gravity_direction`, or with `gravity_point` toward
  `gravity_point_center` in the area's space, measured from each body's origin. With
  `gravity_point_unit_distance` the pull is `gravity` at that distance and weakens with
  distance squared, as a planet's.
- A `RigidBody2D` multiplies the gravity by its `gravity_scale`, and adds its own
  `linear_damp` and `angular_damp` or replaces the zones' with them, as its damp modes say.
- Overrides take effect from the step after a body enters, as in Godot, so on its first step
  every body falls with the project's gravity. A resting body wakes when its zone changes.
- Characters aren't pulled, since gameplay code sets their velocity: as in Godot, the code
  adds the gravity `physics2d::gravity` returns, as Godot's `get_gravity`.
- [`physics.gravity`](commands.md#physicsgravity) returns each body's gravity, damping and the
  areas that set them.

A `StaticBody2D` with `constant_linear_velocity` or `constant_angular_velocity` stays where it
is, but what touches it moves as if it moved: rigid bodies are carried by friction and
characters standing on it by `platform_velocity`. A conveyor belt is a static box with a
constant linear velocity. An `AnimatableBody2D` adds its constant velocities to how it moves.
[examples/gravity-and-conveyors](../../examples/gravity-and-conveyors/) has a belt, a planet and
a zone with no gravity.

## 3D physics

3D physics follows Godot 4.4's Jolt module, on [Jolt Physics](https://github.com/jrouwe/JoltPhysics).
An entity is a body or an area by having one of `StaticBody3D`, `AnimatableBody3D`,
`RigidBody3D`, `CharacterBody3D` or `Area3D`, placed by its `Transform3D`, and takes its shapes
from its `CollisionShape3D`, then from its [children's](#parent-transforms) that have no body or
area of their own, as Godot's shape nodes. Lengths are meters and y points up. The `physics3d`
system steps the world each tick at priority 950, beside 2D physics; gravity and damping come
from `physics_3d` in [`project.json`](project-format.md).
[examples/physics-3d](../../examples/physics-3d/) has every kind of body and an area.

```json
"hero": {
  "components": {
    "CharacterBody3D": {"collision_layer": 2},
    "CollisionShape3D": {"height": 1.8, "radius": 0.4, "shape": "capsule"},
    "Transform3D": {"position": [0.0, 1.0, 0.0]}
  }
}
```

| Component | Fields (defaults) |
| --- | --- |
| `CollisionShape3D` | `shape` `"box"` (or `sphere`, `capsule`, `cylinder`, `convex_polygon`, `concave_polygon`, `world_boundary`), `size` `[1, 1, 1]`, `radius` `0.5`, `height` `2` (a capsule's end to end), `points` `[]` (a convex polygon's hull), `faces` `[]` (a concave polygon's triangles, three points each), `backface_collision` `false`, `plane` `[0, 1, 0, 0]` (a world boundary's normal and distance; below it is solid), `margin` `0.04`, `position` `[0, 0, 0]`, `rotation` `[0, 0, 0]`, `disabled` `false` |
| `StaticBody3D` | `collision_layer` `1`, `collision_mask` `1`, `friction` `1`, `bounce` `0`, `constant_linear_velocity` `[0, 0, 0]`, `constant_angular_velocity` `[0, 0, 0]` |
| `AnimatableBody3D` | as `StaticBody3D`, and `sync_to_physics` `true` |
| `RigidBody3D` | `collision_layer`, `collision_mask`, `friction`, `bounce` as `StaticBody3D`; `mass` `1`, `inertia` `[0, 0, 0]` (0 computes that axis's), `gravity_scale` `1`, `linear_velocity` and `angular_velocity` `[0, 0, 0]`, `linear_damp_mode` and `angular_damp_mode` `"combine"`, `linear_damp` and `angular_damp` `0`, `constant_force` and `constant_torque` `[0, 0, 0]`, `lock_rotation` `false`, `axis_lock_linear_x/y/z` and `axis_lock_angular_x/y/z` `false`, `freeze` `false`, `freeze_mode` `"static"`, `continuous_cd` `false`, `can_sleep` `true`, `sleeping` `false`, `contact_monitor` `false` |
| `CharacterBody3D` | `collision_layer`, `collision_mask`; `motion_mode` `"grounded"` (or `floating`), `up_direction` `[0, 1, 0]`, `velocity` `[0, 0, 0]`, `max_slides` `6`, `slide_on_ceiling` `true`, `wall_min_slide_angle` `0.261799`, `floor_stop_on_slope` `true`, `floor_constant_speed` `false`, `floor_block_on_wall` `true`, `floor_max_angle` `0.785398`, `floor_snap_length` `0.1`, `platform_on_leave` `"add_velocity"`, `platform_floor_layers` all, `platform_wall_layers` `0`, `safe_margin` `0.001`; written by each move: `on_floor`, `on_wall`, `on_ceiling`, `floor_normal`, `wall_normal`, `platform_velocity` |
| `Area3D` | `collision_layer`, `collision_mask`, `monitoring` `true`, `monitorable` `true`, `priority` `0`, `gravity_space_override` `"disabled"`, `gravity_point` `false`, `gravity_point_unit_distance` `0`, `gravity_point_center` `[0, -1, 0]`, `gravity_direction` `[0, -1, 0]`, `gravity` `9.8`, `linear_damp_space_override` and `angular_damp_space_override` `"disabled"`, `linear_damp` and `angular_damp` `0.1`; for [3D sound](#3d-sound): `audio_bus_override` `false`, `audio_bus_name` `"Master"`, `reverb_bus_enabled` `false`, `reverb_bus_name` `"Master"`, `reverb_bus_amount` `0`, `reverb_bus_uniformity` `0` |

- Bodies act as their 2D namesakes do ([2D physics](#2d-physics)): a static body stays put and
  jumps where its `Transform3D` is set, an animatable body moves there during the step and
  pushes what is in its way, the step writes a rigid body's `Transform3D`, velocities and
  `sleeping` back, and setting them moves or wakes it.
- A `CharacterBody3D` moves as Godot's `move_and_slide`, ported line for line onto Godot's Jolt
  motion tests: gameplay code sets its `velocity`, then the step moves it, slides it along what
  it hits, snaps it to the floor and writes what it found. Its options are Godot's, as the 2D
  [character options](#character-options) describe. As Godot's, it stops at rigid bodies rather
  than pushing them; gameplay code pushes them with `physics3d::apply_impulse`.
- Shapes are built as Godot's Jolt module builds them: a box's, cylinder's and convex polygon's
  `margin` rounds no more than 8% of its shortest half extent, a capsule shorter than twice its
  radius is an error, a concave polygon is solid from the side its triangles wind clockwise
  from, as Godot's, and from behind too with `backface_collision`, and a world boundary reaches 1000 meters
  each way. A shape that can't be made, such as a hull of two points, is left out with a
  warning. The entity's `Transform3D` scale scales its shapes; a sphere takes one scale.
- An `Area3D` collides with nothing and detects the bodies and areas in its `collision_mask`,
  emitting the 2D events. It leaves out static bodies unless `physics_3d`'s
  `areas_detect_static_bodies` is on, as Godot's Jolt module does. Space overrides change the
  gravity and damping of rigid bodies in it by `priority`, as the 2D
  [zones](#gravity-and-damping-zones) do, with gravity in meters per second squared.
- Static and animatable bodies' constant velocities carry what touches them, as in 2D.
- Jolt is built cross-platform deterministic and steps on one thread, and bodies are added and
  changed in entity order, so results are the same on every platform for the same scene and
  ticks.
- [`physics.raycast`](commands.md#physicsraycast), [`physics.overlap`](commands.md#physicsoverlap),
  [`physics.contacts`](commands.md#physicscontacts) and [`physics.gravity`](commands.md#physicsgravity)
  ask 3D physics when given three-number points, a 3D shape or a 3D body; C++ code calls
  `physics3d::raycast`, `overlap_point`, `overlap_shape`, `contacts` and `gravity`.

### 3D joints

A joint is an entity with a joint component and a `Transform3D`, as Godot's `Joint3D` nodes on
its Jolt module. `node_a` and `node_b` name the bodies it holds (any body but an area); a missing
one is the world, and with only one named the joint holds it to the world. The joint is made
where its `Transform3D` puts it, after its parents', in its own axes, once both bodies exist.
[examples/joints-and-vehicles](../../examples/joints-and-vehicles/) has one of each but the
6DOF joint.

```json
"door_hinge": {
  "components": {
    "HingeJoint3D": {"angular_limit_enabled": true, "angular_limit_lower": 0.0,
                     "angular_limit_upper": 1.5707964, "node_a": "door"},
    "Transform3D": {"position": [-0.5, 1.05, 4.0], "rotation": [-1.5707964, 0.0, 0.0]}
  }
}
```

| Component | Fields (defaults) |
| --- | --- |
| `PinJoint3D` | `node_a`, `node_b`, `exclude_nodes_from_collision` `true`; the bodies turn every way about the joint's point |
| `HingeJoint3D` | as `PinJoint3D`, turning about the joint's z axis; `angular_limit_enabled` `false`, `angular_limit_upper` `1.5708`, `angular_limit_lower` `-1.5708` (radians), `motor_enabled` `false`, `motor_target_velocity` `1` (radians per second), `motor_max_impulse` `1` (per tick) |
| `SliderJoint3D` | as `PinJoint3D`, sliding along the joint's x axis without turning; `linear_limit_upper` `1`, `linear_limit_lower` `-1` (meters; limits that cross leave it free) |
| `ConeTwistJoint3D` | as `PinJoint3D`, swinging in a cone around the joint's x axis and twisting about it; `swing_span` `0.785398`, `twist_span` `3.14159` (radians; out of range leaves that way free) |
| `Generic6DOFJoint3D` | as `PinJoint3D`; `linear_x`, `linear_y`, `linear_z` and `angular_x`, `angular_y`, `angular_z`, each with `limit_enabled` `true`, `limit_upper` and `limit_lower` `0`, `motor_enabled` `false`, `motor_target_velocity` `0`, `motor_force_limit` (`0` linear, `300` angular), `spring_enabled` `false`, `spring_stiffness`, `spring_damping`, `spring_equilibrium_point`; every axis is locked by default |

- Limits and motors keep Godot's signs and are centered as its Jolt module centers them. A
  slider's position is how far `node_b` has moved along the joint's x axis from `node_a`, so a
  drawer that is its cabinet's `node_b` opens toward its upper limit.
- While `exclude_nodes_from_collision` is on, the two bodies don't collide with each other.
- Changing a joint's own `Transform3D` or bodies makes it again where it now is; a parent
  carrying it away leaves it where it was made. Changing a limit, motor or spring rebuilds it in
  place. Either way, and when the joint goes, its bodies wake.
- Parameters Godot's Jolt module ignores (bias, softness, relaxation and the like) are left out.

### Vehicles

A `VehicleBody3D` on an entity with a `RigidBody3D` makes it a car, as Godot's: it rolls on the
`VehicleWheel3D` entities among its `Transform3D` [children](#parent-transforms). Godot's
`VehicleBody3D` is a rigid body of 40 kilograms; give the `RigidBody3D` that `mass` for Godot's
feel.

| Component | Fields (defaults) |
| --- | --- |
| `VehicleBody3D` | `engine_force` `0` (newtons, to the wheels `use_as_traction`), `brake` `0` (to every wheel), `steering` `0` (radians, to the wheels `use_as_steering`; positive turns left) |
| `VehicleWheel3D` | `engine_force`, `brake`, `steering` `0`, `use_as_traction` and `use_as_steering` `false`, `wheel_roll_influence` `0.1`, `wheel_radius` `0.5`, `wheel_rest_length` `0.15`, `wheel_friction_slip` `10.5`, `suspension_travel` `0.2`, `suspension_stiffness` `5.88`, `suspension_max_force` `6000`, `damping_compression` `0.83`, `damping_relaxation` `0.88`; written by each step: `in_contact`, `contact_point`, `contact_normal`, `contact_body`, `suspension_length`, `rotation`, `rpm`, `skid_info` |

- Each step runs Godot's `VehicleBody3D` code, ported line for line: a wheel casts a ray down
  its y axis from where it is mounted, its spring and dampers push the body up, and friction
  along and across its axle (its x axis, turned by `steering`) drives, brakes and steers it.
  Positive `engine_force` drives toward +z.
- The car's `engine_force`, `brake` and `steering` go to its wheels when they change, so a
  wheel's own can be set between changes. A change of input wakes a car that has fallen asleep.
- The step moves each wheel's `Transform3D` with its suspension, steering and roll. Setting a
  wheel's `Transform3D` by hand mounts it there.
- Godot's suspension defaults are soft for a car of four wheels resting on them; stiffer springs
  (`suspension_stiffness` around 30) hold it up.

### Ragdolls

Ragdolls follow Godot 4.6's `PhysicalBoneSimulator3D` and `PhysicalBone3D`. A
`PhysicalBoneSimulator3D` is a [skeleton modifier](#ik-modifiers): it goes on the entity that
draws a model or on one of its `Transform3D` children, and runs in its place among the other
modifiers. Each entity in the simulator's entity's `Transform3D` `children` with a
`PhysicalBone3D` is a body for the bone `bone_name` names, shaped by its `CollisionShape3D` as
any body is. [examples/ragdoll](../../examples/ragdoll/) lets a waving arm go limp.

```json
"ragdoll": {"components": {
  "Transform3D": {"children": ["upper_arm", "forearm"]},
  "PhysicalBoneSimulator3D": {"simulated_bones": ["Elbow"]}
}},
"forearm": {"components": {
  "Transform3D": {},
  "PhysicalBone3D": {"bone_name": "Elbow", "body_offset_position": [0, 0.25, 0],
                     "joint_type": "cone", "joint_rotation": [0, 0, 1.5707964],
                     "swing_span": 2.4},
  "CollisionShape3D": {"shape": "capsule", "radius": 0.05, "height": 0.3}
}}
```

- A body sits at its bone times its body offset (`body_offset_position` and
  `body_offset_rotation`, in the bone's space), so an offset half way along a bone puts the body
  over it.
- While the simulator is `active` and not `simulating`, the `skeleton_modifiers` system moves
  each body to its bone every tick and physics moves it there as a kinematic body, so it pushes
  what is in its way and other bodies land on it.
- `simulating` (Godot's `physical_bones_start_simulation`) hands the bodies to physics as rigid
  bodies, `top_level` so their parents no longer carry them, keeping the velocity they had; each
  simulating bone then goes where its body is, after the modifiers before it and blended by
  `influence`, and the bones under it follow. `simulated_bones` simulates only the bones it
  names and those under them (all of them when it names none the model has), and the others
  keep following the pose. Stopping puts the bodies back on the pose.
- An inactive simulator neither moves bodies nor moves bones; its bodies don't collide unless
  they simulate.
- `joint_type` holds a body to the body of the nearest bone above its own that has one, at its
  bone's origin, in the axes `joint_rotation` turns from the body's: `"pin"`, `"cone"` (a
  `ConeTwistJoint3D` with `swing_span` and `twist_span`), `"hinge"` (about z, with its angular
  limit), `"slider"` (along x, with its linear limit) or `"6dof"` (`x`, `y` and `z`, each with
  Godot's linear and angular limits and springs); for a bone along y, a `joint_rotation` of
  `[0, 0, 1.5707964]` turns a cone's or slider's x axis along it. Joined bodies collide with
  each other, as on Godot's Jolt module, so leave room between their shapes. `"none"` holds
  nothing.
- `collision_exceptions` names bodies the simulator's bodies never collide with, such as the
  character's own capsule.
- A bone with no body, or a second body for one bone, is a body that never moves or collides;
  a `PhysicalBone3D` outside any simulator's children is a static body.

| Component | Fields (defaults) |
| --- | --- |
| `PhysicalBoneSimulator3D` | `active` `true`, `influence` `1`, `simulating` `false`, `simulated_bones` `[]`, `collision_exceptions` `[]` (entity IDs) |
| `PhysicalBone3D` | `bone_name` `""`, `body_offset_position` and `body_offset_rotation` `[0, 0, 0]`, `joint_type` `"none"`, `joint_rotation` `[0, 0, 0]`, `swing_span` `0.785398`, `twist_span` `3.14159`, `angular_limit_enabled` `false`, `angular_limit_upper` `1.5708`, `angular_limit_lower` `-1.5708`, `linear_limit_upper` `1`, `linear_limit_lower` `-1`, `x`, `y` and `z` (each with `linear_limit_enabled` and `angular_limit_enabled` `true`, their `_upper` and `_lower` `0`, `linear_spring_enabled` and `angular_spring_enabled` `false` with `_stiffness`, `_damping` and `_equilibrium_point` `0`), `collision_layer` `1`, `collision_mask` `1`, `mass` `1`, `friction` `1`, `bounce` `0`, `gravity_scale` `1`, `linear_damp_mode` and `angular_damp_mode` `"combine"`, `linear_damp` and `angular_damp` `0`, `can_sleep` `true`; written by each step while it simulates: `linear_velocity`, `angular_velocity` |

`scene validate` reports `simulated_bones` and `bone_name`s the model hasn't (with "did you
mean"), a bone with two bodies and a `PhysicalBone3D` no simulator holds.

## Navigation

Navigation follows Godot 4.6's `NavigationServer2D` and `NavigationServer3D`. Each world has a
2D map and a 3D map, made of the regions of navigation mesh and the links that components place
on them. Paths are found with Godot's own A* over the regions' polygons and pulled tight with its
funnel, so they come out as Godot's do, and the same on every machine. Each tick, after physics,
the `navigation` system (priority 960) rebuilds the maps whose regions or links changed, then
updates each agent's path and progress, then gives agents that avoid each other their safe
velocities. [examples/navigation](../../examples/navigation/) sends a guard through a hatch and
a cat round the corner of a yard.

### Navigation meshes and polygons

A `.navmesh.json` file is Godot's `NavigationMesh`: convex polygons over shared vertices, in
meters. A `.navpoly.json` file is its `NavigationPolygon`, the same in pixels. An agent can write
the polygons, or bake them from the scene with [`navigation.bake`](commands.md#navigationbake)
and the settings below ([Baking](#baking)).

```json
{
  "format": 1,
  "vertices": [[0, 0, 0], [0, 0, 4], [4, 0, 4], [4, 0, 0], [0, 0, 8], [4, 0, 8]],
  "polygons": [[0, 1, 2, 3], [1, 4, 5, 2]],
  "cell_size": 0.25,
  "cell_height": 0.25
}
```

| Field | Required | Default | Meaning |
| --- | --- | --- | --- |
| `format` | Yes | | `1` |
| `vertices` | No | `[]` | Points, as `[x, y, z]` (`[x, y]` in a `.navpoly.json` file) |
| `polygons` | No | `[]` | Convex polygons, each a list of three or more indices into `vertices` |
| `cell_size` | No | `0.25` (`1` in 2D) | The cell size it was made for, as Godot's |
| `cell_height` | No | `0.25` | The cell height it was made for, in `.navmesh.json` files only |

`$cli scene validate` and `$cli scene format` take both, and `$cli schema navmesh` and
`$cli schema navpoly` print their schemas. Validating reports an index past the vertices, an
index used twice in one polygon and a polygon that isn't convex, each with its JSON Pointer.
Files reload when they change, and their maps rebuild.

### Baking

`navigation.bake` bakes a region's file from the geometry under the region, as Godot's
`bake_navigation_mesh` and `bake_navigation_polygon`: the region's entity and those under it in
`Transform3D` (or `Transform2D`) children, in the region's space. It replaces the file's
`vertices` and `polygons` and keeps everything else as written, so the settings below stay in
the file. [examples/navigation-baking](../../examples/navigation-baking/) bakes a courtyard and
a garden.

In 3D, what bakes is each `MeshInstance3D`'s primitive mesh or model file and the shapes of
each `StaticBody3D` and `AnimatableBody3D` (on the body or on children with no body of their
own), as Godot's parsers take them: a primitive shape as Godot's mesh of it, a convex polygon
as its hull's faces. Recast marks the triangles agents can walk, voxelizes them into cells,
keeps agents `agent_radius` from walls and returns the walkable area; its detail triangles are
the polygons, wound clockwise seen from above, as Godot's are. A bake warns when a setting
doesn't fit whole cells, as Godot does, and fails past 30 million cells.

A `NavigationObstacle3D` under the region with `affect_navigation_mesh` is cut out too, as
Godot's projected obstructions: its `vertices` (turned only about y with the entity, and scaled)
from its elevation up by `height`, and a circle of 12 sides of its `radius`. Its cells are marked
unwalkable before the walkable area shrinks by `agent_radius`, so the hole grows by it like any
wall's; with `carve_navigation_mesh` they are marked after, so it cuts only its own shape.

| Field | Default | Meaning |
| --- | --- | --- |
| `sample_partition_type` | `"watershed"` | How Recast splits the walkable area: `"watershed"`, `"monotone"` or `"layers"` |
| `geometry_parsed_geometry_type` | `"both"` | What bakes: `"mesh_instances"`, `"static_colliders"` or `"both"` |
| `geometry_collision_mask` | `4294967295` | Static bodies bake when their `collision_layer` shares a bit with this |
| `border_size` | `0` | Space kept free inside `filter_baking_aabb`, in meters |
| `agent_height` | `1.5` | How tall agents are; lower ceilings aren't walkable |
| `agent_radius` | `0.5` | How far agents keep from walls |
| `agent_max_climb` | `0.25` | The tallest step agents climb |
| `agent_max_slope` | `45` | The steepest slope agents walk, in degrees (at most 90) |
| `region_min_size` | `2` | Islands smaller than this many cells squared are dropped |
| `region_merge_size` | `20` | Areas smaller than this many cells squared merge into their neighbors |
| `edge_max_length` | `0` | The longest edge along a border; `0` for any |
| `edge_max_error` | `1.3` | How far a border may stray from the walkable area's outline, in cells |
| `vertices_per_polygon` | `6` | The most corners Recast's polygons have before they are split into triangles |
| `detail_sample_distance` | `6` | How far apart the detail mesh samples heights, in cells |
| `detail_sample_max_error` | `1` | How far the detail mesh may stray from the surface, in cell heights |
| `filter_low_hanging_obstacles` | `false` | Walk over obstacles lower than `agent_max_climb` |
| `filter_ledge_spans` | `false` | Leave out ledges agents would step off |
| `filter_walkable_low_height_spans` | `false` | Leave out places lower than `agent_height` |
| `filter_baking_aabb_position`, `filter_baking_aabb_size`, `filter_baking_aabb_offset` | `[0, 0, 0]` | Bake only what is in this box, when its size has volume |

In 2D, a `.navpoly.json` file's `outlines` say where agents may walk: closed outlines of
`[x, y]` points, merged where they overlap. The shapes of each `StaticBody2D` and
`AnimatableBody2D` under the region are cut out of them, as Godot's parser takes them: a
rectangle, a circle of 12 sides, a capsule of 14 corners, a polygon's convex hull, or a
`CollisionPolygon2D` as written. So is each `NavigationObstacle2D` with
`affect_navigation_mesh`: its `vertices` and a circle of 12 sides of its `radius`. Clipper2
merges the outlines and the obstructions, takes one from the other and shrinks the rest by
`agent_radius`, then PolyPartition splits it into convex polygons or triangles. An obstacle with
`carve_navigation_mesh` is taken out after the shrink, so it cuts only its own shape.

A `TileMapLayer` under the region adds its cells' tiles, as Godot's parser takes them: each
tile's [navigation outlines](#tile-navigation) on every navigation layer are outlines agents may
walk, and its collision polygons on physics layers whose `collision_layer` shares a bit with
`parsed_collision_mask` are cut out, both turned with the cell. A region whose file has no
`outlines` can bake from its tile map layers alone.

| Field | Default | Meaning |
| --- | --- | --- |
| `outlines` | `[]` | Closed outlines of where agents may walk, each three or more `[x, y]` points |
| `sample_partition_type` | `"convex_partition"` | `"convex_partition"` or `"triangulate"` |
| `parsed_geometry_type` | `"both"` | What bakes: `"mesh_instances"`, `"static_colliders"` or `"both"`. Static bodies and tile collision polygons bake with the last two, tiles' navigation outlines with any |
| `parsed_collision_mask` | `4294967295` | Static bodies, and tiles on physics layers, bake when their `collision_layer` shares a bit with this |
| `border_size` | `0` | Space kept free inside `baking_rect`, in pixels |
| `agent_radius` | `10` | How far agents keep from walls and obstructions, in pixels |
| `baking_rect_position`, `baking_rect_size`, `baking_rect_offset` | `[0, 0]` | Bake only what is in this rect, when it has area |

### Regions

A `NavigationRegion3D` places a `.navmesh.json` file on the 3D map at the entity's `Transform3D`
(position, rotation and scale), and a `NavigationRegion2D` places a `.navpoly.json` file on the
2D map at its `Transform2D`.

```json
"house": {"components": {
  "Transform3D": {},
  "NavigationRegion3D": {"navigation_mesh": "nav/house.navmesh.json"}
}}
```

- A region's polygons join where they share an edge. Regions join each other where their edges
  meet, or, with `use_edge_connections` on in both the region and the map, where they come within
  the map's `edge_connection_margin` of each other.
- `navigation_layers` is a bit mask: a path uses only the regions and links whose layers share a
  bit with its own.
- `travel_cost` multiplies the cost of each unit of distance in the region, and `enter_cost` is
  added when a path enters it from another region. The search finds the cheapest way, which is
  not always the shortest. As in Godot, the search charges `enter_cost` by the polygon it took
  last rather than the one the path comes from, so `travel_cost` steers paths more reliably.
- A region that isn't `enabled` is left out of paths, but `navigation.closest` still finds it,
  as Godot's `map_get_closest_point` does.
- A file that doesn't load, or is of the wrong kind, leaves the region out of the map, and
  `navigation.map` reports why under `errors`.
- A `TileMapLayer`'s cells are regions too, of its tiles' navigation polygons: see
  [tile navigation](#tile-navigation).

### Links

A `NavigationLink3D` or `NavigationLink2D` joins two places the regions don't, such as a jump, a
ladder or a door, as Godot's. `start_position` and `end_position` are in the space of the
entity's transform, and each end joins the closest region polygon within the map's
`link_connection_radius`.

```json
"hatch": {"components": {
  "Transform3D": {},
  "NavigationLink3D": {"start_position": [3, 0, 2], "end_position": [9, 0, 2], "travel_cost": 1.5}
}}
```

- A link that isn't `bidirectional` goes only from start to end.
- `travel_cost` multiplies the cost of its length and `enter_cost` is added when a path takes
  it; `navigation_layers` and `enabled` work as a region's.
- A path through a link has a point at each end. Its metadata names the link as the owner of
  the first, and the region it leads to as the owner of the second.

### Agents

A `NavigationAgent3D` finds a path on the 3D map from the entity's `Transform3D` to its
`target_position` and keeps track of its progress, as Godot's; a `NavigationAgent2D` does the
same on the 2D map from the entity's `Transform2D`. Agents move nothing: gameplay code moves the
entity toward `navigation::next_path_position_3d` (or `_2d`), as a Godot script calls
`get_next_path_position`, and asks `navigation::is_navigation_finished` when to stop.

```json
"guard": {"components": {
  "Transform3D": {"position": [1, 0, 1]},
  "NavigationAgent3D": {"target_position": [10, 0, 6], "path_desired_distance": 0.25,
                        "target_desired_distance": 0.5}
}}
```

- Changing `target_position`, or calling `navigation::set_target_position`, asks for a new path.
  So do a rebuild of the map and the entity straying more than `path_max_distance` from its
  path.
- The agent moves on to the next path point when the entity comes within
  `path_desired_distance` of the one it is going to, and has reached its target within
  `target_desired_distance` of `target_position`. For a target it can't reach, navigation
  finishes at the end of the path, the closest point it can reach.
- `path_height_offset` is how far the navigation mesh is above the entity's origin: it is taken
  off each path point's y, as Godot's.
- `path_postprocessing` shapes the path, as Godot's: `"corridorfunnel"` pulls it tight round
  corners, `"edgecentered"` goes through the middle of each edge it crosses, and `"none"` keeps
  the points where the search entered each polygon. `simplify_path` then drops the points within
  `simplify_epsilon` of a straight line, as Godot's Ramer-Douglas-Peucker.
- `path_search_max_polygons` and `path_search_max_distance` limit the search, and
  `path_return_max_length` and `path_return_max_radius` cut the path it returns; `0` means no
  limit.
- The agent emits world events as Godot's emits signals: `path_changed` when it has a new path,
  `waypoint_reached` at each point (with its `position`, `type` (`"region"` or `"link"`) and
  `owner`), also `link_reached` at a link's start (with `link_entry_position` and
  `link_exit_position`), then `target_reached` and `navigation_finished`.
- Paths and progress live beside the components, kept by entity ID, so saving, snapshots and
  undo leave them alone; a failed command or a dry run puts them back.

### Avoidance

Agents with `avoidance_enabled` steer round each other and round obstacles, as Godot's, with
the optimal reciprocal collision avoidance of RVO2. Gameplay code sets the agent's `velocity` to
where it wants to go, and the `navigation` system, after updating paths, gives back a safe
velocity that keeps it `radius` from the others for `time_horizon_agents` seconds (and from
obstacles' outlines for `time_horizon_obstacles`), emitted as a `velocity_computed` event with
its `safe_velocity` and returned by `navigation::safe_velocity_2d` (or `_3d`). Gameplay code then
moves the entity at that velocity: in a system above priority 960, or the next tick.
[examples/navigation-avoidance](../../examples/navigation-avoidance/) has walkers weaving past
each other in a plaza.

```json
"walker": {"components": {
  "Transform2D": {"position": [20, 280]},
  "NavigationAgent2D": {"target_position": [380, 280], "avoidance_enabled": true,
                        "radius": 10, "max_speed": 100}
}}
```

- An agent asks for its `velocity` only while it follows a path: once it finishes, it asks for
  nothing until it has a new target, though others still steer round it. Without
  `avoidance_enabled`, its safe velocity is its `velocity`.
- It looks at the `max_neighbors` closest agents within `neighbor_distance`, and the safe
  velocity is no faster than `max_speed`.
- `avoidance_mask` picks whom an agent makes way for: those on an `avoidance_layers` bit it
  shares. It makes way for none of lower `avoidance_priority` (0 to 1), who make way for it
  fully.
- On the 3D map agents avoid on the ground, by default: those whose spans of `height` up from
  their origins overlap. The velocity's y is left out, and kept and put back after
  (`keep_y_velocity`) or dropped. With `use_3d_avoidance` they avoid as spheres in every
  direction, among the others that do.
- `navigation::set_velocity_forced` replaces the velocity avoidance starts the next step from,
  as Godot's `set_velocity_forced`, for an agent moved to a new place.

A `NavigationObstacle2D` or `NavigationObstacle3D` is in agents' way, as Godot's. Its `radius`
is a circle agents keep out of, which moves at its `velocity` (gameplay code moves the entity);
its `vertices` are an outline, in the entity's space, that agents don't cross. Listed
counterclockwise in x and y (in 2D, clockwise on screen, since y points down), an outline keeps
agents out; the other way round, it keeps them in. Avoidance finds no way round an outline,
though: an agent heading straight at one stops at its edge, so give obstacles
`affect_navigation_mesh` for paths to go round them ([Baking](#baking)). A 3D obstacle's outline
spans `height` up from its origin, and agents on the ground avoid it only where their heights
overlap; with `use_3d_avoidance` its circle is a sphere among agents that avoid in 3D. Agents
avoid an obstacle when their `avoidance_mask` shares a bit with its `avoidance_layers`.

| Component | Fields (defaults) |
| --- | --- |
| `NavigationRegion2D` | `navigation_polygon` `""` (a `.navpoly.json` file), `enabled` `true`, `use_edge_connections` `true`, `navigation_layers` `1`, `enter_cost` `0`, `travel_cost` `1` |
| `NavigationRegion3D` | `navigation_mesh` `""` (a `.navmesh.json` file), and the rest as `NavigationRegion2D`'s |
| `NavigationLink2D` | `enabled` `true`, `bidirectional` `true`, `navigation_layers` `1`, `start_position` and `end_position` `[0, 0]`, `enter_cost` `0`, `travel_cost` `1` |
| `NavigationLink3D` | The same, with `start_position` and `end_position` `[0, 0, 0]` |
| `NavigationAgent2D` | `target_position` `[0, 0]`, `path_desired_distance` `20`, `target_desired_distance` `10`, `path_max_distance` `100`, `navigation_layers` `1`, `path_postprocessing` `"corridorfunnel"`, `simplify_path` `false`, `simplify_epsilon` `0`, `path_return_max_length` `0`, `path_return_max_radius` `0`, `path_search_max_polygons` `4096`, `path_search_max_distance` `0`; avoidance: `avoidance_enabled` `false`, `velocity` `[0, 0]`, `radius` `10`, `neighbor_distance` `500`, `max_neighbors` `10`, `time_horizon_agents` `1`, `time_horizon_obstacles` `0`, `max_speed` `100`, `avoidance_layers` `1`, `avoidance_mask` `1`, `avoidance_priority` `1` |
| `NavigationAgent3D` | `target_position` `[0, 0, 0]`, `path_desired_distance` `1`, `target_desired_distance` `1`, `path_height_offset` `0`, `path_max_distance` `5`, and the rest as `NavigationAgent2D`'s; avoidance as `NavigationAgent2D`'s but `velocity` `[0, 0, 0]`, `height` `1`, `radius` `0.5`, `neighbor_distance` `50`, `max_speed` `10`, `use_3d_avoidance` `false`, `keep_y_velocity` `true` |
| `NavigationObstacle2D` | `radius` `0`, `vertices` `[]`, `avoidance_enabled` `true`, `velocity` `[0, 0]`, `avoidance_layers` `1`, `affect_navigation_mesh` `false`, `carve_navigation_mesh` `false` |
| `NavigationObstacle3D` | `radius` `0`, `height` `1`, `vertices` `[]` (`[x, y, z]` points; y is ignored), `velocity` `[0, 0, 0]`, `use_3d_avoidance` `false`, and the rest as `NavigationObstacle2D`'s |

The maps' settings are `project.json`'s [`navigation_2d` and `navigation_3d`](project-format.md).
`scene validate` and exports check each region's file and report one that doesn't exist or
validate, or a `.navpoly.json` file on a `NavigationRegion3D`. The
[`navigation.*` commands](commands.md#navigationpath) find paths and points and report maps and
agents.

## Particles

Particles follow Godot's `CPUParticles2D`. Each tick, after physics, the `cpu_particles_2d`
system (priority 1100) emits, moves and ages every `CPUParticles2D`'s particles on the CPU, and
`CanvasRenderer` draws them with the sprites. [examples/particles](../../examples/particles/) has
a fountain, a one-shot burst, smoke and a ring of orbiting particles.

```json
"fountain": {
  "components": {
    "CPUParticles2D": {
      "amount": 32,
      "color_ramp": [
        {"color": [0.6, 0.85, 1.0, 1.0], "offset": 0.0},
        {"color": [0.3, 0.5, 1.0, 0.0], "offset": 1.0}
      ],
      "direction": [0.0, -1.0],
      "gravity": [0.0, 300.0],
      "initial_velocity_max": 200.0,
      "initial_velocity_min": 160.0,
      "scale_amount_curve": [[0.0, 1.0], [1.0, 0.3]],
      "spread": 12.0,
      "texture": "dot.png"
    },
    "Transform2D": {"position": [80.0, 160.0]}
  }
}
```

- An emitter has `amount` slots and emits from each once a cycle of `lifetime` seconds, slot
  `i` at `i / amount` of the way through, moved later by up to one slot's share times
  `randomness` and pulled toward the cycle's start by `explosiveness`. A particle emitted
  between ticks starts moved on by the time since, so emission is smooth at any tick rate.
- A new particle starts where `emission_shape` says and heads along `direction`, turned up to
  `spread` degrees either way, at a speed between `initial_velocity_min` and `_max`. Unless
  `local_coords` is set, both go through the entity's `Transform2D` (scaled, turned, moved) and
  the particle stays in the world when the entity moves.
- Each tick a particle speeds up by `gravity`, its linear, radial and tangential acceleration,
  orbits the entity, slows by `damping`, then moves by its velocity. Radial and tangential
  acceleration and orbits go round the entity's position.
- Each other parameter takes a random value between its `_min` and `_max` when the particle is
  emitted, times its `_curve` at the particle's age as a fraction of its life. Its color is
  `color` × `color_ramp` at that age × its pick of `color_initial_ramp` × its point's
  `emission_colors`, with its hue turned by `hue_variation`.
- With `hframes` or `vframes` over 1, a particle draws the texture frame `anim_offset` +
  `anim_speed` × age-fraction of the way through the frames, as Godot's particle animation.
- Random values come from the entity's ID and `seed`, so a scene makes the same particles every
  run and on every platform. [`particles.eval`](commands.md#particleseval) returns them as
  numbers, as they are or after a time, for checks without a screenshot.
- A `one_shot` emits one cycle, then sets `emitting` to false; when its last particle dies it
  emits a `finished` event. Setting `emitting` to true starts a new cycle with new random
  values. Changing `amount` starts the emitter over.
- Particles are not saved: scenes, snapshots, [`undo`](commands.md#undo), dry runs and failed
  commands leave them as they are. An emitter whose entity is destroyed or loses its
  `CPUParticles2D` loses its particles.

### GPU particles

`GPUParticles2D` follows Godot's: the device moves and draws its particles, as a process material
says. Each tick, after the CPU particles, the `gpu_particles_2d` system (priority 1110) works out
each emitter's steps as Godot's node and its particles storage do, and keeps them; the renderer
runs the steps it hasn't run on its device, then draws the particles there. On Metal, Direct3D 12
and Vulkan a compute shader (`particles_process.hlsl`) runs them and the particles stay on the
GPU, so the CPU's work is the same for ten particles or a hundred thousand. WebGL 2 has no
compute shaders, so the OpenGL ES device (web exports) runs them on the CPU, as the software
device does. [examples/gpu-particles](../../examples/gpu-particles/) has a fire, a burst of
sparks and snow.

```json
"fire": {
  "components": {
    "GPUParticles2D": {
      "amount": 48,
      "process_material": "fire.process_material.json",
      "texture": "dot.png"
    },
    "Transform2D": {"position": [80.0, 150.0]}
  }
}
```

- A step moves the particles on by `1 / fixed_fps` seconds (times `speed_scale`); with
  `fixed_fps` 0 there is one a tick. With `interpolate`, particles draw where they are between
  steps. `preprocess` runs steps of `1 / fixed_fps` seconds (`1 / 30` with `fixed_fps` 0) when
  the emitter starts.
- The emission cycle is Godot's: slot `i` of `amount` emits `i / amount` of the way through each
  `lifetime`, pulled toward the start by `explosiveness` and moved by up to `randomness`; only
  the share `amount_ratio` emits. A particle starts and moves as its process material says,
  through the entity's `Transform2D` unless `local_coords`, with the emitter's velocity for
  `inherit_velocity_ratio`.
- The steps are the same on every machine, and every device (Metal, Direct3D 12, Vulkan, OpenGL
  ES, the software device) runs them to the same particles, as `render::process_particles` does
  on the CPU. A renderer that starts late, or misses frames, catches up from the steps kept:
  those since the last restart that can still have made a live particle.
- Random values come from `seed` with `use_fixed_seed`; otherwise from the entity's ID and
  `seed`, varied by each `one_shot` restart.
  [`particles.eval`](commands.md#particleseval) runs the steps kept on the CPU to return the
  particles as numbers, or runs the settings from the start for a `time`.
- A `one_shot` stops emitting after one `lifetime` and sets `emitting` to false; it emits a
  `finished` event `lifetime` × (2 - `explosiveness`) after it started, when its particles have
  had time to die, as Godot's. Setting `emitting` to true starts a new cycle at once, as Godot's
  `restart` does (Godot's renderer waits for the last cycle's phase to come round, so a burst set
  off from `finished` can emit nothing there). Turning `one_shot` off while it emits restarts
  it. Changing `amount`, or turning trails on or off or changing `trail_sections`, starts it over.
- Particles are not saved: scenes, snapshots, [`undo`](commands.md#undo), dry runs and failed
  commands leave them as they are.
- Attractors are on the [roadmap](../roadmap.md).

### Process materials

A `.process_material.json` file is Godot's `ParticleProcessMaterial` in 2D, with its property
names and Godot's defaults, except `gravity`: `[0, 98]`, as Godot gives a process material on a
`GPUParticles2D`. `slash-cli schema process_material` prints its JSON Schema, and
`slash-cli scene validate` and `scene format` take these files.

```json
{
  "format": 1,
  "emission_shape": "sphere",
  "emission_sphere_radius": 6,
  "direction": [0, -1],
  "spread": 15,
  "gravity": [0, -40],
  "initial_velocity_min": 30,
  "initial_velocity_max": 60,
  "scale_curve": [[0, 1], [1, 0.2]],
  "color_ramp": [{"offset": 0, "color": [1, 0.9, 0.4, 1]}, {"offset": 1, "color": [0.4, 0.1, 0.05, 0]}]
}
```

| Field | Default | What |
| --- | --- | --- |
| `format` | | Must be 1 |
| `emission_shape` | `"point"` | Where particles start: `point`, `sphere` (in a circle), `sphere_surface` (on its edge), `box`, `points`, `directed_points` or `ring` |
| `emission_shape_offset`, `emission_shape_scale` | `[0, 0]`, `[1, 1]` | Move and scale the shape, in pixels |
| `emission_sphere_radius` | `1` | The circle's radius |
| `emission_box_extents` | `[1, 1]` | Half the box's width and height |
| `emission_ring_radius`, `emission_ring_inner_radius` | `1`, `0` | The ring's radii |
| `emission_points`, `emission_normals`, `emission_colors` | `[]` | For `points` and `directed_points`: points in pixels, each one's direction (`directed_points`) and color multiplying its particles; normals and colors are none or one per point |
| `direction`, `spread` | `[1, 0]`, `45` | Which way particles go, and degrees either side, 0 to 180 |
| `gravity` | `[0, 98]` | Pixels per second squared |
| `inherit_velocity_ratio` | `0` | How much of the emitter's velocity particles start with |
| `velocity_pivot` | `[0, 0]` | What `radial_velocity` pushes away from |
| `initial_velocity_min`, `_max` | `0` | Starting speed in pixels per second |
| `angular_velocity_min`, `_max` | `0` | Degrees per second it turns |
| `orbit_velocity_min`, `_max` | `0` | Turns per second round the emitter |
| `radial_velocity_min`, `_max` | `0` | Pixels per second away from `velocity_pivot` |
| `linear_accel_min`, `_max` | `0` | Pixels per second squared along its velocity |
| `radial_accel_min`, `_max` | `0` | Pixels per second squared away from the emitter |
| `tangential_accel_min`, `_max` | `0` | Pixels per second squared round the emitter |
| `damping_min`, `_max` | `0` | Pixels per second squared it slows by, from 0 |
| `angle_min`, `_max` | `0` | Rotation in degrees |
| `scale_min`, `_max` | `1` | Size, times the texture's |
| `hue_variation_min`, `_max` | `0` | Turns its hue by this fraction of the color wheel, -1 to 1 |
| `anim_speed_min`, `_max` | `0` | Times its frames play over its life, from 0 |
| `anim_offset_min`, `_max` | `0` | The frame it starts on, as a fraction of the frames, 0 to 1 |
| `turbulence_influence_min`, `_max` | `0.1` | How far turbulence turns its velocity, 0 to 1 |
| `turbulence_initial_displacement_min`, `_max` | `0` | Pixels turbulence moves it where it starts, times the noise direction's length (a tenth of `turbulence_noise_strength`, as Godot's) |
| `*_curve` | `[]` | Each parameter but `initial_velocity` and `turbulence_initial_displacement` has one (`turbulence_influence_over_life` for turbulence): points `[offset, value]` that multiply it over each particle's life (`anim_offset_curve` adds), offsets from 0 to 1 in order, joined by straight lines |
| `velocity_limit_curve` | `[]` | The fastest a particle moves over its life, in pixels per second |
| `alpha_curve` | `[]` | Multiplies alpha over its life |
| `emission_curve` | `[]` | Brightens it over its life: its color times 1 plus this |
| `color` | `[1, 1, 1, 1]` | Multiplies the texture |
| `color_ramp` | `[]` | Colors over each particle's life, multiplying `color`: stops `{"offset", "color"}` in order |
| `color_initial_ramp` | `[]` | Each particle multiplies its color by one picked at random from this gradient |
| `lifetime_randomness` | `0` | Shortens each particle's life by up to this fraction, 0 to 1 |
| `particle_flag_align_y` | `false` | Turn each particle so its y axis points along its velocity |
| `particle_flag_damping_as_friction` | `false` | Damping grows with speed, as friction |
| `turbulence_enabled` | `false` | Turn particles along moving curl noise, as Godot's turbulence |
| `turbulence_noise_strength` | `1` | How sharply the noise turns, 0 to 20 |
| `turbulence_noise_scale` | `9` | The noise's size, Godot's slider: swirls about 1 / (4 - scale^0.25 × 2.249) pixels across, so 9 makes them 10 pixels and 9.8 about 50; 10 has none |
| `turbulence_noise_speed` | `[0, 0, 0]` | How the noise moves per second, as `[x, y, z]` |
| `turbulence_noise_speed_random` | `0.2` | How fast the noise changes as it moves, 0 to 4 |
| `collision_mode` | `"disabled"` | What a particle does on touching a shape: `rigid` bounces off it, `hide_on_contact` dies |
| `collision_friction` | `0` | For `rigid`, the fraction of its speed along the surface it loses, 0 to 1 |
| `collision_bounce` | `0` | For `rigid`, the fraction of its speed into the surface sent back, 0 to 1 |
| `collision_use_scale` | `false` | Multiply the emitter's collision size by the particle's scale |
| `sub_emitter_mode` | `"disabled"` | When each particle starts particles in the emitter's [`sub_emitter`](#sub-emitters): `constant`, `at_end` (dying of age), `at_collision` or `at_start` |
| `sub_emitter_frequency` | `4` | For `constant`, times a second of its life, more than 0 |
| `sub_emitter_amount_at_end`, `_at_collision`, `_at_start` | `1` | Particles it starts each time, for that mode |
| `sub_emitter_keep_velocity` | `false` | The particles it starts move on with its velocity too |
| `params`, `nodes`, `start`, `process` | | [Particle graphs](#particle-graphs) that change what the material works out |

Turbulence is Godot's: each step a particle's velocity, built up and controlled, turns towards
the curl of 4D gradient noise at its position by its `turbulence_influence`, keeping its speed
(the part turned out of the screen still counts toward it). The noise moves with the time since
the emitter started over, back to 0 every hour, where Godot's moves with the engine's clock;
its lattice gradients come from an integer hash of each point rather than Godot's fractions of
float products, so every device and the CPU make the same noise.

### GPU particle collision

A process material with a `collision_mode` collides with the outlines of `LightOccluder2D`s
whose `sdf_collision` is on and with tile occluders on occlusion layers whose `sdf_collision` is
on, in every visible canvas, as Godot's particles collide with the signed distance field its
viewport draws from them. A closed outline of three or more points is solid; an open one is a
line, solid on neither side. A particle collides where it comes nearer to an outline than its
collision size (the emitter's `collision_base_size`, through its texture and, with
`collision_use_scale`, its scale), or is inside a solid one. Then a `rigid` one is pushed out to
that distance, loses its speed into the outline, slides with `collision_friction` and bounces
back by `collision_bounce` (only when it hits at least 2 / (1 + bounce) pixels a second, as
Godot's), and turbulence leaves it alone that step; a `hide_on_contact` one dies. A particle is
measured where it is at the start of each step, so one that has just reached a shape is in it
until its next step, and one that moves past the middle of a shape in a step comes out the far
side, as in Godot: give fast particles shapes thicker than twice a step's travel.

The shapes are measured exactly, where Godot samples a field drawn on the GPU, so every device
and the CPU agree. They count everywhere, where Godot's field covers the screen and a margin, and
each step takes the nearest the emitter that fit in 1024 points between them. Particles in the
emitter's coordinates (`local_coords`) reach them through its transform, and an emitter in a
`CanvasLayer` through that layer's.

### Sub-emitters

A `GPUParticles2D`'s `sub_emitter` names another that its particles start particles in, as
Godot's: its process material's `sub_emitter_mode` says when (`constant`, `at_end`,
`at_collision` or `at_start`) and its `sub_emitter_amount_*` how many. Each starts where the
particle is, turned and scaled as it is, moving as its own process material says, and with
`sub_emitter_keep_velocity` adds the particle's velocity. An `at_end` particle starts them in
the step it dies of age, as Godot's code finds it (in its last two or three), and an
`at_collision` one in each step it touches a shape.

```json
"rockets": {
  "components": {
    "GPUParticles2D": {"amount": 4, "process_material": "rockets.process_material.json",
                       "sub_emitter": "burst", "texture": "dot.png"},
    "Transform2D": {"position": [30.0, 175.0]}
  }
},
"burst": {
  "components": {
    "GPUParticles2D": {"amount": 64, "lifetime": 0.8,
                       "process_material": "burst.process_material.json", "texture": "dot.png"},
    "Transform2D": {}
  }
}
```

- A sub-emitter emits only the particles it is given: its `emitting`, `one_shot`, `fixed_fps`
  and `preprocess` go unused, and it emits no `finished`. It steps with the emitter before it,
  a step for each of that one's, as long as its `speed_scale` makes it, and sees the particles
  started in that step. Godot steps each in its own time, so a sub-emitter there can take
  particles from a frame before.
- A sub-emitter takes particles into its slots in turn, carrying on from where the last step
  left off. When they are all alive, new ones take the place of the oldest; Godot drops the new
  ones.
- A sub-emitter can name its own, up to 8 emitters in a chain, which step together. Where two
  name the same one, the one whose ID sorts first has it; a `sub_emitter` naming its own entity
  or one with no `GPUParticles2D`, or that would loop back or make a chain longer than 8, is
  left out, and the emitter runs on its own. `slash-cli scene validate` and exports report each
  one left out, with why.

### GPU particle trails

With `trail_enabled`, each particle draws as Godot's trail ribbon: a strip as wide as a frame of
its texture, from the particle back along the path it took over the last `trail_lifetime`
seconds, with the frame running down its length. As in Godot, the particle keeps
`trail_sections` copies of itself; when it starts, each copy starts with it and waits, then
follows its path that much later (copy `j` of `trail_sections` `j` / `trail_sections` of the
way back), and the ribbon bends through them in `trail_section_subdivisions` segments between
each two, blending their places and colors. A trail that waits is drawn back to where its
particle started, so it grows out behind it.

```json
"comets": {
  "components": {
    "GPUParticles2D": {"amount": 4, "lifetime": 1.6,
                       "process_material": "comets.process_material.json",
                       "texture": "streak.png", "trail_enabled": true, "trail_lifetime": 0.4,
                       "trail_section_subdivisions": 2, "trail_sections": 12},
    "Transform2D": {"position": [20.0, 24.0]}
  }
}
```

- Trails step 60 times a second when `fixed_fps` is 0, as Godot's, since a copy runs a whole
  number of steps behind its particle. Each draws back along its velocity by the part of a step
  its place rounded off.
- When a particle dies its copies run on until they reach the end of the same life, so the
  ribbon shortens from the front and goes; a particle that starts again takes its copies with it.
  An emitter that stops emitting steps until its trails have run out too; Godot stops drawing
  one 1.2 lifetimes after it stops, cutting off trails longer than a fifth of a lifetime.
- A copy takes exactly its particle's path: each runs the step it is behind, with that step's
  time, transform and shapes, so it collides where its particle did. Godot's copies run with the
  current step's time and collision field, so theirs can drift from it. A copy starts nothing in
  a [sub-emitter](#sub-emitters); Godot's start particles as their particle did.
- Godot's Compatibility renderer draws no trails; every SlashEngine device does, the same.
  [`particles.eval`](commands.md#particleseval) reports `trails` and `trail_bounds`, and with
  `list` each particle's `trail`.

### Particle graphs

A process material can carry particle graphs, as Godot's particle shaders: node graphs, written
as [materials](#materials)' are, that change what the material works out for each particle.
`start` runs after the material places a particle, as a particle shader's `start()`, and
`process` each step after the material has worked the step out, as its `process()`. Each names
the parts of the particle it changes and leaves the rest to the material, as a process material
converted to a shader in Godot and then edited would.

```json
{
  "format": 1,
  "gravity": [0, 0],
  "initial_velocity_min": 30,
  "initial_velocity_max": 30,
  "params": {"swirl": {"type": "float", "value": 120, "doc": "Pixels per second squared round the emitter."}},
  "nodes": {
    "out": {"op": "sub", "args": ["position", "emitter_position"]},
    "round": {"op": "normalize", "args": [{"op": "vec2", "args": [{"op": "negate", "args": ["out.y"]}, "out.x"]}]}
  },
  "start": {"active": {"op": "step", "args": [0.25, "random.x"]}},
  "process": {
    "velocity": {"op": "add", "args": ["velocity", {"op": "mul", "args": ["round", {"op": "mul", "args": ["swirl", "delta"]}]}]},
    "color": {"op": "vec4", "args": [{"op": "hsv_to_rgb", "args": [{"op": "vec3", "args": ["life", 0.7, 1]}]}, {"op": "one_minus", "args": ["life"]}]}
  }
}
```

| Output | Type | In | What it changes |
| --- | --- | --- | --- |
| `position` | vec2 | both | Where the particle is, in pixels. In `process`, where it ends the step instead of moving by its velocity |
| `velocity` | vec2 | both | Its velocity in pixels per second. A change in `process` carries on into the next step, as the forces' velocity does |
| `color` | vec4 | `process` | Its color, multiplying the texture |
| `angle` | float | `process` | Its rotation in radians; with `particle_flag_align_y` it turns the particle instead of its velocity |
| `scale` | float | `process` | Its size, times the texture's |
| `frame` | float | `process` | Where it is in the sprite sheet, from 0 to 1 |
| `active` | float | both | Below 0.5 the particle dies; in `start` it never shows |

| Input | Type | In | Godot's | Value |
| --- | --- | --- | --- | --- |
| `position` | vec2 | both | `TRANSFORM[3].xy` | Where the particle is: in `start` where the material placed it, in `process` where it was at the start of the step |
| `velocity` | vec2 | both | `VELOCITY` | Its velocity, as the material worked it out |
| `color` | vec4 | both | `COLOR` | Its color, as the material worked it out |
| `angle`, `scale` | float | `process` | | Its rotation in radians and its size, as the material worked them out |
| `frame` | float | `process` | `CUSTOM.z` | Where it is in the sprite sheet, from 0 to 1 |
| `age` | float | `process` | `CUSTOM.y * LIFETIME` | Seconds since it started |
| `life` | float | `process` | | How far through its life it is, from 0 to 1, as the material's curves take it |
| `lifetime` | float | both | | Seconds it lives, shortened by `lifetime_randomness` |
| `delta` | float | `process` | `DELTA` | Seconds the step lasts |
| `time` | float | both | `TIME` | Seconds since the emitter started over, back to 0 every hour |
| `index` | float | both | `INDEX` | Its slot, from 0 to `amount` - 1 |
| `number` | float | both | `NUMBER` | The emitter's emission cycle times `amount`, plus `index`: different for each particle a slot starts |
| `amount_ratio` | float | both | `AMOUNT_RATIO` | The emitter's `amount_ratio` |
| `random` | vec4 | both | | Four random numbers from 0 to 1, the same for the whole of its life |
| `emitter_position`, `emitter_velocity` | vec2 | both | `EMISSION_TRANSFORM[3].xy` | Where the emitter is, in the coordinates the particles move in (`[0, 0]` with `local_coords`), and its velocity |
| `collided` | float | `process` | `COLLIDED` | 1 in a step it touches a shape (with a `collision_mode`), else 0 |
| `collision_normal`, `collision_depth` | vec2, float | `process` | `COLLISION_NORMAL`, `COLLISION_DEPTH` | The way out of the shape and how far in it is |

- The ops, args and names are a material's, with no textures: `params` are numbers (`float`,
  `vec2`, `vec3`, `vec4` or `color`) the graphs read, set in the file as a particle shader's
  uniforms are set on its material. `nodes` are shared by both stages.
- Each stage runs as one program of at most 64 instructions and 32 constants, with 21 values
  alive at once, and the file has at most 64 nodes. A mistake fails when the file loads, with
  its JSON Pointer and the closest name, as a material's do; `start` reading what only
  `process` knows is one.
- The GPU runs graphs in the particle compute shader, and the CPU (the software and OpenGL ES
  devices, and [`particles.eval`](commands.md#particleseval)) runs them the same way, with
  functions such as `sin` and `exp` that give the same bits on every platform and can differ
  from a GPU's in the last ones.
- A trail's copies run `process` as their particle does, so they keep to its path, and a
  [sub-emitter](#sub-emitters) takes the place and velocity a graph leaves.
- Godot's particle shaders can also keep values of their own in `CUSTOM` and `USERDATA`, start
  particles in a sub-emitter with `emit_subparticle` and restart particles; graphs can't yet.

## UI and text
## UI and text

UI follows Godot's `Control` nodes. A `Control` places a rect in its parent's rect, or a
[container](#containers) places it; `Label`, `ColorRect`, `Panel`, `TextureRect`,
`NinePatchRect`, `ProgressBar` and the [buttons](#buttons) draw in it. An entity with one of those, or a container, and
no `Control` is placed with `Control`'s defaults. `CanvasRenderer` draws controls above the
world, unlit. [examples/ui-and-text](../../examples/ui-and-text/) is a pause screen, and
[examples/ui-containers](../../examples/ui-containers/) a themed inventory screen, and
[examples/ui-buttons](../../examples/ui-buttons/) a title menu worked by mouse, keys and touch.

```json
"panel": {
  "components": {
    "ColorRect": {"color": [0.12, 0.13, 0.2, 0.95]},
    "Control": {
      "anchor_bottom": 0.5,
      "anchor_left": 0.5,
      "anchor_right": 0.5,
      "anchor_top": 0.5,
      "children": ["title", "body"],
      "offset_bottom": 90.0,
      "offset_left": -150.0,
      "offset_right": 150.0,
      "offset_top": -90.0
    }
  }
},
"title": {
  "components": {
    "Control": {
      "anchor_right": 1.0,
      "offset_top": 10.0,
      "theme_override_constants": {"outline_size": 8},
      "theme_override_font_sizes": {"font_size": 36}
    },
    "Label": {"horizontal_alignment": "center", "text": "Paused"}
  }
}
```

- A control's left edge is at `offset_left` + `anchor_left` × its parent's width, and so on for
  each edge; controls no `children` list names are placed in the viewport, the project's
  `window` size (Godot's viewport size). Anchors 0.5 and offsets ±150 keep the panel centered
  at 300 pixels wide on any window.
- A control is at least its minimum size: the bigger of `custom_minimum_size` and its content's,
  such as a label's text. When its edges make it smaller, `grow_horizontal` and
  `grow_vertical` say which edge moves: `end` moves the right or bottom one, `begin` the left
  or top one, `both` each by half.
- `rotation` and `scale` turn and scale a control and its children around `pivot_offset` as it
  draws, without changing layout. `visible`, `modulate` and `z_index` apply to its children too.
- Controls draw by `z_index`, then parents before children and children in `children` order,
  then roots in the order their entities were made. An entity with several UI components draws
  its style boxes and textures (panel, color, texture, nine-patch, bar), then its text.
- A child listed by two parents, a control listed in its own subtree, and an entry naming no
  entity or an entity that isn't a control are reported, and the entry is left out.
  [`ui.layout`](commands.md#uilayout) lists them with every control's rect, and
  `slash-cli scene validate` fails them.

### Labels

A `Label` lays out its text as Godot's does:

- Its minimum size is its widest line by its lines' height. With `autowrap_mode` set, the
  text wraps to the rect's width and doesn't widen it. With `clip_text` or a
  `text_overrun_behavior`, the text doesn't set the height either when it wraps (or the width
  when it doesn't), so the rect decides and the text is cut to it; a wrapping label that trims
  with `max_lines_visible` is as tall as those lines.
- Lines are `font`'s line height apart plus `line_spacing`, and paragraphs (lines between
  `"\n"`s) `paragraph_spacing` more. The lines that fit the rect's height are shown, at most
  `max_lines_visible`, after skipping `lines_skipped`.
- Without wrapping, `text_overrun_behavior` cuts every line wider than the rect. With
  wrapping, it cuts the last line shown when lines are hidden, adding the ellipsis in the
  ellipsis modes.
- `visible_characters` counts characters (Unicode code points) from the start of the text.
- Each label draws its shadow's outline, its shadow, its outline, then its text.
- Text is shaped with HarfBuzz, one run per script and direction, and ordered by the Unicode
  Bidirectional Algorithm (SheenBidi, as Godot's text server): each paragraph goes the way its
  `text_direction` says, by default its first letter's, and runs inside it the other way (an
  English word in Arabic, numbers in Hebrew) keep their own order. `ui.layout` says `rtl` for
  lines that go right to left. Such a line's trailing spaces hang off its left end, and it is
  trimmed at its left end, with the ellipsis there.
- Characters a label's font doesn't have come from its [fallback fonts](#fonts), then from fonts
  installed on the machine; `ui.layout` counts the ones drawn from installed fonts in
  `system_glyphs` and the ones no font has in `missing_glyphs`.

### Right-to-left layout

`layout_direction` `rtl` lays a control out as if its parent were mirrored, as Godot's
`is_layout_rtl`; `inherited` (the default) takes the parent's, and left to right at the root.

- Anchors and offsets count from the right edge, and `grow_horizontal` `end` grows to the left:
  `offset_left` 10 and `offset_right` 110 put a control 10 pixels from its parent's right edge.
- Containers place their children from the right: a row's first child at its right (`begin`
  and `end` alignment swap), a grid's first column, a flow's lines. `shrink_begin` across a
  column is at the right.
- `Label`'s `left` and `right` alignments swap, and a wrapped right-to-left paragraph's lines
  that `fill` doesn't stretch start at the right. A label's text direction is its own: Arabic in
  a left-to-right control goes right to left but is aligned left, as in Godot.
- `Button` swaps its icon's and its text's `left` and `right`, and draws a state's
  `<state>_mirrored` style box (`normal_mirrored`, `hover_mirrored` and so on) when a theme sets
  one. `ProgressBar`'s `begin_to_end` fills from the right.
- [examples/text-languages](../../examples/text-languages/) is a menu in Arabic laid out right
  to left.

### Containers

A container component places the controls in its `children` and ignores their anchors and
offsets, as Godot's containers do; its own rect is placed as any control's. Each is a port of
Godot's code, so a layout comes out as it would in Godot, to the pixel.

| Component | Places its children | Theme constants |
| --- | --- | --- |
| `BoxContainer` | In a row (`HBoxContainer`), or a column with `vertical` (`VBoxContainer`), each at its minimum size along it plus its share of what is left if it expands | `separation` (4) |
| `GridContainer` | In `columns`, left to right, each column as wide as its widest child and each row as tall as its tallest | `h_separation`, `v_separation` (4) |
| `FlowContainer` | In rows that wrap to its width, or columns that wrap to its height with `vertical` (`HFlowContainer`, `VFlowContainer`) | `h_separation`, `v_separation` (4) |
| `MarginContainer` | Each in its whole rect less its margins | `margin_left`, `margin_top`, `margin_right`, `margin_bottom` (0) |
| `CenterContainer` | Each at its minimum size in its center | |
| `PanelContainer` | Each in its rect less its `panel` style box's margins, drawn under them | |
| `ScrollContainer` | Each at its minimum size, or as wide or tall as the visible area on an axis it expands on, moved by the [scroll](#scrolling) | `scrollbar_margin_*`, `scrollbar_h_separation`, `scrollbar_v_separation` |

- A container is at least as big as its children need: a box the sum of their minimum sizes
  along it plus the separations, a grid its columns' and rows', a margin or panel container its
  biggest child's plus the margins. A container in a container grows to fit, up the tree.
- Where a child gets more room than its minimum size, its `size_flags_horizontal` and
  `size_flags_vertical` say what it does with it: `fill` takes all of it, `shrink_begin`,
  `shrink_center` and `shrink_end` keep its minimum size at the start, center or end, and the
  `expand` flags also take a share of the space left over along a box or flow line, or make a
  grid column or row expand. Expanding children share by `size_flags_stretch_ratio`; leftover
  fractions of pixels go to the next child, and the last expanding child reaches the end.
- Hidden children take no room. A wrapping label in a container gets the height of its lines at
  the width the container gives it.
- An entity has one container; `ui.layout` and `scene validate` report a second one.
- Unlike Godot, a container's children keep their `rotation` and `scale`, drawn around their
  `pivot_offset` without changing the layout.

### Panels, textures and progress bars

- `Panel` draws the theme's `panel` style box over its whole rect. `PanelContainer` draws the
  same item and also places children in it.
- `TextureRect`'s minimum size is its texture's with `expand_mode` `keep_size`; `ignore_size`
  lets its rect decide, and the `fit_*` modes take one side from the other. `stretch_mode`
  places the texture: `keep_aspect_centered` scales it to fit inside the rect, centered,
  rounded to whole pixels as Godot's.
- `NinePatchRect`'s corners keep their size, its top and bottom stretch (or tile) across, its
  sides down, and its middle both ways. Its minimum size is its margins.
- `ProgressBar` draws its `background` style box, then its `fill` box as far as its value: the
  fill's margins plus the rest of its length times the value's share of the range. Its
  minimum height fits "100%" in its font, inside the background's margins. `ui.layout` reports
  the percentage it shows as its line.
- `texture_filter` picks how a control's textures are sampled; `inherit` takes the nearest
  parent's that isn't, then the project's `rendering.texture_filter`.

### Buttons

`Button`, `TextureButton` and the [check boxes](#check-boxes) are Godot's: the `gui` system
(priority -900, right after `input`) sends each tick's [input events](commands.md#inputinject)
to controls as Godot's viewport does, and buttons answer them as Godot's `BaseButton`.

```json
"quit": {
  "components": {
    "Button": {"shortcut": [{"action": "ui_cancel", "type": "action"}], "text": "Quit"},
    "Control": {}
  }
}
```

- A button is pressed when a click (by a mouse button in `button_mask`) or a tap that started on
  it ends over it, or as it starts with `action_mode` `press`; when `ui_accept` (Enter, Space)
  goes down and up while it has focus; when its `shortcut` is pressed, from anywhere and before
  `ui_*` keys a focused control doesn't use; and by [`ui.press`](commands.md#uipress). A
  disabled button isn't pressed. A toggle button flips `button_pressed` first, and turns the
  others in its `button_group` off; the one that is on stays on.
- Controls emit world events, as Godot's signals: buttons `button_down`, `pressed`, `toggled`
  (data `{"toggled_on": bool}`) and `button_up`, and any control `mouse_entered`,
  `mouse_exited`, `focus_entered` and `focus_exited`. Gameplay code reads a tick's with
  `ui::events(world)` or `ui::was_pressed(world, button)`; agents subscribe to them with
  [`events.subscribe`](commands.md#eventssubscribe). Only entities with IDs take part.
- The pointer reaches the last control drawn under it, then its parents as `mouse_filter` says:
  `stop` keeps it, `pass` hands it on to the parent too, `ignore` lets it through to whatever is
  under. A control is hovered from when the pointer moves onto it, and its parents too up to the
  first that stops it.
- One control has focus, as Godot's: a left click gives it to the first control up from the one
  clicked whose `focus_mode` is `all` or `click`, without showing it; `ui_focus_next` (Tab),
  `ui_focus_prev` (Shift+Tab), `ui_up`, `ui_down`, `ui_left` and `ui_right` (the arrow keys, a
  gamepad's d-pad and left stick, once per push) move it and show it, to the control a
  `focus_*` field names or else the closest one with `focus_mode` `all` that way, as Godot's
  default focus strategy measures. A hidden control, or one whose `focus_mode` becomes `none`,
  loses focus.
- A button draws by its state, its `draw_mode` in [`ui.layout`](commands.md#uilayout): `normal`,
  `hover`, `pressed` (held down, or a toggle button that is on), `hover_pressed` (on and
  hovered, or just pressed by its shortcut) or `disabled`. `Button` draws the style box of that
  name and the `focus` box over it while it shows focus; `TextureButton` draws
  `texture_pressed` for `pressed` and `hover_pressed`, `texture_hover`, `texture_disabled`, else
  `texture_normal`.
- A `Button` is as big as its text and icon plus its style box's margins, so a button that says
  "Play" is 8 pixels wider and taller than a `Label` that does; with `clip_text`, a
  `text_overrun_behavior` or an `autowrap_mode`, the text doesn't widen it. Its text lays out as
  a `Label`'s, centered by default.
- `ui.layout` and `scene validate` report a `shortcut` entry that isn't a binding or names no
  action (with the closest name, among the project's actions too), a `button_group` without
  `toggle_mode`, a `Label` on a button entity (put it in a child), two buttons on one entity and
  a `focus_*` field that names no control.
- Not yet: tooltips, drag and drop, cursor shapes, theme sounds, `allow_unpress` groups and a
  click mask for `TextureButton`. Hiding a button while it is held lets go of it without
  pressing it.

### Check boxes

`CheckBox` and `CheckButton` are Godot's: toggle buttons with every field of `Button`
(`toggle_mode` starts `true` and `alignment` `"left"`), pressed, toggled and grouped as buttons
are. Instead of a style box they draw an icon beside their text, from their
[theme items](#theme-items): a check box a box before the text, `checked` or `unchecked`, or
`radio_checked` or `radio_unchecked` when it is in a `button_group`; a check button a switch
after it, `checked` or `unchecked` (Godot's `toggle_on` and `toggle_off`). Disabled, they draw
the `_disabled` icons; laid out right to left, the icon goes to the other side, and a check
button draws the `_mirrored` ones.

```json
"vsync": {"components": {"CheckButton": {"button_pressed": true, "text": "V-Sync"}, "Control": {}}}
```

- A check box is as big as its text, plus `h_separation` and the largest of its icons, plus its
  style boxes' margins (4 on each side, 6 left and right for a check button), so a check box
  that says "Sound" is 28 pixels wider than a `Label` that does.
- The icon is drawn at its own size, `icon_max_width` aside, at the left margin of the `normal`
  style box (the right one, at the other end), and centered down the button plus
  `check_v_offset`; a check box rounds that down to a whole pixel, as Godot's does.
- The default icons are Godot's, drawn by the engine in the same shapes and colors, so no
  image files are needed; their names start with `builtin:` (`builtin:checked`,
  `builtin:toggle_on_mirrored`), and a theme or `theme_override_icons` can name them or a PNG.
  `ui.theme` lists the icons a control uses and where each comes from.

### Sliders

`Slider` is Godot's `HSlider`, or `VSlider` with `vertical`: a value between `min_value` and
`max_value` that the pointer, keys and gamepads move, as Godot's `Slider` and `Range`.

```json
"volume": {"components": {"Control": {"size_flags_horizontal": "expand_fill"},
  "Slider": {"step": 5, "tick_count": 5, "ticks_on_borders": true, "value": 80}}}
```

- The value is always on its step: a multiple of `step` from `min_value` (Godot's
  `Math::snapped`, so steps such as 0.1 give 0.3, not 0.30000000000000004), rounded with
  `rounded`, and no more than `max_value - page` or less than `min_value` unless
  `allow_greater` or `allow_lesser`. The `gui` system snaps a value set any other way, such as
  with `component.set`, at the next tick.
- A left click moves the middle of the grabber to the pointer and starts a drag
  (`drag_started`, then `value_changed` even if the value stayed); moving drags the grabber with
  the pointer, and letting go emits `drag_ended` with `{"value_changed": bool}`, whether the
  drag changed it. The wheel steps it, and gives it focus.
- With focus, `ui_left` and `ui_right` (or `ui_down` and `ui_up` for a vertical one) step it by
  `step`, mirrored right to left, and `ui_home` and `ui_end` move it to its ends. A gamepad held
  that way steps it again after half a second, then 20 times a second.
- Each change emits `value_changed` with `{"value": number}`. A change made any other way
  emits it at the next tick, once however many times the value changed.
- It draws as Godot's: its `slider` style box across it, `grabber_area` (or
  `grabber_area_highlight` while hovered or focused) up to the grabber, `tick_count` `tick`
  icons along it, and the `grabber` icon (`grabber_highlight`, `grabber_disabled` when not
  `editable`). Its minimum size is the track's width and the grabber's height (the other way
  round for a vertical one).
- Not yet: `exp_edit`, `custom_step`, theme sounds and `share` between ranges.

### Scrolling

`ScrollContainer` is Godot's: it shows its children through its rect, moved by its scroll, with
scroll bars when they don't fit. `ScrollBar` is Godot's `HScrollBar`, or `VScrollBar` with
`vertical`, on its own. `Control.clip_contents` cuts any control's children off at its rect.

```json
"log": {"components": {
  "Control": {"children": ["quests"], "offset_right": 160, "offset_bottom": 164},
  "ScrollContainer": {"follow_focus": true, "horizontal_scroll_mode": "disabled"}}}
```

- Each child takes its minimum size, or the whole visible width or height on an axis where its
  size flags expand, then moves left and up by `scroll_horizontal` and `scroll_vertical`.
  Scrolling goes as far as the biggest child leaves: `ui.layout`'s `scroll_max`. A scroll set
  from outside the UI, such as with `component.set`, is held to that at the next tick.
- With `auto`, a scroll bar shows on an axis whose children don't fit, along the right edge (the
  left one right to left) or the bottom, and the bars take room from the view. `always` shows
  it anyway, `reserve` keeps its room even while hidden, `never` scrolls with no bar and
  `disabled` doesn't scroll: the container grows to its children's size on that axis.
- The wheel scrolls an eighth of the visible height a turn (Shift, or
  `scroll_horizontal_by_default`, sideways); what it can't scroll goes on to the container
  outside. Buttons and other controls inside let wheel events through, as Godot's
  `force_pass_scroll_events` does.
- A finger scrolls it by dragging past `scroll_deadzone` (`scroll_started`), and it glides on
  when let go, slowing by 1000 pixels a second each second, until it stops or reaches an end
  (`scroll_ended`). Touches start on the container itself or a child that passes the pointer
  (`mouse_filter` `pass`); a button stops them, as in Godot.
- With `follow_focus`, a control inside that gets focus is scrolled into view, the inner
  container first when they nest.
- A scroll bar's grabber is `page`'s share of the bar long. A click before or after it moves the
  value a page; dragging it moves the value with the pointer; the wheel moves it an eighth of a
  page. Each change emits `value_changed` with `{"value": number}` and `scrolling`. A
  container's own bars work the same and move its scroll, emitting nothing; `ui.layout` lists
  which are showing.
- A clipping control (`clip_contents`, or any `ScrollContainer`) draws its children only inside
  its rect, and the pointer reaches only what shows. `ui.layout` gives a control partly cut off
  a `shown_rect` and one cut off entirely `clipped`; `frame.summary` lists those under
  `clipped` and counts only what shows.
- Not yet: scroll hints (`scroll_hint_mode`) and a scroll bar's smooth scrolling.

### Menus

`OptionButton`, `MenuButton` and `PopupMenu` are Godot's. A `PopupMenu` is a list of items
shown over every control while `visible`, at its `position`; in Godot it is a window, so here it
is an entity of its own, not a control. An option button opens a list of its options below it,
as wide as the button at least, and a menu button a menu of its items, both drawn as a
`PopupMenu` with the [`PopupMenu` theme items](#theme-items).

```json
"view": {"components": {"Control": {}, "MenuButton": {"text": "View", "items": [
  {"text": "Show Grid", "checkable": "check", "checked": true},
  {"separator": true, "text": "Zoom"},
  {"text": "1x", "checkable": "radio", "checked": true}, {"text": "2x", "checkable": "radio"}]}}}
```

- One list is open at a time: opening one closes the others. A press of the button opens it (it
  emits `toggled` and `pressed`, and stays pressed while its list is open), and a press while
  it is open closes it.
- A click on an item picks it. A click outside the list closes it at the end of the tick and
  still reaches the control it lands on; one on the list's margin does nothing. A click held
  from the button and let go over an item picks it, once the list has been open 0.4 seconds.
  The pointer over an item focuses it; the wheel scrolls a list taller than the viewport.
- `ui_up` and `ui_down` move to the previous or next item, going round and skipping
  separators and disabled items; `ui_accept` picks the item focused; `ui_cancel` closes the
  list. Typing the start of an item's text, within two seconds a letter, moves to the next item
  that starts so (`allow_search`). Opened by a key, an option button's list starts on the option
  shown, and a menu button's on its first item that isn't disabled.
- Picking an item of a `PopupMenu` or a menu button emits `id_pressed` and `index_pressed` (data
  `{"id", "index"}`, the ID being the item's index when it has none) and closes the menu as its
  `hide_on_*` fields say. Picking an option of an option button sets `selected` and emits
  `item_selected` (data `{"index"}`) unless it was selected and `allow_reselect` is off. A check
  or radio item isn't checked by being picked: as in Godot, the game sets `checked`.
- Opening and closing emit `about_to_popup` and `popup_hide` from a `PopupMenu` or menu button;
  moving to an item with the keys emits `id_focused` (data `{"id"}`), or `item_focused` (data
  `{"index"}`) from an option button.
- A list is as wide as its widest item, its icon and check box, `item_start_padding` and
  `item_end_padding`, plus the panel's margins and a scroll bar's width, and as tall as its items
  with `v_separation` between them, at most the viewport's height; then it scrolls. A separator
  with no text is a line of no height; one with text is a line each side of it. Right to left,
  items lay out from the right and the scroll bar goes on the left.
- A list closes when its button is hidden or destroyed. [`ui.popup`](commands.md#uipopup)
  opens and closes lists, [`ui.select`](commands.md#uiselect) picks an item, and `ui.layout`
  and `frame.summary` report each open list and its items' rects.
- `ui.layout` and `scene validate` report a `checkable` that isn't `none`, `check` or `radio`, a
  `PopupMenu` on a control entity and a `selected` past the options.
- An item's `tooltip` shows once the pointer rests on it, as a control's
  [`tooltip_text`](#tooltips-and-drag-and-drop) does.
- Not yet: submenus, item shortcuts, multistate items, dragging a list's scroll bar and holding a
  gamepad direction to move on.

### Tooltips and drag and drop

Tooltips and drag and drop are Godot's: a control's `tooltip_text` shows after the pointer rests
on it, and the `DragAndDrop` component does what a Godot script's `_get_drag_data`,
`_can_drop_data` and `_drop_data` do. From
[examples/ui-drag-and-drop](../../examples/ui-drag-and-drop/):

```json
"sword": {"components": {
  "Control": {"mouse_filter": "pass", "tooltip_text": "Sword: 12 damage"},
  "ColorRect": {"color": [0.75, 0.78, 0.82, 1]},
  "DragAndDrop": {"drag_data": {"kind": "item", "id": "sword"}, "drag_preview": "sword_ghost"}}},
"slot_3": {"components": {
  "Control": {"offset_left": 128, "offset_top": 40, "offset_right": 176, "offset_bottom": 88},
  "ColorRect": {},
  "DragAndDrop": {"can_drop": [{"kind": "item"}]}}}
```

- The pointer resting on a control for `gui.tooltip_delay_sec` (0.5 seconds) with no button held
  shows its `tooltip_text`, or the first parent's up to one that stops the mouse, without the
  blanks at its ends. The tooltip is a `TooltipLabel` in a `TooltipPanel`
  ([theme items](#theme-items)) at `gui.tooltip_position_offset` (`[10, 10]`) from where the
  pointer came to rest, over every control and popup menu; where it would leave the viewport it
  goes on the other side of the pointer, or against the viewport's edge.
- Moving over the same tooltip keeps it; moving onto a control with another, or more than 5
  pixels while it waits, starts the wait over. A press, Escape (which goes no further), the
  pointer entering the tooltip, or its control hidden or destroyed hides it.
- Pressing on a control and moving more than `gui.drag_threshold` (10) pixels starts a drag of
  what it gives: its `drag_data`, a text field's selection, or else its first parent's that
  gives something, up to one that stops the mouse. The control emits `drag_begin` (data
  `{"data"}`), and its `drag_preview` follows the pointer with its top-left corner, over the
  other controls and under popup menus; the pointer passes through the preview.
- Letting go over a control that takes the data (one of its `can_drop` patterns matches, or a
  text field that is editable and given text), or the first parent that does, drops it there:
  that control emits `drop_data` (data `{"data", "from", "position"}`, the position in its
  rect). The control dragged then emits `drag_end` (data `{"successful"}`). Letting go where
  nothing takes it, a right click or Escape cancels the drag. What a drop means is up to the
  game: read `drop_data` and change the world.
- A text field's selection drags as Godot's: dropped on another field it moves there, selected,
  with Ctrl (Command on a Mac) held it is copied, and dropped on its own selection nothing
  changes. A drop caret shows where it would go.
- Gameplay code can do the same in C++ with `ui::set_drag_forwarding` (Godot's
  `set_drag_forwarding`), `ui::set_drag_preview`, `ui::force_drag`, `ui::show_tooltip` and
  `ui::cancel_tooltip`.
- [`ui.drag`](commands.md#uidrag) drags one control onto another, [`ui.tooltip`](commands.md#uitooltip)
  shows a tooltip, and `ui.layout` and `frame.summary` report the tooltip shown and the drag
  going on.
- `ui.layout` and `scene validate` report a `tooltip_text` on a control the pointer passes by, a
  `drag_preview` that isn't a control with no parent, and a `DragAndDrop` on an entity with no
  `Control`.
- Not yet: custom tooltip controls (`_make_custom_tooltip`) and dragging between windows.

### Text fields

`LineEdit` is Godot's one-line text field. The `gui` system edits it from the tick's
[input events](commands.md#inputinject) as Godot's `LineEdit::gui_input` does, so the same keys,
clicks and input methods work on every platform. From
[examples/text-input](../../examples/text-input/):

```json
"name": {
  "components": {
    "LineEdit": {"clear_button_enabled": true, "max_length": 20, "placeholder_text": "Your name"}
  }
}
```

- A click, Tab, `ui.focus` or [`ui_text_submit`](project-format.md#input-actions) while it
  has focus starts editing; focus moved by the arrow keys or a gamepad doesn't, as in Godot.
  `ui_text_submit` (Enter) emits `text_submitted` and stops editing unless
  `keep_editing_on_text_submit`; `ui_cancel` (Escape) and losing focus stop it. A field takes
  focus by default (`focus_mode` `all`).
- While editing, typed characters (key events with a `unicode` character and no Ctrl, Alt or
  Command) go in at the caret, replacing the selection. The
  [text actions](project-format.md#input-actions) move the caret by character, word and line,
  select with Shift, delete, select all, cut, copy, paste, undo and redo. A double click selects
  a word, a triple click all of it, and a drag selects; a right click places the caret.
- An input method's composition (`ime` events) shows at the caret, underlined, until it commits
  the result as typed text. The platform shows its candidate window next to the caret: games
  pass `ui::text_input(world)` to `SdlReader::set_text_input` each tick, which also shows the
  on-screen keyboard of `virtual_keyboard_type` on phones.
- `text` and `caret_column` are the truth: the field writes them as it is edited, and setting
  `text` from outside (a command, an animation, gameplay code) starts its undo history again.
  Undo takes back a tick's typing at a time, as Godot takes back a frame's.
- A field emits `text_changed` (data `{"text": string}`, once a tick, after the tick's typing),
  `text_submitted` (`{"text": string}`), `text_change_rejected` (`{"rejected_substring":
  string}`, what `max_length` left out) and `editing_toggled` (`{"toggled_on": bool}`) as world
  events, with the focus and hover events every control has.
- The clipboard is the session's own headless and in tests; a game in a window shares the
  system's. [`ui.clipboard`](commands.md#uiclipboard) reads and sets it.
- It draws its `normal` style box (`read_only` when not `editable`), the `focus` box while it has
  focus, the selection in `selection_color`, the text (`font_placeholder_color` for the
  placeholder, `font_uneditable_color` when not `editable`, `font_selected_color` where
  selected), the caret `caret_width` wide and the clear button or right icon. Text that doesn't
  fit scrolls to keep the caret in view. Its minimum size is its style box's margins round
  `minimum_character_width` "W"s of its font, its icon and a line's height.
- Not yet: the context menu, middle-click paste of the primary selection, drag and drop of text
  and `structured_text_bidi_override`. `editing_toggled` is emitted only when editing really
  starts or stops.

### Text fields of several lines

`TextEdit` is Godot's text field of several lines. The `gui` system edits it from the tick's
input events as Godot's `TextEdit::gui_input` does. From
[examples/text-editor](../../examples/text-editor/):

```json
"letter": {
  "components": {
    "Control": {"custom_minimum_size": [0.0, 124.0]},
    "TextEdit": {"highlight_current_line": true, "placeholder_text": "Write to Mira",
                 "wrap_mode": "boundary"}
  }
}
```

- A click, Tab or `ui.focus` gives it focus, and typing goes in at the caret, as in a
  `LineEdit`; it has no editing mode. `ui_text_newline` (Enter) starts a line,
  `ui_text_newline_blank` (Ctrl+Enter) a blank one below the caret's and
  `ui_text_newline_above` (Ctrl+Shift+Enter) one above. `ui_text_indent` (Tab) types a tab
  while `tab_input_mode` is on; `ui_focus_mode` (Ctrl+M) turns that off and on.
- The [text actions](project-format.md#input-actions) move the caret by character, word, row
  (keeping its x), page and to the ends of the row, line and text, select with Shift, delete,
  scroll a line (`ui_text_scroll_up`, `ui_text_scroll_down`), select all, select the word
  under the caret, drop the selection (Escape), cut, copy, paste, undo and redo; Insert
  switches to typing over characters. With nothing selected, cut and copy take the caret's line
  and pasting it puts it back as a line above the caret's. A double click selects a word, a
  triple click a line, and a drag selects by character, word or line.
- The wheel scrolls three lines a notch (fifteen with Alt, sideways with Shift); a finger's
  tap places the caret and its drag scrolls. The view follows the caret, a scroll bar shows when
  the text doesn't fit, and tabs stop every four spaces. The scroll bars work as a
  `ScrollBar`'s: a click beside the grabber pages, the grabber drags and the wheel over one
  moves an eighth of a page; the caret stays where it is.
- `text`, `caret_line`, `caret_column`, `scroll_vertical` and `scroll_horizontal` are the
  truth: the field writes them as it is edited. Text set from outside is set as Godot's
  `set_text` sets it, as one step undo takes back, and a caret moved from outside drops the
  selection. Typing is one undo step until a three-second pause or another kind of edit.
- It emits `text_changed` and `caret_changed` (once a tick, after the tick's edits),
  `lines_edited_from` (`{"from_line": int, "to_line": int}`, from the last line to the first
  for a removal, as Godot's) for each insertion, removal and change of composition, and
  `text_set` when text is set from outside, as world events.
- It draws its `normal` style box (`read_only` when not `editable`), the `focus` box while it has
  focus, `background_color`, the caret's row in `current_line_color` when
  `highlight_current_line`, the selection, the text (`font_readonly_color` when not `editable`,
  `font_placeholder_color` for the placeholder, `font_selected_color` where selected if not
  transparent), the caret, an input method's composition underlined and the scroll bars, in
  the `VScrollBar` and `HScrollBar` theme types' styles, the grabber lit under the pointer and
  while dragged. Its minimum size is its style box's margins, plus its text with
  `scroll_fit_content_height` or `scroll_fit_content_width`.
- Not yet: several carets, the context menu, the minimap, drag and drop of text, middle-click
  paste and smooth scrolling.

### Searching and highlighting code

A `TextEdit` marks text it is told to look for and colors code, as Godot's `TextEdit` search
highlights and `CodeHighlighter` do:

```json
"script": {
  "components": {
    "Control": {"custom_minimum_size": [0.0, 120.0]},
    "TextEdit": {"text": "func jump():\n\tvelocity.y = -400 # up", "search_text": "velocity",
                 "highlight_all_occurrences": true},
    "CodeHighlighter": {"number_color": [0.6, 1, 0.7, 1], "symbol_color": [0.7, 0.8, 1, 1],
                        "function_color": [0.3, 0.8, 1, 1],
                        "member_variable_color": [0.7, 0.9, 1, 1],
                        "keyword_colors": {"func": [1, 0.4, 0.5, 1]},
                        "color_regions": {"#": [0.8, 0.8, 0.8, 0.5], "\" \"": [1, 0.9, 0.6, 1]}}
  }
}
```

- `search_text` is marked wherever it occurs in the rows in view, in `search_result_color`
  with a one-pixel `search_result_border_color` outline. It ignores case unless `search_flags`
  has `match_case`, and with `whole_words` takes only matches with a symbol, a space or the
  line's edge on each side. [`ui.search`](commands.md#uisearch) finds the next match from the
  caret, or every match, and `ui.layout` reports the ones in view.
- With `highlight_all_occurrences`, a selection that isn't only whitespace marks its other
  copies in view, whole words of the same case, in `word_highlighted_color`.
- A `CodeHighlighter` on the entity colors each line as Godot's
  `CodeHighlighter::_get_line_syntax_highlighting_impl` does: color regions first, from their
  start key to their end key (over several lines until it comes) or to the line's end, the
  longest start key tried first; in a region that starts with `"` or `'`, a `\` and the
  character after it take `symbol_color`. Then keywords, member keywords (not after a `.`), a
  word before `(` (spaces between allowed) in `function_color`, a word after `.` in
  `member_variable_color`, symbols, and numbers, with hex digits after `0x`, a decimal point,
  an `f` and an exponent's `e`. The rest takes the `TextEdit`'s font color, and when it isn't
  `editable` no color is more opaque than `font_readonly_color`. Selected text takes
  `font_selected_color` when it isn't transparent.
- `scene validate` reports a `CodeHighlighter` with no `TextEdit` beside it and color region
  keys Godot's `add_color_region` refuses: a start or end that isn't all symbols, an empty
  start, or two regions with the same start.

### Gutters and line numbers

A `TextEdit` draws columns left of its text, Godot's gutters, and a `CodeEdit` beside it makes
it a code editor, as Godot's `CodeEdit`: line numbers, breakpoints, bookmarks, executing lines
and line length guidelines.

```json
"script": {
  "components": {
    "Control": {"custom_minimum_size": [0.0, 160.0]},
    "TextEdit": {"text": "func jump():\n\tvelocity.y = -400",
                 "gutters": [{"name": "warnings", "width": 16,
                              "lines": [{"line": 1, "text": "!", "color": [1, 0.8, 0.2, 1]}]}]},
    "CodeEdit": {"gutters_draw_line_numbers": true, "gutters_draw_breakpoints_gutter": true,
                 "breakpointed_lines": [1], "line_length_guidelines": [80]}
  }
}
```

- A `CodeEdit` adds two gutters before the `TextEdit`'s own: `main_gutter`, a row wide (half
  that when it shows only bookmarks), drawn when it shows breakpoints, bookmarks or executing
  lines, and `line_numbers`, a `0` wider than the digits of the line count (at least
  `gutters_line_numbers_min_digits`), drawn with `gutters_draw_line_numbers`. The text starts 2
  pixels after the last gutter drawn. Gutters take the right side when the control is laid out
  right to left.
- The main gutter draws the `breakpoint` icon on `breakpointed_lines` in `breakpoint_color`,
  the `bookmark` icon on `bookmarked_lines` in `bookmark_color` and the `executing_line` icon on
  `executing_lines` in `executing_line_color`, each inset as Godot's
  `_main_gutter_draw_callback` does. Under the pointer it shows where a click would put a
  breakpoint (darker) or take one off (lighter), or a bookmark with Shift held. Line numbers are
  drawn in `line_number_color`, padded to their digits, and `line_length_guidelines` as lines a
  pixel wide in `line_length_guideline_color`, the first one brightest.
- The `TextEdit`'s own gutters show each entry's `text` (`string`) or `icon` (`icon`, inset a
  sixth of the gutter on each side, keeping its shape) in its `color` on its `line`.
- A left click or a tap in a gutter emits `gutter_clicked` (`{"line": int, "gutter": string}`).
  In a `CodeEdit`, a click in the main gutter toggles a breakpoint (a bookmark with Shift) and
  emits `breakpoint_toggled` (`{"line": int}`); a click on a line number selects the line, and
  dragging from it selects whole lines.
- Breakpoints, bookmarks, executing lines and gutter entries stay on their lines as the user
  types, as Godot keeps them in each line's data: lines inserted above move them down, a new
  line at a line's start moves that line's down with it, and a line joined onto the one before
  loses its own. `breakpoint_toggled` is emitted for each line a breakpoint left or came to.
  Unlike Godot, which clears breakpoints when text is set from outside, they are part of the
  component and stay as they are then.
- [`ui.layout`](commands.md#uilayout) reports each gutter drawn with its rect on screen and the
  rows' `line_height`. `scene validate` reports a `CodeEdit` with no `TextEdit`, a gutter `type`
  other than `string` or `icon`, a negative `width`, and lines below 0 or past the text's last.
- A `CodeEdit` uses its own theme type, which falls back to `TextEdit`'s.

### Theme items

A control's look comes from theme items, as Godot's default theme gives them: a control uses
its `theme_override_*` fields, then its [themes](#themes), then the defaults below.
`ui.layout` and `scene validate` report names its type doesn't have, with the closest one, and
[`ui.theme`](commands.md#uitheme) lists the items a control uses and where each comes from.

| Type | Kind | Name | Default |
| --- | --- | --- | --- |
| `Label` | font | `font` | The built-in font, Roboto Regular |
| | font size | `font_size` | `16` |
| | color | `font_color` | `[1, 1, 1, 1]` |
| | color | `font_outline_color` | `[0, 0, 0, 1]` |
| | color | `font_shadow_color` | `[0, 0, 0, 0]`, no shadow |
| | constant | `outline_size` | `0`; the outline reaches a quarter of it in pixels, as Godot's |
| | constant | `shadow_offset_x`, `shadow_offset_y` | `1` |
| | constant | `shadow_outline_size` | `1` |
| | constant | `line_spacing` | `3` |
| | constant | `paragraph_spacing` | `0` |
| `Panel`, `PanelContainer` | style | `panel` | flat `[0.1, 0.1, 0.1, 0.6]`, `corner_radius` 3 |
| `ProgressBar` | style | `background` | flat `[0.1, 0.1, 0.1, 0.3]`, `corner_radius` 6, `content_margin` 2 |
| | style | `fill` | flat `[1, 1, 1, 0.4]`, `corner_radius` 6, `content_margin` 2 |
| | font, font size | `font`, `font_size` | The built-in font, `16` |
| | color | `font_color` | `[0.95, 0.95, 0.95, 1]` |
| | color | `font_outline_color` | `[0, 0, 0, 1]` |
| | constant | `outline_size` | `0` |
| `Button` | style | `normal` | flat `[0.1, 0.1, 0.1, 0.6]`, `corner_radius` 3, `content_margin` 4 |
| | style | `hover` | flat `[0.225, 0.225, 0.225, 0.6]`, the same shape |
| | style | `pressed` | flat `[0, 0, 0, 0.6]`, the same shape |
| | style | `disabled` | flat `[0.1, 0.1, 0.1, 0.3]`, the same shape |
| | style | `focus` | a `[1, 1, 1, 0.75]` border 2 wide, `expand_margin` 2, no center |
| | style | `hover_pressed` | none: `pressed` is drawn unless a theme sets it |
| | style | `normal_mirrored`, `hover_mirrored`, `pressed_mirrored`, `disabled_mirrored`, `hover_pressed_mirrored` | none: drawn in place of the state's box when the button lays out right to left and a theme sets it |
| | font, font size | `font`, `font_size` | The built-in font, `16` |
| | color | `font_color`, `font_focus_color` | `[0.875, 0.875, 0.875, 1]`, `[0.95, 0.95, 0.95, 1]` |
| | color | `font_hover_color` | `[0.95, 0.95, 0.95, 1]` |
| | color | `font_pressed_color`, `font_hover_pressed_color` | `[1, 1, 1, 1]` |
| | color | `font_disabled_color` | `[0.875, 0.875, 0.875, 0.5]` |
| | color | `font_outline_color` | `[0, 0, 0, 1]` |
| | color | `icon_normal_color`, `icon_hover_color`, `icon_pressed_color`, `icon_hover_pressed_color`, `icon_focus_color` | `[1, 1, 1, 1]` |
| | color | `icon_disabled_color` | `[1, 1, 1, 0.4]` |
| | constant | `outline_size`, `line_spacing` | `0` |
| | constant | `h_separation` | `4`, between the icon and the text |
| | constant | `icon_max_width` | `0`, no limit |
| | constant | `align_to_largest_stylebox` | `0`; 1 keeps the text in place as the style changes |
| `Button`, `TextureButton` | constant | `click_margin` | `0`, pixels round the rect that still hit it |
| `CheckBox`, `CheckButton` | style | `normal`, `pressed`, `hover`, `hover_pressed`, `disabled` | empty, `content_margin` 4 (a check button's 6 left and right) |
| | style | `focus` | as `Button`'s |
| | font, font size, color | `font`, `font_size`, `font_*_color` | as `Button`'s |
| | constant | `h_separation` | `4`, between the icon and the text |
| | constant | `check_v_offset` | `0`, pixels to move the icon down |
| | constant | `outline_size` | `0` |
| `CheckBox` | icon | `checked`, `unchecked` | `builtin:checked`, `builtin:unchecked`: a 16-pixel rounded box, white with a dark check mark, or dark and empty |
| | icon | `radio_checked`, `radio_unchecked` | `builtin:radio_checked`, `builtin:radio_unchecked`: a 16-pixel circle with a dot, or empty |
| | icon | `checked_disabled`, `unchecked_disabled`, `radio_checked_disabled`, `radio_unchecked_disabled` | the same, fainter (`builtin:` and the item's name) |
| | color | `checkbox_checked_color`, `checkbox_unchecked_color` | `[1, 1, 1, 1]`, tinting the icon |
| `CheckButton` | icon | `checked`, `unchecked` | `builtin:toggle_on`, `builtin:toggle_off`: a 32 x 16 switch with its knob at the right, or dark at the left |
| | icon | `checked_disabled`, `unchecked_disabled` | `builtin:toggle_on_disabled`, `builtin:toggle_off_disabled` |
| | icon | `checked_mirrored`, `unchecked_mirrored`, `checked_disabled_mirrored`, `unchecked_disabled_mirrored` | the same switches the other way round, for right to left (`builtin:toggle_on_mirrored` and so on) |
| | color | `button_checked_color`, `button_unchecked_color` | `[1, 1, 1, 1]`, tinting the icon |
| `OptionButton` | style | `normal`, `hover`, `pressed`, `disabled` and their `_mirrored` boxes | as `Button`'s, with `content_margin` 8 left and right |
| | style | `focus` | as `Button`'s |
| | icon | `arrow` | `builtin:option_button_arrow`: a 12-pixel chevron pointing down, at the end of the button |
| | constant | `arrow_margin` | `4`, between the arrow and the button's edge |
| | constant | `modulate_arrow` | `0`; 1 tints the arrow with the font color |
| | font, font size, color, constant | `font`, `font_size`, `font_*_color`, `h_separation`, `outline_size` | as `Button`'s |
| `MenuButton` | style, font, font size, color, constant | | as `Button`'s, but `font_disabled_color` is `[1, 1, 1, 0.3]` |
| `PopupMenu` | style | `panel` | flat `[0.25, 0.25, 0.25, 1]`, `corner_radius` 3, `content_margin` 4, a `[0.175, 0.175, 0.175, 1]` border 2 wide |
| | style | `hover` | flat `[0.4, 0.4, 0.4, 1]`, `corner_radius` 3, `content_margin` 4: behind the item focused |
| | style | `separator`, `labeled_separator_left`, `labeled_separator_right` | a `[0.5, 0.5, 0.5, 1]` line 1 thick, `content_margin` 4 left and right |
| | icon | `checked`, `unchecked`, `radio_checked`, `radio_unchecked` and their `_disabled` ones | as `CheckBox`'s |
| | font, font size | `font`, `font_size`, `font_separator`, `font_separator_size` | The built-in font, `16` |
| | color | `font_color`, `font_hover_color`, `font_separator_color` | `[0.875, 0.875, 0.875, 1]` |
| | color | `font_disabled_color` | `[0.4, 0.4, 0.4, 0.8]` |
| | color | `font_outline_color`, `font_separator_outline_color` | `[0, 0, 0, 1]` |
| | constant | `v_separation`, `h_separation` | `4`, between items, and between an item's check box, icon and text |
| | constant | `indent` | `10`, pixels per `indent` of an item |
| | constant | `item_start_padding`, `item_end_padding` | `2` |
| | constant | `icon_max_width` | `0`, no limit |
| | constant | `outline_size`, `separator_outline_size` | `0` |
| `TooltipPanel` | style | `panel` | flat `[0, 0, 0, 0.5]`, `corner_radius` 3, `content_margin` 8 left and right, 2 top and bottom |
| `TooltipLabel` | font, font size | `font`, `font_size` | as `Label`'s |
| | color | `font_color` | `[0.875, 0.875, 0.875, 1]` |
| | color, constant | `font_shadow_color`, `font_outline_color`, `shadow_offset_x`, `shadow_offset_y`, `outline_size` | as `Label`'s |
| `HSlider`, `VSlider` | style | `slider` | flat `[0.1, 0.1, 0.1, 0.6]`, `corner_radius` 4, `content_margin` 4: the track |
| | style | `grabber_area`, `grabber_area_highlight` | flat `[1, 1, 1, 0.4]` and `[1, 1, 1, 0.75]`, the same shape: the track up to the grabber |
| | icon | `grabber`, `grabber_highlight`, `grabber_disabled` | `builtin:slider_grabber`, `builtin:slider_grabber_hl`, `builtin:slider_grabber_disabled`: a 16-pixel white circle at 0.75, 1 and 0.37 |
| | icon | `tick` | `builtin:hslider_tick` (4 x 8) or `builtin:vslider_tick` (8 x 4) |
| | constant | `center_grabber` | `0`; 1 lets the grabber's middle reach the ends |
| | constant | `grabber_offset`, `tick_offset` | `0`, pixels to move the grabber and ticks across the track |
| | constant | `grabber_max_size`, `tick_max_size` | `0`, no limit: the largest the icon is drawn, keeping its aspect |
| `LineEdit` | style | `normal` | flat `[0.1, 0.1, 0.1, 0.6]`, `corner_radius` 3, `content_margin` 4, a `[0, 0, 0, 0.6]` bottom border 2 wide |
| | style | `read_only` | flat `[0.1, 0.1, 0.1, 0.3]`, the same shape, a `[0, 0, 0, 0.3]` bottom border |
| | style | `focus` | as `Button`'s |
| | font, font size | `font`, `font_size` | The built-in font, `16` |
| | color | `font_color`, `font_selected_color` | `[0.875, 0.875, 0.875, 1]`, `[1, 1, 1, 1]` |
| | color | `font_uneditable_color` | `[0.875, 0.875, 0.875, 0.5]` |
| | color | `font_placeholder_color` | `[0.875, 0.875, 0.875, 0.6]` |
| | color | `font_outline_color` | `[0, 0, 0, 1]` |
| | color | `caret_color` | `[0.95, 0.95, 0.95, 1]` |
| | color | `selection_color` | `[0.5, 0.5, 0.5, 1]` |
| | color | `clear_button_color`, `clear_button_color_pressed` | `[0.875, 0.875, 0.875, 1]`, `[1, 1, 1, 1]` |
| | constant | `minimum_character_width` | `4` |
| | constant | `outline_size` | `0` |
| | constant | `caret_width` | `1` |
| `TextEdit` | style | `normal`, `read_only`, `focus` | As `LineEdit`'s |
| | font, font size | `font`, `font_size` | The built-in font, `16` |
| | color | `font_color` | `[0.875, 0.875, 0.875, 1]` |
| | color | `font_selected_color` | `[0, 0, 0, 0]`: selected text keeps its color |
| | color | `font_readonly_color` | `[0.875, 0.875, 0.875, 0.5]` |
| | color | `font_placeholder_color` | `[0.875, 0.875, 0.875, 0.6]` |
| | color | `font_outline_color`, `caret_background_color` | `[0, 0, 0, 1]` |
| | color | `caret_color` | `[0.875, 0.875, 0.875, 1]` |
| | color | `selection_color` | `[0.5, 0.5, 0.5, 1]` |
| | color | `current_line_color` | `[0.25, 0.25, 0.26, 0.8]` |
| | color | `word_highlighted_color` | `[0.5, 0.5, 0.5, 0.25]` |
| | color | `search_result_color` | `[0.3, 0.3, 0.3, 1]` |
| | color | `search_result_border_color` | `[0.3, 0.3, 0.3, 0.4]` |
| | color | `background_color` | `[0, 0, 0, 0]`, none |
| | constant | `line_spacing` | `4` |
| | constant | `outline_size` | `0` |
| | constant | `caret_width` | `1` |
| | constant | `wrap_offset` | `10`, pixels left free at the right of wrapped rows |
| `CodeEdit` | icon | `breakpoint`, `bookmark`, `executing_line` | Godot's, built in |
| | color | `breakpoint_color` | `[0.9, 0.29, 0.3, 1]` |
| | color | `bookmark_color` | `[0.5, 0.64, 1, 0.8]` |
| | color | `executing_line_color` | `[0.98, 0.89, 0.27, 1]` |
| | color | `line_number_color` | `[0.67, 0.67, 0.67, 0.4]` |
| | color | `line_length_guideline_color` | `[0.3, 0.5, 0.8, 0.1]` |
| | color | `word_highlighted_color` | `[0.8, 0.9, 0.9, 0.15]` |
| | others | | As `TextEdit`'s |
| `HScrollBar`, `VScrollBar` | style | `scroll` | flat `[0.1, 0.1, 0.1, 0.6]`, `corner_radius` 10, `content_margin` 4 across it |
| | style | `scroll_focus` | as `Button`'s `focus` |
| | style | `grabber` | flat `[1, 1, 1, 0.4]`, `corner_radius` 10, `content_margin` 4 |
| | style | `grabber_highlight`, `grabber_pressed` | the same shape, `[1, 1, 1, 0.75]` while hovered and `[0.75, 0.75, 0.75, 0.75]` while dragged |
| | icon | `increment`, `increment_highlight`, `increment_pressed`, `decrement`, `decrement_highlight`, `decrement_pressed` | `""`: no arrows |
| | constant | `padding_top`, `padding_bottom` (`HScrollBar`), `padding_left`, `padding_right` (`VScrollBar`) | `0` |
| `ScrollContainer` | style | `panel` | empty: it draws nothing behind its children |
| | style | `focus` | a `[1, 1, 1, 0.75]` border 2 wide, `expand_margin` 4, no center, drawn with `draw_focus_border` |
| | constant | `scrollbar_margin_left`, `scrollbar_margin_top`, `scrollbar_margin_right`, `scrollbar_margin_bottom` | `-1`: the `panel` style box's margin on that side; the gap between the bars and its edges |
| | constant | `scrollbar_h_separation`, `scrollbar_v_separation` | `0`, between the vertical or horizontal bar and the children |
| `BoxContainer` (`HBoxContainer`, `VBoxContainer`) | constant | `separation` | `4` |
| `GridContainer`, `FlowContainer` (`HFlowContainer`, `VFlowContainer`) | constant | `h_separation`, `v_separation` | `4` |
| `MarginContainer` | constant | `margin_left`, `margin_top`, `margin_right`, `margin_bottom` | `0` |

`Control`, `ColorRect`, `TextureRect`, `NinePatchRect` and `CenterContainer` have no theme items.
An entity with several UI components has the items of each.

### Themes

A theme is a `.theme.json` file of items by control type, as Godot's `Theme` resource
(`slash-cli schema theme`):

```json
{
  "format": 1,
  "default_font_size": 18,
  "types": {
    "Header": {
      "base_type": "Label",
      "font_sizes": {"font_size": 24}
    },
    "Label": {
      "colors": {"font_color": [0.9, 0.9, 0.95, 1]}
    },
    "PanelContainer": {
      "styles": {
        "panel": {"bg_color": [0.12, 0.13, 0.18, 0.95], "content_margin": 16, "corner_radius": 10}
      }
    }
  }
}
```

| Field | Meaning |
| --- | --- |
| `format` | Must be 1 |
| `default_font` | The font of every type in this theme that names none: a `.ttf`, `.otf` or `.font.json` file |
| `default_font_size` | The font size of every type in this theme that names none |
| `types` | Items by type: `colors`, `constants`, `fonts`, `font_sizes`, `icons` and `styles`, each by name |

- A type is a control type, with Godot's class names (`Label`, `VBoxContainer`, `Container`,
  `Range` and so on), or a variation: a name with a `base_type`, which can be another
  variation. A control whose `theme_type_variation` names one uses its items first, then its
  base types', then its own type's.
- A type's items apply to the types built on it: `BoxContainer`'s to `HBoxContainer` and
  `VBoxContainer`, `Container`'s to every container, `Control`'s to every control.
- A control looks each item up in its `theme_override_*` fields, then in the themes on its own
  and its parents' `Control.theme`, nearest first, then in the project's `gui.theme`
  ([project.json](project-format.md)), then in Godot's default theme. In each theme a type's
  own item comes before the theme's `default_font` and `default_font_size`.
- `scene validate` checks a theme file for unknown types and item names (with the closest
  ones), base types that lead nowhere or round in a loop, style boxes, icons and font paths. A
  theme that doesn't load, and a `theme_type_variation` no theme defines, are reported by
  `ui.layout` and `scene validate` on the control.
- Themes reload when their files change, as every asset does.

### Style boxes

A style box is what a panel or a bar draws behind its content, and the margins its content
keeps, as Godot's `StyleBox`. In a theme's `styles` or a control's `theme_override_styles` it is
an object with a `type` and Godot's property names:

| Field | Types | Default | Meaning |
| --- | --- | --- | --- |
| `type` | | `"flat"` | `flat` (Godot's `StyleBoxFlat`), `texture` (`StyleBoxTexture`), `line` (`StyleBoxLine`) or `empty` (`StyleBoxEmpty`) |
| `content_margin_left`, `_top`, `_right`, `_bottom` | all | `-1` | Room kept for the content on each side; -1 takes the style's own: its border, or its texture margin |
| `expand_margin_left`, `_top`, `_right`, `_bottom` | flat, texture | `0` | Drawn this far outside the rect |
| `draw_center` | flat, texture | `true` | Draw the middle, not only the border or the nine-patch's edges |
| `bg_color` | flat | `[0.6, 0.6, 0.6, 1]` | The fill |
| `border_width_left`, `_top`, `_right`, `_bottom` | flat | `0` | Border widths in pixels |
| `border_color` | flat | `[0.8, 0.8, 0.8, 1]` | |
| `border_blend` | flat | `false` | Blend the border into the fill |
| `corner_radius_top_left`, `_top_right`, `_bottom_right`, `_bottom_left` | flat | `0` | Corner radii in pixels, shrunk to fit as Godot's |
| `corner_detail` | flat | `8` | Points along each corner, 1 to 20 |
| `shadow_color`, `shadow_size`, `shadow_offset` | flat | `[0, 0, 0, 0.6]`, `0`, `[0, 0]` | A shadow that fades out over `shadow_size` pixels |
| `skew` | flat | `[0, 0]` | Slants it: an x of 1 moves the top edge right by half the height and the bottom edge left, a y of 1 the left edge down and the right edge up |
| `anti_aliasing`, `anti_aliasing_size` | flat | `true`, `1` | Smooth the edges of rounded or skewed boxes over this many pixels |
| `texture` | texture | | Project path of a PNG, BMP or KTX2 file |
| `region_rect` | texture | `[0, 0, 0, 0]` | The part of the texture to use; a zero size uses all of it |
| `texture_margin_left`, `_top`, `_right`, `_bottom` | texture | `0` | Pixels on each side that keep their size, as `NinePatchRect`'s |
| `axis_stretch_horizontal`, `axis_stretch_vertical` | texture | `"stretch"` | `stretch`, `tile` or `tile_fit` |
| `modulate_color` | texture | `[1, 1, 1, 1]` | Tints the texture |
| `color` | line | `[0, 0, 0, 1]` | The line's color |
| `thickness` | line | `1` | The line's width in pixels; half of it is the style's own margin on each side across the line |
| `grow_begin`, `grow_end` | line | `1` | Pixels the line reaches before the rect's start and past its end |
| `vertical` | line | `false` | Run down the rect's left side instead of along its top |

- `content_margin`, `expand_margin`, `border_width`, `corner_radius` and `texture_margin` set all
  four sides or corners at once; the named ones override them.
- Flat boxes are drawn as Godot's `StyleBoxFlat::draw` draws them: rings of triangles round
  each corner, a feathered edge where anti-aliasing is on, and the shadow under them. A box
  with no corners, border, shadow or skew is one rect.
- A field its type doesn't use is an error, as is a texture box with no texture.
  [`ui.theme`](commands.md#uitheme) returns style boxes in this form, with only the fields that
  differ from the defaults.

### Fonts

A font is a `.ttf` or `.otf` file (scalable TrueType or OpenType), or a `.font.json` file that
names one and says how it draws, as Godot's font import options (`slash-cli schema font`):

```json
{
  "font": "fonts/roboto.ttf",
  "format": 1,
  "multichannel_signed_distance_field": true
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `format` | | Must be 1 |
| `font` | | Project path of the `.ttf` or `.otf` file |
| `antialiasing` | `"grayscale"` | `grayscale` for smooth edges, `none` for pixel art fonts |
| `hinting` | `"light"` | Snap outlines to pixels: `none`, `light` (vertically only) or `normal` |
| `multichannel_signed_distance_field` | `false` | Draw from one distance field per glyph, made once, which stays sharp at any size, scale and outline |
| `msdf_pixel_range` | `16` | How far from the outline the field reaches, in pixels at `msdf_size`; outlines are at most half of it |
| `msdf_size` | `48` | The font size the field is made at |
| `fallbacks` | `[]` | Fonts (`.ttf`, `.otf` or `.font.json` files) for the characters this one lacks, tried in order; their own fallbacks follow theirs |
| `allow_system_fallback` | `true` | Then take characters no fallback has from fonts installed on the machine |
| `language_support` | `{}` | Whether the font is for a language, by BCP 47 tag, such as `{"ja": true, "*": false}`; `*` is any other |
| `script_support` | `{}` | Whether the font is for a script, by ISO 15924 code, such as `{"Hani": true}`; `*` is any other |

- Each character is drawn by the first font that has it, as Godot's font fallback picks: the
  font, then its `fallbacks`, those that say they support the text's script and language first,
  then those for its script, then the rest, each list in order. A font that doesn't list a
  language or script supports it. The text's language is the label's or button's `language`;
  its script is the character's own.
- Where no fallback has a character and both the font and the project allow it
  (`gui.allow_system_fallback` in [project.json](project-format.md)), it comes from a font
  installed on the machine: the one CoreText (macOS, iOS), DirectWrite (Windows), fontconfig
  (Linux) or the system font list (Android) picks for it. Other machines may lack it, and web
  exports have none, so `ui.layout` and `text.measure` count such characters in
  `system_glyphs`. Characters no font has draw as the first font's missing-glyph box and are
  counted in `missing_glyphs`.
- Lines are as tall as the tallest font drawn in them.
- Glyph images are made the first time they are drawn, for each font, size and outline (once
  per glyph with a distance field), and packed into atlas textures.
- A font that doesn't load is reported by `ui.layout` and `scene validate`, and the label draws
  with the built-in font.
- Fonts reload when their files change, as every asset does.

## Audio

Sound follows Godot's `AudioServer` and its players. An `AudioStreamPlayer` plays a sound the
same wherever its entity is; an `AudioStreamPlayer2D` plays at its entity's `Transform2D`, heard
from the current `AudioListener2D`, and an [`AudioStreamPlayer3D`](#3d-sound) at its
`Transform3D`, heard from the current `AudioListener3D` or camera. Each plays through a bus of
the project's [bus layout](#bus-layouts), and every bus mixes into Master, which is what plays.
[examples/audio](../../examples/audio/) has music, footsteps and both kinds of bus change,
[examples/audio-3d](../../examples/audio-3d/) a car passing a listener and a cave's reverb, and
[examples/surround-sound](../../examples/surround-sound/) a bee flying round a listener in 7.1.

```json
"music": {
  "components": {
    "AudioStreamPlayer": {"autoplay": true, "bus": "Music", "stream": "audio/theme.ogg"}
  }
}
```

- Sound files load as assets and reload when they change: `.wav` files (8, 16, 24 or 32-bit
  integer or 32-bit float samples, mono or stereo) decode when loaded, as Godot imports them,
  and `.ogg` (Vorbis) and `.mp3` files decode as they play. More than two channels mix down to
  two.
- Each tick, after every other system, the `audio` system (priority 1200) starts and stops
  players, then mixes the tick's share of frames at the project's `audio.mix_rate` (735 frames
  a tick at 60 ticks a second and 44100 Hz). Mixing follows the ticks rather than the sound
  card, so the same ticks always make the same sound, and headless runs mix too.
- As in Godot, `autoplay` plays when the player is first seen and `playing` plays or stops when
  it changes; [`audio.play`](commands.md#audioplay) and `audio.stop` (and `audio::play` and
  `audio::stop` in C++) do so at the next tick. Playing again restarts the sound, or with
  `max_polyphony` over 1, adds a voice and stops the oldest past the limit. Music, generators,
  polyphonic streams and microphones are monophonic, as in Godot: playing again always starts
  them over. A new `stream` stops the old one.
- When a player's last sound ends, it sets `playing` to false and emits a `finished` event with
  `{"component": "AudioStreamPlayer"}` (or `AudioStreamPlayer2D`, `AudioStreamPlayer3D`) as its
  data. Music also emits
  a [`clip_changed`](#clip-changes) event for each clip it starts.
- An `AudioStreamPlayer2D` fades and pans as Godot's `_update_panning`: its volume is times
  (1 - distance / `max_distance`) ^ `attenuation`, and it pans by its offset from the listener
  over the viewport's width, times `panning_strength` and half of `audio.panning_strength_2d`.
- Volume changes ramp over one mix step of 512 frames, and a stopped sound fades out over one,
  so neither clicks. Sound starts 64 frames late, Godot's lookahead for those fades.
- The voices are not saved: scenes, snapshots, undo and dry runs keep only the components'
  fields. Plays and stops a failed command or dry run asked for are dropped, as are its bus
  changes.

### Buses by area

As Godot's `AudioStreamPlayer2D::_get_actual_bus`, a 2D player inside an `Area2D` with
`audio_bus_override` plays through the area's `audio_bus_name` instead of its own `bus`, when
the area's `collision_layer` shares a bit with the player's `area_mask`. The audio system checks
each tick, after physics, at the player's position, so a sound moves to the cave's reverb bus as
it walks in and back as it walks out, fading out of the bus it left over one mix step. With several such areas, the
first by ID wins (Godot takes the physics server's order). `audio.state` shows the bus each
player plays through. Godot's reverb bus overrides belong to [3D areas](#3d-sound).

```json
"cave": {
  "components": {
    "Area2D": {"audio_bus_name": "Cave", "audio_bus_override": true},
    "CollisionShape2D": {"size": [200, 120]},
    "Transform2D": {"position": [400, 180]}
  }
}
```

### 3D sound

An `AudioStreamPlayer3D` plays at its entity's `Transform3D`, heard from the current
`AudioListener3D`, as Godot's `AudioStreamPlayer3D` hears it, in stereo or
[surround](#surround-sound). Distances are meters.

```json
"engine": {
  "components": {
    "AudioStreamPlayer3D": {"autoplay": true, "bus": "Sfx", "doppler_tracking": "physics_step", "max_distance": 60, "stream": "audio/engine.wav", "unit_size": 4},
    "Transform3D": {"position": [0, 0.5, -20]}
  }
}
```

| Component | Field | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| `AudioStreamPlayer3D` | `stream`, `volume_db`, `pitch_scale`, `playing`, `autoplay`, `stream_paused`, `max_polyphony`, `bus`, `parameters` | | | As `AudioStreamPlayer`'s |
| | `attenuation_model` | `std::string` | `"inverse_distance"` | How it fades with distance: `inverse_distance` (as 1 / distance), `inverse_square_distance`, `logarithmic` (20 dB each time the distance is multiplied by e) or `disabled` |
| | `unit_size` | `float` | `10` | Meters from the listener at which it plays at `volume_db`; larger carries farther |
| | `max_db` | `float` | `3` | The loudest it gets close up, -24 to 6 dB |
| | `max_distance` | `float` | `0` | Meters from the listener beyond which it is silent, fading to it linearly; 0 for no limit |
| | `panning_strength` | `float` | `1` | Times the project's `audio.panning_strength_3d`; 0 for no panning |
| | `area_mask` | `int` | `1` | `Area3D` layers, as bits, whose bus and reverb bus take it while it is inside one; 0 ignores areas |
| | `emission_angle_enabled` | `bool` | `false` | Muffle it for a listener outside a cone down the entity's -z |
| | `emission_angle_degrees` | `float` | `45` | The cone's half angle, 0.1 to 90 |
| | `emission_angle_filter_attenuation_db` | `float` | `-12` | How much the high shelf takes off outside the cone, -80 to 0 dB |
| | `attenuation_filter_cutoff_hz` | `float` | `5000` | Where the high shelf that muffles it starts, 1 to 20500 Hz |
| | `attenuation_filter_db` | `float` | `-24` | How much the high shelf takes off as distance silences it, -80 to 0 dB |
| | `doppler_tracking` | `std::string` | `"disabled"` | `idle_step` or `physics_step`: measure how fast it moves, for the Doppler effect |
| `AudioListener3D` | `current` | `bool` | `false` | Hear 3D sounds from this entity's `Transform3D`, facing its -z |
| | `doppler_tracking` | `std::string` | `"disabled"` | As the player's, for the listener's own speed |

- The listener is the first `AudioListener3D` by ID with `current` set, else the first one by
  ID (Godot makes the first listener in a viewport current), else the current `Camera3D`, whose
  `doppler_tracking` counts as a listener's. With neither, 3D sounds are silent; Godot always
  has a viewport's camera to hear through.
- Each tick, after physics, the audio system places every 3D player, as Godot's
  `_update_panning` does each frame. Its volume is `volume_db` faded by the attenuation model at
  its distance, at most `max_db`, then times (1 - distance / `max_distance`) when that is set.
  Each side gets the square root of (1 -+ *s* cos *a*) / 2 of that, where *a* is the angle from
  the listener's right to the sound, height left out, and *s* grows with `panning_strength`
  times `audio.panning_strength_3d`, as Godot's `_calc_output_vol_stereo`. In surround it pans
  over every speaker instead, by Godot's Spcap.
- As distance fades it, a high shelf above `attenuation_filter_cutoff_hz` takes off the faded
  share of `attenuation_filter_db`, so far sounds are muffled, and with an emission angle
  `emission_angle_filter_attenuation_db` more while the listener is outside the cone. That is the
  shelf's gain; as in Godot, whose filter squares it, the highest frequencies lose twice as many
  decibels.
- Past `max_distance` it keeps playing, unheard, and its filter and pitch stay as they were, as
  Godot's do.
- With `doppler_tracking`, the player's speed is measured over its last four moves within a
  fifth of a second, as Godot's `VelocityTracker3D` (counting ticks in both modes), less the
  listener's when it tracks too. The pitch is `pitch_scale` times 343 / (343 + *v*), where *v* is
  that speed along the line from the listener, positive going away, so it rises as the sound
  comes and falls as it goes, within 1/8 and 8.
- `audio.state` shows a 3D player's `pitch_scale` with Doppler, its `filter_db` and any `sends`.

As Godot's `_get_overriding_area`, a 3D player inside an `Area3D` that overrides the bus or has
a reverb bus, and whose `collision_layer` shares a bit with its `area_mask`, follows the first
such area by ID:

- With `audio_bus_override`, it plays through the area's `audio_bus_name`.
- With `reverb_bus_enabled`, it also sends `reverb_bus_amount` of itself to `reverb_bus_name`.
  `reverb_bus_uniformity` blends that send from the sound's direction toward the area's nearest
  point to the listener, so a room's echo surrounds a listener inside it.
- As in Godot, an area with a reverb bus and no bus override plays the sound on the reverb bus
  alone, and an area with uniformity whose nearest point is farther from the listener than a
  player's `max_distance` silences it. A bus the layout doesn't have is Master.

```json
"cave": {
  "components": {
    "Area3D": {"audio_bus_name": "Cave", "audio_bus_override": true, "reverb_bus_amount": 0.6, "reverb_bus_enabled": true, "reverb_bus_name": "Echo"},
    "CollisionShape3D": {"size": [12, 6, 12]},
    "Transform3D": {"position": [0, 3, -30]}
  }
}
```

### Surround sound

Sound mixes for the speakers of the project's `audio.speaker_mode`, as Godot's
`AudioServer::SpeakerMode`: `stereo`, `surround_31`, `surround_51` or `surround_71`. Its default,
`auto`, is the output device's (5.1 for six channels, 7.1 for eight or more, stereo otherwise),
and stereo where nothing plays the sound, as in headless runs and sessions. SDL folds a mix for
more speakers than the device has, so a 7.1 game plays on headphones too.
[examples/surround-sound](../../examples/surround-sound/) has a bee flying round the listener.

```json
"audio": {"speaker_mode": "surround_71"}
```

- As in Godot, each bus mixes a buffer per speaker pair: front left and right, then center and
  LFE (3.1 and up), rear left and right (5.1 and up) and side left and right (7.1). Each pair
  runs its own copy of the bus's [effects](#effects), and a compressor's `sidechain` listens to
  the same pair of its bus. Capture, record and spectrum effects that `audio.*` commands read
  are the front pair's.
- An `AudioStreamPlayer`'s `mix_target` picks its speakers: the front pair, every speaker or the
  center and LFE, at its `volume_db`. Godot plays the LFE at full volume whatever `volume_db`
  is; here it follows it, as a 3D player's does.
- An `AudioStreamPlayer2D` plays on the front pair alone, as Godot's.
- An `AudioStreamPlayer3D` pans over the speakers by Godot's Spcap (speaker-placement
  correction amplitude panning): each speaker's gain is 0.5 (1 + cos *a*) ^ *t*, where *a* is
  the angle from the speaker to the sound, height included, over how many speakers face the
  same way, scaled so the gains keep the sound's power. The tightness *t* is twice
  `panning_strength` times `audio.panning_strength_3d`, so 1 by default; the speakers face
  45 degrees off ahead and behind, straight ahead (center) and to each side. The LFE gets all of
  it, faded with distance as every speaker is. An `Area3D`'s reverb with uniformity spreads over
  every speaker alike, as Godot's.
- `audio.state` adds `speaker_mode` and each player's `speakers`, and
  [`audio.capture`](commands.md#audiocapture) each speaker's level and `.wav` files with a
  channel for each speaker.

### Bus layouts

A `.bus_layout.json` file, named by `audio.bus_layout` in [project.json](project-format.md),
lists the buses, as Godot's `default_bus_layout.tres`. With none, there is one Master bus.

```json
{
  "buses": [
    {"name": "Master"},
    {"effects": [{"room_size": 0.6, "type": "reverb", "wet": 0.3}], "name": "Music", "volume_db": -6},
    {"name": "Footsteps", "send": "Sfx"},
    {"effects": [{"ceiling_db": -1, "type": "hard_limiter"}], "name": "Sfx"}
  ],
  "format": 1
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `name` | | What players and sends call it; the first bus is `Master` and names are unique |
| `volume_db` | `0` | Gain in dB after its effects, -80 to 24 |
| `mute` | `false` | Silences it |
| `solo` | `false` | While any bus is solo, only solo buses and the buses they send to play |
| `bypass_effects` | `false` | Skips its effects |
| `send` | `"Master"` | The bus it mixes into, which must come before it in the list; Master sends nowhere |
| `effects` | `[]` | Up to 16 effects it runs in order on what was mixed into it |

- At most 64 buses. A bus goes quiet (and its effects stop) after two seconds under -60 dB with
  nothing playing into it, as Godot's, so a reverb's tail still plays out.
- The layout reloads when its file changes, which starts the buses over. A layout that doesn't
  load is logged, and the last one keeps playing.
- [`audio.set_bus`](commands.md#audioset_bus) changes a bus's volume, switches and effects'
  `enabled` while the game runs; the file stays as it is.

### Effects

Each effect is a `type` and its settings, by Godot's property names, plus `enabled` (default
`true`). Settings left out take Godot's defaults, listed here; `slash-cli schema bus_layout`
gives each one's range and meaning.

| Type | Godot's | Settings and defaults |
| --- | --- | --- |
| `amplify` | AudioEffectAmplify: changes the volume | `volume_db` 0 |
| `low_pass_filter` | AudioEffectLowPassFilter: cuts frequencies above the cutoff | `cutoff_hz` 2000, `resonance` 0.5, `db` 6 (6, 12, 18 or 24 dB per octave) |
| `high_pass_filter` | AudioEffectHighPassFilter: cuts frequencies below the cutoff | as `low_pass_filter` |
| `band_pass_filter` | AudioEffectBandPassFilter: keeps frequencies near the cutoff | as `low_pass_filter` |
| `notch_filter` | AudioEffectNotchFilter: cuts frequencies near the cutoff | as `low_pass_filter` |
| `band_limit_filter` | AudioEffectBandLimitFilter: keeps a band from the cutoff | as `low_pass_filter` |
| `low_shelf_filter`, `high_shelf_filter` | AudioEffectLowShelfFilter and HighShelfFilter: scale frequencies below or above the cutoff | as `low_pass_filter`, and `gain` 1 |
| `eq6`, `eq10`, `eq21` | AudioEffectEQ6, EQ10 and EQ21: 6, 10 or 21 bands | `band_db`: dB by band frequency, e.g. `{"1000_hz": -3}`; bands left out are 0 |
| `delay` | AudioEffectDelay: two echoes and repeating feedback | `dry` 1, `tap1_active` true, `tap1_delay_ms` 250, `tap1_level_db` -6, `tap1_pan` 0.2, `tap2_active` true, `tap2_delay_ms` 500, `tap2_level_db` -12, `tap2_pan` -0.4, `feedback_active` false, `feedback_delay_ms` 340, `feedback_level_db` -6, `feedback_lowpass` 16000 |
| `reverb` | AudioEffectReverb: the sound of a room | `predelay_msec` 150, `predelay_feedback` 0.4, `room_size` 0.8, `damping` 0.5, `spread` 1, `hipass` 0, `dry` 1, `wet` 0.5 |
| `chorus` | AudioEffectChorus: delayed, wavering copies | `voice_count` 2, `dry` 1, `wet` 0.5, `voices`: up to 4 of `{delay_ms, rate_hz, depth_ms, level_db, cutoff_hz, pan}`, with Godot's defaults for the first two |
| `phaser` | AudioEffectPhaser: notches that sweep | `range_min_hz` 440, `range_max_hz` 1600, `rate_hz` 0.5, `feedback` 0.7, `depth` 1 |
| `distortion` | AudioEffectDistortion: clips or shapes the waveform | `mode` `"clip"` (`clip`, `atan`, `lofi`, `overdrive`, `waveshape`), `pre_gain` 0, `keep_hf_hz` 16000, `drive` 0, `post_gain` 0 |
| `compressor` | AudioEffectCompressor: turns loud sound down | `threshold` 0, `ratio` 4, `gain` 0, `attack_us` 20, `release_ms` 250, `mix` 1, `sidechain`: a bus whose level drives it instead |
| `hard_limiter` | AudioEffectHardLimiter: keeps the sound under a ceiling | `ceiling_db` -0.3, `pre_gain_db` 0, `release` 0.1 |
| `panner` | AudioEffectPanner: moves the sound left or right | `pan` 0 |
| `stereo_enhance` | AudioEffectStereoEnhance: widens or narrows the stereo image | `pan_pullout` 1, `time_pullout_ms` 0, `surround` 0 |
| `pitch_shift` | AudioEffectPitchShift: changes the pitch and keeps the speed | `pitch_scale` 1 (0.01 to 16), `oversampling` 4 (4 to 32), `fft_size` 2048 (256, 512, 1024, 2048 or 4096) |
| `spectrum_analyzer` | AudioEffectSpectrumAnalyzer: measures frequencies, for [`audio.spectrum`](commands.md#audiospectrum) | `buffer_length` 2, `fft_size` 1024 |
| `capture` | AudioEffectCapture: holds what passes for game code to take | `buffer_length` 0.1 (seconds, 0.01 to 10) |
| `record` | AudioEffectRecord: records what passes, for [`audio.record`](commands.md#audiorecord) | `format` `"16_bits"` (`8_bits` or `16_bits`) |

A misspelled type is an error naming the closest one, and the type a slot names says which
settings it takes.

- `pitch_shift` is Stephan Bernsee's phase vocoder (`smbPitchShift`, under the Wide Open
  License), as in Godot: each FFT window of `fft_size` frames is analyzed at `oversampling`
  overlapping steps, so it adds `fft_size` frames of latency. At a `pitch_scale` of 1 it passes
  the sound through.
- `spectrum_analyzer`, `capture` and `record` pass the sound through unchanged. In C++,
  `audio::spectrum_analyzer`, `audio::capture_effect` and `audio::record_effect` give a slot's
  running effect (as Godot's `AudioServer::get_bus_effect_instance`), with Godot's
  `magnitude_for_frequency_range`, `get_buffer`, `frames_available`, `set_recording_active` and
  `recording` (a `Sound` to play). The spectrum analyzer answers from its newest window of
  `fft_size` * 2 frames; Godot keeps `buffer_length` seconds of them but reads only the newest
  too. Capture and record keep running while their bus is silent, so time has no gaps; the
  spectrum analyzer pauses with its bus. A recording's IMA ADPCM and QOA formats aren't
  offered: it stays in memory as samples.
- `audio.state` shows a capture's `frames_available` (and `discarded_frames`, when its buffer
  filled) and whether a record effect is `recording`, with the `seconds` it holds.

### Audio streams

A `.audio.json` file is one of eight kinds of stream, each Godot's: a sound with its import
settings, an `AudioStreamRandomizer` (one of several streams each time it plays), an
`AudioStreamPlaylist` (streams in turn), an `AudioStreamSynchronized` (streams together, as
layers), an `AudioStreamInteractive` ([music in clips](#interactive-music) that switch as the
game asks), an `AudioStreamGenerator` or an `AudioStreamPolyphonic`
([sound a game makes as it runs](#generators-and-polyphonic-streams)), or an
`AudioStreamMicrophone` ([what the microphone records](#microphone)). A player's `stream` can
name one, as can another file's streams and clips (except a microphone).
[examples/interactive-music](../../examples/interactive-music/) plays the music kinds,
[examples/generated-audio](../../examples/generated-audio/) generators and polyphonic streams and
[examples/microphone](../../examples/microphone/) a microphone.

```json
{"format": 1, "loop": true, "loop_offset": 4, "source": "music/theme.ogg"}
{"format": 1, "random_pitch": 1.1, "streams": [{"stream": "sfx/step1.wav"}, {"stream": "sfx/step2.wav"}]}
{"format": 1, "fade_time": 0.5, "playlist": [{"stream": "music/a.ogg"}, {"stream": "music/b.ogg"}]}
{"format": 1, "synchronized": [{"stream": "music/drums.ogg"}, {"stream": "music/strings.ogg", "volume_db": -80}]}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `format` | | Must be 1 |
| `source` | | The `.wav`, `.ogg` or `.mp3` file, for a sound |
| `loop_mode` | `"disabled"` | `.wav`: `disabled`, `forward`, `pingpong` or `backward` between `loop_begin` and `loop_end` |
| `loop_begin`, `loop_end` | `0`, `-1` | `.wav`: the frames the loop starts and ends at; -1 is the end of the sound |
| `loop` | `false` | `.ogg` and `.mp3`: play again from `loop_offset` seconds at the end |
| `loop_offset` | `0` | `.ogg` and `.mp3`: seconds into the sound a loop goes back to |
| `bpm` | `0` | Beats per minute, which playlists and interactive music time by; an `.ogg` or `.mp3` with `beat_count` too loops after that many beats instead of at the end |
| `beat_count` | `0` | Its length in beats at `bpm`, for loops, playlists and transitions |
| `bar_beats` | `4` | Beats in a bar, 2 to 32, for `next_bar` transitions |
| `streams` | | A randomizer's streams, each `{"stream": path, "weight": 1}` |
| `playback_mode` | `"random_no_repeats"` | How a randomizer picks: `random_no_repeats`, `random` or `sequential` |
| `random_pitch` | `1` | A randomizer's pitch scale varies up to this many times higher or lower |
| `random_volume_offset_db` | `0` | A randomizer's volume varies up to this many dB louder or quieter |
| `playlist` | | A playlist's streams, at most 64, each `{"stream": path}`, played in turn |
| `shuffle` | `false` | A playlist plays in a new random order each time round |
| `loop` | `true` | A playlist starts over after its last stream |
| `fade_time` | `0.3` | Seconds a playlist's stream fades out over the start of the next |
| `synchronized` | | A synchronized stream's layers, at most 32, each `{"stream": path, "volume_db": 0}` |
| `clips`, `initial_clip`, `transitions` | | [Interactive music](#interactive-music) |
| `generator`, `polyphonic` | | [Generators and polyphonic streams](#generators-and-polyphonic-streams) |

- A file is one kind, named by the field it has: `source`, `streams`, `playlist`,
  `synchronized`, `clips`, `generator` or `polyphonic`. It takes only that kind's fields; `bpm`,
  `beat_count` and `bar_beats` go with a sound or a playlist.
- A randomizer's choices, and a shuffled playlist's order, come from the player's entity ID and
  how many times it has played, so a scene makes the same picks every run.
- A playlist plays each stream for its `beat_count` beats at `bpm` if it has them, else to its
  end, then the next starts as that one fades out under it over `fade_time` seconds. It ignores
  the streams' own loops, and their pitch: as Godot's, it mixes them at their own speed. Its
  length is the sum of theirs.
- A synchronized stream starts every layer at once and ends when the last does. Each layer
  plays at its `volume_db`, which a player's `stream_<index>_volume_db` parameter changes
  (index from 0), ramping over 128 frames.

### Interactive music

As Godot's `AudioStreamInteractive`, interactive music is `clips`, one playing at a time, that a
player's `switch_to_clip` [parameter](#stream-parameters) switches between. `transitions` say
when a switch happens and how the clips fade, timed by the beats of the clip it leaves (its
`bpm`, `beat_count` and `bar_beats`, from its own `.audio.json` file).

```json
{
  "clips": [
    {"name": "explore", "stream": "music/explore.audio.json"},
    {"name": "combat", "stream": "music/combat.audio.json"},
    {"auto_advance": "enabled", "name": "victory", "next_clip": "explore", "stream": "music/victory.ogg"}
  ],
  "format": 1,
  "transitions": [
    {"fade_mode": "cross", "from": "explore", "from_time": "next_bar", "to": "combat"},
    {"from_time": "end", "to": "explore"}
  ]
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `clips` | | At most 63, each `{"name", "stream", "auto_advance", "next_clip"}`; names are unique |
| `auto_advance` | `"disabled"` | At a clip's end: `disabled` (it ends), `enabled` (go on to `next_clip`) or `return_to_hold` (go back to the clip a `hold_previous` transition left) |
| `next_clip` | | The clip `enabled` goes on to, by name; another clip |
| `initial_clip` | the first | The clip it starts with |
| `transitions` | | Each `{"from", "to", ...}`; leave out `from` or `to` for any clip |
| `from_time` | `"next_beat"` | When it leaves: `immediate`, `next_beat`, `next_bar` or `end` (of the clip's beats, or its length) |
| `to_time` | `"start"` | Where the next clip starts: `start`, `same_position` (as far in as the last was) or `previous_position` (where it was last left) |
| `fade_mode` | `"automatic"` | `disabled`, `in` (the next fades in), `out` (the last fades out), `cross` (both), or `automatic`: `out` when the next starts at its start, else `cross` |
| `fade_beats` | `1` | Beats a fade takes, or seconds if the clip it leaves has no `bpm` |
| `filler_clip` | | A clip played once between the two, at full volume |
| `hold_previous` | `false` | Remember the clip it left, for a `return_to_hold` clip, the clip it goes to included |

- A switch takes the first transition that fits, in Godot's order: from and to both named,
  then from named, then to named, then neither. With none, it waits for the next beat and fades
  the last clip out over one. A clip with no `bpm` switches at once for `next_beat` and
  `next_bar`.
- A clip that goes on by itself (`auto_advance`) always leaves at its end. Leaving at its
  `end` with no fade out, a clip that doesn't loop plays on past its beats to the end of its
  sound, under the next.
- Two transitions from and to the same clips, a `next_clip` without `"auto_advance":
  "enabled"`, and a name that is no clip are errors; a misspelled name gets a suggestion.
- A `hold_previous` transition straight into a `return_to_hold` clip goes back to the clip it
  left. Godot records the hold after reading the clip, so there the clip goes back to an older
  hold, or ends.

### Stream parameters

A player's `parameters` steer its stream as it plays, as the `parameters/` of Godot's players.
They reach each new playback before it starts, and every playback when they change.

| Parameter | Stream | Meaning |
| --- | --- | --- |
| `switch_to_clip` | Interactive music | The clip to switch to, by name. It follows the music: the audio system writes back the clip the music plays or is heading to, so it reads the clip a sting will go on to while the sting plays |
| `stream_<index>_volume_db` | Synchronized | Layer `<index>`'s volume in dB, from 0 |

- Asking for the clip the music plays, or is heading to, changes nothing; Godot would start it
  over.
- [`audio.state`](commands.md#audiostate) names the clip each player's music is playing.

### Clip changes

Each time a player's music starts a clip, the player emits a `clip_changed` event (Godot has no
such signal), so gameplay code can follow the music and agents can check it follows the game.
Interactive music emits one for each clip it starts, its first and a transition's filler
included, and a playlist for each of its streams. [`step`](commands.md#step) and
[`run.until`](commands.md#rununtil) list the events of the ticks they ran as `music`, and
[`playtest.run`](commands.md#playtestrun) sums them up over its runs.

| Field | Meaning |
| --- | --- |
| `component` | `AudioStreamPlayer`, `AudioStreamPlayer2D` or `AudioStreamPlayer3D` |
| `clip` | The clip's name, or the playlist stream's path; `""` once the music stops |
| `from` | The clip before it; left out as the music starts |
| `transition` | The index of the `transitions` entry the switch followed; left out with none |
| `filler` | `true` for a transition's `filler_clip` |
| `automatic` | `true` when it came by itself (`auto_advance`, a playlist moving on), not a switch asked for |
| `waited` | Seconds from the switch to the clip starting: the wait for its beat or bar, plus any filler; left out when it started at once |

```json
{"automatic":true,"clip":"explore","component":"AudioStreamPlayer","from":"victory","transition":2,"waited":1.003}
```

Waits are measured in mixed frames, rounded to milliseconds. They can be a few milliseconds off
the beat, since a sound's position counts the frames it has read ahead and the mixer works in
steps of 512 frames, as Godot's does; the same ticks always give the same waits.

### Generators and polyphonic streams

A `generator`, as Godot's `AudioStreamGenerator`, plays frames a game's code pushes as it runs:
a synthesizer, speech from a library, sound decoded from a network. A `polyphonic` stream, as
Godot's `AudioStreamPolyphonic`, plays streams a game starts on it, many at once on one player,
each with its own volume and pitch. [examples/generated-audio](../../examples/generated-audio/)
has both.

```json
{"format": 1, "generator": {"buffer_length": 0.1, "mix_rate": 22050}}
{"format": 1, "polyphonic": {"polyphony": 16}}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `generator.mix_rate_mode` | `"custom"` | The rate frames are pushed at: `output` (the project's `audio.mix_rate`), `input` (the rate microphone input is recorded at, which is the mix rate) or `custom` (`mix_rate`) |
| `generator.mix_rate` | `44100` | Frames per second pushed, 20 to 192000, with `custom` |
| `generator.buffer_length` | `0.5` | Seconds of frames it holds, 0.01 to 10; it holds the smallest power of two frames above that, less one, as Godot's ring buffer |
| `polyphonic.polyphony` | `32` | Streams that can play at once, 1 to 128 |

- Gameplay code gets a player's playback with `audio::stream_playback(world, entity)`, as
  Godot's `get_stream_playback`, and its `generator()` or `polyphonic()`. The playback exists
  from the tick after the player starts (the audio system starts voices after gameplay systems
  run) until its voice ends, so ask for it each tick.
- A generator's playback has Godot's `push_frame`, `push_buffer`, `can_push_buffer`,
  `frames_available`, `skips` and `clear_buffer`. Push each tick at least the frames the tick
  will mix (`mix_rate` / 60 at 60 ticks a second); a mix that runs out plays silence for the
  rest and counts a skip. Its frames are resampled to the mix rate as other sounds are, which
  delays them by 128 of its frames. It plays until stopped and has no length.
- A polyphonic playback has Godot's `play_stream` (a `Stream`, from seconds, volume in dB, pitch
  scale), `set_stream_volume`, `set_stream_pitch_scale`, `is_stream_playing` and `stop_stream`,
  with Godot's stream IDs (slot and count). A stream starts at the next mix, a stop fades it out
  over one mix step, and a new volume applies at the next mix. It plays until stopped, silent
  when no stream is going. Agents and `audio::play_stream` play a stream on one with
  [`audio.play`](commands.md#audioplay)'s `stream` at the next tick, starting the player if
  needed.
- [`audio.state`](commands.md#audiostate) shows a generator's free frames and skips, and the
  streams a polyphonic player has going.

### Microphone

A `microphone` stream, as Godot's `AudioStreamMicrophone`, plays what the microphone records. It
needs the project's [`audio.enable_input`](project-format.md), as Godot's
`audio/driver/enable_input`, and plays only as a player's own `stream`, not inside another
`.audio.json` file or with `audio.play`'s `stream`. As in Godot's microphone demo, a game
usually plays it on a muted bus whose effects (a `record`, `capture` or `spectrum_analyzer`)
hear the player without the speakers playing them back.
[examples/microphone](../../examples/microphone/) lights lanterns when the player sings.

```json
{"format": 1, "microphone": {}}
```

- A playing microphone player turns input on, which opens the microphone (`audio::SdlInput` in
  games and `slash-cli run`); input turns off, and the microphone closes, when no microphone
  player plays and the game hasn't turned it on. Without `enable_input` the player stops at
  once and a warning is logged.
- Each tick, before the tick is mixed, the audio system pushes that tick's frames from the input
  into the server's input buffer, which holds one second at the mix rate. A microphone plays
  them once 50 ms is recorded, Godot's delay; the mix runs up to a mix step ahead of the ticks,
  so the delay heard is a little under that.
- Gameplay code reads input too, with the server's Godot calls: `set_input_active` (turns input
  on for the game; fails without `enable_input`), `input_frames_available`,
  `input_buffer_length_frames` and `input_frames(n)`, which returns the next `n` frames or none
  when fewer are waiting. Frames pushed in a tick are there for gameplay systems the next tick.
- Headless there is no microphone, so the input is silent.
  [`audio.input`](commands.md#audioinput) (`audio::set_input_stream` in C++) plays a sound into
  the input from the next tick in place of the microphone, so agents and tests can speak into a
  game; `audio::set_input_device` gives the world another `audio::InputDevice`.
- [`audio.state`](commands.md#audiostate) shows whether input is on, what feeds it and its peak
  over the last tick.
- On Android, opening the microphone asks the player for the `RECORD_AUDIO` permission and waits
  for the answer. Exports of a project with `enable_input` ask for the microphone on every
  platform that needs it: see [`audio.input_usage_description`](project-format.md).

### Scores

A `.score.json` file is a piece of music written as text, so an agent can change one bar or one
instrument exactly instead of making the whole piece again (`slash-cli schema score`). Games
don't play scores: a score is the source that music is made from, and exports leave it out.
[`score.export`](commands.md#scoreexport) writes it as a Standard MIDI File to play or edit in
any music program, and [`score.import`](commands.md#scoreimport) reads one back.
[examples/music-scores](../../examples/music-scores/) has an eight-bar piece.

```json
{
  "bpm": 92,
  "format": 1,
  "key": "A minor",
  "sections": [{"bar": 0, "name": "intro"}, {"bar": 4, "name": "loop"}],
  "tracks": [
    {
      "bars": ["A1:1.5 A2:0.5 E2:1 A2", "F1:1.5 F2:0.5 C2:1 F2"],
      "instrument": "acoustic_bass",
      "name": "bass"
    },
    {
      "bars": ["", "kick+closed_hat:0.5 closed_hat snare+closed_hat closed_hat"],
      "instrument": "drums",
      "name": "drums",
      "velocity": 90
    }
  ]
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `format` | | Must be `1` |
| `bpm` | `120` | Beats per minute, 1 to 400 |
| `bar_beats` | `4` | Beats in a bar, 2 to 32 |
| `beat_unit` | `4` | The note a beat is, as a time signature's lower number: 2, 4, 8 or 16 |
| `key` | | Such as `"D minor"` or `"F# major"`: a tonic A to G with `#` or `b`, then `major` or `minor` |
| `bars` | the longest track's | Its length in bars, up to 1024 |
| `sections` | | Named parts in order, each `{"name", "bar"}` from its bar to the next one's |
| `tracks` | | Up to 64 parts, each with a unique `name` |
| `tracks[].instrument` | `"acoustic_grand_piano"` | One of General MIDI's 128 instruments in snake case, such as `"string_ensemble_1"`, `"electric_bass_finger"` or `"lead_square"`, or `"drums"` |
| `tracks[].volume_db` | `0` | Its volume, -80 to 0 |
| `tracks[].pan` | `0` | From -1 (left) to 1 (right) |
| `tracks[].velocity` | `100` | What its notes play at unless they say, 1 to 127 |
| `tracks[].bars` | | A line of notes per bar, `""` for a silent one |

Bars and sections count from 0, as the `bars` arrays do: section `{"bar": 4}` starts at
`/tracks/0/bars/4`. A bar's line is tokens separated by spaces, played one after another from
the start of the bar:

- A note is a letter A to G, then `#` or `b` (at most two), then an octave from -1 to 9: `C4` is
  middle C (MIDI note 60) and `A4` 440 Hz. A MIDI note number from 0 to 127 works too, and on a
  `"drums"` track so do General MIDI's drums by name: `acoustic_kick`, `kick`, `side_stick`,
  `snare`, `clap`, `electric_snare`, the toms (`low_floor_tom`, `high_floor_tom`, `low_tom`,
  `low_mid_tom`, `high_mid_tom`, `high_tom`), `closed_hat`, `pedal_hat`, `open_hat`, `crash`,
  `crash_2`, `ride`, `ride_2`, `ride_bell`, `china`, `splash`, `tambourine`, `cowbell`,
  `vibraslap`, the bongos, congas, timbales and agogos (`high_bongo`, `mute_high_conga`,
  `open_high_conga`, `low_conga`, `high_timbale`, `high_agogo` and their low ones), `cabasa`,
  `maracas`, `short_whistle`, `long_whistle`, `short_guiro`, `long_guiro`, `claves`,
  `high_wood_block`, `low_wood_block`, `mute_cuica`, `open_cuica`, `mute_triangle` and
  `open_triangle`.
- `+` joins notes into a chord: `C4+E4+G4`. `r` is a rest.
- `:` and a length in beats follow a note, chord or rest: `1`, `0.5` or `1/3` for triplets. A
  length carries on to the tokens after it in the voice, and a voice starts at one beat.
- `@` and a velocity from 1 to 127 come last, for that token only: `D4:1@110`.
- `;` starts another voice from the start of the bar, for notes that overlap: `"D2:4 ; D3 F3 A3
  D4"` holds the bass under four quarter notes.
- A voice's tokens must fit in the bar, but its last note may ring past the bar line.

`slash-cli scene validate` checks a score and counts its bars, tracks and notes; every error
names the bar's JSON Pointer and the token at fault, with the closest instrument or drum for a
misspelled one:

```json
{"errors":[{"message":"\"B5:0.5\": starts at beat 4, after the bar's 4 beats","path":"/tracks/0/bars/0"}],"valid":false}
```

## Replays

A `.replay.json` file is a run recorded with [`replay.record`](commands.md#replayrecord) or
`slash-cli run --record`, which [`replay.run`](commands.md#replayrun) plays back. Engines write
them; `slash-cli schema replay` prints the schema and `slash-cli scene validate` checks one.

```json
{
  "checks": [{"hash": "5f3a0c2b9d4e8a17", "tick": 120}],
  "end": {"tick": 150, "world": {"entities": {...}, "format": 1}},
  "format": 1,
  "prefabs": {"prefabs/crate.prefab.json": {"RigidBody2D": {}, "CollisionShape2D": {"size": [24.0, 24.0]}}},
  "scene": "main.scene.json",
  "start": {"next_id": 1, "tick": 0, "viewport": [320.0, 180.0], "world": {"entities": {...}, "format": 1}},
  "steps": [{"count": 150, "seconds": 0.016666666666666666, "tick": 0}],
  "timeline": [
    {"input": [{"key": "D", "pressed": true, "type": "key"}], "tick": 20},
    {"command": "component.set", "hash": "0b6d1e95c7a2f348", "params": {...}, "tick": 40},
    {"command": "undo", "next_id": 1, "tick": 70, "world": {"entities": {...}, "format": 1}}
  ]
}
```

| Field | Meaning |
| --- | --- |
| `format` | Must be 1 |
| `scene` | The scene file open when the recording started, or `""` |
| `start` | The world's `tick`, the number in its next generated ID (`next_id`), the UI `viewport` and the entities (`world`, as a scene file holds them) when the recording started |
| `prefabs` | The components of each prefab the entities extend, by path, so the file plays back after the prefab files change |
| `steps` | The ticks the host stepped outside commands, in runs: `tick` (the first), `count` and `seconds` (each tick's length) |
| `timeline` | What happened from outside the systems, in order, each before the step of its `tick` |
| `checks` | A hash of the entities before every 60th tick the host stepped |
| `end` | The world's `tick` and the entities when the recording stopped |

A timeline entry is one of:

- `input`: events the platform queued, as [`input.inject`](commands.md#inputinject) takes them, or
  `{"type": "release_all"}` when the window lost focus.
- `command` with `params`: a command to run again, and the `hash` of the entities after it.
- `command` with `world`: what the command left, to put back, with `next_id`, `end_tick` if it
  stepped, `path` for a scene it opened and `prefabs` whose files changed since the start. Undo,
  redo, `world.restore`, `scene.open`, `scene.new`, hot reloads (`"command": "asset.reload"`) and
  batches holding any of them are kept this way.
- `viewport`: the size the host set the UI viewport to, as a window does when it is resized.

A hash is FNV-1a 64 of the entities' compact JSON (the scene form, keys sorted), as 16 lowercase
hex digits. Ticks only grow: steps don't overlap, entries come in order and every tick is
between the start's and the end's.

## Tests

A `.test.json` file holds headless tests, which [`test.run`](commands.md#testrun) and
`slash-cli test` run. Keep them anywhere in the project (games made by `slash-cli init` use
`tests/`); `slash-cli schema test` prints the schema and `slash-cli scene validate` checks one.

```json
{
  "scene": "main.scene.json",
  "tests": {
    "crate_lands": {
      "steps": [
        {
          "command": "run.until",
          "expect": {"entities": ["crate"], "matched": true},
          "params": {"ticks": 600, "where": [{"component": "RigidBody2D", "field": "sleeping", "value": true}]}
        }
      ]
    },
    "sound_button_toggles": {
      "steps": [
        {"command": "ui.press", "params": {"id": "sound"}},
        {"command": "step"},
        {
          "command": "world.query",
          "expect": {"entities": ["sound"]},
          "params": {"where": [{"component": "Button", "field": "button_pressed", "value": true}]}
        }
      ]
    }
  }
}
```

| Field | Meaning |
| --- | --- |
| `scene` | The scene file every test starts from, relative to the project. With none, tests start from an empty world |
| `tests` | Tests by name. Each has `steps` and, to start from another scene than the file's, its own `scene` |

A step is a command call, as [`batch`](commands.md#batch) takes one: `command`, `params`, and
`expect`, what its reply must hold.

- An object needs only the members `expect` names, each holding what it says.
- An array needs the same number of items, each holding what its counterpart says.
- Any other value must be equal; numbers compare by value, so `2` holds `2.0`.

Each test opens its scene file in a session of its own, with no undo history, at tick 0, and
runs its steps in order. It fails at its first failed check: a step's command failing, a reply
not holding what the step expects, or a warning or an error logged while the scene opened or a
step ran. Tests can't write files: `scene.save`, `prefab.save`, `storyboard.edit`,
`replay.record`, and `frame.capture` or `audio.capture` with a `path` fail, and `export.run`
isn't there. A test also can't run `test.run`.
