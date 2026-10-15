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
| `AnimatedSprite2D` | `sprite_frames` | `std::string` | `""` | Project path of a [`.frames.json`](#sprite-frames) file |
| | `animation` | `std::string` | `"default"` | Name of the animation to play |
| | `playing` | `bool` | `true` | Advance the animation each tick |
| | `speed_scale` | `float` | `1` | Multiplies the animation's frames per second; negative plays backwards |
| | `frame` | `int` | `0` | The animation's current frame, from 0 |
| | `frame_progress` | `float` | `0` | How far through the current frame, from 0 to 1 |

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
