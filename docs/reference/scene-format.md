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
