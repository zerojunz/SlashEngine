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
| | `children` | Array of entity references | `[]` | Entities with a `Transform2D` placed in its space, in drawing order; each has one parent ([parent transforms](#parent-transforms)) |
| `Transform3D` | `position` | `Vec3` | `[0, 0, 0]` | Position in meters, as [x, y, z]; y points up |
| | `rotation` | `Quat` | `[0, 0, 0, 1]` | Unit quaternion [x, y, z, w]; each element from -1 to 1 |
| | `scale` | `Vec3` | `[1, 1, 1]` | Scale factor on each axis |
| `StoryRef` | `ids` | `std::vector<std::string>` | `[]` | The [storyboard](storyboard-format.md) entries this entity stands for, e.g. `["character/mira"]` |
| `Sprite2D` | `texture` | `std::string` | `""` | Project path of a PNG or BMP file; `""` draws nothing |
| | `centered` | `bool` | `true` | Center the texture on the position; `false` puts its top left corner there |
| | `offset` | `Vec2` | `[0, 0]` | Moves the texture from the position, in pixels |
| | `flip_h`, `flip_v` | `bool` | `false` | Mirror the texture left to right, or top to bottom, in place |
| | `hframes`, `vframes` | `int` | `1` | Columns and rows of frames in a sprite sheet, 1 to 16384 |
| | `frame` | `int` | `0` | The sheet's frame to draw, counted along rows from 0 |
| | `region` | `Vec4` | `[0, 0, 0, 0]` | Part of the texture to use, as [x, y, width, height] in pixels, before splitting it into frames; a zero size uses the whole texture |
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Color the texture and its children are multiplied by, as [r, g, b, a] from 0 to 1 |
| | `z_index` | `int` | `0` | Higher draws on top, -4096 to 4096, added to its parents'; equal values draw in tree order: the roots in the order the entities were made, each followed by its children |
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
| `AnimationPlayer` | `library` | `std::string` | `""` | Project path of a [`.anim.json`](#animations) file |
| | `animation` | `std::string` | `""` | Name of the animation to play; `""` plays nothing |
| | `playing` | `bool` | `true` | Advance the animation each tick |
| | `speed_scale` | `float` | `1` | Multiplies the playback speed; negative plays backwards |
| | `time` | `double` | `0` | Seconds into the animation, from 0 to its length |
| | `backwards` | `bool` | `false` | Play toward the start, as Godot's `play_backwards`; pingpong loops flip it at each end |
| | `targets` | `std::map<std::string, entt::entity>` | `{}` | The entity each track [`target`](#animations) names, by name; `null` leaves that target's tracks unplayed |
| `TileMapLayer` | `tile_set` | `std::string` | `""` | Project path of a [`.tileset.json`](#tile-sets) file |
| | `origin` | `Vec2i` | `[0, 0]` | The cell the first key of the first row stands for, as [column, row] |
| | `legend` | `std::map<std::string, std::string>` | `{}` | The tile each key in `rows` stands for, by name or as `"source:column,row"`, then `:<alternative>` and `:<turns>` if any ([turned cells](#alternative-tiles-and-turned-cells)) |
| | `rows` | `std::vector<std::string>` | `[]` | The [cells](#tile-map-layers), one string of keys per row from `origin` rightward; `.` is an empty cell |
| | `z_index` | `int` | `0` | Higher draws on top, -4096 to 4096, added to its parents'; layers and sprites draw in one order |
| | `visible` | `bool` | `true` | Draw it and its children |
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Color every tile and its children are multiplied by, as [r, g, b, a] from 0 to 1 |
| | `texture_filter` | `std::string` | `"inherit"` | `"linear"`, `"nearest"` (sharp pixel art) or `"inherit"`, which uses the project's `rendering.texture_filter` |
| | `light_mask` | `int` | `1` | [Lit](#2d-lights) by lights whose `range_item_cull_mask` shares a bit |
| | `collision_enabled` | `bool` | `true` | Its tiles' [collision polygons](#tile-collision) make it a static body in 2D physics |
| `PointLight2D` | `enabled` | `bool` | `true` | Shine |
| | `texture` | `std::string` | `""` | Project path of a PNG or BMP file whose color and alpha shape the light; `""` is a soft round light 256 pixels across |
| | `texture_scale` | `float` | `1` | Scales the texture |
| | `offset` | `Vec2` | `[0, 0]` | Moves the texture from the light, in pixels |
| | `color` | `Vec4` | `[1, 1, 1, 1]` | The light's color, as [r, g, b, a] from 0 to 1 |
| | `energy` | `float` | `1` | Multiplies the color |
| | `blend_mode` | `std::string` | `"add"` | How the light changes what it lights: `add` brightens, `sub` darkens and `mix` blends toward its color |
| | `height` | `float` | `0` | Pixels above the plane, for normal maps: at 0 it lights them only from the side |
| | `range_z_min`, `range_z_max` | `int` | `-1024`, `1024` | Lights sprites and layers whose `z_index` is in this range |
| | `range_layer_min`, `range_layer_max` | `int` | `0`, `0` | Lights the [canvases](#canvas-layers-and-parallax) whose layer is in this range, from -512 to 512: the world is layer 0, a `CanvasLayer` its `layer` |
| | `range_item_cull_mask` | `int` | `1` | Lights sprites and layers whose `light_mask` shares a bit |
| | `shadow_enabled` | `bool` | `false` | `LightOccluder2D` polygons cast shadows from it |
| | `shadow_color` | `Vec4` | `[0, 0, 0, 0]` | What shadowed pixels get instead of the light; alpha is times the light's |
| | `shadow_filter` | `std::string` | `"none"` | Shadow edges: hard (`none`), or softened over 5 or 13 samples (`pcf5`, `pcf13`) |
| | `shadow_filter_smooth` | `float` | `0` | Spreads the filter's samples further apart |
| | `shadow_item_cull_mask` | `int` | `1` | Occluders whose `occluder_light_mask` shares a bit cast shadows |
| `DirectionalLight2D` | `enabled`, `color`, `energy`, `blend_mode` | | | As `PointLight2D`'s. It lights everything, down the screen turned by its `Transform2D` rotation |
| | `height` | `float` | `0` | For normal maps: from 0, along the plane, to 1, straight down onto it |
| | `max_distance` | `float` | `10000` | How far shadows reach past their occluders, in pixels |
| | `range_*`, `shadow_*` | | | As `PointLight2D`'s |
| `LightOccluder2D` | `polygon` | `std::vector<Vec2>` | `[]` | Points as [x, y] in pixels, placed by the entity's `Transform2D` |
| | `closed` | `bool` | `true` | Join the last point to the first |
| | `cull_mode` | `std::string` | `"disabled"` | `clockwise` or `counter_clockwise` leaves out edges that run that way around the light, as seen on screen |
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
| `AnimatableBody2D` | `collision_layer`, `collision_mask`, `friction`, `bounce` | | | As `StaticBody2D`'s. It goes where its `Transform2D` says, such as a moving platform |
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
| | `floor_stop_on_slope` | `bool` | `true` | Don't slide down slopes while standing |
| | `floor_max_angle` | `float` | `0.785398` | Steepest floor, in radians from `up_direction` |
| | `floor_snap_length` | `float` | `1` | How far down it keeps to a floor while walking off steps and slopes, in pixels |
| | `slide_on_ceiling` | `bool` | `true` | Slide along ceilings; `false` stops and falls |
| | `platform_on_leave` | `std::string` | `"add_velocity"` | What it keeps of a moving floor's velocity on leaving it: `add_velocity`, `add_upward_velocity` or `do_nothing` |
| | `on_floor`, `on_wall`, `on_ceiling` | `bool` | `false` | What the last move touched; the physics step writes them |
| | `floor_normal`, `wall_normal` | `Vec2` | `[0, 0]` | The floor's and the wall's normals in the last move |
| | `platform_velocity` | `Vec2` | `[0, 0]` | The velocity of the moving floor it stands on |
| `Area2D` | `collision_layer` | `int` | `1` | As `StaticBody2D`'s |
| | `collision_mask` | `int` | `1` | The layers of bodies and areas it detects |
| | `monitoring` | `bool` | `true` | Emit `body_entered`, `body_exited`, `area_entered` and `area_exited` events |
| | `monitorable` | `bool` | `true` | Other areas detect it |
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
| | `texture` | `std::string` | `""` | Project path of a PNG or BMP file each particle draws, centered on it; `""` draws a white pixel |
| | `texture_filter`, `z_index`, `visible`, `material`, `material_params`, `light_mask` | | | As `Sprite2D`'s |
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
| `Control` | `children` | `std::vector<entity>` | `[]` | [Controls](#ui-and-text) placed in its rect and drawn over it, in this order; each control has one parent at most |
| | `anchor_left`, `anchor_top`, `anchor_right`, `anchor_bottom` | `float` | `0` | Where each edge is anchored in the parent's rect (or the viewport's): 0 at its left or top, 1 at its right or bottom |
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
| | `z_index` | `int` | `0` | Draws over controls with a lower one, added to its parent's; -4096 to 4096 |
| | `theme_override_colors` | `std::map<std::string, Vec4>` | `{}` | [Theme](#theme-items) colors by name, e.g. `{"font_color": [1, 0.8, 0.2, 1]}` |
| | `theme_override_constants` | `std::map<std::string, int>` | `{}` | Theme constants by name, e.g. `{"outline_size": 4}` |
| | `theme_override_fonts` | `std::map<std::string, std::string>` | `{}` | Theme fonts by name: a `.ttf`, `.otf` or [`.font.json`](#fonts) file, e.g. `{"font": "fonts/title.ttf"}` |
| | `theme_override_font_sizes` | `std::map<std::string, int>` | `{}` | Theme font sizes in pixels by name, 1 to 1024, e.g. `{"font_size": 32}` |
| | `theme_override_icons` | `std::map<std::string, std::string>` | `{}` | Theme icons by name: a PNG or BMP file, a [built-in icon](#theme-items) such as `"builtin:checked"`, or `""` for none, e.g. `{"checked": "ui/tick.png"}` |
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
| `TextureRect` | `texture` | `std::string` | `""` | Project path of a PNG or BMP file drawn in the rect |
| | `expand_mode` | `std::string` | `"keep_size"` | Its minimum size: `keep_size` (the texture's), `ignore_size` (none), `fit_width` (the rect's height as its width), `fit_height` (its width as its height), or `fit_width_proportional` / `fit_height_proportional`, keeping the texture's aspect |
| | `stretch_mode` | `std::string` | `"scale"` | How the texture fills the rect: `scale`, `tile`, `keep` (its own size), `keep_centered`, `keep_aspect` (scaled to fit inside), `keep_aspect_centered` or `keep_aspect_covered` (scaled to cover it, cut to the rect) |
| | `flip_h`, `flip_v` | `bool` | `false` | Draw it mirrored left to right, or upside down |
| `NinePatchRect` | `texture` | `std::string` | `""` | Project path of a PNG or BMP file, cut into nine parts |
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
| | `icon` | `std::string` | `""` | Project path of a PNG or BMP file drawn beside the text |
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
| `TextureButton` | `texture_normal` | `std::string` | `""` | Project path of the PNG or BMP file it draws; its size is the button's minimum size |
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
| | `right_icon` | `std::string` | `""` | Project path of a PNG or BMP file drawn at the end |
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
| `AudioStreamPlayer2D` | `stream`, `volume_db`, `pitch_scale`, `playing`, `autoplay`, `stream_paused`, `max_polyphony`, `bus` | | | As `AudioStreamPlayer`'s; it plays at the entity's `Transform2D` |
| | `max_distance` | `float` | `2000` | Pixels from the listener beyond which it is silent, 1 or more |
| | `attenuation` | `float` | `1` | How fast it fades with distance: volume is (1 - distance / `max_distance`) to this power |
| | `panning_strength` | `float` | `1` | Times the project's `audio.panning_strength_2d`; 0 for no panning |
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
  added to the parents' (Godot's `z_as_relative`), and the sum is held to -4096 to 4096. A
  parent's `Sprite2D` or `TileMapLayer` passes on its `visible` and `modulate` (`self_modulate`
  stays its own); a `CPUParticles2D` passes on `visible` and `z_index`. Sprites, tile layers,
  particles, occluders and lights draw where their global transforms put them.
- **Physics.** Bodies, areas and joints are where their global transforms put them, and a parent
  that moves moves the bodies in it. The physics step writes a `RigidBody2D`'s motion back as
  Godot sets a body's global transform: its own transform becomes what puts it where Box2D says.
  A `CharacterBody2D` slides in the world and is put back in its parent's space. A body or
  area takes the shapes of its own entity, then those of its children that have a
  `CollisionShape2D` or `CollisionPolygon2D` and no body or area of their own, placed by their
  transforms, as a Godot `CollisionObject2D` takes its shape nodes'; hits and events name the
  body. A character moves as the first of its shapes. Shapes and light textures don't lean
  with `skew`.
- **Sound and particles.** `AudioStreamPlayer2D`, `AudioListener2D` and `CPUParticles2D` work
  from global transforms; particles with `local_coords` move with the emitter's.
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
- **Not yet.** Godot's `top_level`, `z_as_relative` off, and Node2D and Control entities in one
  tree.

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
`canvas_item` shader, written as a node graph instead of shader code. `Sprite2D`'s `material`
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

## Animations

A `.anim.json` file holds keyframe animations by name, like a Godot `AnimationLibrary` of
`Animation` resources. An entity's `AnimationPlayer` names the file in `library` and plays one
of them: each tick the `animation_player` system moves `time` on, sets every field the
animation's value tracks name on the entity's own components (adding a component it lacks), and
emits the events its event tracks name. A track with a `target` acts on another entity instead,
the one the player's `targets` maps that name to, as a Godot track names a node by its path from
the player's root node. `slash-cli schema animation` prints the schema,
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
| `animations/<name>/length` | Yes | Seconds, more than 0 |
| `animations/<name>/loop` | No | What happens at the end, as Godot's loop modes: `none` (default) stops there, `linear` starts over, `pingpong` turns round |
| `animations/<name>/tracks` | Yes | Value tracks and event tracks |
| `tracks/<n>/path` | One of these | A value track: the field it sets, as a component, a field and optionally a part of the field |
| `tracks/<n>/event` | One of these | An event track: the event kind it emits |
| `tracks/<n>/target` | No | The entity it acts on: a name the `AnimationPlayer`'s `targets` maps to an entity. Left out, the player's own entity |
| `tracks/<n>/keys` | Yes | At least one key, in time order, each later than the one before and no later than the length |
| `keys/<n>/time` | Yes | Seconds from the start |
| `keys/<n>/value` | Value tracks | A value the field takes |
| `keys/<n>/data` | No | Event tracks: the event's data, any JSON |
| `keys/<n>/transition` | No | Easing on the way to the next key, as Godot's easing curve: 1 (default) is linear, above 1 eases in, between 0 and 1 eases out, below 0 eases in and out, 0 holds the value until the next key |
| `tracks/<n>/interpolation` | No | Value tracks: `linear` (default), `nearest` (each key's value until the next), `cubic` (a smooth curve through the keys), or `linear_angle` and `cubic_angle`, which turn the short way round, for rotations in radians |
| `tracks/<n>/update` | No | Value tracks: `continuous` sets the value every tick, `discrete` sets a key's value when playback reaches it. `auto` (default) is continuous for float fields and discrete for others: integers, strings, booleans and entity references |
| `tracks/<n>/loop_wrap` | No | Value tracks in a looping animation: before the first key and after the last, move between the last key and the first across the loop's end (default `true`); `false` holds them |

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
  each `AnimationPlayer`'s library loads and has its `animation` (with "did you mean") and that
  its `targets` name every target its tracks use. A player whose file, animation or track can't
  be used logs one warning and plays what it can.
- Animation files reload when they change, like every asset, and exports check and ship them.

## Tile sets

A `.tileset.json` file cuts atlas textures into tiles, names the terrains that connect them and
gives tiles collision polygons, like Godot's `TileSet` with `TileSetAtlasSource`s. A [`TileMapLayer`](#tile-map-layers) draws
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
| `sources` | Yes | Atlas sources by name, at least one |
| `sources/<name>/texture` | Yes | Project path of a PNG or BMP file |
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
| `tiles/<cell>/collision` | No | Its [collision polygons](#tile-collision) |
| `tiles/<cell>/alternatives` | No | [Alternative tiles](#alternative-tiles-and-turned-cells) by number, from `"1"` to `"4095"` |

A tile's pixels start at `margins + column × (region_size + separation)` on each axis. Peering
names only directions the shape has and the terrain set's mode connects by, and only terrains of
the tile's own set. Mistakes fail with the JSON Pointer of the bad value and, for a misspelled
terrain, the closest one. Tile sets reload when they change, like every asset.

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

A cell can also turn whatever tile it holds, as Godot's cell transform flags: after the tile, and
its alternative if any, `:h` flips it left to right, `:v` top to bottom and `:t` swaps its axes,
written in that order (`"slope:h"`, `"rock:1:hv"`, `"ground:3,2:vt"`). A quarter turn clockwise
is `:ht`, counterclockwise `:vt`. The tile draws turned, its collision polygons turn with it, and
an alternative's own flips combine with the cell's. Commands give each turned or alternative tile
a legend key of its own.

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
- `LightOccluder2D` polygons cast shadows from lights with `shadow_enabled`, when its
  `occluder_light_mask` shares a bit with the light's `shadow_item_cull_mask`. Shadows begin at
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
- `GPUParticles2D`, with process materials, is on the [roadmap](../roadmap.md).

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
  the text doesn't fit (it can't be dragged yet), and tabs stop every four spaces.
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
  the `VScrollBar` and `HScrollBar` theme types' styles. Its minimum size is its style box's
  margins, plus its text with `scroll_fit_content_height` or `scroll_fit_content_width`.
- Not yet: several carets, the context menu, gutters, the minimap, syntax highlighting, search,
  drag and drop of text, middle-click paste, dragging the scroll bars and smooth scrolling.

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
| | color | `background_color` | `[0, 0, 0, 0]`, none |
| | constant | `line_spacing` | `4` |
| | constant | `outline_size` | `0` |
| | constant | `caret_width` | `1` |
| | constant | `wrap_offset` | `10`, pixels left free at the right of wrapped rows |
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
| `texture` | texture | | Project path of a PNG or BMP file |
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
from the current `AudioListener2D`. Each plays through a bus of the project's
[bus layout](#bus-layouts), and every bus mixes into Master, which is what plays.
[examples/audio](../../examples/audio/) has music, footsteps and both kinds of bus change.

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
  `max_polyphony` over 1, adds a voice and stops the oldest past the limit. A new `stream`
  stops the old one.
- When a player's last sound ends, it sets `playing` to false and emits a `finished` event with
  `{"component": "AudioStreamPlayer"}` (or `AudioStreamPlayer2D`) as its data.
- An `AudioStreamPlayer2D` fades and pans as Godot's `_update_panning`: its volume is times
  (1 - distance / `max_distance`) ^ `attenuation`, and it pans by its offset from the listener
  over the viewport's width, times `panning_strength` and half of `audio.panning_strength_2d`.
- Volume changes ramp over one mix step of 512 frames, and a stopped sound fades out over one,
  so neither clicks. Sound starts 64 frames late, Godot's lookahead for those fades.
- The voices are not saved: scenes, snapshots, undo and dry runs keep only the components'
  fields. Plays and stops a failed command or dry run asked for are dropped, as are its bus
  changes.

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

A misspelled type is an error naming the closest one, and the type a slot names says which
settings it takes.

### Audio streams

A `.audio.json` file is a sound with Godot's import settings, or Godot's
`AudioStreamRandomizer`: one of several streams each time it plays. A player's `stream` can
name one, as can a randomizer's `streams`.

```json
{"format": 1, "loop": true, "loop_offset": 4, "source": "music/theme.ogg"}
{"format": 1, "random_pitch": 1.1, "streams": [{"stream": "sfx/step1.wav"}, {"stream": "sfx/step2.wav"}]}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `format` | | Must be 1 |
| `source` | | The `.wav`, `.ogg` or `.mp3` file, for a sound |
| `loop_mode` | `"disabled"` | `.wav`: `disabled`, `forward`, `pingpong` or `backward` between `loop_begin` and `loop_end` |
| `loop_begin`, `loop_end` | `0`, `-1` | `.wav`: the frames the loop starts and ends at; -1 is the end of the sound |
| `loop` | `false` | `.ogg` and `.mp3`: play again from `loop_offset` seconds at the end |
| `loop_offset` | `0` | `.ogg` and `.mp3`: seconds into the sound a loop goes back to |
| `bpm`, `beat_count` | `0` | `.ogg` and `.mp3`: with both, loop after that many beats instead of at the end |
| `streams` | | A randomizer's streams, each `{"stream": path, "weight": 1}` |
| `playback_mode` | `"random_no_repeats"` | How a randomizer picks: `random_no_repeats`, `random` or `sequential` |
| `random_pitch` | `1` | A randomizer's pitch scale varies up to this many times higher or lower |
| `random_volume_offset_db` | `0` | A randomizer's volume varies up to this many dB louder or quieter |

- A file is either a sound (`source`) or a randomizer (`streams`), and takes only that one's
  fields.
- A randomizer's choices come from the player's entity ID and how many times it has played,
  so a scene makes the same picks every run.

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
