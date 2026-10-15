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
| `entities.<id>.extends` | No | A `.prefab.json` file the entity starts from (see [Prefabs](#prefabs)) |
| `entities.<id>.components` | No | Components keyed by type name. Left out, the entity has none, or its prefab's |
| `entities.<id>.components.<Type>` | No | The component's fields, or `null` to remove a component the prefab has |
| `entities.<id>.components.<Type>.<field>` | No | A field value. Left out, the field keeps its default, or its prefab's value |

No other keys are allowed at any level: an unknown key is an error, with a "did you mean"
suggestion when it is close to a valid one.

## IDs

Entity IDs are 1 to 64 characters from `A-Z`, `a-z`, `0-9`, `_` and `-`, such as `player` or
`enemy_03`. They are stable: saving, loading and undo keep them, and commands refer to entities by
them.

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
| `Transform2D` | `position` | `Vec2` | `[0, 0]` | Position in pixels, as [x, y]; y points down |
| | `rotation` | `float` | `0` | Rotation in radians; positive turns clockwise |
| | `scale` | `Vec2` | `[1, 1]` | Scale factor on each axis |
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
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Color the texture is multiplied by, as [r, g, b, a] from 0 to 1 |
| | `z_index` | `int` | `0` | Higher draws on top, -4096 to 4096; equal values draw in the order the entities were made |
| | `visible` | `bool` | `true` | Draw it |
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
| `TileMapLayer` | `tile_set` | `std::string` | `""` | Project path of a [`.tileset.json`](#tile-sets) file |
| | `origin` | `Vec2i` | `[0, 0]` | The cell the first key of the first row stands for, as [column, row] |
| | `legend` | `std::map<std::string, std::string>` | `{}` | The tile each key in `rows` stands for, by name or as `"source:column,row"` |
| | `rows` | `std::vector<std::string>` | `[]` | The [cells](#tile-map-layers), one string of keys per row from `origin` rightward; `.` is an empty cell |
| | `z_index` | `int` | `0` | Higher draws on top, -4096 to 4096; layers and sprites draw in one order |
| | `visible` | `bool` | `true` | Draw it |
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Color every tile is multiplied by, as [r, g, b, a] from 0 to 1 |
| | `texture_filter` | `std::string` | `"inherit"` | `"linear"`, `"nearest"` (sharp pixel art) or `"inherit"`, which uses the project's `rendering.texture_filter` |
| | `light_mask` | `int` | `1` | [Lit](#2d-lights) by lights whose `range_item_cull_mask` shares a bit |
| `PointLight2D` | `enabled` | `bool` | `true` | Shine |
| | `texture` | `std::string` | `""` | Project path of a PNG or BMP file whose color and alpha shape the light; `""` is a soft round light 256 pixels across |
| | `texture_scale` | `float` | `1` | Scales the texture |
| | `offset` | `Vec2` | `[0, 0]` | Moves the texture from the light, in pixels |
| | `color` | `Vec4` | `[1, 1, 1, 1]` | The light's color, as [r, g, b, a] from 0 to 1 |
| | `energy` | `float` | `1` | Multiplies the color |
| | `blend_mode` | `std::string` | `"add"` | How the light changes what it lights: `add` brightens, `sub` darkens and `mix` blends toward its color |
| | `height` | `float` | `0` | Pixels above the plane, for normal maps: at 0 it lights them only from the side |
| | `range_z_min`, `range_z_max` | `int` | `-1024`, `1024` | Lights sprites and layers whose `z_index` is in this range |
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
| `CanvasModulate` | `color` | `Vec4` | `[1, 1, 1, 1]` | Tints everything lit: the light a scene has where no light reaches. One per scene |
| `CollisionShape2D` | `shape` | `std::string` | `"rectangle"` | A [physics](#2d-physics) shape of the entity's body or area: `rectangle`, `circle`, `capsule`, `segment` or `polygon` |
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
| | `motor_enabled` | `bool` | `false` | Turn `node_b` at `motor_target_velocity` |
| | `motor_target_velocity` | `float` | `0` | In radians per second |
| `DampedSpringJoint2D` | `node_a`, `node_b`, `disable_collision` | | | As `PinJoint2D`'s; a spring from `node_a` at the entity's `Transform2D` to `node_b` |
| | `length` | `float` | `50` | Where `node_b` is attached, in pixels along the entity's y axis |
| | `rest_length` | `float` | `0` | The length it pulls toward; 0 is `length` |
| | `stiffness` | `float` | `20` | How hard it pulls |
| | `damping` | `float` | `1` | How fast it settles |
| `GrooveJoint2D` | `node_a`, `node_b`, `disable_collision` | | | As `PinJoint2D`'s; `node_b` slides along a groove of `node_a`'s and turns freely |
| | `length` | `float` | `50` | The groove's length in pixels, from the entity's `Transform2D` along its y axis |
| | `initial_offset` | `float` | `25` | Where `node_b` starts along the groove, in pixels |
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
| | `modulate` | `Vec4` | `[1, 1, 1, 1]` | Tints it and its children, as [r, g, b, a] |
| | `self_modulate` | `Vec4` | `[1, 1, 1, 1]` | Tints it only |
| | `z_index` | `int` | `0` | Draws over controls with a lower one, added to its parent's; -4096 to 4096 |
| | `theme_override_colors` | `std::map<std::string, Vec4>` | `{}` | [Theme](#theme-items) colors by name, e.g. `{"font_color": [1, 0.8, 0.2, 1]}` |
| | `theme_override_constants` | `std::map<std::string, int>` | `{}` | Theme constants by name, e.g. `{"outline_size": 4}` |
| | `theme_override_fonts` | `std::map<std::string, std::string>` | `{}` | Theme fonts by name: a `.ttf`, `.otf` or [`.font.json`](#fonts) file, e.g. `{"font": "fonts/title.ttf"}` |
| | `theme_override_font_sizes` | `std::map<std::string, int>` | `{}` | Theme font sizes in pixels by name, 1 to 1024, e.g. `{"font_size": 32}` |
| | `theme_override_styles` | `std::map<std::string, json>` | `{}` | Theme [style boxes](#style-boxes) by name, e.g. `{"panel": {"bg_color": [0.1, 0.1, 0.2, 1], "corner_radius": 8}}` |
| | `theme` | `std::string` | `""` | A [`.theme.json`](#themes) file whose items it and its children use before the project's theme |
| | `theme_type_variation` | `std::string` | `""` | A theme type to look its items up in before its own: a variation with a `base_type`, e.g. `"Header"` |
| | `size_flags_horizontal`, `size_flags_vertical` | `std::string` | `"fill"` | How a [container](#containers) sizes it on this axis: `fill`, `expand_fill`, `expand`, `shrink_begin`, `shrink_center`, `shrink_end`, `expand_shrink_center` or `expand_shrink_end` |
| | `size_flags_stretch_ratio` | `float` | `1` | Its share of the space expanding children split in a container |
| | `texture_filter` | `std::string` | `"inherit"` | How its textures are sampled when scaled: `linear`, `nearest`, or `inherit` its parent's, then the project's `rendering.texture_filter` |
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

`engine.describe` lists every registered component with its defaults, including ones a game adds.

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

A prefab is one entity's components in a `.prefab.json` file, like a Godot scene that other
scenes instance. An entity that `"extends"` it starts with the prefab's components, and its own
fields override the prefab's. With `Health` as a game's component, `prefabs/enemy.prefab.json`:

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
emits the events its event tracks name. `slash-cli schema animation` prints the schema,
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
         "keys": [{"time": 0, "data": {"foot": "left"}}, {"time": 0.25, "data": {"foot": "right"}}]}
      ]
    }
  },
  "format": 1
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
  the entity's ID and the key's `data`, and discrete tracks set their key's value.
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
  `slash-cli scene validate` and exports check all of these. A player whose file, animation or
  track can't be used logs one warning and plays what it can.
- Animation files reload when they change, like every asset, and exports check and ship them.

## Tile sets

A `.tileset.json` file cuts atlas textures into tiles and names the terrains that connect them,
like Godot's `TileSet` with `TileSetAtlasSource`s. A [`TileMapLayer`](#tile-map-layers) draws
its tiles. `slash-cli schema tileset` prints the schema, `slash-cli scene validate` and
`scene format` accept `.tileset.json` files, and exports check and ship them.
[examples/tilemaps](../../examples/tilemaps/) has one with grass, water and the tiles between.

```json
{
  "format": 1,
  "tile_size": [16, 16],
  "terrain_sets": {"ground": {"terrains": ["grass", "water"]}},
  "sources": {
    "ground": {
      "texture": "tiles/ground.png",
      "tiles": {
        "1,1": {"name": "grass", "terrain": "grass"},
        "1,0": {"name": "grass_n", "terrain": "grass",
                "peering": {"n": "water", "ne": "water", "nw": "water"}},
        "3,0": {"name": "water", "terrain": "water"},
        "3,2": {"name": "rock"}
      }
    }
  }
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `tile_size` | No | A cell's width and height in pixels; default `[16, 16]` |
| `terrain_sets` | No | Terrain sets by name |
| `terrain_sets/<name>/mode` | No | How tiles connect, as Godot's terrain modes: `match_corners_and_sides` (default) by all eight neighbors, `match_corners` by the four at their corners, `match_sides` by the four at their sides |
| `terrain_sets/<name>/terrains` | Yes | Terrain names, unique in the file |
| `sources` | Yes | Atlas sources by name, at least one |
| `sources/<name>/texture` | Yes | Project path of a PNG or BMP file |
| `sources/<name>/region_size` | No | Each tile's width and height in the texture; default `tile_size` |
| `sources/<name>/margins` | No | Pixels before the first column and row; default `[0, 0]` |
| `sources/<name>/separation` | No | Pixels between columns and rows; default `[0, 0]` |
| `sources/<name>/tiles` | No | Tiles by `"column,row"` in the texture's grid, as `"2,0"` |
| `tiles/<cell>/name` | No | What legends and `tilemap.set` call the tile: unique in the file, no `:`. A tile without one is `"source:column,row"` |
| `tiles/<cell>/terrain` | No | The terrain it paints |
| `tiles/<cell>/peering` | No | The terrain each neighbor must have for the tile to fit, by direction: `n`, `ne`, `e`, `se`, `s`, `sw`, `w`, `nw`, as Godot's terrain peering bits. A direction left out must have the tile's own terrain; `null` means none |
| `tiles/<cell>/probability` | No | How often painting picks the tile over others that fit as well; default `1`, and `0` only when nothing else fits |

A tile's pixels start at `margins + column × (region_size + separation)` on each axis. Peering
names only directions the terrain set's mode connects by, and only terrains of the tile's own
set. Mistakes fail with the JSON Pointer of the bad value and, for a misspelled terrain, the
closest one. Tile sets reload when they change, like every asset.

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
  cell (cell `(c, r)` spans `c × tile_size` to `(c + 1) × tile_size`), row by row. Tiles from one
  texture draw together, so a layer whose tiles share an atlas is one GPU draw.
- `slash-cli scene validate` and exports check every layer's legend and rows against its tile
  set; a layer that takes part of itself from a prefab is checked in the prefab. A layer that
  can't draw, or a tile outside its texture, logs one warning and draws the rest.
- Square tiles only for now; isometric and hexagonal shapes are on the
  [roadmap](../roadmap.md).

## 2D lights

Lights follow Godot's 2D lighting. Draws are unshaded until a scene has an enabled light or a
`CanvasModulate`. Then each lit pixel of a sprite or tile layer starts from the color its
material made, times the `CanvasModulate` color (the ambient light, white if there is none), and
each light whose `range_item_cull_mask` shares a bit with the draw's `light_mask`, and whose z
range holds its `z_index`, adds its color times that pixel color, subtracts it or mixes toward
it. [examples/lights-2d](../../examples/lights-2d/) has a torch, the moon, a shadow and a normal
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
- With a `normal_texture`, a pixel takes a light by the angle between its normal and the light:
  for a point light, toward its position raised `height` pixels; for a directional light, its
  direction tipped `height` of the way toward the viewer. A `specular_texture` adds Blinn-Phong
  highlights. Without a normal map, a pixel faces the viewer and takes each light whole.
- `LightOccluder2D` polygons cast shadows from lights with `shadow_enabled`, when its
  `occluder_light_mask` shares a bit with the light's `shadow_item_cull_mask`. Shadows begin at
  the first edge a ray from the light crosses, so an occluder with `cull_mode` `disabled`
  shadows its own sprite. For a polygon wound clockwise on screen, `counter_clockwise` leaves
  out the edges facing the light, so the shadow starts at its far side and its sprite is lit.
  A directional light's shadows reach `max_distance` pixels past their occluders.
- A frame takes up to 64 lights, in the order their entities were made; more, a light whose
  texture doesn't load, or a second `CanvasModulate` is left out with one warning in the log.
- A material's `light_mode` opts a sprite out of lighting (`unshaded`, for glowing signs and UI)
  or shows it only where lights reach (`light_only`).
- [`light.sample`](commands.md#lightsample) computes the color a white pixel gets at points, and
  which lights reach them, the way the GPU does, so an agent checks lighting without a
  screenshot.

## 2D physics

2D physics follows Godot's, on [Box2D](https://box2d.org) v3. An entity is a body or an area by
having one of `StaticBody2D`, `AnimatableBody2D`, `RigidBody2D`, `CharacterBody2D` or `Area2D`,
placed by its `Transform2D`, and takes its shapes from its `CollisionShape2D` and
`CollisionPolygon2D`. Lengths are pixels and y points down. The `physics2d` system steps the
world each tick at priority 950, after gameplay systems and keyframe animation; gravity and
damping come from `physics_2d` in [`project.json`](project-format.md).
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
  placed by their node; a `null` `node_a` joins `node_b` to the world.
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
`NinePatchRect` and `ProgressBar` draw in it. An entity with one of those, or a container, and
no `Control` is placed with `Control`'s defaults. `CanvasRenderer` draws controls above the
world, unlit. [examples/ui-and-text](../../examples/ui-and-text/) is a pause screen, and
[examples/ui-containers](../../examples/ui-containers/) a themed inventory screen.

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
- Characters the font doesn't have are counted in `ui.layout`'s `missing_glyphs`. Text is shaped with HarfBuzz, one run per script; mixing
  right-to-left and left-to-right text in one line (BiDi) and fallback fonts are on the
  [roadmap](../roadmap.md).

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
| `types` | Items by type: `colors`, `constants`, `fonts`, `font_sizes` and `styles`, each by name |

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
  ones), base types that lead nowhere or round in a loop, style boxes and font paths. A theme
  that doesn't load, and a `theme_type_variation` no theme defines, are reported by
  `ui.layout` and `scene validate` on the control.
- Themes reload when their files change, as every asset does.

### Style boxes

A style box is what a panel or a bar draws behind its content, and the margins its content
keeps, as Godot's `StyleBox`. In a theme's `styles` or a control's `theme_override_styles` it is
an object with a `type` and Godot's property names:

| Field | Types | Default | Meaning |
| --- | --- | --- | --- |
| `type` | | `"flat"` | `flat` (Godot's `StyleBoxFlat`), `texture` (`StyleBoxTexture`) or `empty` (`StyleBoxEmpty`) |
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

- Glyph images are made the first time they are drawn, for each font, size and outline (once
  per glyph with a distance field), and packed into atlas textures.
- A font that doesn't load is reported by `ui.layout` and `scene validate`, and the label draws
  with the built-in font.
- Fonts reload when their files change, as every asset does.
